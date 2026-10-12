#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PS1Context* ps1_runtime_create(void) {
    PS1Context* ctx = (PS1Context*)calloc(1, sizeof(PS1Context));
    if (!ctx) return NULL;

    ps1_memory_init(ctx);
    ps1_cpu_init(ctx);
    ps1_gpu_init(&ctx->gpu);
    ps1_gte_init(&ctx->gte);
    ps1_spu_init(&ctx->spu);
    ps1_dma_init(&ctx->dma);
    ps1_timers_init(&ctx->timers);
    ps1_cdrom_init(&ctx->cdrom);
    ps1_bios_init(ctx);
    ps1_input_init(&ctx->input);
    ps1_card_init(&ctx->card_mgr);
    ps1_card_auto_setup(&ctx->card_mgr);

    ctx->running = true;
    ctx->aspect_ratio = 0; // 4:3
    return ctx;
}

void ps1_runtime_destroy(PS1Context* ctx) {
    if (!ctx) return;
    ps1_card_reset(&ctx->card_mgr);
    ps1_cdrom_close_image(&ctx->cdrom);
    free(ctx);
}

void ps1_runtime_reset(PS1Context* ctx) {
    if (!ctx) return;
    ps1_memory_reset(ctx);
    ps1_cpu_reset(ctx);
    ps1_gpu_reset(&ctx->gpu);
    ps1_gte_reset(&ctx->gte);
    ps1_spu_reset(&ctx->spu);
    ps1_dma_reset(&ctx->dma);
    ps1_timers_reset(&ctx->timers);
    ps1_cdrom_reset(&ctx->cdrom);
    ps1_bios_reset(ctx);
    ps1_input_reset(&ctx->input);
    ps1_card_reset(&ctx->card_mgr);
    ps1_card_auto_setup(&ctx->card_mgr);
    ctx->irq_stat = 0;
    ctx->irq_mask = 0;
    ctx->frame_count = 0;
}

bool ps1_runtime_load_exe(PS1Context* ctx, const char* exe_path) {
    FILE* f = fopen(exe_path, "rb");
    if (!f) return false;

    uint8_t header[2048];
    if (fread(header, 1, 2048, f) != 2048) {
        fclose(f);
        return false;
    }

    // Verify "PS-X EXE" magic
    if (memcmp(header, "PS-X EXE", 8) != 0) {
        fclose(f);
        return false;
    }

    uint32_t init_pc   = *(uint32_t*)&header[0x10];
    uint32_t init_gp   = *(uint32_t*)&header[0x14];
    uint32_t text_addr = *(uint32_t*)&header[0x18];
    uint32_t text_size = *(uint32_t*)&header[0x1C];
    uint32_t init_sp   = *(uint32_t*)&header[0x30];
    uint32_t sp_offset = *(uint32_t*)&header[0x34];

    // Load text section into RAM
    fseek(f, 0x800, SEEK_SET);
    uint32_t paddr = text_addr & 0x1FFFFFFF;
    if (paddr + text_size <= PS1_RAM_SIZE) {
        fread(&ctx->memory.ram[paddr], 1, text_size, f);
    }
    fclose(f);

    // Setup initial CPU state
    ctx->cpu.pc    = init_pc;
    ctx->cpu.r[28] = init_gp; // $gp
    uint32_t sp = init_sp + sp_offset;
    if (sp == 0) sp = 0x801FFFF0; // Default stack pointer
    ctx->cpu.r[29] = sp;      // $sp
    ctx->cpu.r[30] = sp;      // $fp

    printf("[MimiPS1] Loaded EXE: %s\n", exe_path);
    printf("         Entry PC: 0x%08X, Text: 0x%08X (size: %u bytes)\n", init_pc, text_addr, text_size);
    printf("         GP: 0x%08X, SP: 0x%08X\n", init_gp, sp);

    return true;
}

void ps1_trigger_irq(PS1Context* ctx, uint32_t irq_bit) {
    ctx->irq_stat |= irq_bit;
    if (ctx->irq_stat & ctx->irq_mask) {
        // Assert Interrupt Request in COP0 Cause Register
        ctx->cpu.cop0[COP0_CAUSE] |= (1 << 10);
    }

    // Deliver corresponding BIOS events to waiting game loops and callbacks
    if (irq_bit & IRQ_VBLANK) {
        ps1_bios_update_pads(ctx);
        ps1_bios_deliver_event(ctx, 0xF0000001, 0x0001); // V-Blank event
        ps1_bios_deliver_event(ctx, 0xF0000001, 0x0004);
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0004);
        ps1_bios_deliver_event(ctx, 0xF4000001, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF0000002, 0x0001); // RCnt 1 (H-Blank / V-Sync)
        ps1_bios_deliver_event(ctx, 0xF2000002, 0x0001);
    }
    if (irq_bit & IRQ_CDROM) {
        ps1_bios_deliver_event(ctx, 0xF0000003, 0x0020); // CD Complete event
        ps1_bios_deliver_event(ctx, 0xF0000003, 0x0010); // CD Data Ready event
        ps1_bios_deliver_event(ctx, 0xF0000003, 0x0000); // Any CD event
    }
    if (irq_bit & IRQ_DMA) {
        ps1_bios_deliver_event(ctx, 0xF0000002, 0x0001); // DMA Complete event
    }
}

void ps1_runtime_run_frame(PS1Context* ctx) {
    if (!ctx || !ctx->running || ctx->paused) return;

    // Execute static recompiled slice
    // Call entry point or active PC if registered
    if (ctx->cpu.pc != 0) {
        ps1_call_indirect(ctx, ctx->cpu.pc);
    }

    // Tick Hardware Timers and CD-ROM for the frame duration (~564,480 cycles)
    ps1_timers_tick(ctx, PS1_CYCLES_PER_FRAME);
    ps1_cdrom_tick(ctx, PS1_CYCLES_PER_FRAME);

    // Trigger V-Blank Interrupt
    ps1_trigger_irq(ctx, IRQ_VBLANK);

    ctx->frame_count++;
}
