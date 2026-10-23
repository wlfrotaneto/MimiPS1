// MIPS R3000A interpreter: the reference execution engine.
//
// Implements the architectural behaviour games and the real BIOS depend on:
// branch delay slots, load delay slots (including the LWL/LWR forwarding rule),
// COP0 exceptions with EPC/BD/SR stacking, hardware interrupts, overflow traps,
// address errors and cache isolation (stores are dropped while SR.IsC is set).
//
// Statically recompiled functions run natively on the host stack. The interpreter
// calls them at call boundaries; native code calls back into the interpreter with
// ps1_interp_call() when it reaches code that was never recompiled.
#include "mimips1_interp.h"
#include "mimips1_runtime.h"
#include "mimips1_bios.h"
#include "mimips1_gte.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

// Fake return addresses used to detect the end of a nested interpreter run
#define MARKER_INTERRUPT 0xBFC7FF00u // kernel resumes here after servicing an interrupt
#define MARKER_SYSCALL   0xBFC7FF10u // kernel resumes at EPC + 4 after a syscall

#define EXC_INT   0
#define EXC_ADEL  4
#define EXC_ADES  5
#define EXC_SYS   8
#define EXC_BP    9
#define EXC_RI    10
#define EXC_CPU   11
#define EXC_OV    12

// Exceptions raised from native code are delivered to the kernel, which finishes by jumping to the
// EPC it was given (a marker). That jump may happen deep inside nested native/interpreter frames
// (ReturnFromException restores a thread context and never returns to its caller), so every
// kernel entry registers a recovery point and the C stack is unwound to it.
typedef struct KernelEntry {
    uint32_t stop_pc;
    jmp_buf buf;
    struct KernelEntry* prev;
} KernelEntry;
static KernelEntry* s_kentry;
static int s_bios_trace = -1;

static inline bool interp_hle_vector(uint32_t pc) {
    uint32_t p = pc & 0x1FFFFFFF;
    return p == 0xA0 || p == 0xB0 || p == 0xC0;
}

// Ring buffer of the most recent PCs, printed when an unexpected exception is raised
#define PC_RING 64
static uint32_t s_pc_ring[PC_RING][2];
static unsigned s_pc_ring_pos;
static bool s_pc_trace;

static void dump_pc_ring(PS1Context* ctx) {
    for (unsigned i = 0; i < PC_RING; i++) {
        unsigned k = (s_pc_ring_pos + i) % PC_RING;
        if (s_pc_ring[k][0]) printf("  [PCRING] %08X: %08X\n", s_pc_ring[k][0], s_pc_ring[k][1]);
    }
    (void)ctx;
}

static void raise_exception(PS1Context* ctx, uint32_t code, uint32_t epc_pc, bool in_ds, uint32_t badvaddr) {
    PS1CPU* c = &ctx->cpu;
    uint32_t sr = c->cop0[COP0_SR];
    uint32_t cause = c->cop0[COP0_CAUSE];

    if (code != EXC_INT && code != EXC_SYS) {
        PS1_LOG(1, "[EXC] code=%u epc=%08X bd=%d badv=%08X insn=%08X sr=%08X ra=%08X sp=%08X\n", code, epc_pc, in_ds, badvaddr,
                ps1_mem_read32(ctx, epc_pc), sr, c->r[31], c->r[29]);
        if (g_mimips1_log_level >= 1 && getenv("MIMIPS1_PCTRACE")) {
            dump_pc_ring(ctx);
            for (int i = 0; i < 32; i += 8)
                printf("  [REGS] r%02d: %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->r[i], c->r[i+1], c->r[i+2], c->r[i+3], c->r[i+4], c->r[i+5], c->r[i+6], c->r[i+7]);
        }
    }
    else {
        PS1_LOG(3, "[EXC] code=%u epc=%08X sr=%08X a0=%08X t1=%08X\n", code, epc_pc, sr, c->r[4], c->r[9]);
    }
    cause = (cause & ~0x7Cu) | (code << 2);
    if (in_ds) cause |= (1u << 31); else cause &= ~(1u << 31);
    c->cop0[COP0_CAUSE] = cause;
    c->cop0[COP0_EPC] = in_ds ? epc_pc - 4 : epc_pc;
    if (code == EXC_ADEL || code == EXC_ADES) c->cop0[COP0_BADVADDR] = badvaddr;

    // Push the KU/IE stack
    c->cop0[COP0_SR] = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);

    uint32_t vec = (sr & SR_BEV) ? 0xBFC00180u : 0x80000080u;
    c->exc_level++;
    c->pc = vec;
    c->next_pc = vec + 4;
    c->is_in_delay_slot = false;
    // A load issued by the previous instruction still completes
    if (c->ld_reg) c->r[c->ld_reg] = c->ld_val;
    c->ld_reg = 0;
    c->r[0] = 0;
}

