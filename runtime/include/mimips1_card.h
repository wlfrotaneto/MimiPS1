#ifndef MIMIPS1_CARD_H
#define MIMIPS1_CARD_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS1_CARD_SIZE         131072 // 128 KB
#define PS1_CARD_FRAME_SIZE   128    // 128 bytes per frame
#define PS1_CARD_NUM_FRAMES   1024   // 1024 frames total
#define PS1_CARD_BLOCK_SIZE   8192   // 8 KB per save block (64 frames)
#define PS1_CARD_NUM_SLOTS    15     // 15 usable save blocks (1..15)

// Memory card directory entry
typedef struct {
    uint32_t alloc_state; // 0xA0 = Free, 0x51 = Used (first), 0x52 = Mid, 0x53 = End
    uint32_t file_size;   // Size in bytes
    uint16_t next_block;  // 0xFFFF if last/only block
    char     filename[65];// 64-char null-terminated filename
    int      first_block; // Physical block (1..15)
} PS1CardDirEntry;

typedef struct {
    char            file_path[512];
    uint8_t         data[PS1_CARD_SIZE];
    bool            loaded;
    bool            dirty;
    PS1CardDirEntry entries[PS1_CARD_NUM_SLOTS];
} PS1MemoryCard;

typedef struct {
    PS1MemoryCard cards[2]; // Slot 0: bu00:, Slot 1: bu10:
} PS1CardManager;

void ps1_card_init(PS1CardManager* mgr);
void ps1_card_reset(PS1CardManager* mgr);
bool ps1_card_load(PS1CardManager* mgr, int slot, const char* path);
bool ps1_card_save(PS1CardManager* mgr, int slot);
void ps1_card_format(PS1CardManager* mgr, int slot);
void ps1_card_auto_setup(PS1CardManager* mgr);

// High-level file operations
int  ps1_card_find_file(PS1CardManager* mgr, int slot, const char* filename);
int  ps1_card_create_file(PS1CardManager* mgr, int slot, const char* filename, uint32_t size);
int  ps1_card_read_file(PS1CardManager* mgr, int slot, int dir_idx, uint32_t offset, void* dst, uint32_t len);
int  ps1_card_write_file(PS1CardManager* mgr, int slot, int dir_idx, uint32_t offset, const void* src, uint32_t len);
int  ps1_card_delete_file(PS1CardManager* mgr, int slot, const char* filename);

// Low-level frame access (128 bytes per frame)
bool ps1_card_read_frame(PS1CardManager* mgr, int slot, uint32_t frame_idx, uint8_t* dst);
bool ps1_card_write_frame(PS1CardManager* mgr, int slot, uint32_t frame_idx, const uint8_t* src);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_CARD_H
