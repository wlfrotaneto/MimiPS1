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
    bool     is_open;
    bool     enabled;
    bool     triggered;
} PS1Event;

#define PS1_MAX_EVENTS 64

typedef struct {
    bool     is_open;
    bool     is_card;
    int      card_slot;     // 0 = bu00:, 1 = bu10:
    int      card_file_idx; // Slot index (0..14)
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
    bool        heap_ready;     // Heap block list written into guest RAM
    
    // File descriptors (for cdrom: and bu00:)
    PS1OpenFile files[16];

    // BIOS Pad buffers (InitPAD)
    uint32_t    pad_buf[2];
    uint32_t    pad_len[2];
    uint32_t    pad_button_dest;
    bool        pad_active;

    // Psy-Q / Hardware IRQ callback table (registered via A0:72)
    uint32_t    irq_callback_table;

    // HookEntryInt (B0:19) custom exception handler buffer address
    uint32_t    hook_entry_int_addr;

    // SysEnqIntRP queues (registered via C0:02, priorities 0..3)
    uint32_t    intrp_heads[4];
};

void ps1_bios_init(PS1Context* ctx);
void ps1_bios_reset(PS1Context* ctx);

// Dispatch A0, B0, C0 and Syscall vectors
bool ps1_bios_handle_syscall(PS1Context* ctx);
bool ps1_bios_handle_a0(PS1Context* ctx, uint32_t func);
bool ps1_bios_handle_b0(PS1Context* ctx, uint32_t func);
bool ps1_bios_handle_c0(PS1Context* ctx, uint32_t func);
void ps1_bios_deliver_event(PS1Context* ctx, uint32_t class_id, uint32_t spec);
void ps1_bios_update_pads(PS1Context* ctx);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_BIOS_H
