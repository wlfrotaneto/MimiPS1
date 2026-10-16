#include "mimips1_memory.h"
#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>

void ps1_memory_init(PS1Context* ctx) {
    memset(&ctx->memory, 0, sizeof(PS1Memory));
    ctx->memory.ram_base = ctx->memory.ram;
}

void ps1_memory_reset(PS1Context* ctx) {
    memset(ctx->memory.ram, 0, sizeof(ctx->memory.ram));
    memset(ctx->memory.scratchpad, 0, sizeof(ctx->memory.scratchpad));
}

bool ps1_memory_load_bios(PS1Context* ctx, const char* bios_path) {
    FILE* f = fopen(bios_path, "rb");
    if (!f) return false;
    size_t read_bytes = fread(ctx->memory.bios, 1, PS1_BIOS_SIZE, f);
    fclose(f);
    ctx->memory.has_bios = (read_bytes == PS1_BIOS_SIZE);
    return ctx->memory.has_bios;
}

uint8_t* ps1_mem_get_direct_ram_ptr(PS1Context* ctx, uint32_t addr) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (__builtin_expect(paddr < 0x00800000, 1)) {
        return &ctx->memory.ram[paddr & PS1_RAM_MIRROR_MASK];
    }
    return NULL;
}

uint8_t ps1_mem_read8(PS1Context* ctx, uint32_t addr) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (paddr < 0x00800000) {
        return ctx->memory.ram[paddr & PS1_RAM_MIRROR_MASK];
    } else if (paddr >= 0x1F800000 && paddr < 0x1F800400) {
        return ctx->memory.scratchpad[paddr - 0x1F800000];
    } else if (paddr >= 0x1FC00000 && paddr < 0x1FC80000) {
        return ctx->memory.bios[paddr - 0x1FC00000];
    } else if (paddr >= 0x1F801000 && paddr < 0x1F803000) {
        return (uint8_t)ps1_mmio_read(ctx, paddr, 1);
    }
    return 0xFF;
}

uint16_t ps1_mem_read16(PS1Context* ctx, uint32_t addr) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (paddr < 0x00800000) {
        return *(uint16_t*)&ctx->memory.ram[(paddr & PS1_RAM_MIRROR_MASK) & ~1];
    } else if (paddr >= 0x1F800000 && paddr < 0x1F800400) {
        return *(uint16_t*)&ctx->memory.scratchpad[paddr - 0x1F800000];
    } else if (paddr >= 0x1FC00000 && paddr < 0x1FC80000) {
        return *(uint16_t*)&ctx->memory.bios[paddr - 0x1FC00000];
    } else if (paddr >= 0x1F801000 && paddr < 0x1F803000) {
        return (uint16_t)ps1_mmio_read(ctx, paddr, 2);
    }
    return 0xFFFF;
}

uint32_t ps1_mem_read32(PS1Context* ctx, uint32_t addr) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (__builtin_expect(paddr < 0x00800000, 1)) {
        return *(uint32_t*)&ctx->memory.ram[(paddr & PS1_RAM_MIRROR_MASK) & ~3];
    } else if (paddr >= 0x1F800000 && paddr < 0x1F800400) {
        return *(uint32_t*)&ctx->memory.scratchpad[paddr - 0x1F800000];
    } else if (paddr >= 0x1FC00000 && paddr < 0x1FC80000) {
        return *(uint32_t*)&ctx->memory.bios[paddr - 0x1FC00000];
    } else if (paddr >= 0x1F801000 && paddr < 0x1F803000) {
        return ps1_mmio_read(ctx, paddr, 4);
    }
    return 0xFFFFFFFF;
}

void ps1_mem_write8(PS1Context* ctx, uint32_t addr, uint8_t val) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (paddr < 0x00800000) {
        ctx->memory.ram[paddr & PS1_RAM_MIRROR_MASK] = val;
    } else if (paddr >= 0x1F800000 && paddr < 0x1F800400) {
        ctx->memory.scratchpad[paddr - 0x1F800000] = val;
    } else if (paddr >= 0x1F801000 && paddr < 0x1F803000) {
        ps1_mmio_write(ctx, paddr, val, 1);
    }
}

void ps1_mem_write16(PS1Context* ctx, uint32_t addr, uint16_t val) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (paddr < 0x00800000) {
        *(uint16_t*)&ctx->memory.ram[(paddr & PS1_RAM_MIRROR_MASK) & ~1] = val;
    } else if (paddr >= 0x1F800000 && paddr < 0x1F800400) {
        *(uint16_t*)&ctx->memory.scratchpad[paddr - 0x1F800000] = val;
    } else if (paddr >= 0x1F801000 && paddr < 0x1F803000) {
        ps1_mmio_write(ctx, paddr, val, 2);
    }
}

void ps1_mem_write32(PS1Context* ctx, uint32_t addr, uint32_t val) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (__builtin_expect(paddr < 0x00800000, 1)) {
        *(uint32_t*)&ctx->memory.ram[(paddr & PS1_RAM_MIRROR_MASK) & ~3] = val;
    } else if (paddr >= 0x1F800000 && paddr < 0x1F800400) {
        *(uint32_t*)&ctx->memory.scratchpad[paddr - 0x1F800000] = val;
    } else if (paddr >= 0x1F801000 && paddr < 0x1F803000) {
        ps1_mmio_write(ctx, paddr, val, 4);
    }
}

