#include "mimips1_cpu.h"
#include "mimips1_runtime.h"
#include "mimips1_bios.h"
#include <stdio.h>
#include <stdlib.h>

void ps1_cpu_init(PS1Context* ctx) {
    memset(&ctx->cpu, 0, sizeof(PS1CPU));
    ctx->cpu.cop0[COP0_PRID] = 0x00000002; // R3000A
    ctx->cpu.cop0[COP0_SR]   = 0x10900000; // Normal status (Coprocessors enabled)
}

void ps1_cpu_reset(PS1Context* ctx) {
    uint32_t preserved_table[PS1_FUNC_TABLE_SIZE];
    memcpy(preserved_table, ctx->cpu.func_table, sizeof(preserved_table));

    memset(&ctx->cpu, 0, sizeof(PS1CPU));
    memcpy(ctx->cpu.func_table, preserved_table, sizeof(preserved_table));

    ctx->cpu.cop0[COP0_PRID] = 0x00000002;
    ctx->cpu.cop0[COP0_SR]   = 0x10900000;
}

void ps1_register_function(PS1Context* ctx, uint32_t mips_addr, PS1RecompiledFunc func) {
    uint32_t offset = (mips_addr & 0x001FFFFC) >> 2;
    if (offset < PS1_FUNC_TABLE_SIZE) {
        ctx->cpu.func_table[offset] = func;
    }
}

void ps1_register_function_range(PS1Context* ctx, uint32_t start, uint32_t end, PS1RecompiledFunc func) {
    uint32_t first = (start & 0x001FFFFC) >> 2;
    uint32_t last  = (end   & 0x001FFFFC) >> 2;
    for (uint32_t off = first; off <= last && off < PS1_FUNC_TABLE_SIZE; off++) {
        if (!ctx->cpu.func_table[off]) {
            ctx->cpu.func_table[off] = func;
        }
    }
}

#include "mips_disasm.h"
#include "mimips1_gte.h"

