#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "mimips1_interp.h"

int g_mimips1_log_level = -1;

void ps1_trace_report(void);
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
    ps1_mdec_init(&ctx->mdec);
    ps1_timers_init(&ctx->timers);
    ps1_cdrom_init(&ctx->cdrom);
    ps1_bios_init(ctx);
    ps1_input_init(&ctx->input);
    ps1_card_init(&ctx->card_mgr);
    ps1_card_auto_setup(&ctx->card_mgr);

    ctx->running = true;
    ctx->native_enabled = true;   // HLE mode runs recompiled code from the start; BIOS boot validates first
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
    ps1_mdec_reset(&ctx->mdec);
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
    ctx->exe_entry = init_pc;

    // Keep a pristine copy of the text image: recompiled code is only valid while RAM matches it
    free(ctx->exe_shadow);
    ctx->exe_shadow = (uint8_t*)malloc(text_size);
    if (ctx->exe_shadow && (paddr + text_size <= PS1_RAM_SIZE)) {
        memcpy(ctx->exe_shadow, &ctx->memory.ram[paddr], text_size);
    }

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
    irq_update_cause(ctx);
    if (ctx->bios_boot) {
        // The real kernel dispatches interrupts. The interpreter takes them itself; native
        // code reaches here and hands the exception to the kernel at this safe point.
        if (ctx->in_native && ctx->interp_depth < IRQ_MAX_DEPTH + 4) {
            uint32_t sr = ctx->cpu.cop0[COP0_SR];
            if ((sr & SR_IEc) && (ctx->cpu.cop0[COP0_CAUSE] & sr & 0x0700)) {
                ps1_interp_interrupt_from_native(ctx);
            }
        }
        return;
    }
    if (ctx->irq_depth >= IRQ_MAX_DEPTH || ctx->irq_in_queue) return;

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

// Scripted pad input for headless testing: MIMIPS1_INPUT="start@300+8,cross@500+4"
// presses a button at the given frame for the given number of frames.
typedef struct { PS1Button btn; uint64_t from, to; } InputStep;

static void apply_scripted_input(PS1Context* ctx) {
    static InputStep steps[64];
    static int nsteps = -1;
    if (nsteps < 0) {
        nsteps = 0;
        static const struct { const char* name; PS1Button btn; } names[] = {
            {"start", PS1_BTN_START}, {"select", PS1_BTN_SELECT}, {"up", PS1_BTN_UP}, {"down", PS1_BTN_DOWN},
            {"left", PS1_BTN_LEFT}, {"right", PS1_BTN_RIGHT}, {"cross", PS1_BTN_CROSS}, {"circle", PS1_BTN_CIRCLE},
            {"square", PS1_BTN_SQUARE}, {"triangle", PS1_BTN_TRIANGLE}, {"l1", PS1_BTN_L1}, {"r1", PS1_BTN_R1},
            {"l2", PS1_BTN_L2}, {"r2", PS1_BTN_R2},
        };
        const char* env = getenv("MIMIPS1_INPUT");
        char buf[512];
        if (env) {
            strncpy(buf, env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            for (char* tok = strtok(buf, ","); tok && nsteps < 64; tok = strtok(NULL, ",")) {
                char* at = strchr(tok, '@');
                if (!at) continue;
                *at = '\0';
                unsigned long from = strtoul(at + 1, NULL, 10);
                char* plus = strchr(at + 1, '+');
                unsigned long len = plus ? strtoul(plus + 1, NULL, 10) : 4;
                for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                    if (strcasecmp(tok, names[i].name) == 0) {
                        steps[nsteps++] = (InputStep){ names[i].btn, from, from + len };
                    }
                }
            }
        }
    }
    for (int i = 0; i < nsteps; i++) {
        bool down = ctx->frame_count >= steps[i].from && ctx->frame_count < steps[i].to;
        ps1_input_set_button(&ctx->input, 0, steps[i].btn, down);
    }
    PS1_LOG(2, "[PAD] frame=%llu buttons=%04X active=%d dest=%08X mem=%08X\n", (unsigned long long)ctx->frame_count, ctx->input.pads[0].buttons, ctx->bios.pad_active, ctx->bios.pad_button_dest, ctx->bios.pad_button_dest ? ps1_mem_read32(ctx, ctx->bios.pad_button_dest) : 0);
}

