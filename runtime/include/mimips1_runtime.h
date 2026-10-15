#ifndef MIMIPS1_RUNTIME_H
#define MIMIPS1_RUNTIME_H

#include "mimips1_types.h"
#include "mimips1_memory.h"
#include "mimips1_cpu.h"
#include "mimips1_gte.h"
#include "mimips1_gpu.h"
#include "mimips1_spu.h"
#include "mimips1_dma.h"
#include "mimips1_timers.h"
#include "mimips1_cdrom.h"
#include "mimips1_bios.h"
#include "mimips1_input.h"
#include "mimips1_card.h"

#ifdef __cplusplus
extern "C" {
#endif

// IRQ Bits (I_STAT & I_MASK at 0x1F801070/0x1F801074)
#define IRQ_VBLANK      (1 << 0)
#define IRQ_GPU         (1 << 1)
#define IRQ_CDROM       (1 << 2)
#define IRQ_DMA         (1 << 3)
#define IRQ_TIMER0      (1 << 4)
#define IRQ_TIMER1      (1 << 5)
#define IRQ_TIMER2      (1 << 6)
#define IRQ_CONTROLLER  (1 << 7)
#define IRQ_SIO         (1 << 8)
#define IRQ_SPU         (1 << 9)
#define IRQ_PIO         (1 << 10)

struct PS1Context {
    PS1CPU         cpu;
    PS1Memory      memory;
    PS1GPU         gpu;
    PS1GTE         gte;
    PS1SPU         spu;
    PS1DMA         dma;
    PS1Timers      timers;
    PS1CDROM       cdrom;
    PS1BIOS        bios;
    PS1Input       input;
    PS1CardManager card_mgr;

    uint32_t    irq_stat;
    uint32_t    irq_mask;

    bool        running;
    bool        paused;
    uint64_t    frame_count;

    // Cycle & event timing
    uint64_t    cycle_counter;
    uint32_t    cycles_since_vblank;
    bool        in_tick;
    bool        in_irq_callback;
    uint32_t    pending_irq_bits;
    uint64_t    start_time_ns;
    uint64_t    next_vblank_cycle;

    // Executable bounds
    uint32_t    exe_text_addr;
    uint32_t    exe_text_size;

    // Video display / presentation options
    int         aspect_ratio; // 0 = 4:3 (original), 1 = 16:9, 2 = Stretch
    bool        crt_scanlines;
    bool        linear_filtering;
};

// Global log level and logging macro
extern int g_mimips1_log_level;
int ps1_get_log_level(void);
#define PS1_LOG(level, ...) do { \
    if (g_mimips1_log_level < 0) g_mimips1_log_level = ps1_get_log_level(); \
    if (g_mimips1_log_level >= (level)) { printf(__VA_ARGS__); } \
} while(0)

PS1Context* ps1_runtime_create(void);
void        ps1_runtime_destroy(PS1Context* ctx);
void        ps1_runtime_reset(PS1Context* ctx);

// Loading PlayStation Executable (.EXE / .BIN)
bool        ps1_runtime_load_exe(PS1Context* ctx, const char* exe_path);

// Run a single video frame (~1/60th second, ~564,480 CPU cycles)
void        ps1_runtime_run_frame(PS1Context* ctx);

// Hardware Interrupt trigger
void        ps1_trigger_irq(PS1Context* ctx, uint32_t irq_bit);

// Advance CPU and hardware subsystem cycles deterministically
void        ps1_cpu_tick(PS1Context* ctx, uint32_t cycles);

#if defined(__APPLE__)
bool        macos_audio_init(PS1Context* ctx);
void        macos_audio_shutdown(void);
void        macos_audio_tick(PS1Context* ctx, uint32_t delta_cycles);
uint64_t    macos_audio_get_underruns(void);
#endif

// Convenience macros for generated recompiled C code
#define R(reg)          (ctx->cpu.r[reg])
#define PC              (ctx->cpu.pc)
#define HI              (ctx->cpu.hi)
#define LO              (ctx->cpu.lo)

#define MEM_R8(a)       ps1_mem_read8(ctx, (a))
#define MEM_R16(a)      ps1_mem_read16(ctx, (a))
#define MEM_R32(a)      ps1_mem_read32(ctx, (a))

#define MEM_W8(a, v)    ps1_mem_write8(ctx, (a), (v))
#define MEM_W16(a, v)   ps1_mem_write16(ctx, (a), (v))
#define MEM_W32(a, v)   ps1_mem_write32(ctx, (a), (v))

#define MEM_LWL(a, c)   ps1_mem_lwl(ctx, (a), (c))
#define MEM_LWR(a, c)   ps1_mem_lwr(ctx, (a), (c))
#define MEM_SWL(a, v)   ps1_mem_swl(ctx, (a), (v))
#define MEM_SWR(a, v)   ps1_mem_swr(ctx, (a), (v))

#define CALL_INDIRECT(t) ps1_call_indirect(ctx, (t))

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_RUNTIME_H
