#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

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

    // Deliver to SysEnqIntRP queues (registered via C0:02)
    for (int p = 0; p < 4; p++) {
        uint32_t cur = ctx->bios.intrp_heads[p];
        while (cur != 0) {
            uint32_t next = ps1_mem_read32(ctx, cur);
            uint32_t func = ps1_mem_read32(ctx, cur + 4);
            if (func != 0) {
                uint32_t saved_r[32];
                uint32_t saved_pc = ctx->cpu.pc;
                uint32_t saved_hi = ctx->cpu.hi;
                uint32_t saved_lo = ctx->cpu.lo;
                memcpy(saved_r, ctx->cpu.r, sizeof(saved_r));

                ps1_call_indirect(ctx, func);

                memcpy(ctx->cpu.r, saved_r, sizeof(saved_r));
                ctx->cpu.pc = saved_pc;
                ctx->cpu.hi = saved_hi;
                ctx->cpu.lo = saved_lo;
            }
            cur = next;
        }
    }

    // Deliver to registered Psy-Q interrupt callbacks if present
    if (ctx->bios.irq_callback_table != 0) {
        for (int i = 0; i <= 10; i++) {
            if (irq_bit & (1 << i)) {
                uint32_t cb_func = ps1_mem_read32(ctx, ctx->bios.irq_callback_table + (i * 4));
                if (cb_func != 0) {
                    uint32_t saved_r[32];
                    uint32_t saved_pc = ctx->cpu.pc;
                    uint32_t saved_hi = ctx->cpu.hi;
                    uint32_t saved_lo = ctx->cpu.lo;
                    memcpy(saved_r, ctx->cpu.r, sizeof(saved_r));

                    ps1_call_indirect(ctx, cb_func);

                    memcpy(ctx->cpu.r, saved_r, sizeof(saved_r));
                    ctx->cpu.pc = saved_pc;
                    ctx->cpu.hi = saved_hi;
                    ctx->cpu.lo = saved_lo;
                }
            }
        }
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
        ps1_bios_deliver_event(ctx, 0xF2000002, 0x0002);
    }
    if (irq_bit & IRQ_TIMER0) {
        ps1_bios_deliver_event(ctx, 0xF2000000, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000000, 0x0002);
    }
    if (irq_bit & IRQ_TIMER1) {
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0002);
    }
    if (irq_bit & IRQ_TIMER2) {
        ps1_bios_deliver_event(ctx, 0xF2000002, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000002, 0x0002);
    }
    if (irq_bit & IRQ_CDROM) {
        if (ctx->cdrom.irq_flag == 2 || ctx->cdrom.irq_flag == 3) {
            ps1_bios_deliver_event(ctx, 0xF0000003, 0x0020); // CD Complete / Ack event
        } else if (ctx->cdrom.irq_flag == 1) {
            ps1_bios_deliver_event(ctx, 0xF0000003, 0x0010); // CD Data Ready event
        } else if (ctx->cdrom.irq_flag == 5) {
            ps1_bios_deliver_event(ctx, 0xF0000003, 0x0040); // CD Disk Error event
        }
        ps1_bios_deliver_event(ctx, 0xF0000003, 0x0000); // Any CD event
    }
    if (irq_bit & IRQ_DMA) {
        ps1_bios_deliver_event(ctx, 0xF0000002, 0x0001); // DMA Complete event
    }
}

void ps1_cpu_tick(PS1Context* ctx, uint32_t cycles) {
    if (!ctx || ctx->in_tick) return;
    ctx->in_tick = true;

    ctx->cycle_counter += cycles;
    ctx->cycles_since_vblank += cycles;

    // Tick hardware timers and CD-ROM
    ps1_timers_tick(ctx, cycles);
    ps1_cdrom_tick(ctx, cycles);

    // Trigger V-Blank when frame period completes (~564,480 cycles = ~1/60th sec)
    if (ctx->cycles_since_vblank >= PS1_CYCLES_PER_FRAME) {
        ctx->cycles_since_vblank -= PS1_CYCLES_PER_FRAME;
        ctx->frame_count++;
        ctx->gpu.interlace_field = !ctx->gpu.interlace_field;

        ps1_trigger_irq(ctx, IRQ_VBLANK);

        // Throttle to authentic ~60 FPS
        static struct timeval last_frame_tv = {0, 0};
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        if (last_frame_tv.tv_sec != 0) {
            long elapsed_us = (now_tv.tv_sec - last_frame_tv.tv_sec) * 1000000 + (now_tv.tv_usec - last_frame_tv.tv_usec);
            if (elapsed_us > 0 && elapsed_us < 16666) {
                usleep((useconds_t)(16666 - elapsed_us));
                gettimeofday(&now_tv, NULL);
            }
        }
        last_frame_tv = now_tv;
    }

    ctx->in_tick = false;
}

void ps1_runtime_run_frame(PS1Context* ctx) {
    if (!ctx || !ctx->running || ctx->paused) return;

    if (ctx->cpu.pc != 0) {
        ps1_call_indirect(ctx, ctx->cpu.pc);
    }
}