static inline bool irq_ready(const PS1CPU* c) {
    uint32_t sr = c->cop0[COP0_SR];
    return (sr & SR_IEc) && (c->cop0[COP0_CAUSE] & sr & 0x0700);
}

// Runs the interpreter until pc == stop_pc (and sp == stop_sp when check_sp) or the
// emulator stops. Used both as the master loop (stop_pc = 0xFFFFFFFF) and for nested calls.
static void interp_loop(PS1Context* ctx, uint32_t stop_pc, bool check_sp, uint32_t stop_sp) {
    PS1CPU* c = &ctx->cpu;
    uint32_t* r = c->r;
    unsigned tick_count = 0;

    for (;;) {
        uint32_t pc = c->pc;

        if (pc == stop_pc && (!check_sp || r[29] == stop_sp)) return;
        if (__builtin_expect(!ctx->running, 0)) return;

        if (__builtin_expect((pc & 0xFFFFFFE0u) == (MARKER_INTERRUPT & 0xFFFFFFE0u), 0)) {
            // The kernel resumed a context that was interrupted in native code: unwind to its owner
            for (KernelEntry* e = s_kentry; e; e = e->prev) {
                if (e->stop_pc == pc) longjmp(e->buf, 1);
            }
        }

        if (__builtin_expect(++tick_count >= 16, 0)) {
            tick_count = 0;
            ps1_cpu_tick(ctx, (16 * ctx->cpi) >> 1);
        }

        if (__builtin_expect(ctx->bios_boot && irq_ready(c), 0)) {
            // The interrupted instruction re-executes after the handler returns
            raise_exception(ctx, EXC_INT, pc, c->is_in_delay_slot, 0);
            continue;
        }

        if (__builtin_expect(ctx->bios_boot && pc == ctx->exe_entry && !ctx->native_enabled && ctx->exe_shadow && !ctx->native_tried, 0)) {
            ctx->native_tried = true;
            ps1_native_try_enable(ctx);
        }

        if (__builtin_expect(c->hook_pending, 0)) {
            // A call or tail jump just landed on a recompiled function entry
            c->hook_pending = false;
            uint32_t off = (pc & 0x001FFFFC) >> 2;
            if (ctx->native_enabled && c->exc_level == 0 && ps1_native_entry_allowed(pc) && (c->entry_bits[off >> 3] & (1u << (off & 7)))) {
                PS1RecompiledFunc fn = c->func_table[off];
                if (fn) {
                    // A load issued in the call's delay slot completes before the callee's code runs on
                    if (c->ld_reg) { r[c->ld_reg] = c->ld_val; c->ld_reg = 0; r[0] = 0; }
                    c->pc = pc;
                    bool saved_native = ctx->in_native;
                    ctx->in_native = true;
                    fn(ctx);
                    ctx->in_native = saved_native;
                    // The function returned to its caller: resume at $ra
                    c->pc = r[31];
                    c->next_pc = r[31] + 4;
                    c->is_in_delay_slot = false;
                    c->ld_reg = 0;
                    continue;
                }
            }
        }

        if (!ctx->bios_boot && __builtin_expect(interp_hle_vector(pc), 0)) {
            // Built-in kernel (HLE) entry points: run the service and return to $ra
            uint32_t p = pc & 0x1FFFFFFF;
            if (p == 0xA0) ps1_bios_handle_a0(ctx, r[9]);
            else if (p == 0xB0) ps1_bios_handle_b0(ctx, r[9]);
            else ps1_bios_handle_c0(ctx, r[9]);
            c->pc = r[31];
            c->next_pc = r[31] + 4;
            c->is_in_delay_slot = false;
            continue;
        }

        if (__builtin_expect(s_bios_trace, 0)) {
            uint32_t p = pc & 0x1FFFFFFF;
            if (pc == 0xBFC0D970u || pc == 0xA000D970u) printf("[BIOS] DeliverEvent(internal) %08X %08X ra=%08X frame=%llu\n", r[4], r[5], r[31], (unsigned long long)ctx->frame_count);
            static uint32_t hits[8]; static int nhits = -1;
            if (nhits < 0) { nhits = 0; const char* e = getenv("MIMIPS1_PCHIT"); while (e && *e && nhits < 8) { hits[nhits++] = (uint32_t)strtoul(e, (char**)&e, 16); if (*e == ',') e++; } }
            for (int hi = 0; hi < nhits; hi++) if ((pc & 0x1FFFFFFF) == (hits[hi] & 0x1FFFFFFF))
                printf("[HIT] %08X v0=%08X v1=%08X a0=%08X a1=%08X t3=%08X ra=%08X frame=%llu\n", pc, r[2], r[3], r[4], r[5], r[11], r[31], (unsigned long long)ctx->frame_count);
            static uint32_t ret_pc, ret_fn, tev;
            if (ret_pc && pc == ret_pc) { if ((ret_fn & 0xFFFF) != 0xB00B || r[2] == 1) printf("[BIOS]   -> %02X returned v0=%08X ev=%08X\n", ret_fn, r[2], tev); ret_pc = 0; }
            uint32_t fnid = (p << 8) | (r[9] & 0xFF);
            if (fnid == 0xB00B) { tev = r[4]; ret_pc = r[31]; ret_fn = fnid; }
            bool noisy = fnid == 0xB00B || fnid == 0xB016 || fnid == 0xB017 || fnid == 0xA02F || fnid == 0xB007 || fnid == 0xB05B ||
                         fnid == 0xC00A || fnid == 0xB03F || fnid == 0xB03D || fnid == 0xB020 || fnid == 0xB00A || fnid == 0xB00C;
            if ((p == 0xA0 || p == 0xB0 || p == 0xC0) && !noisy) { ret_pc = r[31]; ret_fn = fnid; }
            if ((p == 0xA0 || p == 0xB0 || p == 0xC0) && !noisy)
                printf("[BIOS] %02X:%02X a0=%08X a1=%08X a2=%08X ra=%08X frame=%llu\n", (unsigned)p, (unsigned)r[9], r[4], r[5], r[6], r[31], (unsigned long long)ctx->frame_count);
        }
        uint32_t npc = c->next_pc;
        bool in_ds = c->is_in_delay_slot;
        uint32_t insn = ps1_mem_read32(ctx, pc);
#ifdef PS1_VERIFY
        if (__builtin_expect(ps1_step_file != NULL, 0)) {
            uint32_t sv = c->r[c->ld_reg & 31]; if (c->ld_reg) c->r[c->ld_reg] = c->ld_val;
            ps1_step_print(ctx, pc);
            if (c->ld_reg) c->r[c->ld_reg] = sv;
        }
#endif
        if (__builtin_expect(s_pc_trace, 0)) {
            s_pc_ring[s_pc_ring_pos][0] = pc;
            s_pc_ring[s_pc_ring_pos][1] = insn;
            s_pc_ring_pos = (s_pc_ring_pos + 1) % PC_RING;
        }

        // Load delay: the value of a load issued by the previous instruction becomes
        // visible only after this instruction has executed.
        uint32_t lreg = c->ld_reg;
        uint32_t lval = c->ld_val;
        c->ld_reg = 0;

        uint32_t next_npc = npc + 4;
        bool is_branch = false;
        bool hookable = false; // jump kinds that may enter a recompiled function

        const uint32_t op = insn >> 26;
        const uint32_t rs = (insn >> 21) & 31;
        const uint32_t rt = (insn >> 16) & 31;
        const uint32_t rd = (insn >> 11) & 31;
        const uint32_t sa = (insn >> 6) & 31;
        const uint32_t imm = insn & 0xFFFF;
        const int32_t simm = (int16_t)imm;

        // Write helpers honour "a later write to the same register cancels the pending load"
#define WR(reg, val) do { uint32_t _r = (reg); r[_r] = (val); if (_r == lreg) lreg = 0; } while (0)
#define LD(reg, val) do { uint32_t _r = (reg); if (_r == lreg) lreg = 0; c->ld_reg = _r; c->ld_val = (val); } while (0)
#define BRANCH(cond, tgt) do { is_branch = true; if (cond) next_npc = (tgt); } while (0)
#define EXC(code, bad) do { if (lreg) { r[lreg] = lval; lreg = 0; } raise_exception(ctx, (code), pc, in_ds, (bad)); goto next_insn_exception; } while (0)

        switch (op) {
        case 0x00: { // SPECIAL
            switch (insn & 0x3F) {
            case 0x00: WR(rd, r[rt] << sa); break;                              // SLL
            case 0x02: WR(rd, r[rt] >> sa); break;                              // SRL
            case 0x03: WR(rd, (uint32_t)((int32_t)r[rt] >> sa)); break;         // SRA
            case 0x04: WR(rd, r[rt] << (r[rs] & 31)); break;                    // SLLV
            case 0x06: WR(rd, r[rt] >> (r[rs] & 31)); break;                    // SRLV
            case 0x07: WR(rd, (uint32_t)((int32_t)r[rt] >> (r[rs] & 31))); break; // SRAV
            case 0x08: BRANCH(true, r[rs]); hookable = (rs != 31); break;       // JR
            case 0x09: { uint32_t t = r[rs]; WR(rd, pc + 8); BRANCH(true, t); hookable = true; break; } // JALR
            case 0x0C: // SYSCALL
                if (ctx->bios_boot) EXC(EXC_SYS, 0);
                c->pc = pc;
                ps1_trigger_syscall(ctx);
                break;
            case 0x0D: EXC(EXC_BP, 0);                                          // BREAK
            case 0x10: WR(rd, c->hi); break;                                    // MFHI
            case 0x11: c->hi = r[rs]; break;                                    // MTHI
            case 0x12: WR(rd, c->lo); break;                                    // MFLO
            case 0x13: c->lo = r[rs]; break;                                    // MTLO
            case 0x18: { int64_t v = (int64_t)(int32_t)r[rs] * (int64_t)(int32_t)r[rt]; c->lo = (uint32_t)v; c->hi = (uint32_t)(v >> 32); break; } // MULT
            case 0x19: { uint64_t v = (uint64_t)r[rs] * (uint64_t)r[rt]; c->lo = (uint32_t)v; c->hi = (uint32_t)(v >> 32); break; }             // MULTU
            case 0x1A: { // DIV (results for the undefined cases match the hardware)
                int32_t n = (int32_t)r[rs], d = (int32_t)r[rt];
                if (d == 0) { c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; c->hi = (uint32_t)n; }
                else if (n == INT32_MIN && d == -1) { c->lo = 0x80000000u; c->hi = 0; }
                else { c->lo = (uint32_t)(n / d); c->hi = (uint32_t)(n % d); }
                break;
            }
            case 0x1B: { // DIVU
                uint32_t n = r[rs], d = r[rt];
                if (d == 0) { c->lo = 0xFFFFFFFFu; c->hi = n; }
                else { c->lo = n / d; c->hi = n % d; }
                break;
            }
            case 0x20: { int32_t v; if (__builtin_add_overflow((int32_t)r[rs], (int32_t)r[rt], &v)) EXC(EXC_OV, 0); WR(rd, (uint32_t)v); break; } // ADD
            case 0x21: WR(rd, r[rs] + r[rt]); break;                            // ADDU
            case 0x22: { int32_t v; if (__builtin_sub_overflow((int32_t)r[rs], (int32_t)r[rt], &v)) EXC(EXC_OV, 0); WR(rd, (uint32_t)v); break; } // SUB
            case 0x23: WR(rd, r[rs] - r[rt]); break;                            // SUBU
            case 0x24: WR(rd, r[rs] & r[rt]); break;                            // AND
            case 0x25: WR(rd, r[rs] | r[rt]); break;                            // OR
            case 0x26: WR(rd, r[rs] ^ r[rt]); break;                            // XOR
            case 0x27: WR(rd, ~(r[rs] | r[rt])); break;                         // NOR
            case 0x2A: WR(rd, (int32_t)r[rs] < (int32_t)r[rt]); break;          // SLT
            case 0x2B: WR(rd, r[rs] < r[rt]); break;                            // SLTU
            default: EXC(EXC_RI, 0);
            }
            break;
        }
        case 0x01: { // REGIMM
            bool link = (rt & 0x1E) == 0x10;
            bool ge = rt & 1;
            int32_t v = (int32_t)r[rs];
            uint32_t tgt = pc + 4 + ((uint32_t)simm << 2);
            if (link) WR(31, pc + 8);
            BRANCH(ge ? (v >= 0) : (v < 0), tgt);
            break;
        }
        case 0x02: BRANCH(true, (pc & 0xF0000000u) | ((insn & 0x03FFFFFF) << 2)); hookable = true; break; // J
        case 0x03: WR(31, pc + 8); BRANCH(true, (pc & 0xF0000000u) | ((insn & 0x03FFFFFF) << 2)); hookable = true; break; // JAL
        case 0x04: BRANCH(r[rs] == r[rt], pc + 4 + ((uint32_t)simm << 2)); break;        // BEQ
        case 0x05: BRANCH(r[rs] != r[rt], pc + 4 + ((uint32_t)simm << 2)); break;        // BNE
        case 0x06: BRANCH((int32_t)r[rs] <= 0, pc + 4 + ((uint32_t)simm << 2)); break;   // BLEZ
        case 0x07: BRANCH((int32_t)r[rs] > 0, pc + 4 + ((uint32_t)simm << 2)); break;    // BGTZ
        case 0x08: { int32_t v; if (__builtin_add_overflow((int32_t)r[rs], simm, &v)) EXC(EXC_OV, 0); WR(rt, (uint32_t)v); break; } // ADDI
        case 0x09: WR(rt, r[rs] + (uint32_t)simm); break;                    // ADDIU
        case 0x0A: WR(rt, (int32_t)r[rs] < simm); break;                     // SLTI
        case 0x0B: WR(rt, r[rs] < (uint32_t)simm); break;                    // SLTIU
        case 0x0C: WR(rt, r[rs] & imm); break;                               // ANDI
        case 0x0D: WR(rt, r[rs] | imm); break;                               // ORI
        case 0x0E: WR(rt, r[rs] ^ imm); break;                               // XORI
        case 0x0F: WR(rt, imm << 16); break;                                 // LUI

        case 0x10: { // COP0
            uint32_t sub = rs;
            if (sub == 0x00) { LD(rt, c->cop0[rd]); }                        // MFC0
            else if (sub == 0x04) {                                          // MTC0
                uint32_t v = r[rt];
                switch (rd) {
                case COP0_CAUSE: c->cop0[COP0_CAUSE] = (c->cop0[COP0_CAUSE] & ~0x300u) | (v & 0x300u); break; // only SW interrupt bits writable
                case COP0_SR: c->cop0[COP0_SR] = v; break;
                case COP0_PRID: case COP0_EPC: case COP0_BADVADDR: break;      // read-only here
                default: c->cop0[rd] = v; break;
                }
            }
            else if (sub == 0x10 && (insn & 0x3F) == 0x10) {                 // RFE
                uint32_t sr = c->cop0[COP0_SR];
                c->cop0[COP0_SR] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
                if (c->exc_level > 0) c->exc_level--;
            }
            else EXC(EXC_RI, 0);
            break;
        }
        case 0x12: { // COP2 (GTE)
            if (insn & (1u << 25)) { ps1_gte_execute_command(&ctx->gte, insn); break; }
            switch (rs) {
            case 0x00: LD(rt, ps1_gte_get_data(&ctx->gte, rd)); break;       // MFC2
            case 0x02: LD(rt, ps1_gte_get_ctrl(&ctx->gte, rd)); break;       // CFC2
            case 0x04: ps1_gte_set_data(&ctx->gte, rd, r[rt]); break;        // MTC2
            case 0x06: ps1_gte_set_ctrl(&ctx->gte, rd, r[rt]); break;        // CTC2
            default: EXC(EXC_RI, 0);
            }
            break;
        }
        case 0x11: case 0x13: EXC(EXC_CPU, 0); // COP1 / COP3 do not exist

        case 0x20: { uint32_t a = r[rs] + (uint32_t)simm; LD(rt, (uint32_t)(int32_t)(int8_t)MEM_R8(a)); break; }   // LB
        case 0x24: { uint32_t a = r[rs] + (uint32_t)simm; LD(rt, MEM_R8(a)); break; }                              // LBU
        case 0x21: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 1) EXC(EXC_ADEL, a); LD(rt, (uint32_t)(int32_t)(int16_t)MEM_R16(a)); break; } // LH
        case 0x25: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 1) EXC(EXC_ADEL, a); LD(rt, MEM_R16(a)); break; }                              // LHU
        case 0x23: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 3) EXC(EXC_ADEL, a); LD(rt, MEM_R32(a)); break; }                              // LW
        case 0x22: { // LWL (sees a pending load of the same register without waiting)
            uint32_t a = r[rs] + (uint32_t)simm;
            uint32_t cur = (rt == lreg) ? lval : r[rt];
            LD(rt, ps1_mem_lwl(ctx, a, cur));
            break;
        }
        case 0x26: { // LWR
            uint32_t a = r[rs] + (uint32_t)simm;
            uint32_t cur = (rt == lreg) ? lval : r[rt];
            LD(rt, ps1_mem_lwr(ctx, a, cur));
            break;
        }
        case 0x28: { uint32_t a = r[rs] + (uint32_t)simm; if (!(c->cop0[COP0_SR] & SR_IsC)) ps1_mem_write8(ctx, a, (uint8_t)r[rt]); break; }                     // SB
        case 0x29: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 1) EXC(EXC_ADES, a); if (!(c->cop0[COP0_SR] & SR_IsC)) ps1_mem_write16(ctx, a, (uint16_t)r[rt]); break; } // SH
        case 0x2B: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 3) EXC(EXC_ADES, a); if (!(c->cop0[COP0_SR] & SR_IsC)) ps1_mem_write32(ctx, a, r[rt]); break; }   // SW
        case 0x2A: { uint32_t a = r[rs] + (uint32_t)simm; if (!(c->cop0[COP0_SR] & SR_IsC)) ps1_mem_swl(ctx, a, r[rt]); break; } // SWL
        case 0x2E: { uint32_t a = r[rs] + (uint32_t)simm; if (!(c->cop0[COP0_SR] & SR_IsC)) ps1_mem_swr(ctx, a, r[rt]); break; } // SWR
        case 0x32: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 3) EXC(EXC_ADEL, a); ps1_gte_set_data(&ctx->gte, rt, MEM_R32(a)); break; } // LWC2
        case 0x3A: { uint32_t a = r[rs] + (uint32_t)simm; if (a & 3) EXC(EXC_ADES, a); if (!(c->cop0[COP0_SR] & SR_IsC)) ps1_mem_write32(ctx, a, ps1_gte_get_data(&ctx->gte, rt)); break; } // SWC2
        default: EXC(EXC_RI, 0);
        }

        // Retire: apply the delayed load, advance the pipeline
        if (lreg) r[lreg] = lval;
        r[0] = 0;
        c->pc = npc;
        c->next_pc = next_npc;
        c->is_in_delay_slot = is_branch;

        // Remember jumps/calls so that landing on a recompiled entry (after the
        // delay slot) runs the native version. Only meaningful for the branch
        // instruction itself; the delay slot's own retire carries the flag over.
        if (is_branch) c->hook_next = hookable && ctx->native_enabled;
        else if (in_ds) { c->hook_pending = c->hook_next && npc != pc + 4; c->hook_next = false; }
        continue;

