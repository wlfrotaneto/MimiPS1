#include "mimips1_bios.h"
#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps1_bios_init(PS1Context* ctx) {
    memset(&ctx->bios, 0, sizeof(PS1BIOS));
    ctx->bios.hle_enabled = true;
    ctx->bios.heap_base = 0x80100000;
    ctx->bios.heap_size = 0x80000; // 512KB default heap
    ctx->bios.heap_cur = ctx->bios.heap_base;
}

void ps1_bios_reset(PS1Context* ctx) {
    ps1_bios_init(ctx);
}

// Convert MIPS virtual address to host RAM pointer if possible
static void* get_host_ptr(PS1Context* ctx, uint32_t addr) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (paddr < PS1_RAM_SIZE) {
        return &ctx->memory.ram[paddr];
    }
    return NULL;
}

bool ps1_bios_handle_a0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;

    switch (func) {
        case 0x27: // memcpy(dst, src, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                void* src = get_host_ptr(ctx, r[5]);
                if (dst && src) {
                    memcpy(dst, src, r[6]);
                }
                r[2] = r[4]; // return dst
            }
            return true;
        case 0x28: // memset(dst, val, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                if (dst) {
                    memset(dst, (uint8_t)r[5], r[6]);
                }
                r[2] = r[4];
            }
            return true;
        case 0x2A: // memcmp(s1, s2, n)
            {
                void* s1 = get_host_ptr(ctx, r[4]);
                void* s2 = get_host_ptr(ctx, r[5]);
                if (s1 && s2) {
                    r[2] = memcmp(s1, s2, r[6]);
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x3E: // puts(str)
        case 0x3F: // printf(fmt, ...)
            {
                const char* fmt = (const char*)get_host_ptr(ctx, r[4]);
                if (fmt) {
                    printf("[PS1 TTY] %s\n", fmt);
                    fflush(stdout);
                }
                r[2] = 0;
            }
            return true;
        case 0x44: // FlushCache()
            return true;
        default:
            return false;
    }
}

void ps1_bios_deliver_event(PS1Context* ctx, uint32_t class_id, uint32_t spec) {
    for (int i = 0; i < PS1_MAX_EVENTS; i++) {
        if (ctx->bios.events[i].enabled &&
            ctx->bios.events[i].class_id == class_id &&
            (ctx->bios.events[i].spec == spec || ctx->bios.events[i].spec == 0)) {
            ctx->bios.events[i].triggered = true;
            if (ctx->bios.events[i].func != 0) {
                ps1_call_indirect(ctx, ctx->bios.events[i].func);
            }
        }
    }
}

bool ps1_bios_handle_b0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;

    switch (func) {
        case 0x17: // DeliverEvent(class, spec)
            ps1_bios_deliver_event(ctx, r[4], r[5]);
            r[2] = 0;
            return true;
        case 0x18: // OpenEvent(class, spec, mode, func)
            for (int i = 0; i < PS1_MAX_EVENTS; i++) {
                if (!ctx->bios.events[i].enabled && !ctx->bios.events[i].triggered) {
                    ctx->bios.events[i].class_id = r[4];
                    ctx->bios.events[i].spec = r[5];
                    ctx->bios.events[i].mode = r[6];
                    ctx->bios.events[i].func = r[7];
                    ctx->bios.events[i].enabled = true;
                    ctx->bios.events[i].triggered = false;
                    r[2] = i | 0xF0000000;
                    return true;
                }
            }
            r[2] = 0xFFFFFFFF;
            return true;
        case 0x19: // CloseEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = false;
                    r[2] = 1;
                }
            }
            return true;
        case 0x1A: // WaitEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    if (ctx->bios.events[ev_idx].triggered) {
                        ctx->bios.events[ev_idx].triggered = false;
                        r[2] = 1;
                    } else {
                        // Tick CD-ROM to advance pending operations
                        ps1_cdrom_tick(ctx, 10000);
                        if (ctx->bios.events[ev_idx].triggered) {
                            ctx->bios.events[ev_idx].triggered = false;
                            r[2] = 1;
                        } else {
                            r[2] = 0;
                        }
                    }
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x1B: // TestEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS && ctx->bios.events[ev_idx].triggered) {
                    ctx->bios.events[ev_idx].triggered = false;
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x1C: // EnableEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = true;
                    r[2] = 1;
                }
            }
            return true;
        case 0x1D: // DisableEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = false;
                    r[2] = 1;
                }
            }
            return true;
        case 0x20: // UnDeliverEvent(class, spec)
            for (int i = 0; i < PS1_MAX_EVENTS; i++) {
                if (ctx->bios.events[i].class_id == r[4] &&
                    (ctx->bios.events[i].spec == r[5] || r[5] == 0)) {
                    ctx->bios.events[i].triggered = false;
                }
            }
            r[2] = 0;
            return true;
        case 0x3F: // InitHeap(addr, size)
            ctx->bios.heap_base = r[4];
            ctx->bios.heap_size = r[5];
            ctx->bios.heap_cur = r[4];
            r[2] = ctx->bios.heap_base;
            return true;
        case 0x5B: // ChangeClearPAD(val)
            r[2] = 0;
            return true;
        default:
            return false;
    }
}

bool ps1_bios_handle_c0(PS1Context* ctx, uint32_t func) {
    ctx->cpu.r[2] = 0;
    return true;
}

bool ps1_bios_handle_syscall(PS1Context* ctx) {
    uint32_t call_code = ctx->cpu.r[4]; // $a0

    if (call_code == 1) {
        // EnterCriticalSection()
        ctx->cpu.r[2] = ctx->cpu.cop0[COP0_SR];
        ctx->cpu.cop0[COP0_SR] &= ~SR_IEc;
        return true;
    } else if (call_code == 2) {
        // ExitCriticalSection()
        ctx->cpu.cop0[COP0_SR] |= SR_IEc;
        return true;
    }
    return false;
}
