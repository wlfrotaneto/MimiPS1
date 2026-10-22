#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int g_mimips1_log_level = -1;

#define SCHED_QUANTUM_CYCLES 64
#define PS1_VBLANK_PERIOD    ((uint64_t)(PS1_CPU_CLOCK_HZ / 59.94))
#define PS1_FRAME_NS         16683333ULL


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
    ctx->next_vblank_cycle = PS1_VBLANK_PERIOD;
    ctx->unthrottled = getenv("MIMIPS1_UNTHROTTLED") != NULL;
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
    ctx->irq_depth = 0;
    ctx->irq_in_queue = false;
    ctx->last_sched_cycle = 0;
    ctx->next_vblank_cycle = PS1_VBLANK_PERIOD;
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

static inline void irq_update_cause(PS1Context* ctx) {
    if (ctx->irq_stat & ctx->irq_mask) {
        ctx->cpu.cop0[COP0_CAUSE] |= (1 << 10);
    } else {
        ctx->cpu.cop0[COP0_CAUSE] &= ~(1 << 10);
    }
}

void ps1_trigger_irq(PS1Context* ctx, uint32_t irq_bit) {
    ctx->irq_stat |= irq_bit;
    irq_update_cause(ctx);
}

// Runs fn(ctx) as an interrupt callback with the interrupted context preserved.
static void irq_call_guest(PS1Context* ctx, uint32_t func) {
    PS1_LOG(3, "[IRQ] call 0x%08X (I_STAT=0x%04X I_MASK=0x%04X SR=0x%08X cyc=%llu)\n",
            func, ctx->irq_stat, ctx->irq_mask, ctx->cpu.cop0[COP0_SR], (unsigned long long)ctx->cycle_counter);
    uint32_t saved_r[32];
    uint32_t saved_pc = ctx->cpu.pc;
    uint32_t saved_hi = ctx->cpu.hi;
    uint32_t saved_lo = ctx->cpu.lo;
    memcpy(saved_r, ctx->cpu.r, sizeof(saved_r));

    // Run on the interrupted stack, below the live frame; fall back to a
    // dedicated stack only if $sp is not inside RAM.
    uint32_t sp = ctx->cpu.r[29];
    uint32_t p_sp = sp & 0x1FFFFFFF;
    if (p_sp < 0x2000 || p_sp >= PS1_RAM_SIZE) {
        ctx->cpu.r[29] = 0x801FFF00;
    } else {
        ctx->cpu.r[29] = (sp - 256) & ~7u;
    }

    ps1_call_indirect(ctx, func);

    memcpy(ctx->cpu.r, saved_r, sizeof(saved_r));
    ctx->cpu.pc = saved_pc;
    ctx->cpu.hi = saved_hi;
    ctx->cpu.lo = saved_lo;
}

static void irq_deliver_bit(PS1Context* ctx, uint32_t bit) {
    // Psy-Q interrupt callback table (A0:72), indexed by I_STAT bit
    if (ctx->bios.irq_callback_table != 0) {
        for (int i = 0; i <= 10; i++) {
            if (bit & (1u << i)) {
                uint32_t cb = ps1_mem_read32(ctx, ctx->bios.irq_callback_table + (i * 4));
                if (cb != 0) irq_call_guest(ctx, cb);
            }
        }
    }

    // BIOS events waiting on the interrupt source
    if (bit & IRQ_VBLANK) {
        ps1_bios_deliver_event(ctx, 0xF0000001, 0x0001); // V-Blank event
        ps1_bios_deliver_event(ctx, 0xF0000001, 0x0004);
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0004);
        ps1_bios_deliver_event(ctx, 0xF4000001, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF0000002, 0x0001); // RCnt 1 (H-Blank / V-Sync)
    }
    if (bit & IRQ_TIMER0) {
        ps1_bios_deliver_event(ctx, 0xF2000000, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000000, 0x0002);
    }
    if (bit & IRQ_TIMER1) {
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000001, 0x0002);
    }
    if (bit & IRQ_TIMER2) {
        ps1_bios_deliver_event(ctx, 0xF2000002, 0x0001);
        ps1_bios_deliver_event(ctx, 0xF2000002, 0x0002);
    }
    if (bit & IRQ_CDROM) {
        if (ctx->cdrom.irq_flag == 2 || ctx->cdrom.irq_flag == 3) {
            ps1_bios_deliver_event(ctx, 0xF0000003, 0x0020); // CD Complete / Ack event
        } else if (ctx->cdrom.irq_flag == 1) {
            ps1_bios_deliver_event(ctx, 0xF0000003, 0x0010); // CD Data Ready event
        } else if (ctx->cdrom.irq_flag == 5) {
            ps1_bios_deliver_event(ctx, 0xF0000003, 0x0040); // CD Disk Error event
        }
        ps1_bios_deliver_event(ctx, 0xF0000003, 0x0000); // Any CD event
    }
    if (bit & IRQ_DMA) {
        ps1_bios_deliver_event(ctx, 0xF0000002, 0x0001); // DMA Complete event
    }
}

