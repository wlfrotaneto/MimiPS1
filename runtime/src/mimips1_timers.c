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
    for (int i = 0; i < 3; i++) {
        RootCounter* t = &ctx->timers.counters[i];
        uint32_t step = cycles;
        if (i == 2 && (t->mode & 0x0200)) {
            step = cycles / 8; // Div 8 mode
        }

        uint32_t new_val = (uint32_t)t->value + step;
        if (t->target > 0 && new_val >= t->target) {
            new_val %= t->target;
            if (t->mode & (1 << 4)) { // IRQ on target
                ps1_trigger_irq(ctx, IRQ_TIMER0 << i);
            }
        } else if (new_val >= 0xFFFF) {
            new_val &= 0xFFFF;
            if (t->mode & (1 << 5)) { // IRQ on overflow
                ps1_trigger_irq(ctx, IRQ_TIMER0 << i);
            }
        }
        t->value = (uint16_t)new_val;
    }
}

uint16_t ps1_timers_read(PS1Timers* timers, uint32_t addr) {
    int idx = (addr - 0x1F801100) / 0x10;
    int reg = (addr & 0x0F);
    if (idx < 3) {
        if (reg == 0x00) return timers->counters[idx].value;
        if (reg == 0x04) return timers->counters[idx].mode;
        if (reg == 0x08) return timers->counters[idx].target;
    }
    return 0;
}

void ps1_timers_write(PS1Timers* timers, uint32_t addr, uint16_t val) {
    int idx = (addr - 0x1F801100) / 0x10;
    int reg = (addr & 0x0F);
    if (idx < 3) {
        if (reg == 0x00) timers->counters[idx].value = val;
        else if (reg == 0x04) {
            timers->counters[idx].mode = val;
            timers->counters[idx].value = 0;
        } else if (reg == 0x08) {
            timers->counters[idx].target = val;
        }
    }
}
