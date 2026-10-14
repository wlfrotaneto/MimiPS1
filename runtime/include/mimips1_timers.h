#ifndef MIMIPS1_TIMERS_H
#define MIMIPS1_TIMERS_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t value;
    uint16_t mode;
    uint16_t target;
    bool     irq;
    uint32_t frac;
} RootCounter;

struct PS1Timers {
    RootCounter counters[3];
};

void     ps1_timers_init(PS1Timers* timers);
void     ps1_timers_reset(PS1Timers* timers);
void     ps1_timers_tick(PS1Context* ctx, uint32_t cycles);
uint16_t ps1_timers_read(PS1Timers* timers, uint32_t addr);
void     ps1_timers_write(PS1Timers* timers, uint32_t addr, uint16_t val);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_TIMERS_H
