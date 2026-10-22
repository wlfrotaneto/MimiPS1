#include "mimips1_card.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint8_t calc_checksum(const uint8_t* frame) {
    uint8_t c = 0;
    for (int i = 0; i < 127; i++) {
        c ^= frame[i];
    }
    return c;
}

static void parse_card_directory(PS1MemoryCard* card) {
    memset(card->entries, 0, sizeof(card->entries));
    for (int i = 0; i < PS1_CARD_NUM_SLOTS; i++) {
        int frame_idx = 1 + i;
        const uint8_t* f = &card->data[frame_idx * PS1_CARD_FRAME_SIZE];
        
        uint32_t alloc = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
        card->entries[i].alloc_state = alloc;
        card->entries[i].file_size = (uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
        card->entries[i].next_block = (uint16_t)f[8] | ((uint16_t)f[9] << 8);
        card->entries[i].first_block = 1 + i;

        memcpy(card->entries[i].filename, &f[10], 64);
        card->entries[i].filename[64] = '\0';
    }
}

// Frames 16..62 of a standard card. Software validates them (checksums included), so a card image
// produced by an earlier version of this emulator is repaired on load.
static bool card_fix_reserved_frames(PS1MemoryCard* card) {
    bool changed = false;
    for (int frame_idx = 16; frame_idx < 63; frame_idx++) {
        uint8_t want[PS1_CARD_FRAME_SIZE];
        if (frame_idx < 36) {                       // broken sector list: no entries in use
            memset(want, 0, sizeof(want));
            memset(want, 0xFF, 4);
            want[8] = want[9] = 0xFF;
            want[127] = calc_checksum(want);
        } else if (frame_idx < 56) {                // replacement data
            memset(want, 0xFF, sizeof(want));
        } else {                                    // unused
            memset(want, 0, sizeof(want));
        }
        uint8_t* f = &card->data[frame_idx * PS1_CARD_FRAME_SIZE];
        // Only touch frames that still hold the layout of earlier versions of this emulator
        bool legacy = (f[0] == 0xFF && f[1] == 0x00) || (frame_idx < 36 && f[0] == 0xFF && f[8] == 0x00);
        if (memcmp(f, want, PS1_CARD_FRAME_SIZE) != 0 && legacy) {
            memcpy(f, want, PS1_CARD_FRAME_SIZE);
            changed = true;
        }
    }
    return changed;
}

void ps1_card_init(PS1CardManager* mgr) {
    memset(mgr, 0, sizeof(PS1CardManager));
}

void ps1_card_reset(PS1CardManager* mgr) {
    for (int s = 0; s < 2; s++) {
        if (mgr->cards[s].dirty && mgr->cards[s].loaded) {
            ps1_card_save(mgr, s);
        }
    }
    ps1_card_init(mgr);
}

void ps1_card_format(PS1CardManager* mgr, int slot) {
    if (slot < 0 || slot >= 2) return;
    PS1MemoryCard* card = &mgr->cards[slot];

    memset(card->data, 0, PS1_CARD_SIZE);

    // Frame 0: Header frame ('MC' magic + checksum 0x0E)
    card->data[0] = 'M';
    card->data[1] = 'C';
    card->data[127] = calc_checksum(&card->data[0]);

    // Frames 1..15: Directory frames
    for (int i = 0; i < PS1_CARD_NUM_SLOTS; i++) {
        int frame_idx = 1 + i;
        uint8_t* f = &card->data[frame_idx * PS1_CARD_FRAME_SIZE];
        f[0] = 0xA0; // Free / unallocated block
        f[8] = 0xFF; // No next block
        f[9] = 0xFF;
        f[127] = calc_checksum(f);
    }

    // Frames 16..35: broken sector list (every entry 0xFFFFFFFF = none); 36..62 replacement data / unused
    card_fix_reserved_frames(card);
    memcpy(&card->data[63 * PS1_CARD_FRAME_SIZE], &card->data[0], PS1_CARD_FRAME_SIZE); // write-test frame

    card->loaded = true;
    card->dirty = true;
    parse_card_directory(card);
    printf("[MemCard] Formatted standard 128KB PS1 Memory Card in Slot %d\n", slot);
}

bool ps1_card_load(PS1CardManager* mgr, int slot, const char* path) {
    if (slot < 0 || slot >= 2 || !path) return false;
    PS1MemoryCard* card = &mgr->cards[slot];

    strncpy(card->file_path, path, sizeof(card->file_path) - 1);
    FILE* f = fopen(path, "rb");
    if (!f) {
        // File doesn't exist yet: format a fresh card and save it
        ps1_card_format(mgr, slot);
        return ps1_card_save(mgr, slot);
    }

    size_t r = fread(card->data, 1, PS1_CARD_SIZE, f);
    fclose(f);

    if (r != PS1_CARD_SIZE || card->data[0] != 'M' || card->data[1] != 'C') {
        printf("[MemCard] Corrupt or unformatted card image at '%s', reformatting...\n", path);
        ps1_card_format(mgr, slot);
        ps1_card_save(mgr, slot);
    } else {
        card->loaded = true;
        card->dirty = card_fix_reserved_frames(card);
        parse_card_directory(card);
        if (card->dirty) ps1_card_save(mgr, slot);
        printf("[MemCard] Successfully loaded 128KB Memory Card from: %s\n", path);
    }

    return true;
}

bool ps1_card_save(PS1CardManager* mgr, int slot) {
    if (slot < 0 || slot >= 2) return false;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded || card->file_path[0] == '\0') return false;

    FILE* f = fopen(card->file_path, "wb");
    if (!f) {
        fprintf(stderr, "[MemCard Error] Failed to write Memory Card to '%s'\n", card->file_path);
        return false;
    }

    size_t w = fwrite(card->data, 1, PS1_CARD_SIZE, f);
    fclose(f);

    if (w == PS1_CARD_SIZE) {
        card->dirty = false;
        return true;
    }
    return false;
}

