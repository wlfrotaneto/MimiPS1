#ifndef MIMIPS1_BIOS_H
#define MIMIPS1_BIOS_H

#include "mimips1_types.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Event structure for PS1 event system
typedef struct {
    uint32_t class_id;
    uint32_t spec;
    uint32_t mode;
    uint32_t func;
    bool     enabled;
    bool     triggered;
} PS1Event;

#define PS1_MAX_EVENTS 32

typedef struct {
    bool     is_open;
    uint32_t start_lba;
    uint32_t file_size;
    uint32_t cur_offset;
    char     name[64];
} PS1OpenFile;

struct PS1BIOS {
    bool        hle_enabled;
    PS1Event    events[PS1_MAX_EVENTS];
    uint32_t    heap_base;
    uint32_t    heap_size;
    uint32_t    heap_cur;
    
    // File descriptors (for cdrom: and bu00:)
    PS1OpenFile files[16];
};

void ps1_bios_init(PS1Context* ctx);
void ps1_bios_reset(PS1Context* ctx);

// Dispatch A0, B0, C0 and Syscall vectors
bool ps1_bios_handle_syscall(PS1Context* ctx);
bool ps1_bios_handle_a0(PS1Context* ctx, uint32_t func);
bool ps1_bios_handle_b0(PS1Context* ctx, uint32_t func);
bool ps1_bios_handle_c0(PS1Context* ctx, uint32_t func);
void ps1_bios_deliver_event(PS1Context* ctx, uint32_t class_id, uint32_t spec);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_BIOS_H
