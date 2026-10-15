#include "mimips1_cdrom.h"
#include "mimips1_runtime.h"
#include <string.h>

void ps1_cdrom_init(PS1CDROM* cdrom) {
    memset(cdrom, 0, sizeof(PS1CDROM));
    cdrom->status = CD_STAT_SPINDLE_ON;
    cdrom->sector_size = CDROM_SECTOR_SIZE_DATA;
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

    // Check if RAW sector image (sync pattern 00 FF FF ... 00)
    uint8_t header[16];
    size_t hr = fread(header, 1, 16, cdrom->image_file);
    const uint8_t sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (hr >= 16 && memcmp(header, sync, 12) == 0) {
        cdrom->is_raw = true;
        cdrom->sector_size = CDROM_SECTOR_SIZE_RAW; // 2352
        cdrom->data_offset = 24; // Mode 2 Form 1 data starts at byte 24
    } else {
        cdrom->is_raw = false;
        cdrom->sector_size = CDROM_SECTOR_SIZE_DATA; // 2048
        cdrom->data_offset = 0;
    }

    fseek(cdrom->image_file, 0, SEEK_END);
    long size = ftell(cdrom->image_file);
    fseek(cdrom->image_file, 0, SEEK_SET);

    cdrom->total_sectors = (uint32_t)(size / cdrom->sector_size);
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
    PS1_LOG(2, "[CDROM] Command 0x%02X (params=%d)\n", cmd, cd->param_count);
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
                cd->last_read_lba = cd->current_lba;
                PS1_LOG(2, "[CDROM] Setloc %02X:%02X:%02X (%u:%u:%u) -> LBA %u\n",
                       cd->target_m, cd->target_s, cd->target_f, m, s, f, cd->current_lba);
            }
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x03: // Play
            cd->reading = false;
            cd->playing = true;
            cd->status |= CD_STAT_PLAY_BIT;
            cd->status &= ~CD_STAT_READ_BIT;
            cd->cdda_sample_offset = 0;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x06: // ReadN
        case 0x1B: // ReadS
            cd->status &= ~CD_STAT_SEEK_BIT;
            cd->status |= CD_STAT_READ_BIT;
            cd->seeking = false;
            cd->pending_int2 = false;
            cd->reading = true;
            cd->read_cycles = 0;
            PS1_LOG(2, "[CDROM] ReadN started at LBA %u (mode=0x%02X)\n", cd->current_lba, cd->mode);
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x08: // Stop
            cd->playing = false;
            cd->reading = false;
            cd->read_cycles = 0;
            cd->status &= ~(CD_STAT_READ_BIT | CD_STAT_PLAY_BIT);
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            cd->pending_int2 = true;
            cd->int2_delay_cycles = 1000;
            break;
        case 0x09: // Pause
            cd->reading = false;
            cd->playing = false;
            cd->read_cycles = 0;
            cd->status &= ~(CD_STAT_READ_BIT | CD_STAT_PLAY_BIT);
            PS1_LOG(2, "[CDROM] Pause at LBA %u\n", cd->current_lba);
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            cd->pending_int2 = true;
            cd->int2_delay_cycles = 1000;
            break;
        case 0x0A: // Init
            cd->reading = false;
            cd->playing = false;
            cd->read_cycles = 0;
            cd->status = CD_STAT_SPINDLE_ON;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            cd->pending_int2 = true;
            cd->int2_delay_cycles = 1000;
            break;
        case 0x0B: // Mute
            cd->muted = true;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x0C: // Demute
            cd->muted = false;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x0E: // Setmode
            if (cd->param_count > 0) cd->mode = cd->param_fifo[0];
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            break;
        case 0x10: // GetlocL (4 bytes header + 4 bytes subheader)
            if (cd->image_file && cd->is_raw) {
                uint8_t hdr[8] = {0};
                uint32_t lba_idx = cd->last_read_lba;
                long off = (long)lba_idx * cd->sector_size + 12;
                fseek(cd->image_file, off, SEEK_SET);
                size_t r = fread(hdr, 1, 8, cd->image_file);
                (void)r;
                for (int i = 0; i < 8; i++) push_response(cd, hdr[i]);
            } else {
                uint32_t lba = cd->last_read_lba + 150;
                uint8_t m = (uint8_t)(lba / (60 * 75));
                uint8_t s = (uint8_t)((lba % (60 * 75)) / 75);
                uint8_t f = (uint8_t)(lba % 75);
                push_response(cd, ((m / 10) << 4) | (m % 10));
                push_response(cd, ((s / 10) << 4) | (s % 10));
                push_response(cd, ((f / 10) << 4) | (f % 10));
                push_response(cd, 0x02); // Mode 2 Form 1
                for (int i = 0; i < 4; i++) push_response(cd, 0x00);
            }
            cd->irq_flag = 3;
            break;
        case 0x11: // GetlocP (Track, Index, Relative M/S/F, Absolute M/S/F)
            {
                uint32_t rel_lba = cd->last_read_lba;
                uint32_t abs_lba = rel_lba + 150;
                uint8_t rm = (uint8_t)(rel_lba / (60 * 75));
                uint8_t rs = (uint8_t)((rel_lba % (60 * 75)) / 75);
                uint8_t rf = (uint8_t)(rel_lba % 75);
                uint8_t am = (uint8_t)(abs_lba / (60 * 75));
                uint8_t as = (uint8_t)((abs_lba % (60 * 75)) / 75);
                uint8_t af = (uint8_t)(abs_lba % 75);
                push_response(cd, 0x01); // Track 1
                push_response(cd, 0x01); // Index 1
                push_response(cd, ((rm / 10) << 4) | (rm % 10));
                push_response(cd, ((rs / 10) << 4) | (rs % 10));
                push_response(cd, ((rf / 10) << 4) | (rf % 10));
                push_response(cd, ((am / 10) << 4) | (am % 10));
                push_response(cd, ((as / 10) << 4) | (as % 10));
                push_response(cd, ((af / 10) << 4) | (af % 10));
                cd->irq_flag = 3;
            }
            break;
        case 0x15: // SeekL
        case 0x16: // SeekP
            cd->seeking = true;
            cd->last_read_lba = cd->current_lba;
            cd->status |= CD_STAT_SEEK_BIT;
            push_response(cd, cd->status);
            cd->irq_flag = 3;
            cd->pending_int2 = true;
            cd->int2_delay_cycles = 1000;
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

    // Handle asynchronous completion INT2 for commands like Pause, Stop, Seek, Init
    if (cd->pending_int2) {
        if (cd->irq_flag == 0) { // Only deliver after INT3 has been acknowledged
            if (cd->int2_delay_cycles > cycles) {
                cd->int2_delay_cycles -= cycles;
            } else {
                cd->int2_delay_cycles = 0;
                cd->pending_int2 = false;
                cd->seeking = false;
                cd->status &= ~CD_STAT_SEEK_BIT;
                cd->response_count = 0;
                cd->response_read_pos = 0;
                push_response(cd, cd->status);
                cd->irq_flag = 2; // INT2: Complete
                ps1_trigger_irq(ctx, IRQ_CDROM);
            }
        }
    }

    if (cd->reading && cd->image_file) {
        // Do not interrupt while previous IRQ (e.g. INT3 acknowledge) is still unhandled
        if (cd->irq_flag != 0) {
            return;
        }

        // If sector was partially read by DMA/CPU, don't overwrite until finished
        if (cd->data_bytes_avail > 0 && cd->data_read_pos > 0) {
            return;
        }

        cd->read_cycles += cycles;
        // Pacing for static recompilation
        uint32_t sector_period = (cd->mode & 0x80) ? 16384 : 32768;

        if (cd->read_cycles >= sector_period) {
            cd->read_cycles -= sector_period;

            // Read sector (support subheader mode 0x20)
            bool subheader_mode = (cd->mode & 0x20) != 0;
            uint32_t sec_offset = (cd->is_raw ? (subheader_mode ? 12 : 24) : 0);
            uint32_t sec_size = subheader_mode ? (cd->is_raw ? 2340 : 2048) : CDROM_SECTOR_SIZE_DATA;
            long offset = (long)cd->current_lba * cd->sector_size + sec_offset;
            fseek(cd->image_file, offset, SEEK_SET);
            size_t r = fread(cd->data_fifo, 1, sec_size, cd->image_file);
            if (r > 0) {
                cd->data_bytes_avail = (int)r;
                cd->data_read_pos = 0;
                cd->last_read_lba = cd->current_lba;
                cd->current_lba++;
                cd->response_count = 0;
                cd->response_read_pos = 0;
                push_response(cd, cd->status);
                cd->irq_flag = 1; // Sector data ready IRQ (INT1)
                PS1_LOG(2, "[CDROM] Sector Delivered: LBA %u (avail=%d, mode=0x%02X)\n", cd->last_read_lba, cd->data_bytes_avail, cd->mode);
                ps1_trigger_irq(ctx, IRQ_CDROM);
            }
        }
    }
}