static void ps1_interpret_execute_insn(PS1Context* ctx, const MipsInsn* insn) {
    uint32_t* r = ctx->cpu.r;
    switch (insn->id) {
        // ALU
        case MIPS_OP_ADD:
        case MIPS_OP_ADDU:  r[insn->rd] = r[insn->rs] + r[insn->rt]; break;
        case MIPS_OP_SUB:
        case MIPS_OP_SUBU:  r[insn->rd] = r[insn->rs] - r[insn->rt]; break;
        case MIPS_OP_AND:   r[insn->rd] = r[insn->rs] & r[insn->rt]; break;
        case MIPS_OP_OR:    r[insn->rd] = r[insn->rs] | r[insn->rt]; break;
        case MIPS_OP_XOR:   r[insn->rd] = r[insn->rs] ^ r[insn->rt]; break;
        case MIPS_OP_NOR:   r[insn->rd] = ~(r[insn->rs] | r[insn->rt]); break;
        case MIPS_OP_SLT:   r[insn->rd] = ((int32_t)r[insn->rs] < (int32_t)r[insn->rt]) ? 1 : 0; break;
        case MIPS_OP_SLTU:  r[insn->rd] = (r[insn->rs] < r[insn->rt]) ? 1 : 0; break;
        case MIPS_OP_ADDI:
        case MIPS_OP_ADDIU: r[insn->rt] = r[insn->rs] + insn->simm; break;
        case MIPS_OP_SLTI:  r[insn->rt] = ((int32_t)r[insn->rs] < insn->simm) ? 1 : 0; break;
        case MIPS_OP_SLTIU: r[insn->rt] = (r[insn->rs] < (uint32_t)insn->simm) ? 1 : 0; break;
        case MIPS_OP_ANDI:  r[insn->rt] = r[insn->rs] & insn->imm; break;
        case MIPS_OP_ORI:   r[insn->rt] = r[insn->rs] | insn->imm; break;
        case MIPS_OP_XORI:  r[insn->rt] = r[insn->rs] ^ insn->imm; break;
        case MIPS_OP_LUI:   r[insn->rt] = (uint32_t)insn->imm << 16; break;

        // Shifts
        case MIPS_OP_SLL:   r[insn->rd] = r[insn->rt] << insn->sa; break;
        case MIPS_OP_SRL:   r[insn->rd] = r[insn->rt] >> insn->sa; break;
        case MIPS_OP_SRA:   r[insn->rd] = (uint32_t)((int32_t)r[insn->rt] >> insn->sa); break;
        case MIPS_OP_SLLV:  r[insn->rd] = r[insn->rt] << (r[insn->rs] & 0x1F); break;
        case MIPS_OP_SRLV:  r[insn->rd] = r[insn->rt] >> (r[insn->rs] & 0x1F); break;
        case MIPS_OP_SRAV:  r[insn->rd] = (uint32_t)((int32_t)r[insn->rt] >> (r[insn->rs] & 0x1F)); break;

        // Multiply / Divide
        case MIPS_OP_MULT: {
            int64_t res = (int64_t)(int32_t)r[insn->rs] * (int64_t)(int32_t)r[insn->rt];
            ctx->cpu.lo = (uint32_t)res;
            ctx->cpu.hi = (uint32_t)(res >> 32);
            break;
        }
        case MIPS_OP_MULTU: {
            uint64_t res = (uint64_t)r[insn->rs] * (uint64_t)r[insn->rt];
            ctx->cpu.lo = (uint32_t)res;
            ctx->cpu.hi = (uint32_t)(res >> 32);
            break;
        }
        case MIPS_OP_DIV: {
            int32_t n = (int32_t)r[insn->rs];
            int32_t d = (int32_t)r[insn->rt];
            if (d != 0) {
                if (n == (int32_t)0x80000000 && d == -1) {
                    ctx->cpu.lo = 0x80000000;
                    ctx->cpu.hi = 0;
                } else {
                    ctx->cpu.lo = (uint32_t)(n / d);
                    ctx->cpu.hi = (uint32_t)(n % d);
                }
            }
            break;
        }
        case MIPS_OP_DIVU: {
            uint32_t d = r[insn->rt];
            if (d != 0) {
                ctx->cpu.lo = r[insn->rs] / d;
                ctx->cpu.hi = r[insn->rs] % d;
            }
            break;
        }
        case MIPS_OP_MFHI:  r[insn->rd] = ctx->cpu.hi; break;
        case MIPS_OP_MFLO:  r[insn->rd] = ctx->cpu.lo; break;
        case MIPS_OP_MTHI:  ctx->cpu.hi = r[insn->rs]; break;
        case MIPS_OP_MTLO:  ctx->cpu.lo = r[insn->rs]; break;

        // Loads
        case MIPS_OP_LB:    r[insn->rt] = (int32_t)(int8_t)ps1_mem_read8(ctx, r[insn->rs] + insn->simm); break;
        case MIPS_OP_LBU:   r[insn->rt] = ps1_mem_read8(ctx, r[insn->rs] + insn->simm); break;
        case MIPS_OP_LH:    r[insn->rt] = (int32_t)(int16_t)ps1_mem_read16(ctx, r[insn->rs] + insn->simm); break;
        case MIPS_OP_LHU:   r[insn->rt] = ps1_mem_read16(ctx, r[insn->rs] + insn->simm); break;
        case MIPS_OP_LW:    r[insn->rt] = ps1_mem_read32(ctx, r[insn->rs] + insn->simm); break;
        case MIPS_OP_LWL:   r[insn->rt] = ps1_mem_lwl(ctx, r[insn->rs] + insn->simm, r[insn->rt]); break;
        case MIPS_OP_LWR:   r[insn->rt] = ps1_mem_lwr(ctx, r[insn->rs] + insn->simm, r[insn->rt]); break;

        // Stores
        case MIPS_OP_SB:    ps1_mem_write8(ctx, r[insn->rs] + insn->simm, (uint8_t)r[insn->rt]); break;
        case MIPS_OP_SH:    ps1_mem_write16(ctx, r[insn->rs] + insn->simm, (uint16_t)r[insn->rt]); break;
        case MIPS_OP_SW:    ps1_mem_write32(ctx, r[insn->rs] + insn->simm, r[insn->rt]); break;
        case MIPS_OP_SWL:   ps1_mem_swl(ctx, r[insn->rs] + insn->simm, r[insn->rt]); break;
        case MIPS_OP_SWR:   ps1_mem_swr(ctx, r[insn->rs] + insn->simm, r[insn->rt]); break;

        // Coprocessor 0
        case MIPS_OP_MFC0:  r[insn->rt] = ctx->cpu.cop0[insn->rd]; break;
        case MIPS_OP_MTC0:  ctx->cpu.cop0[insn->rd] = r[insn->rt]; break;
        case MIPS_OP_RFE:   ps1_rfe(ctx); break;

        // Coprocessor 2 (GTE)
        case MIPS_OP_MFC2:  r[insn->rt] = ps1_gte_get_data(&ctx->gte, insn->rd); break;
        case MIPS_OP_MTC2:  ps1_gte_set_data(&ctx->gte, insn->rd, r[insn->rt]); break;
        case MIPS_OP_CFC2:  r[insn->rt] = ps1_gte_get_ctrl(&ctx->gte, insn->rd); break;
        case MIPS_OP_CTC2:  ps1_gte_set_ctrl(&ctx->gte, insn->rd, r[insn->rt]); break;
        case MIPS_OP_LWC2:  ps1_gte_set_data(&ctx->gte, insn->rt, ps1_mem_read32(ctx, r[insn->rs] + insn->simm)); break;
        case MIPS_OP_SWC2:  ps1_mem_write32(ctx, r[insn->rs] + insn->simm, ps1_gte_get_data(&ctx->gte, insn->rt)); break;
        case MIPS_OP_COP2_CMD: ps1_gte_execute_command(&ctx->gte, insn->raw); break;

        // System
        case MIPS_OP_SYSCALL: ps1_trigger_syscall(ctx); break;
        case MIPS_OP_BREAK:   ps1_trigger_break(ctx); break;

        default: break;
    }
    r[0] = 0;
}