next_insn_exception:
        // raise_exception() already redirected the PC; the faulting instruction's load/ALU effects are dropped
        (void)lval;
        c->hook_next = false;
        c->hook_pending = false;
        continue;

#undef WR
#undef LD
#undef BRANCH
#undef EXC
    }
}

static void bios_trace_init(void) { s_bios_trace = getenv("MIMIPS1_BIOSTRACE") != NULL; }
void ps1_interp_run(PS1Context* ctx) {
    s_pc_trace = getenv("MIMIPS1_PCTRACE") != NULL;
    bios_trace_init();
    ctx->interp_depth++;
    interp_loop(ctx, 0xFFFFFFFFu, false, 0);
    ctx->interp_depth--;
}

// Calls guest code at `target` from native code and returns when it returns to
// the caller ($ra at entry) with the stack pointer restored.
void ps1_interp_call(PS1Context* ctx, uint32_t target) {
    PS1CPU* c = &ctx->cpu;
    uint32_t entry_ra = c->r[31];
    uint32_t entry_sp = c->r[29];

    uint32_t saved_pc = c->pc, saved_npc = c->next_pc;
    bool saved_ds = c->is_in_delay_slot;
    uint32_t saved_lreg = c->ld_reg, saved_lval = c->ld_val;
    bool saved_native = ctx->in_native;

    c->pc = target;
    c->next_pc = target + 4;
    c->is_in_delay_slot = false;
    c->ld_reg = 0;
    ctx->in_native = false;

    ctx->interp_depth++;
    interp_loop(ctx, entry_ra, true, entry_sp);
    ctx->interp_depth--;

    ctx->in_native = saved_native;
    c->pc = saved_pc;
    c->next_pc = saved_npc;
    c->is_in_delay_slot = saved_ds;
    c->ld_reg = saved_lreg;
    c->ld_val = saved_lval;
}

