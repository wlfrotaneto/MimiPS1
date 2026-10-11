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

void ps1_call_indirect(PS1Context* ctx, uint32_t target_pc) {
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

    // In case target function is not found or is dynamic
    // fprintf(stderr, "[CPU] Indirect call to unmapped PC: 0x%08X (ra=0x%08X)\n", target_pc, ctx->cpu.r[31]);
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
    uint32_t sr = ctx->cpu.cop0[COP0_SR];
    ctx->cpu.cop0[COP0_SR] = (sr & ~0x3F) | ((sr << 2) & 0x3C);
    ctx->cpu.pc = 0x80000080;
}

void ps1_rfe(PS1Context* ctx) {
    // Restore SR from exception
    uint32_t sr = ctx->cpu.cop0[COP0_SR];
    ctx->cpu.cop0[COP0_SR] = (sr & ~0x0F) | ((sr >> 2) & 0x0F);
}
