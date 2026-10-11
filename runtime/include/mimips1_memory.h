#ifndef MIMIPS1_MEMORY_H
#define MIMIPS1_MEMORY_H

#include "mimips1_types.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PS1Memory {
    uint8_t ram[PS1_RAM_SIZE];                     // 2MB Main RAM
    uint8_t scratchpad[PS1_SCRATCHPAD_SIZE];       // 1KB Scratchpad (Fast D-Cache)
    uint8_t bios[PS1_BIOS_SIZE];                   // 512KB BIOS ROM (optional LLE)
    bool has_bios;
    
    // Fast RAM pointer for direct pointer math
    uint8_t* ram_base;
};

void ps1_memory_init(PS1Context* ctx);
void ps1_memory_reset(PS1Context* ctx);
bool ps1_memory_load_bios(PS1Context* ctx, const char* bios_path);

// Fast Address Translation (checks if in RAM)
uint8_t* ps1_mem_get_direct_ram_ptr(PS1Context* ctx, uint32_t addr);

// Memory Bus Read operations
uint8_t  ps1_mem_read8(PS1Context* ctx, uint32_t addr);
uint16_t ps1_mem_read16(PS1Context* ctx, uint32_t addr);
uint32_t ps1_mem_read32(PS1Context* ctx, uint32_t addr);

// Memory Bus Write operations
void ps1_mem_write8(PS1Context* ctx, uint32_t addr, uint8_t val);
void ps1_mem_write16(PS1Context* ctx, uint32_t addr, uint16_t val);
void ps1_mem_write32(PS1Context* ctx, uint32_t addr, uint32_t val);

// Unaligned MIPS Load/Store primitives (LWL, LWR, SWL, SWR)
uint32_t ps1_mem_lwl(PS1Context* ctx, uint32_t addr, uint32_t cur_val);
uint32_t ps1_mem_lwr(PS1Context* ctx, uint32_t addr, uint32_t cur_val);
void     ps1_mem_swl(PS1Context* ctx, uint32_t addr, uint32_t val);
void     ps1_mem_swr(PS1Context* ctx, uint32_t addr, uint32_t val);

// MMIO Handlers
uint32_t ps1_mmio_read(PS1Context* ctx, uint32_t addr, int size);
void     ps1_mmio_write(PS1Context* ctx, uint32_t addr, uint32_t val, int size);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_MEMORY_H