// Delivers an exception to the guest kernel while native code is running. The
// kernel's handler runs in the interpreter and returns to `marker`.
static void interp_kernel_entry(PS1Context* ctx, uint32_t code, uint32_t marker, uint32_t stop_pc) {
    PS1CPU* c = &ctx->cpu;
    uint32_t saved_pc = c->pc, saved_npc = c->next_pc;
    bool saved_ds = c->is_in_delay_slot;
    uint32_t saved_lreg = c->ld_reg, saved_lval = c->ld_val;
    bool saved_native = ctx->in_native;
    int saved_depth = ctx->interp_depth;
    bool saved_hook_next = c->hook_next, saved_hook_pending = c->hook_pending;
    int saved_exc = c->exc_level;

    KernelEntry entry;
    entry.stop_pc = stop_pc;
    entry.prev = s_kentry;
    s_kentry = &entry;

    if (setjmp(entry.buf) == 0) {
        raise_exception(ctx, code, marker, false, 0);
        ctx->in_native = false;
        ctx->interp_depth++;
        interp_loop(ctx, stop_pc, false, 0);
    }
    s_kentry = entry.prev;

    ctx->interp_depth = saved_depth;
    ctx->in_native = saved_native;
    c->pc = saved_pc;
    c->next_pc = saved_npc;
    c->is_in_delay_slot = saved_ds;
    c->ld_reg = saved_lreg;
    c->ld_val = saved_lval;
    c->hook_next = saved_hook_next;
    c->hook_pending = saved_hook_pending;
    c->exc_level = saved_exc;
}

void ps1_interp_interrupt_from_native(PS1Context* ctx) {
    interp_kernel_entry(ctx, EXC_INT, MARKER_INTERRUPT, MARKER_INTERRUPT);
}

void ps1_interp_syscall_from_native(PS1Context* ctx) {
    // The kernel returns to EPC + 4
    interp_kernel_entry(ctx, EXC_SYS, MARKER_SYSCALL, MARKER_SYSCALL + 4);
}
