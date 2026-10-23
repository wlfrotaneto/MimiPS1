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
#include "mimips1_mdec.h"

#include <setjmp.h>

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
    PS1MDEC        mdec;

    uint32_t    irq_stat;
    uint32_t    irq_mask;

    bool        running;
    bool        paused;
    uint64_t    frame_count;

    // Cycle & event timing
    uint64_t    cycle_counter;
    uint32_t    cycles_since_vblank;
    bool        in_tick;
    bool        in_irq;
    bool        bios_boot;          // Real BIOS boots the game (otherwise built-in HLE kernel)
    bool        native_enabled;     // Recompiled functions may be entered from the interpreter
    bool        native_tried;       // Boot executable already validated once
    bool        in_native;          // Currently executing recompiled code
    int         interp_depth;
    bool        irq_in_queue;       // Running SysEnqIntRP handlers (kernel runs these with IRQs off)
    int         irq_depth;          // Nesting level of guest interrupt callbacks
    jmp_buf     irq_env;
    bool        in_irq_handler;
    uint64_t    start_time_ns;      // Wall-clock origin used only for 60 Hz frame pacing
    uint64_t    next_vblank_cycle;
    uint64_t    last_sched_cycle;   // Cycle count at which hardware was last advanced
    uint32_t    load_cost;          // extra cycles for a CPU load from RAM (no data cache on the R3000A; MIMIPS1_LOADCOST, default 4)
    uint32_t    cpi;                // average cycles per instruction x2 (MIMIPS1_CPI, default 1.0)
    bool        unthrottled;        // MIMIPS1_UNTHROTTLED=1: skip real-time frame pacing

    // Executable bounds
    uint32_t    exe_text_addr;
    uint32_t    exe_text_size;
    uint32_t    exe_entry;          // initial PC of the boot executable
    uint8_t*    exe_shadow;         // pristine copy of the executable's text image

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

// Recompiled code is only trusted while RAM still holds the original instructions
void        ps1_native_check_range(PS1Context* ctx, uint32_t addr, uint32_t bytes);
void        ps1_native_try_enable(PS1Context* ctx);
bool        ps1_native_entry_allowed(uint32_t pc);

// Boot through the real BIOS ROM at the reset vector / run the emulation thread
void        ps1_runtime_boot_bios(PS1Context* ctx);
void        ps1_runtime_run(PS1Context* ctx);

// Run a single video frame (~1/60th second, ~564,480 CPU cycles)
void        ps1_runtime_run_frame(PS1Context* ctx);

// Latch an interrupt source into I_STAT. Never runs guest code: delivery to
// handlers happens at safe points via ps1_irq_poll().
void        ps1_trigger_irq(PS1Context* ctx, uint32_t irq_bit);

// Deliver pending interrupts if (I_STAT & I_MASK) != 0 and SR.IEc/IM2 allow it.
// Must only be called at an instruction boundary of guest code.
void        ps1_irq_poll(PS1Context* ctx);

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

// CPU loads from RAM stall the pipeline (the R3000A in the PS1 has no data cache); scratchpad is free
#define LOAD_COST(a)    ((void)(ctx->cycle_counter += ((((a) & 0x1FFFFFFFu) < 0x00800000u) ? ctx->load_cost : 0u)))
#define MEM_R8(a)       (LOAD_COST(a), ps1_mem_read8(ctx, (a)))
#define MEM_R16(a)      (LOAD_COST(a), ps1_mem_read16(ctx, (a)))
#define MEM_R32(a)      (LOAD_COST(a), ps1_mem_read32(ctx, (a)))

#define MEM_W8(a, v)    ps1_mem_write8(ctx, (a), (v))
#define MEM_W16(a, v)   ps1_mem_write16(ctx, (a), (v))
#define MEM_W32(a, v)   ps1_mem_write32(ctx, (a), (v))

#define MEM_LWL(a, c)   (LOAD_COST(a), ps1_mem_lwl(ctx, (a), (c)))
#define MEM_LWR(a, c)   (LOAD_COST(a), ps1_mem_lwr(ctx, (a), (c)))
#define MEM_SWL(a, v)   ps1_mem_swl(ctx, (a), (v))
#define MEM_SWR(a, v)   ps1_mem_swr(ctx, (a), (v))

#define CALL_INDIRECT(t) ps1_call_indirect(ctx, (t))

// Direct call of a recompiled function. PS1_VERIFY builds run each (sampled) call a second time in
// the interpreter from the same state and report any divergence (MIMIPS1_VERIFY=<first frame>).
#ifdef PS1_VERIFY
#include <stdio.h>
extern FILE* ps1_step_file;
void ps1_step_print(PS1Context* ctx, uint32_t pc);
#define PS1_STEP(pc) do { if (ps1_step_file) ps1_step_print(ctx, (pc)); } while (0)
void ps1_verify_call(PS1Context* ctx, uint32_t addr, PS1RecompiledFunc fn);
#define PS1_CALL(addr, fn) ps1_verify_call(ctx, (addr), (fn))
#else
#define PS1_STEP(pc) ((void)0)
#define PS1_CALL(addr, fn) fn(ctx)
#endif

// Optional function-entry tracing (build with -DPS1_TRACE; MIMIPS1_TRACE=addr,addr,... at runtime)
#ifdef PS1_TRACE
void ps1_trace_enter(PS1Context* ctx, uint32_t addr);
#define PS1_TRACE_ENTER(a) ps1_trace_enter(ctx, (a))
#else
#define PS1_TRACE_ENTER(a) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_RUNTIME_H