uint32_t ps1_mem_lwl(PS1Context* ctx, uint32_t addr, uint32_t cur_val) {
    uint32_t aligned = ps1_mem_read32(ctx, addr & ~3);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0x00FFFFFF >> shift;
    return (cur_val & mask) | (aligned << (24 - shift));
}

uint32_t ps1_mem_lwr(PS1Context* ctx, uint32_t addr, uint32_t cur_val) {
    uint32_t aligned = ps1_mem_read32(ctx, addr & ~3);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0xFFFFFF00 << (24 - shift);
    return (cur_val & mask) | (aligned >> shift);
}

void ps1_mem_swl(PS1Context* ctx, uint32_t addr, uint32_t val) {
    uint32_t aligned = ps1_mem_read32(ctx, addr & ~3);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0xFFFFFF00 << shift;
    uint32_t res = (aligned & mask) | (val >> (24 - shift));
    ps1_mem_write32(ctx, addr & ~3, res);
}

void ps1_mem_swr(PS1Context* ctx, uint32_t addr, uint32_t val) {
    uint32_t aligned = ps1_mem_read32(ctx, addr & ~3);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0x00FFFFFF >> (24 - shift);
    uint32_t res = (aligned & mask) | (val << shift);
    ps1_mem_write32(ctx, addr & ~3, res);
}

uint32_t ps1_mmio_read(PS1Context* ctx, uint32_t addr, int size) {
    ps1_cpu_tick(ctx, 32);

    // Interrupt Controller
    if (addr == 0x1F801070) {
        return ctx->irq_stat;
    } else if (addr == 0x1F801074) {
        return ctx->irq_mask;
    }
    // GPU Registers
    else if (addr == 0x1F801810) {
        return ps1_gpu_read_data(&ctx->gpu);
    } else if (addr == 0x1F801814) {
        return ps1_gpu_read_status(&ctx->gpu);
    }
    // DMA Registers
    else if (addr >= 0x1F801080 && addr <= 0x1F8010F4) {
        return ps1_dma_read(ctx, addr);
    }
    // Timers
    else if (addr >= 0x1F801100 && addr <= 0x1F80112E) {
        return ps1_timers_read(&ctx->timers, addr);
    }
    // CD-ROM
    else if (addr >= 0x1F801800 && addr <= 0x1F801803) {
        return ps1_cdrom_read(ctx, addr);
    }
    // SPU
    else if (addr >= 0x1F801C00 && addr <= 0x1F801E80) {
        return ps1_spu_read(&ctx->spu, addr);
    }
    // MDEC Registers
    else if (addr == 0x1F801820) {
        return 0;
    } else if (addr == 0x1F801824) {
        return 0x80040000; // MDEC Status: FIFO empty, not busy, 15-bit output
    }
    // Controller / SIO
    else if (addr >= 0x1F801040 && addr <= 0x1F80104E) {
        return ps1_input_read16(&ctx->input, addr);
    }
    return 0;
}

void ps1_mmio_write(PS1Context* ctx, uint32_t addr, uint32_t val, int size) {
    // Interrupt Controller
    if (addr == 0x1F801070) {
        // Acknowledge IRQ bits (write 0 to acknowledge, or AND)
        ctx->irq_stat &= val;
        if ((ctx->irq_stat & ctx->irq_mask) == 0) {
            ctx->cpu.cop0[COP0_CAUSE] &= ~(1 << 10);
        }
    } else if (addr == 0x1F801074) {
        ctx->irq_mask = val;
    }
    // GPU Registers
    else if (addr == 0x1F801810) {
        ps1_gpu_write_gp0(&ctx->gpu, val);
    } else if (addr == 0x1F801814) {
        ps1_gpu_write_gp1(&ctx->gpu, val);
    }
    // DMA Registers
    else if (addr >= 0x1F801080 && addr <= 0x1F8010F4) {
        ps1_dma_write(ctx, addr, val);
    }
    // Timers
    else if (addr >= 0x1F801100 && addr <= 0x1F80112E) {
        ps1_timers_write(&ctx->timers, addr, (uint16_t)val);
    }
    // CD-ROM
    else if (addr >= 0x1F801800 && addr <= 0x1F801803) {
        ps1_cdrom_write(ctx, addr, (uint8_t)val);
    }
    // MDEC Registers
    else if (addr == 0x1F801820 || addr == 0x1F801824) {
        // MDEC command/control write stub
    }
    // SPU
    else if (addr >= 0x1F801C00 && addr <= 0x1F801E80) {
        ps1_spu_write(&ctx->spu, addr, (uint16_t)val);
    }
    // Controller / SIO
    else if (addr >= 0x1F801040 && addr <= 0x1F80104E) {
        ps1_input_write16(&ctx->input, addr, (uint16_t)val);
    }
}