static void scheduler_run(PS1Context* ctx) {
    uint32_t delta = (uint32_t)(ctx->cycle_counter - ctx->last_sched_cycle);
    ctx->last_sched_cycle = ctx->cycle_counter;

    ps1_timers_tick(ctx, delta);
    ps1_cdrom_tick(ctx, delta);
    ps1_input_tick(ctx, delta);
    ps1_dma_tick(ctx, delta);
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

        if ((ctx->frame_count % 60) == 0) ps1_trace_report();
        {
            static long dump_at = -2;
            if (dump_at == -2) { const char* e = getenv("MIMIPS1_RAMDUMP"); dump_at = e ? atol(e) : -1; }
            if (dump_at >= 0 && (long)ctx->frame_count == dump_at) {
                FILE* f = fopen("/tmp/ps1_ram.bin", "wb");
                if (f) { fwrite(ctx->memory.ram, 1, PS1_RAM_SIZE, f); fclose(f); }
                printf("[RAMDUMP] frame %ld pc=%08X ra=%08X sp=%08X\n", dump_at, ctx->cpu.pc, ctx->cpu.r[31], ctx->cpu.r[29]);
            }
        }
        apply_scripted_input(ctx);
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
static void check_watch(PS1Context* ctx) {
    static uint32_t watch_addr = 0xFFFFFFFF, last = 0;
    static int init = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("MIMIPS1_WATCH");
        if (e) { watch_addr = (uint32_t)strtoul(e, NULL, 16); last = ps1_mem_read32(ctx, watch_addr); }
    }
    if (watch_addr == 0xFFFFFFFF) return;
    uint32_t v = ps1_mem_read32(ctx, watch_addr);
    if (v != last) {
        printf("[WATCH] %08X: %08X -> %08X  pc=%08X ra=%08X (cycle %llu) lastdma=ch%d madr=%08X bcr=%08X\n", watch_addr, last, v,
               ctx->cpu.pc, ctx->cpu.r[31], (unsigned long long)ctx->cycle_counter,
               ctx->dma.last_channel, ctx->dma.last_madr, ctx->dma.last_bcr);
        last = v;
    }
}