uint8_t ps1_cdrom_read(PS1Context* ctx, uint32_t addr) {
    PS1CDROM* cd = &ctx->cdrom;
    uint32_t reg = addr & 3;
    uint8_t ret = 0;

    switch (reg) {
        case 0: // Status Register
            ret = cd->reg_index | (cd->param_count == 0 ? (1 << 3) : 0) |
                   (cd->param_count < 16 ? (1 << 4) : 0) |
                   (cd->response_count > cd->response_read_pos ? (1 << 5) : 0) |
                   (cd->data_bytes_avail > 0 ? (1 << 6) : 0);
            break;
        case 1: // Response FIFO
            if (cd->response_read_pos < cd->response_count) {
                ret = cd->response_fifo[cd->response_read_pos++];
            }
            break;
        case 2: // Data FIFO byte read
            if (cd->data_bytes_avail > 0) {
                cd->data_bytes_avail--;
                ret = cd->data_fifo[cd->data_read_pos++];
            }
            break;
        case 3: // 0x1F801803: Index 0/2 = Interrupt Enable, Index 1/3 = Interrupt Flag
            if (cd->reg_index & 1) {
                ret = 0xE0 | (cd->irq_flag & 0x1F);
            } else {
                ret = 0xE0 | (cd->irq_enable & 0x1F);
            }
            break;
    }
    return ret;
}

