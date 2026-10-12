#ifndef MIMIPS1_CDROM_H
#define MIMIPS1_CDROM_H

#include "mimips1_types.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDROM_SECTOR_SIZE_RAW  2352
#define CDROM_SECTOR_SIZE_DATA 2048

typedef enum {
    CD_STAT_SPINDLE_ON = (1 << 1),
    CD_STAT_SEEK_ERROR = (1 << 2),
    CD_STAT_ID_ERROR   = (1 << 3),
    CD_STAT_SHELL_OPEN = (1 << 4),
    CD_STAT_READ_BIT   = (1 << 5),
    CD_STAT_SEEK_BIT   = (1 << 6),
    CD_STAT_PLAY_BIT   = (1 << 7)
} CDStatusBits;

struct PS1CDROM {
    FILE*    image_file;
    char     image_path[512];
    uint32_t total_sectors;
    bool     is_raw;
    uint32_t sector_size;
    uint32_t data_offset;

    uint8_t  status;
    uint8_t  mode;
    uint8_t  reg_index;
    
    // Command FIFO
    uint8_t  param_fifo[16];
    int      param_count;
    
    // Response FIFO
    uint8_t  response_fifo[16];
    int      response_count;
    int      response_read_pos;

    // Data FIFO (Sector data to DMA 3 or CPU)
    uint8_t  data_fifo[CDROM_SECTOR_SIZE_RAW];
    int      data_read_pos;
    int      data_bytes_avail;

    // Seek / Location target (Minute, Second, Sector)
    uint8_t  target_m, target_s, target_f;
    uint32_t current_lba;
    bool     reading;
    bool     seeking;

    uint8_t  irq_flag;
    uint8_t  irq_enable;
};

void     ps1_cdrom_init(PS1CDROM* cdrom);
void     ps1_cdrom_reset(PS1CDROM* cdrom);
bool     ps1_cdrom_open_image(PS1CDROM* cdrom, const char* path);
void     ps1_cdrom_close_image(PS1CDROM* cdrom);

// ISO9660 filesystem functions
typedef struct {
    uint32_t lba;
    uint32_t size;
    bool     is_dir;
    char     name[64];
} PS1ISOFile;

bool     ps1_cdrom_find_file(PS1CDROM* cdrom, const char* filename, PS1ISOFile* out_file);
bool     ps1_cdrom_read_bytes(PS1CDROM* cdrom, uint32_t lba, uint32_t offset_in_file, void* dst, uint32_t size);

uint8_t  ps1_cdrom_read(PS1Context* ctx, uint32_t addr);
void     ps1_cdrom_write(PS1Context* ctx, uint32_t addr, uint8_t val);
void     ps1_cdrom_tick(PS1Context* ctx, uint32_t cycles);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_CDROM_H
