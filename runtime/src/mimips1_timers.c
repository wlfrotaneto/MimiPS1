#include "mimips1_timers.h"
#include "mimips1_runtime.h"
#include <string.h>

void ps1_timers_init(PS1Timers* timers) {
    memset(timers, 0, sizeof(PS1Timers));
}

void ps1_timers_reset(PS1Timers* timers) {
    memset(timers, 0, sizeof(PS1Timers));
}

void ps1_timers_tick(PS1Context* ctx, uint32_t cycles) {
    if (!ctx || cycles == 0) return;

    for (int i = 0; i < 3; i++) {
        RootCounter* t = &ctx->timers.counters[i];
        uint32_t step = 0;

        if (i == 0) {
            // Timer 0: dot clock if mode bit 8 is set, otherwise system clock
            if (t->mode & 0x0100) {
                // Dot clock NTSC (~53.2224 MHz vs CPU ~33.8688 MHz)
                uint64_t total = (uint64_t)cycles * 53222400ULL + t->frac;
                step = (uint32_t)(total / 33868800ULL);
                t->frac = (uint32_t)(total % 33868800ULL);
            } else {
                step = cycles;
            }
        } else if (i == 1) {
            // Timer 1: H-Blank if mode bit 8 is set (~2152 cycles per scanline)
            if (t->mode & 0x0100) {
                uint64_t total = (uint64_t)cycles + t->frac;
                step = (uint32_t)(total / 2152);
                t->frac = (uint32_t)(total % 2152);
            } else {
                step = cycles;
            }
        } else if (i == 2) {
            // Timer 2: system clock / 8 if mode bit 9 is set
            if (t->mode & 0x0200) {
                uint64_t total = (uint64_t)cycles + t->frac;
                step = (uint32_t)(total / 8);
                t->frac = (uint32_t)(total % 8);
            } else {
                step = cycles;
            }
        }

        if (step == 0) continue;

        uint32_t current = t->value;
        bool reset_on_target = (t->mode & (1 << 3)) != 0;
        bool irq_on_target   = (t->mode & (1 << 4)) != 0;
        bool irq_on_overflow = (t->mode & (1 << 5)) != 0;

        if (reset_on_target && t->target > 0) {
            uint32_t next = current + step;
            if (next >= t->target) {
                t->value = (uint16_t)(next % t->target);
                t->mode |= (1 << 11); // reached target
                if (irq_on_target) {
                    ps1_trigger_irq(ctx, IRQ_TIMER0 << i);
                }
            } else {
                t->value = (uint16_t)next;
            }
        } else {
            // Free-running count up to 0xFFFF, wrapping at 0x10000
            uint32_t next = current + step;
            if (t->target > 0 && current < t->target && next >= t->target) {
                t->mode |= (1 << 11); // reached target
                if (irq_on_target) {
                    ps1_trigger_irq(ctx, IRQ_TIMER0 << i);
                }
            }
            if (next >= 0x10000) {
                t->value = (uint16_t)(next & 0xFFFF);
                t->mode |= (1 << 12); // reached 0xFFFF
                if (irq_on_overflow) {
                    ps1_trigger_irq(ctx, IRQ_TIMER0 << i);
                }
            } else {
                t->value = (uint16_t)next;
            }
        }
    }
}

uint16_t ps1_timers_read(PS1Timers* timers, uint32_t addr) {
    int idx = (addr - 0x1F801100) / 0x10;
    int reg = (addr & 0x0F);
    if (idx < 3) {
        if (reg == 0x00) return timers->counters[idx].value;
        if (reg == 0x04) {
            uint16_t m = timers->counters[idx].mode;
            // Bits 11 and 12 are cleared after reading, as in hardware
            timers->counters[idx].mode &= ~((1 << 11) | (1 << 12));
            return m;
        }
        if (reg == 0x08) return timers->counters[idx].target;
    }
    return 0;
}

void ps1_timers_write(PS1Timers* timers, uint32_t addr, uint16_t val) {
    int idx = (addr - 0x1F801100) / 0x10;
    int reg = (addr & 0x0F);
    if (idx < 3) {
        if (reg == 0x00) {
            timers->counters[idx].value = val;
        } else if (reg == 0x04) {
            timers->counters[idx].mode = val;
            timers->counters[idx].value = 0;
            timers->counters[idx].frac = 0;
        } else if (reg == 0x08) {
            timers->counters[idx].target = val;
        }
    }
}