static void ps1_interpret_fallback(PS1Context* ctx, uint32_t start_pc) {
    uint32_t pc = start_pc;
    uint32_t entry_ra = ctx->cpu.r[31];
    int call_depth = 0;
    int insn_count = 0;

    PS1_LOG(1, "[INTERP] Entered MIPS interpreter fallback at 0x%08X (RA=0x%08X)\n", start_pc, entry_ra);

    while (pc != 0 && (pc & 0x1FFFFFFF) < PS1_RAM_SIZE) {
        if (++insn_count % 64 == 0) {
            ps1_cpu_tick(ctx, 32);
            if (insn_count > 10000000) {
                fprintf(stderr, "[INTERP] Safety limit exceeded at 0x%08X (RA=0x%08X)\n", pc, ctx->cpu.r[31]);
                break;
            }
        }

        uint32_t raw = ps1_mem_read32(ctx, pc);
        MipsInsn insn;
        mips_decode_instruction(raw, pc, &insn);

        if (insn.has_delay_slot) {
            uint32_t ds_raw = ps1_mem_read32(ctx, pc + 4);
            MipsInsn ds_insn;
            mips_decode_instruction(ds_raw, pc + 4, &ds_insn);

            bool take_branch = false;
            uint32_t target_addr = 0;

            if (insn.is_branch) {
                switch (insn.id) {
                    case MIPS_OP_BEQ:  take_branch = (ctx->cpu.r[insn.rs] == ctx->cpu.r[insn.rt]); break;
                    case MIPS_OP_BNE:  take_branch = (ctx->cpu.r[insn.rs] != ctx->cpu.r[insn.rt]); break;
                    case MIPS_OP_BLEZ: take_branch = ((int32_t)ctx->cpu.r[insn.rs] <= 0); break;
                    case MIPS_OP_BGTZ: take_branch = ((int32_t)ctx->cpu.r[insn.rs] > 0); break;
                    case MIPS_OP_BLTZ: take_branch = ((int32_t)ctx->cpu.r[insn.rs] < 0); break;
                    case MIPS_OP_BGEZ: take_branch = ((int32_t)ctx->cpu.r[insn.rs] >= 0); break;
                    case MIPS_OP_BLTZAL:
                        ctx->cpu.r[31] = pc + 8;
                        take_branch = ((int32_t)ctx->cpu.r[insn.rs] < 0);
                        break;
                    case MIPS_OP_BGEZAL:
                        ctx->cpu.r[31] = pc + 8;
                        take_branch = ((int32_t)ctx->cpu.r[insn.rs] >= 0);
                        break;
                    default: break;
                }
                target_addr = pc + 4 + (insn.simm << 2);
            } else if (insn.is_call) {
                uint32_t ret_reg = (insn.id == MIPS_OP_JALR) ? (insn.rd != 0 ? insn.rd : 31) : 31;
                uint32_t call_dest = (insn.id == MIPS_OP_JAL) ? insn.target : ctx->cpu.r[insn.rs];
                ctx->cpu.r[ret_reg] = pc + 8;
                take_branch = true;
                target_addr = call_dest;
            } else if (insn.is_return) {
                target_addr = ctx->cpu.r[insn.rs];
                take_branch = true;
            } else if (insn.id == MIPS_OP_J) {
                take_branch = true;
                target_addr = insn.target;
            } else if (insn.id == MIPS_OP_JR) {
                take_branch = true;
                target_addr = ctx->cpu.r[insn.rs];
            }

            // Execute delay slot
            ps1_interpret_execute_insn(ctx, &ds_insn);

            if (insn.is_return) {
                if (call_depth == 0 || target_addr == entry_ra) {
                    ctx->cpu.pc = target_addr;
                    return;
                }
                call_depth--;
            }

            if (take_branch) {
                uint32_t off = (target_addr & 0x001FFFFC) >> 2;
                if (off < PS1_FUNC_TABLE_SIZE && ctx->cpu.func_table[off] != NULL) {
                    ctx->cpu.pc = target_addr;
                    ps1_call_indirect(ctx, target_addr);
                    if (insn.is_call) {
                        pc += 8;
                        continue;
                    } else {
                        return;
                    }
                }
                if (insn.is_call) {
                    call_depth++;
                }
                if (target_addr <= pc) {
                    ps1_cpu_tick(ctx, 32);
                }
                pc = target_addr;
            } else {
                pc += 8;
            }
        } else {
            ps1_interpret_execute_insn(ctx, &insn);
            pc += 4;
        }
    }
}