void ps1_cdrom_write(PS1Context* ctx, uint32_t addr, uint8_t val) {
    PS1CDROM* cd = &ctx->cdrom;
    uint32_t reg = addr & 3;
    PS1_LOG(2, "[CD-W] reg=%d val=0x%02X (idx=%d)\n", reg, val, cd->reg_index);

    switch (reg) {
        case 0:
            cd->reg_index = val & 3;
            break;
        case 1:
            if (cd->reg_index == 0) {
                if (val == 0x02) {
                    PS1_LOG(2, "[CDROM Setloc Caller] PC=0x%08X, RA=0x%08X, SP=0x%08X, A0=0x%08X, A1=0x%08X\n",
                           ctx->cpu.pc, ctx->cpu.r[31], ctx->cpu.r[29], ctx->cpu.r[4], ctx->cpu.r[5]);
                }
                handle_cdrom_command(ctx, val);
            } else if (cd->reg_index == 3) {
                // Audio volume setting
            }
            break;
        case 2:
            if (cd->reg_index == 0 && cd->param_count < 16) {
                cd->param_fifo[cd->param_count++] = val;
            } else if (cd->reg_index == 1 || cd->reg_index == 3) {
                // Interrupt Enable Register (0x1F801802.1 / 0x1F801802.3)
                cd->irq_enable = val & 0x1F;
            }
            break;
        case 3:
            if (cd->reg_index == 0) {
                // Request register (0x1F801803.0): Bit 7 = BFRD (Want Data)
                if (val & 0x80) {
                    if (cd->data_bytes_avail == 0) {
                        cd->data_read_pos = 0;
                        cd->data_bytes_avail = (cd->mode & 0x20) ? (cd->is_raw ? 2340 : 2048) : CDROM_SECTOR_SIZE_DATA;
                    }
                } else {
                    // BFRD=0 resets data buffer read state
                    cd->data_bytes_avail = 0;
                    cd->data_read_pos = 0;
                }
            } else if (cd->reg_index == 1 || cd->reg_index == 3) {
                // Acknowledge IRQ bits (0x1F801803.1 / 0x1F801803.3)
                cd->irq_flag &= ~(val & 0x1F);
                if (val & 0x40) {
                    cd->param_count = 0; // Reset parameter FIFO
                }
            }
            break;
    }
}

// -------------------------------------------------------------
// ISO9660 Filesystem Implementation for CD-ROM Images
// -------------------------------------------------------------