void ps1_cpu_tick(PS1Context* ctx, uint32_t cycles) {
    if (!ctx) return;
    check_watch(ctx);
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

#ifdef PS1_TRACE
// Counts entries of the functions listed in MIMIPS1_TRACE and prints the deltas on every heartbeat.
#define TRACE_MAX 32
static uint32_t s_trace_addr[TRACE_MAX];
static uint64_t s_trace_count[TRACE_MAX];
static int s_trace_n = -1;

void ps1_trace_enter(PS1Context* ctx, uint32_t addr) {
    (void)ctx;
    if (s_trace_n < 0) {
        s_trace_n = 0;
        const char* env = getenv("MIMIPS1_TRACE");
        char buf[512];
        if (env) {
            strncpy(buf, env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            for (char* t = strtok(buf, ","); t && s_trace_n < TRACE_MAX; t = strtok(NULL, ",")) {
                s_trace_addr[s_trace_n++] = (uint32_t)strtoul(t, NULL, 16);
            }
        }
    }
    for (int i = 0; i < s_trace_n; i++) {
        if (s_trace_addr[i] == addr) { s_trace_count[i]++; break; }
    }
}

void ps1_trace_report(void) {
    for (int i = 0; i < s_trace_n; i++) {
        printf("[TRACE] %08X calls=%llu\n", s_trace_addr[i], (unsigned long long)s_trace_count[i]);
    }
}
#else
void ps1_trace_report(void) {}
#endif

// Puts the CPU at the reset vector so the real BIOS boots the disc.
void ps1_runtime_boot_bios(PS1Context* ctx) {
    memset(ctx->memory.ram, 0, sizeof(ctx->memory.ram));
    memset(ctx->cpu.r, 0, sizeof(ctx->cpu.r));
    ctx->cpu.hi = ctx->cpu.lo = 0;
    ctx->cpu.pc = 0xBFC00000u;
    ctx->cpu.next_pc = 0xBFC00004u;
    ctx->cpu.is_in_delay_slot = false;
    ctx->cpu.ld_reg = 0;
    ctx->cpu.cop0[COP0_SR] = SR_BEV;        // reset state: boot exception vectors, interrupts off
    ctx->cpu.cop0[COP0_CAUSE] = 0;
    ctx->bios_boot = true;
    ctx->native_enabled = false;
    ctx->irq_stat = 0;
    ctx->irq_mask = 0;
}

// Entry point of the emulation thread.
void ps1_runtime_run(PS1Context* ctx) {
    if (ctx->bios_boot) {
        ps1_interp_run(ctx);
    } else if (ctx->cpu.pc != 0) {
        ps1_call_indirect(ctx, ctx->cpu.pc);
    }
}

// Returns the pristine word for a RAM physical address, or false if outside the executable image.
static bool shadow_word(const PS1Context* ctx, uint32_t paddr, uint32_t* out) {
    if (!ctx->exe_shadow) return false;
    uint32_t base = ctx->exe_text_addr & 0x1FFFFFFF;
    if (paddr < base || paddr + 4 > base + ctx->exe_text_size) return false;
    memcpy(out, ctx->exe_shadow + (paddr - base), 4);
    return true;
}

// Drops the recompiled function that owns word `w`, so it is interpreted from now on.
static void native_invalidate_function(PS1Context* ctx, uint32_t w) {
    uint32_t e = w, steps = 0;
    while (e > 0 && !(ctx->cpu.entry_bits[e >> 3] & (1u << (e & 7))) && steps++ < 8192) e--;
    uint32_t n = w + 1;
    steps = 0;
    while (n < PS1_FUNC_TABLE_SIZE && !(ctx->cpu.entry_bits[n >> 3] & (1u << (n & 7))) && steps++ < 8192) n++;
    for (uint32_t off = e; off < n; off++) {
        ctx->cpu.func_table[off] = NULL;
        ctx->cpu.entry_bits[off >> 3] &= (uint8_t)~(1u << (off & 7));
        ctx->cpu.code_bits[off >> 3] &= (uint8_t)~(1u << (off & 7));
    }
    static int logged = 0;
    if (logged++ < 20) {
        printf("[MimiPS1] Code at 0x%08X changed: function 0x%08X..0x%08X now interpreted\n",
               0x80000000u | (w << 2), 0x80000000u | (e << 2), 0x80000000u | (n << 2));
    }
}

// Invalidates every recompiled function with a word in [addr, addr+bytes) that no longer matches
// the original instruction. Only words flagged as code are compared.
void ps1_native_check_range(PS1Context* ctx, uint32_t addr, uint32_t bytes) {
    if (!ctx->native_enabled || bytes == 0) return;
    uint32_t first = (addr & 0x001FFFFC) >> 2;
    uint32_t last = ((addr + bytes - 1) & 0x001FFFFC) >> 2;
    for (uint32_t w = first; w <= last && w < PS1_FUNC_TABLE_SIZE; w++) {
        if (!(ctx->cpu.code_bits[w >> 3] & (1u << (w & 7)))) continue;
        uint32_t orig, cur = *(uint32_t*)&ctx->memory.ram[w << 2];
        if (!shadow_word(ctx, w << 2, &orig) || orig != cur) native_invalidate_function(ctx, w);
    }
}

// Called when the boot executable is about to run: trust the recompiled functions only if the
// whole code image in RAM is identical to the one they were built from.
void ps1_native_try_enable(PS1Context* ctx) {
    if (ctx->native_enabled || !ctx->exe_shadow) return;
    ctx->native_enabled = true;
    ps1_native_check_range(ctx, ctx->exe_text_addr & 0x1FFFFFFF, ctx->exe_text_size);
    printf("[MimiPS1] Boot executable verified: running recompiled code natively\n");
}