void ps1_card_auto_setup(PS1CardManager* mgr) {
    const char* home = getenv("HOME");
    if (!home) return;

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/Library/Application Support/MimiPS1", home);
    mkdir(dir_path, 0755);

    snprintf(dir_path, sizeof(dir_path), "%s/Library/Application Support/MimiPS1/memcards", home);
    mkdir(dir_path, 0755);

    char card1_path[512];
    char card2_path[512];
    snprintf(card1_path, sizeof(card1_path), "%s/card1.mcd", dir_path);
    snprintf(card2_path, sizeof(card2_path), "%s/card2.mcd", dir_path);

    ps1_card_load(mgr, 0, card1_path);
    ps1_card_load(mgr, 1, card2_path);
}

int ps1_card_find_file(PS1CardManager* mgr, int slot, const char* filename) {
    if (slot < 0 || slot >= 2 || !filename) return -1;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded) return -1;

    for (int i = 0; i < PS1_CARD_NUM_SLOTS; i++) {
        if ((card->entries[i].alloc_state & 0xF0) == 0x50) { // In use
            if (strncmp(card->entries[i].filename, filename, 64) == 0) {
                return i;
            }
        }
    }
    return -1;
}

int ps1_card_create_file(PS1CardManager* mgr, int slot, const char* filename, uint32_t size) {
    if (slot < 0 || slot >= 2 || !filename) return -1;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded) return -1;

    // Check if file already exists
    int existing = ps1_card_find_file(mgr, slot, filename);
    if (existing >= 0) return existing;

    // Find free slot
    for (int i = 0; i < PS1_CARD_NUM_SLOTS; i++) {
        if ((card->entries[i].alloc_state & 0xFF) == 0xA0) { // Free
            int frame_idx = 1 + i;
            uint8_t* f = &card->data[frame_idx * PS1_CARD_FRAME_SIZE];

            f[0] = 0x51; // Allocated first block
            f[1] = 0x00; f[2] = 0x00; f[3] = 0x00;
            f[4] = (uint8_t)(size & 0xFF);
            f[5] = (uint8_t)((size >> 8) & 0xFF);
            f[6] = (uint8_t)((size >> 16) & 0xFF);
            f[7] = (uint8_t)((size >> 24) & 0xFF);
            f[8] = 0xFF; f[9] = 0xFF;

            memset(&f[10], 0, 64);
            strncpy((char*)&f[10], filename, 63);
            f[127] = calc_checksum(f);

            card->dirty = true;
            parse_card_directory(card);
            ps1_card_save(mgr, slot);
            printf("[MemCard] Created save slot '%s' (%u bytes) at block %d\n", filename, size, 1 + i);
            return i;
        }
    }
    return -1; // Card full
}