static bool read_iso_sector(PS1CDROM* cdrom, uint32_t lba, uint8_t* buf) {
    if (!cdrom || !cdrom->image_file) return false;
    long offset = (long)lba * cdrom->sector_size + cdrom->data_offset;
    if (fseek(cdrom->image_file, offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, 2048, cdrom->image_file) == 2048;
}

static bool iso_name_match(const char* pattern, const char* entry, int entry_len) {
    char clean_entry[64];
    int len = entry_len < 63 ? entry_len : 63;
    memcpy(clean_entry, entry, len);
    clean_entry[len] = '\0';
    char* semi = strchr(clean_entry, ';');
    if (semi) *semi = '\0';

    // Strip leading path prefix from pattern (e.g. "cdrom:\", "\", etc.)
    const char* p = pattern;
    if (strncasecmp(p, "cdrom:", 6) == 0) p += 6;
    while (*p == '\\' || *p == '/') p++;

    // Strip trailing version from pattern if present
    char clean_pat[64];
    strncpy(clean_pat, p, 63);
    clean_pat[63] = '\0';
    char* pat_semi = strchr(clean_pat, ';');
    if (pat_semi) *pat_semi = '\0';

    return strcasecmp(clean_pat, clean_entry) == 0;
}

bool ps1_cdrom_find_file(PS1CDROM* cdrom, const char* filename, PS1ISOFile* out_file) {
    if (!cdrom || !cdrom->image_file || !filename || !out_file) return false;

    // Read Primary Volume Descriptor (PVD) at LBA 16
    uint8_t pvd[2048];
    if (!read_iso_sector(cdrom, 16, pvd)) return false;

    if (memcmp(&pvd[1], "CD001", 5) != 0) return false;

    const uint8_t* root_rec = &pvd[156];
    uint32_t root_lba = *(const uint32_t*)&root_rec[2];
    uint32_t root_size = *(const uint32_t*)&root_rec[10];

    uint32_t sectors = (root_size + 2047) / 2048;
    uint8_t sec_buf[2048];

    for (uint32_t s = 0; s < sectors; s++) {
        if (!read_iso_sector(cdrom, root_lba + s, sec_buf)) break;

        uint32_t pos = 0;
        while (pos < 2048) {
            uint8_t rec_len = sec_buf[pos];
            if (rec_len == 0) break;

            uint32_t file_lba = *(const uint32_t*)&sec_buf[pos + 2];
            uint32_t file_size = *(const uint32_t*)&sec_buf[pos + 10];
            uint8_t flags = sec_buf[pos + 25];
            uint8_t name_len = sec_buf[pos + 32];
            const char* name_ptr = (const char*)&sec_buf[pos + 33];

            if (name_len > 0 && iso_name_match(filename, name_ptr, name_len)) {
                out_file->lba = file_lba;
                out_file->size = file_size;
                out_file->is_dir = (flags & 2) != 0;
                int copy_len = name_len < 63 ? name_len : 63;
                memcpy(out_file->name, name_ptr, copy_len);
                out_file->name[copy_len] = '\0';
                char* semi = strchr(out_file->name, ';');
                if (semi) *semi = '\0';
                return true;
            }

            pos += rec_len;
        }
    }

    return false;
}

bool ps1_cdrom_read_bytes(PS1CDROM* cdrom, uint32_t lba, uint32_t offset_in_file, void* dst, uint32_t size) {
    if (!cdrom || !cdrom->image_file || !dst || size == 0) return false;

    uint32_t start_sec = lba + (offset_in_file / 2048);
    uint32_t sec_off = offset_in_file % 2048;
    uint8_t* out = (uint8_t*)dst;
    uint32_t remaining = size;

    uint8_t sec_buf[2048];
    while (remaining > 0) {
        if (!read_iso_sector(cdrom, start_sec, sec_buf)) return false;
        uint32_t to_copy = 2048 - sec_off;
        if (to_copy > remaining) to_copy = remaining;
        memcpy(out, sec_buf + sec_off, to_copy);
        out += to_copy;
        remaining -= to_copy;
        start_sec++;
        sec_off = 0;
    }

    return true;
}

int ps1_cdrom_read_cdda_samples(PS1CDROM* cdrom, int16_t* buffer_stereo, int num_samples) {
    if (!cdrom || !cdrom->image_file || !cdrom->playing || num_samples <= 0) {
        return 0;
    }

    if (cdrom->muted) {
        memset(buffer_stereo, 0, num_samples * 2 * sizeof(int16_t));
        return num_samples;
    }

    int samples_read = 0;
    while (samples_read < num_samples) {
        int samples_in_sector = 588; // 2352 bytes / 4 bytes per stereo sample
        int remaining_in_sector = samples_in_sector - (int)cdrom->cdda_sample_offset;
        int needed = num_samples - samples_read;
        int to_copy = needed < remaining_in_sector ? needed : remaining_in_sector;

        long sector_byte_offset = (long)cdrom->current_lba * cdrom->sector_size;
        long sample_offset = sector_byte_offset + ((long)cdrom->cdda_sample_offset * 4);

        if (fseek(cdrom->image_file, sample_offset, SEEK_SET) != 0) break;
        size_t r = fread(&buffer_stereo[samples_read * 2], sizeof(int16_t) * 2, to_copy, cdrom->image_file);
        if (r == 0) {
            cdrom->playing = false;
            cdrom->status &= ~CD_STAT_PLAY_BIT;
            break;
        }

        samples_read += (int)r;
        cdrom->cdda_sample_offset += (uint32_t)r;
        if (cdrom->cdda_sample_offset >= 588) {
            cdrom->cdda_sample_offset = 0;
            cdrom->current_lba++;
            if (cdrom->total_sectors > 0 && cdrom->current_lba >= cdrom->total_sectors) {
                cdrom->playing = false;
                cdrom->status &= ~CD_STAT_PLAY_BIT;
                break;
            }
        }
    }
    return samples_read;
}
