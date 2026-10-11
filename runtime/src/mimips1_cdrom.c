#include "mimips1_cdrom.h"
#include "mimips1_runtime.h"
#include <string.h>

void ps1_cdrom_init(PS1CDROM* cdrom) {
    memset(cdrom, 0, sizeof(PS1CDROM));
    cdrom->status = CD_STAT_SPINDLE_ON;
}

void ps1_cdrom_reset(PS1CDROM* cdrom) {
    cdrom->status = CD_STAT_SPINDLE_ON;
    cdrom->reading = false;
    cdrom->seeking = false;
    cdrom->param_count = 0;
    cdrom->response_count = 0;
}

bool ps1_cdrom_open_image(PS1CDROM* cdrom, const char* path) {
    if (cdrom->image_file) {
        fclose(cdrom->image_file);
    }
    cdrom->image_file = fopen(path, "rb");
    if (!cdrom->image_file) return false;

    strncpy(cdrom->image_path, path, sizeof(cdrom->image_path) - 1);
    fseek(cdrom->image_file, 0, SEEK_END);
    long size = ftell(cdrom->image_file);
    fseek(cdrom->image_file, 0, SEEK_SET);

    cdrom->total_sectors = (uint32_t)(size / CDROM_SECTOR_SIZE_DATA);
    return true;
}

void ps1_cdrom_close_image(PS1CDROM* cdrom) {
    if (cdrom->image_file) {
        fclose(cdrom->image_file);
        cdrom->image_file = NULL;
    }
}

static void push_response(PS1CDROM* cdrom, uint8_t val) {
    if (cdrom->response_count < 16) {
        cdrom->response_fifo[cdrom->response_count++] = val;
    }
}

static void handle_cdrom_command(PS1Context* ctx, uint8_t cmd) {
    PS1CDROM* cd = &ctx->cdrom;
    cd->response_count = 0;
    cd->response_read_pos = 0;

    switch (cmd) {
        case 0x01: // Getstat
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x02: // Setloc (M, S, F)
            if (cd->param_count >= 3) {
                cd->target_m = cd->param_fifo[0];
                cd->target_s = cd->param_fifo[1];
                cd->target_f = cd->param_fifo[2];
                // BCD to LBA conversion
                uint32_t m = ((cd->target_m >> 4) * 10) + (cd->target_m & 0xF);
                uint32_t s = ((cd->target_s >> 4) * 10) + (cd->target_s & 0xF);
                uint32_t f = ((cd->target_f >> 4) * 10) + (cd->target_f & 0xF);
                cd->current_lba = (m * 60 * 75) + (s * 75) + f;
                if (cd->current_lba >= 150) cd->current_lba -= 150; // 2-second pregap
            }
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x06: // ReadN
        case 0x1B: // ReadS
            cd->status |= CD_STAT_READ_BIT;
            cd->reading = true;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x09: // Pause
            cd->reading = false;
            cd->status &= ~CD_STAT_READ_BIT;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x0A: // Init
            cd->reading = false;
            cd->status = CD_STAT_SPINDLE_ON;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x0E: // Setmode
            if (cd->param_count > 0) cd->mode = cd->param_fifo[0];
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x15: // SeekL
        case 0x16: // SeekP
            cd->seeking = true;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x19: // Test
            push_response(cd, 0x94); // PSX CD-ROM version
            push_response(cd, 0x09);
            push_response(cd, 0x19);
            push_response(cd, 0xC0);
            cd->irq_flag = 3;
            break;
        case 0x1A: // GetID
            push_response(cd, cd->status);
            push_response(cd, 0x00);
            push_response(cd, 0x20); // Licensed PlayStation Disk
            push_response(cd, 0x00);
            push_response(cd, 'S');
            push_response(cd, 'C');
            push_response(cd, 'E');
            push_response(cd, 'A');
            cd->irq_flag = 3;
            break;
        default:
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
    }
    cd->param_count = 0;
    ps1_trigger_irq(ctx, IRQ_CDROM);
}

void ps1_cdrom_tick(PS1Context* ctx, uint32_t cycles) {
    PS1CDROM* cd = &ctx->cdrom;
    if (cd->reading && cd->image_file) {
        // Read sector
        long offset = (long)cd->current_lba * CDROM_SECTOR_SIZE_DATA;
        fseek(cd->image_file, offset, SEEK_SET);
        size_t r = fread(cd->data_fifo, 1, CDROM_SECTOR_SIZE_DATA, cd->image_file);
        if (r > 0) {
            cd->data_bytes_avail = (int)r;
            cd->data_read_pos = 0;
            cd->current_lba++;
            cd->irq_flag = 1; // Sector data ready IRQ
            ps1_trigger_irq(ctx, IRQ_CDROM);
        }
    }
}

uint8_t ps1_cdrom_read(PS1Context* ctx, uint32_t addr) {
    PS1CDROM* cd = &ctx->cdrom;
    uint32_t reg = addr & 3;

    switch (reg) {
        case 0: // Status Register
            return cd->reg_index | (cd->param_count == 0 ? (1 << 3) : 0) |
                   (cd->param_count < 16 ? (1 << 4) : 0) |
                   (cd->response_count > cd->response_read_pos ? (1 << 5) : 0) |
                   (cd->data_bytes_avail > 0 ? (1 << 6) : 0);
        case 1: // Response FIFO
            if (cd->response_read_pos < cd->response_count) {
                return cd->response_fifo[cd->response_read_pos++];
            }
            return 0;
        case 2: // Data FIFO byte read
            if (cd->data_bytes_avail > 0) {
                cd->data_bytes_avail--;
                return cd->data_fifo[cd->data_read_pos++];
            }
            return 0;
        case 3: // Interrupt Flag
            return 0xE0 | (cd->irq_flag & 0x1F);
    }
    return 0;
}

void ps1_cdrom_write(PS1Context* ctx, uint32_t addr, uint8_t val) {
    PS1CDROM* cd = &ctx->cdrom;
    uint32_t reg = addr & 3;

    switch (reg) {
        case 0:
            cd->reg_index = val & 3;
            break;
        case 1:
            if (cd->reg_index == 0) {
                handle_cdrom_command(ctx, val);
            } else if (cd->reg_index == 3) {
                // Audio volume setting
            }
            break;
        case 2:
            if (cd->reg_index == 0 && cd->param_count < 16) {
                cd->param_fifo[cd->param_count++] = val;
            }
            break;
        case 3:
            if (cd->reg_index == 0) {
                cd->irq_enable = val & 0x1F;
            } else if (cd->reg_index == 1) {
                // Acknowledge IRQ bits
                cd->irq_flag &= ~(val & 0x1F);
            }
            break;
    }
}