// Psy-Q interrupt callbacks (e.g. the CD ready callback) issue commands and wait
// for the controller's acknowledge interrupt from inside the callback, so the
// dispatcher leaves interrupts enabled and nested delivery must work. Depth is
// bounded to keep a misbehaving guest from recursing without limit.
#define IRQ_MAX_DEPTH 3

void ps1_irq_poll(PS1Context* ctx) {
    if (ctx->irq_depth >= IRQ_MAX_DEPTH || ctx->irq_in_queue) return;
    irq_update_cause(ctx);

    uint32_t sr = ctx->cpu.cop0[COP0_SR];
    if (!(sr & SR_IEc) || !(sr & (1u << 10))) return;
    if ((ctx->irq_stat & ctx->irq_mask) == 0) return;

    ctx->irq_depth++;

    for (int round = 0; round < 4; round++) {
        uint32_t pending = ctx->irq_stat & ctx->irq_mask;
        if (!pending) break;

        // SysEnqIntRP dispatch queues (C0:02). These inspect and acknowledge
        // I_STAT themselves, so they run before the bits are cleared.
        ctx->irq_in_queue = true;
        for (int p = 0; p < 4; p++) {
            uint32_t cur = ctx->bios.intrp_heads[p];
            while (cur != 0) {
                uint32_t next = ps1_mem_read32(ctx, cur);
                uint32_t func = ps1_mem_read32(ctx, cur + 4);
                if (func != 0) irq_call_guest(ctx, func);
                cur = next;
            }
        }
        ctx->irq_in_queue = false;

        // Kernel-style acknowledge, then per-source callbacks
        ctx->irq_stat &= ~pending;
        for (int i = 0; i <= 10; i++) {
            if (pending & (1u << i)) irq_deliver_bit(ctx, 1u << i);
        }
    }

    ctx->cpu.cop0[COP0_SR] = sr;
    irq_update_cause(ctx);
    ctx->irq_depth--;
}

// Hold the emulated 59.94 Hz clock to wall-clock time (frame pacing only; it
// never influences emulated time).
static void pace_frame(PS1Context* ctx) {
    if (ctx->unthrottled) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (ctx->start_time_ns == 0) ctx->start_time_ns = now - ctx->frame_count * PS1_FRAME_NS;

    uint64_t target = ctx->start_time_ns + ctx->frame_count * PS1_FRAME_NS;
    if (now < target) {
        struct timespec ts = { 0, (long)(target - now) };
        nanosleep(&ts, NULL);
    } else if (now - target > 100000000ULL) {
        // Host fell far behind; resynchronize instead of fast-forwarding
        ctx->start_time_ns = now - ctx->frame_count * PS1_FRAME_NS;
    }
}

static void scheduler_run(PS1Context* ctx) {
    uint32_t delta = (uint32_t)(ctx->cycle_counter - ctx->last_sched_cycle);
    ctx->last_sched_cycle = ctx->cycle_counter;

    ps1_timers_tick(ctx, delta);
    ps1_cdrom_tick(ctx, delta);
    ps1_input_tick(ctx, delta);
#if defined(__APPLE__)
    macos_audio_tick(ctx, delta);
#endif

    while (ctx->cycle_counter >= ctx->next_vblank_cycle) {
        ctx->next_vblank_cycle += PS1_VBLANK_PERIOD;
        ctx->frame_count++;
        if ((ctx->frame_count % 60) == 0) {
            printf("[Heartbeat Frame %llu] PC=0x%08X RA=0x%08X IRQ_STAT=0x%04X IRQ_MASK=0x%04X SR=0x%08X CD_STAT=0x%02X CD_LBA=%u DISP=(%d,%d %dx%d)\n",
                   (unsigned long long)ctx->frame_count, ctx->cpu.pc, ctx->cpu.r[31],
                   ctx->irq_stat, ctx->irq_mask, ctx->cpu.cop0[COP0_SR], ctx->cdrom.status, ctx->cdrom.current_lba,
                   ctx->gpu.disp_x, ctx->gpu.disp_y, ctx->gpu.disp_w, ctx->gpu.disp_h);
            fflush(stdout);
        }

        ps1_bios_update_pads(ctx);
        ps1_trigger_irq(ctx, IRQ_VBLANK);
        ps1_gpu_vblank(&ctx->gpu);
        pace_frame(ctx);
    }
}

// Advances emulated time by `cycles`. Hardware is stepped here but only ever
// latches IRQ lines; handlers run afterwards through ps1_irq_poll(), outside
// the scheduler, so a handler that polls a timer or status register still
// sees time advance.
void ps1_cpu_tick(PS1Context* ctx, uint32_t cycles) {
    if (!ctx) return;
    ctx->cycle_counter += cycles;

    if (ctx->cycle_counter - ctx->last_sched_cycle >= SCHED_QUANTUM_CYCLES) {
        scheduler_run(ctx);
    }
    if (ctx->irq_stat & ctx->irq_mask) {
        ps1_irq_poll(ctx);
    }
}

void ps1_runtime_run_frame(PS1Context* ctx) {
    if (!ctx || !ctx->running || ctx->paused) return;

    if (ctx->cpu.pc != 0) {
        ps1_call_indirect(ctx, ctx->cpu.pc);
    }
}
