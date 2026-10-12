#ifndef MIMIPS1_CPU_H
#define MIMIPS1_CPU_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Coprocessor 0 Registers
#define COP0_BPC        3   // Breakpoint on execute
#define COP0_BDA        5   // Breakpoint on data access
#define COP0_TAR        6   // Target address
#define COP0_DCIC       7   // Breakpoint control
#define COP0_BADVADDR   8   // Bad Virtual Address
#define COP0_BDAM       9   // Data Access Mask
#define COP0_BPCM       11  // Execute Address Mask
#define COP0_SR         12  // Status Register
#define COP0_CAUSE      13  // Cause Register
#define COP0_EPC        14  // Exception Program Counter
#define COP0_PRID       15  // Processor ID

// Status Register Bits
#define SR_IEc          (1 << 0)    // Current Interrupt Enable
#define SR_KUc          (1 << 1)    // Current Kernel/User Mode (0=Kernel, 1=User)
#define SR_IEp          (1 << 2)    // Previous Interrupt Enable
#define SR_KUp          (1 << 3)    // Previous Kernel/User Mode
#define SR_IEo          (1 << 4)    // Old Interrupt Enable
#define SR_KUo          (1 << 5)    // Old Kernel/User Mode
#define SR_Im           (0xFF << 8) // Interrupt Mask
#define SR_IsC          (1 << 16)   // Isolate Cache
#define SR_BEV          (1 << 22)   // Boot Exception Vectors
#define SR_CU0          (1 << 28)   // Coprocessor 0 Enable
#define SR_CU2          (1 << 30)   // Coprocessor 2 (GTE) Enable

// Function lookup table size for 2MB RAM (4-byte aligned)
#define PS1_FUNC_TABLE_SIZE (PS1_RAM_SIZE / 4)

struct PS1CPU {
    uint32_t r[32];         // General Purpose Registers ($0 is hardwired to 0)
    uint32_t pc;            // Current Program Counter
    uint32_t hi;            // Multiply/Divide High Result
    uint32_t lo;            // Multiply/Divide Low Result
    uint32_t cop0[32];      // Coprocessor 0 Registers
    
    uint64_t total_cycles;  // Cycles executed
    uint32_t next_pc;       // Stored next PC for jumps
    bool branch_taken;      // Flag for branch resolution
    bool is_in_delay_slot;  // Flag for exception in delay slot
    
    // Fast O(1) Indirect Call Lookup Table
    PS1RecompiledFunc func_table[PS1_FUNC_TABLE_SIZE];
};

void ps1_cpu_init(PS1Context* ctx);
void ps1_cpu_reset(PS1Context* ctx);

// Function registration for static recompilation
void ps1_register_function(PS1Context* ctx, uint32_t mips_addr, PS1RecompiledFunc func);
// Registers func for every word in [start, end] whose slot is still empty.
// Used for mid-function entry trampolines (targets reached only via function pointers).
void ps1_register_function_range(PS1Context* ctx, uint32_t start, uint32_t end, PS1RecompiledFunc func);

// Fast Indirect Branch Dispatcher
void ps1_call_indirect(PS1Context* ctx, uint32_t target_pc);

// Syscall / Exception Dispatcher
void ps1_trigger_syscall(PS1Context* ctx);
void ps1_trigger_break(PS1Context* ctx);
void ps1_rfe(PS1Context* ctx); // Restore From Exception

// Register getters/setters with r0 protection
static inline void ps1_set_reg(PS1CPU* cpu, uint32_t reg, uint32_t val) {
    if (__builtin_expect(reg != 0, 1)) {
        cpu->r[reg] = val;
    }
}

static inline uint32_t ps1_get_reg(const PS1CPU* cpu, uint32_t reg) {
    return cpu->r[reg];
}

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_CPU_H