void ps1_call_indirect(PS1Context* ctx, uint32_t target_pc) {
    if (target_pc == 0 || target_pc == 0xFFFFFFFF || (target_pc < 0x1000 && target_pc != 0xA0 && target_pc != 0xB0 && target_pc != 0xC0)) return; // Guard against null or invalid function pointer calls

    ps1_cpu_tick(ctx, 32);

    // Check BIOS vector hooks
    uint32_t masked = target_pc & 0x1FFFFFFF;
    if (masked == 0xA0) {
        ps1_bios_handle_a0(ctx, ctx->cpu.r[9]); // $t1 usually holds func on A0
        return;
    } else if (masked == 0xB0) {
        ps1_bios_handle_b0(ctx, ctx->cpu.r[9]);
        return;
    } else if (masked == 0xC0) {
        ps1_bios_handle_c0(ctx, ctx->cpu.r[9]);
        return;
    }

    uint32_t offset = (target_pc & 0x001FFFFC) >> 2;
    if (__builtin_expect(offset < PS1_FUNC_TABLE_SIZE, 1)) {
        PS1RecompiledFunc fn = ctx->cpu.func_table[offset];
        if (fn) {
            ctx->cpu.pc = target_pc;
            fn(ctx);
            return;
        }
    }

    // Target function not found in statically recompiled code.
    // Fall back to interpreting MIPS R3000A instructions if valid RAM address.
    if ((target_pc & 0x1FFFFFFF) < PS1_RAM_SIZE) {
        ps1_interpret_fallback(ctx, target_pc);
        return;
    }

    // Diagnostic: report each unmapped target outside RAM only once
    #define UNMAPPED_HASH_SIZE 1024
    static uint32_t s_seen_unmapped[UNMAPPED_HASH_SIZE];
    static bool s_seen_unmapped_init = false;
    if (!s_seen_unmapped_init) {
        memset(s_seen_unmapped, 0xFF, sizeof(s_seen_unmapped));
        s_seen_unmapped_init = true;
    }
    uint32_t h = (target_pc ^ (target_pc >> 10)) % UNMAPPED_HASH_SIZE;
    bool already_seen = false;
    for (int i = 0; i < 32; i++) {
        uint32_t slot = (h + i) % UNMAPPED_HASH_SIZE;
        if (s_seen_unmapped[slot] == target_pc) {
            already_seen = true;
            break;
        }
        if (s_seen_unmapped[slot] == 0xFFFFFFFF) {
            s_seen_unmapped[slot] = target_pc;
            break;
        }
    }
    if (!already_seen) {
        uint32_t base = ctx->exe_text_addr ? ctx->exe_text_addr : 0x80010000;
        uint32_t end = base + (ctx->exe_text_size ? ctx->exe_text_size : 0x70000);
        bool inside = (target_pc >= base && target_pc < end);
        fprintf(stderr, "[CPU MISS] Unmapped indirect call target: 0x%08X (RA=0x%08X) [%s EXE range 0x%08X..0x%08X]\n",
                target_pc, ctx->cpu.r[31], inside ? "INSIDE" : "OUTSIDE", base, end);
    }
}

void ps1_trigger_syscall(PS1Context* ctx) {
    if (!ps1_bios_handle_syscall(ctx)) {
        // Fallback standard exception
        ctx->cpu.cop0[COP0_EPC] = ctx->cpu.pc;
        ctx->cpu.cop0[COP0_CAUSE] = (8 << 2); // Syscall exception code
        // Push SR
        uint32_t sr = ctx->cpu.cop0[COP0_SR];
        ctx->cpu.cop0[COP0_SR] = (sr & ~0x3F) | ((sr << 2) & 0x3C);
        ctx->cpu.pc = 0x80000080;
    }
}

void ps1_trigger_break(PS1Context* ctx) {
    ctx->cpu.cop0[COP0_EPC] = ctx->cpu.pc;
    ctx->cpu.cop0[COP0_CAUSE] = (9 << 2); // Breakpoint exception code
}

void ps1_rfe(PS1Context* ctx) {
    // Restore SR from exception
    uint32_t sr = ctx->cpu.cop0[COP0_SR];
    ctx->cpu.cop0[COP0_SR] = (sr & ~0x0F) | ((sr >> 2) & 0x0F);
}
