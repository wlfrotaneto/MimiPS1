#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int g_mimips1_log_level = -1;

int ps1_get_log_level(void) {
    const char* env = getenv("MIMIPS1_LOG");
    if (env) {
        return atoi(env);
    }
    return 0;
}

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
    ctx->next_vblank_cycle = (uint64_t)(PS1_CPU_CLOCK_HZ / 59.94);
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
    ctx->cycle_counter = 0;
    ctx->cycles_since_vblank = 0;
    ctx->start_time_ns = 0;
    ctx->in_tick = false;
    ctx->next_vblank_cycle = (uint64_t)(PS1_CPU_CLOCK_HZ / 59.94);
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

    ctx->exe_text_addr = text_addr;
    ctx->exe_text_size = text_size;

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

                uint32_t p_sp = ctx->cpu.r[29] & 0x1FFFFFFF;
                if (p_sp < 0x2000 || p_sp >= PS1_RAM_SIZE) {
                    ctx->cpu.r[29] = 0x801FFF00; // Safe dedicated interrupt stack
                }

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

                    uint32_t p_sp = ctx->cpu.r[29] & 0x1FFFFFFF;
                    if (p_sp < 0x2000 || p_sp >= PS1_RAM_SIZE) {
                        ctx->cpu.r[29] = 0x801FFF00; // Safe dedicated interrupt stack
                    }

                    ps1_call_indirect(ctx, cb_func);

                    memcpy(ctx->cpu.r, saved_r, sizeof(saved_r));
                    ctx->cpu.pc = saved_pc;
                    ctx->cpu.hi = saved_hi;
                    ctx->cpu.lo = saved_lo;
                }
            }
        }
    }

    // Deliver to HookEntryInt (B0:19) custom Psy-Q exception handler if registered
    if ((ctx->irq_stat & ctx->irq_mask) && ctx->bios.hook_entry_int_addr != 0) {
        uint32_t hook_addr = ctx->bios.hook_entry_int_addr;
        uint32_t handler_pc = ps1_mem_read32(ctx, hook_addr + 0x00);
        if (handler_pc != 0 && (handler_pc & 3) == 0) {
            uint32_t saved_r[32];
            uint32_t saved_pc = ctx->cpu.pc;
            uint32_t saved_hi = ctx->cpu.hi;
            uint32_t saved_lo = ctx->cpu.lo;
            memcpy(saved_r, ctx->cpu.r, sizeof(saved_r));

            // Setup registers from jmp_buf layout
            ctx->cpu.r[29] = ps1_mem_read32(ctx, hook_addr + 0x04); // SP
            ctx->cpu.r[30] = ps1_mem_read32(ctx, hook_addr + 0x08); // FP
            for (int i = 0; i < 8; i++) {
                ctx->cpu.r[16 + i] = ps1_mem_read32(ctx, hook_addr + 0x0C + (i * 4)); // S0..S7
            }
            ctx->cpu.r[28] = ps1_mem_read32(ctx, hook_addr + 0x2C); // GP
            ctx->cpu.r[31] = 0x80001000 + (0x17 * 12); // ReturnFromException stub

            uint32_t p_sp = ctx->cpu.r[29] & 0x1FFFFFFF;
            if (p_sp < 0x2000 || p_sp >= PS1_RAM_SIZE) {
                ctx->cpu.r[29] = 0x801FFF00;
            }

            ps1_call_indirect(ctx, handler_pc);

            memcpy(ctx->cpu.r, saved_r, sizeof(saved_r));
            ctx->cpu.pc = saved_pc;
            ctx->cpu.hi = saved_hi;
            ctx->cpu.lo = saved_lo;
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
    (void)cycles;
    if (!ctx || ctx->in_tick) return;
    ctx->in_tick = true;

    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (ctx->start_time_ns == 0) {
        ctx->start_time_ns = now_ns;
        ctx->next_vblank_cycle = (uint64_t)(PS1_CPU_CLOCK_HZ / 59.94);
    }

    uint64_t target_cycles = (now_ns - ctx->start_time_ns) * PS1_CPU_CLOCK_HZ / 1000000000ULL;
    if (target_cycles > ctx->cycle_counter) {
        uint64_t delta = target_cycles - ctx->cycle_counter;
        const uint64_t MAX_DELTA = PS1_CYCLES_PER_FRAME / 2;
        if (delta > MAX_DELTA) {
            ctx->start_time_ns = now_ns - (ctx->cycle_counter + MAX_DELTA) * 1000000000ULL / PS1_CPU_CLOCK_HZ;
            delta = MAX_DELTA;
        }

        ctx->cycle_counter += delta;

        // Tick hardware timers and CD-ROM
        ps1_timers_tick(ctx, (uint32_t)delta);
        ps1_cdrom_tick(ctx, (uint32_t)delta);

#if defined(__APPLE__)
        macos_audio_tick(ctx, (uint32_t)delta);
#endif

        // Trigger V-Blank when frame period completes (~565,045 cycles at 59.94 Hz NTSC)
        while (ctx->cycle_counter >= ctx->next_vblank_cycle) {
            ctx->next_vblank_cycle += (uint64_t)(PS1_CPU_CLOCK_HZ / 59.94);
            ctx->frame_count++;
            ctx->gpu.interlace_field = !ctx->gpu.interlace_field;

            ps1_trigger_irq(ctx, IRQ_VBLANK);
            ps1_gpu_vblank(&ctx->gpu);
        }
    }

    ctx->in_tick = false;
}

void ps1_runtime_run_frame(PS1Context* ctx) {
    if (!ctx || !ctx->running || ctx->paused) return;

    if (ctx->cpu.pc != 0) {
        ps1_call_indirect(ctx, ctx->cpu.pc);
    }
}