int ps1_card_read_file(PS1CardManager* mgr, int slot, int dir_idx, uint32_t offset, void* dst, uint32_t len) {
    if (slot < 0 || slot >= 2 || dir_idx < 0 || dir_idx >= PS1_CARD_NUM_SLOTS || !dst) return -1;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded) return -1;

    int block = card->entries[dir_idx].first_block;
    uint32_t block_start = block * PS1_CARD_BLOCK_SIZE;

    if (offset >= PS1_CARD_BLOCK_SIZE) return 0;
    if (offset + len > PS1_CARD_BLOCK_SIZE) len = PS1_CARD_BLOCK_SIZE - offset;

    memcpy(dst, &card->data[block_start + offset], len);
    return (int)len;
}

int ps1_card_write_file(PS1CardManager* mgr, int slot, int dir_idx, uint32_t offset, const void* src, uint32_t len) {
    if (slot < 0 || slot >= 2 || dir_idx < 0 || dir_idx >= PS1_CARD_NUM_SLOTS || !src) return -1;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded) return -1;

    int block = card->entries[dir_idx].first_block;
    uint32_t block_start = block * PS1_CARD_BLOCK_SIZE;

    if (offset >= PS1_CARD_BLOCK_SIZE) return 0;
    if (offset + len > PS1_CARD_BLOCK_SIZE) len = PS1_CARD_BLOCK_SIZE - offset;

    memcpy(&card->data[block_start + offset], src, len);
    card->dirty = true;
    ps1_card_save(mgr, slot);
    return (int)len;
}

int ps1_card_delete_file(PS1CardManager* mgr, int slot, const char* filename) {
    int idx = ps1_card_find_file(mgr, slot, filename);
    if (idx < 0) return -1;

    PS1MemoryCard* card = &mgr->cards[slot];
    int frame_idx = 1 + idx;
    uint8_t* f = &card->data[frame_idx * PS1_CARD_FRAME_SIZE];

    f[0] = 0xA0; // Mark free
    memset(&f[10], 0, 64);
    f[127] = calc_checksum(f);

    card->dirty = true;
    parse_card_directory(card);
    ps1_card_save(mgr, slot);
    printf("[MemCard] Deleted save file '%s' from Slot %d\n", filename, slot);
    return 0;
}

bool ps1_card_read_frame(PS1CardManager* mgr, int slot, uint32_t frame_idx, uint8_t* dst) {
    if (slot < 0 || slot >= 2 || frame_idx >= PS1_CARD_NUM_FRAMES || !dst) return false;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded) return false;

    memcpy(dst, &card->data[frame_idx * PS1_CARD_FRAME_SIZE], PS1_CARD_FRAME_SIZE);
    return true;
}

bool ps1_card_write_frame(PS1CardManager* mgr, int slot, uint32_t frame_idx, const uint8_t* src) {
    if (slot < 0 || slot >= 2 || frame_idx >= PS1_CARD_NUM_FRAMES || !src) return false;
    PS1MemoryCard* card = &mgr->cards[slot];
    if (!card->loaded) return false;

    memcpy(&card->data[frame_idx * PS1_CARD_FRAME_SIZE], src, PS1_CARD_FRAME_SIZE);
    card->dirty = true;
    ps1_card_save(mgr, slot);
    return true;
}
