#include "mimips1_cdrom.h"
#include "mimips1_runtime.h"
#include <string.h>
#include <stdlib.h>

void ps1_cdrom_init(PS1CDROM* cdrom) {
    memset(cdrom, 0, sizeof(PS1CDROM));
    cdrom->status = CD_STAT_SPINDLE_ON;
    cdrom->sector_size = CDROM_SECTOR_SIZE_DATA;
    cdrom->irq_enable = 0x1F; // State the real BIOS leaves behind; games rely on it
}

void ps1_cdrom_reset(PS1CDROM* cdrom) {
    cdrom->status = CD_STAT_SPINDLE_ON;
    cdrom->reading = false;
    cdrom->seeking = false;
    cdrom->param_count = 0;
    cdrom->response_count = 0;
    cdrom->response_read_pos = 0;
    cdrom->irq_flag = 0;
    cdrom->irq_enable = 0x1F;
    cdrom->busy = false;
    cdrom->setloc_pending = false;
    cdrom->sector_len = 0;
    cdrom->data_bytes_avail = 0;
    cdrom->ev[0].valid = cdrom->ev[1].valid = false;
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

// -------------------------------------------------------------
// Controller timing (CPU cycles @ 33.8688 MHz)
// -------------------------------------------------------------
#define CD_DELAY_FAST        24000u     // Typical first response (INT3)
#define CD_DELAY_INIT        80000u
#define CD_SECTOR_1X         451584u    // 75 sectors/s  (33868800 / 75)
#define CD_SECTOR_2X         225792u    // 150 sectors/s (33868800 / 150)
#define CD_SEEK_BASE         50000u
#define CD_SEEK_MAX          3400000u

// Optional load-time speedup for development (MIMIPS1_CD_SPEEDUP=N, default 1 = real speed)
static uint32_t cd_speedup(void) {
    static int v = 0;
    if (!v) {
        const char* e = getenv("MIMIPS1_CD_SPEEDUP");
        v = (e && atoi(e) > 0) ? atoi(e) : 1;
    }
    return (uint32_t)v;
}

static inline uint32_t cd_sector_period(const PS1CDROM* cd) {
    return ((cd->mode & 0x80) ? CD_SECTOR_2X : CD_SECTOR_1X) / cd_speedup();
}

static uint32_t cd_seek_delay(const PS1CDROM* cd, uint32_t target) {
    uint32_t dist = target > cd->current_lba ? target - cd->current_lba : cd->current_lba - target;
    if (dist == 0) return 0;
    uint64_t d = CD_SEEK_BASE + (uint64_t)dist * 60u;
    return (uint32_t)(d > CD_SEEK_MAX ? CD_SEEK_MAX : d) / cd_speedup();
}

static inline uint8_t to_bcd8(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline uint32_t from_bcd8(uint8_t v) { return (uint32_t)((v >> 4) * 10 + (v & 0xF)); }

// Queue a response. slot 0 = first response (INT3/INT5), slot 1 = second (INT2).
// Delay for slot 1 counts from the moment slot 0 has been delivered.
static void cd_queue(PS1CDROM* cd, int slot, uint32_t delay, uint8_t type, const uint8_t* data, int len) {
    CDEvent* e = &cd->ev[slot];
    e->valid = true;
    e->delay = delay;
    e->type = type;
    e->len = (uint8_t)(len > 16 ? 16 : len);
    memcpy(e->data, data, e->len);
}

static void cd_queue_stat(PS1CDROM* cd, int slot, uint32_t delay, uint8_t type) {
    cd_queue(cd, slot, delay, type, &cd->status, 1);
}

static void cd_error(PS1CDROM* cd, uint8_t code) {
    uint8_t r[2] = { (uint8_t)(cd->status | CD_STAT_SEEK_ERROR), code };
    cd_queue(cd, 0, CD_DELAY_FAST, 5, r, 2);
}

// Raise the CPU interrupt only if the flag is enabled in the INT enable register
static void cd_update_irq(PS1Context* ctx) {
    if (ctx->cdrom.irq_flag & ctx->cdrom.irq_enable & 0x1F) {
        ps1_trigger_irq(ctx, IRQ_CDROM);
    }
}

static void cd_post(PS1Context* ctx, CDEvent* e) {
    PS1CDROM* cd = &ctx->cdrom;
    memcpy(cd->response_fifo, e->data, e->len);
    cd->response_count = e->len;
    cd->response_read_pos = 0;
    cd->irq_flag = e->type;
    e->valid = false;
    cd_update_irq(ctx);
}

static void cd_stop_motion(PS1CDROM* cd) {
    cd->reading = false;
    cd->playing = false;
    cd->status &= ~(CD_STAT_READ_BIT | CD_STAT_PLAY_BIT | CD_STAT_SEEK_BIT);
}

static void handle_cdrom_command(PS1Context* ctx, uint8_t cmd) {
    PS1CDROM* cd = &ctx->cdrom;
    int np = cd->param_count;
    PS1_LOG(2, "[CDROM] Command 0x%02X (params=%d)\n", cmd, np);

    cd->busy = true;
    cd->ev[0].valid = false;
    cd->ev[1].valid = false;

    switch (cmd) {
        case 0x01: // Getstat
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x02: // Setloc (M, S, F): only latches the target, the seek happens on Seek/Read
            if (np < 3) { cd_error(cd, 0x20); break; }
            {
                uint32_t m = from_bcd8(cd->param_fifo[0]);
                uint32_t s = from_bcd8(cd->param_fifo[1]);
                uint32_t f = from_bcd8(cd->param_fifo[2]);
                uint32_t abs_lba = (m * 60 * 75) + (s * 75) + f;
                cd->seek_lba = abs_lba >= 150 ? abs_lba - 150 : 0; // 2-second pregap
                cd->setloc_pending = true;
                PS1_LOG(2, "[CDROM] Setloc %02u:%02u:%02u -> LBA %u\n", m, s, f, cd->seek_lba);
            }
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x03: // Play
            cd->reading = false;
            cd->playing = true;
            cd->status |= CD_STAT_PLAY_BIT;
            cd->status &= ~CD_STAT_READ_BIT;
            cd->cdda_sample_offset = 0;
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x06: // ReadN
        case 0x1B: // ReadS
        {
            uint32_t seek = 0;
            if (cd->setloc_pending) {
                seek = cd_seek_delay(cd, cd->seek_lba);
                cd->current_lba = cd->seek_lba;
                cd->setloc_pending = false;
            }
            cd->playing = false;
            cd->status &= ~(CD_STAT_PLAY_BIT | CD_STAT_SEEK_BIT);
            cd->status |= CD_STAT_READ_BIT;
            cd->reading = true;
            cd->sector_cycles = seek + cd_sector_period(cd);
            PS1_LOG(2, "[CDROM] Read started at LBA %u (mode=0x%02X, seek=%u)\n", cd->current_lba, cd->mode, seek);
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        }
        case 0x07: // MotorOn
            cd->status |= CD_STAT_SPINDLE_ON;
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            cd_queue_stat(cd, 1, 400000, 2);
            break;
        case 0x08: // Stop
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            cd_stop_motion(cd);
            cd->status &= ~CD_STAT_SPINDLE_ON;
            cd_queue_stat(cd, 1, 5000000, 2);
            break;
        case 0x09: // Pause
        {
            bool was_reading = cd->reading || cd->playing;
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            cd_stop_motion(cd);
            PS1_LOG(2, "[CDROM] Pause at LBA %u\n", cd->current_lba);
            cd_queue_stat(cd, 1, was_reading ? 1100000 : 135000, 2);
            break;
        }
        case 0x0A: // Init
            cd_queue_stat(cd, 0, CD_DELAY_INIT, 3);
            cd_stop_motion(cd);
            cd->mode = 0x20;
            cd->setloc_pending = false;
            cd->status = CD_STAT_SPINDLE_ON;
            cd_queue_stat(cd, 1, 120000, 2);
            break;
        case 0x0B: // Mute
            cd->muted = true;
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x0C: // Demute
            cd->muted = false;
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x0D: // Setfilter (file, channel)
            if (np < 2) { cd_error(cd, 0x20); break; }
            cd->filter_file = cd->param_fifo[0];
            cd->filter_channel = cd->param_fifo[1];
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x0E: // Setmode
            if (np < 1) { cd_error(cd, 0x20); break; }
            cd->mode = cd->param_fifo[0];
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            break;
        case 0x0F: // Getparam
        {
            uint8_t r[5] = { cd->status, cd->mode, 0x00, cd->filter_file, cd->filter_channel };
            cd_queue(cd, 0, CD_DELAY_FAST, 3, r, 5);
            break;
        }
        case 0x10: // GetlocL (header + subheader of the last sector read)
        {
            uint8_t r[8] = {0};
            if (cd->image_file && cd->is_raw) {
                fseek(cd->image_file, (long)cd->last_read_lba * cd->sector_size + 12, SEEK_SET);
                if (fread(r, 1, 8, cd->image_file) != 8) memset(r, 0, sizeof(r));
            } else {
                uint32_t lba = cd->last_read_lba + 150;
                r[0] = to_bcd8(lba / (60 * 75));
                r[1] = to_bcd8((lba % (60 * 75)) / 75);
                r[2] = to_bcd8(lba % 75);
                r[3] = 0x02; // Mode 2
            }
            cd_queue(cd, 0, CD_DELAY_FAST, 3, r, 8);
            break;
        }
        case 0x11: // GetlocP (Track, Index, Relative MM:SS:FF, Absolute MM:SS:FF)
        {
            uint32_t rel = cd->last_read_lba, abs_lba = rel + 150;
            uint8_t r[8] = { 0x01, 0x01,
                to_bcd8(rel / (60 * 75)), to_bcd8((rel % (60 * 75)) / 75), to_bcd8(rel % 75),
                to_bcd8(abs_lba / (60 * 75)), to_bcd8((abs_lba % (60 * 75)) / 75), to_bcd8(abs_lba % 75) };
            cd_queue(cd, 0, CD_DELAY_FAST, 3, r, 8);
            break;
        }
        case 0x12: // SetSession
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            cd_queue_stat(cd, 1, 200000, 2);
            break;
        case 0x13: // GetTN (first, last track)
        {
            uint8_t r[3] = { cd->status, 0x01, 0x01 };
            cd_queue(cd, 0, CD_DELAY_FAST, 3, r, 3);
            break;
        }
        case 0x14: // GetTD (track): MM:SS of track start (0 = end of disc)
        {
            uint32_t lba = (np >= 1 && cd->param_fifo[0] == 0) ? cd->total_sectors + 150 : 150;
            uint8_t r[3] = { cd->status, to_bcd8(lba / (60 * 75)), to_bcd8((lba % (60 * 75)) / 75) };
            cd_queue(cd, 0, CD_DELAY_FAST, 3, r, 3);
            break;
        }
        case 0x15: // SeekL
        case 0x16: // SeekP
        {
            uint32_t seek = 0;
            if (cd->setloc_pending) {
                seek = cd_seek_delay(cd, cd->seek_lba);
                cd->current_lba = cd->seek_lba;
                cd->setloc_pending = false;
            }
            cd->last_read_lba = cd->current_lba;
            cd_stop_motion(cd);
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            cd_queue_stat(cd, 1, seek + 50000, 2);
            break;
        }
        case 0x19: // Test
        {
            uint8_t sub = np >= 1 ? cd->param_fifo[0] : 0;
            if (sub == 0x20) { // Get CD controller version/date
                uint8_t r[4] = { 0x94, 0x09, 0x19, 0xC0 };
                cd_queue(cd, 0, CD_DELAY_FAST, 3, r, 4);
            } else {
                cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            }
            break;
        }
        case 0x1A: // GetID: INT3(stat), then INT2 with disc type / region
        {
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            if (cd->image_file) {
                uint8_t r[8] = { cd->status, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };
                cd_queue(cd, 1, 33000, 2, r, 8);
            } else {
                uint8_t r[8] = { (uint8_t)(cd->status | CD_STAT_ID_ERROR), 0x40, 0x00, 0x00, 0, 0, 0, 0 };
                cd_queue(cd, 1, 33000, 5, r, 8);
            }
            break;
        }
        case 0x1E: // ReadTOC
            cd_queue_stat(cd, 0, CD_DELAY_FAST, 3);
            cd_queue_stat(cd, 1, 1000000, 2);
            break;
        default:
            PS1_LOG(1, "[CDROM] Unknown command 0x%02X\n", cmd);
            cd_error(cd, 0x40); // Invalid command
            break;
    }
    cd->param_count = 0;
}

// ---------------------------------------------------------------------------------------------
// XA-ADPCM: 18 sound groups of 8 blocks x 28 samples per sector (4-bit), mono or stereo

static const int xa_pos[4] = { 0, 60, 115, 98 };
static const int xa_neg[4] = { 0, 0, -52, -55 };

static void xa_push(PS1CDROM* cd, int16_t l, int16_t r) {
    if (cd->xa_write - cd->xa_read >= XA_RING_FRAMES) cd->xa_read++;   // overrun: drop the oldest
    uint32_t i = cd->xa_write++ & (XA_RING_FRAMES - 1);
    cd->xa_ring[i * 2] = l;
    cd->xa_ring[i * 2 + 1] = r;
}

static void cd_decode_xa(PS1CDROM* cd, const uint8_t* raw) {
    uint8_t coding = raw[19];
    bool stereo = (coding & 1) != 0;
    uint32_t rate = (coding & 4) ? 18900 : 37800;
    if (rate != cd->xa_rate) { cd->xa_rate = rate; cd->xa_frac = 0; }
    for (int g = 0; g < 18; g++) {
        const uint8_t* grp = raw + 24 + g * 128;
        int16_t out[8][28];
        for (int b = 0; b < 8; b++) {
            uint8_t hdr = grp[4 + b];
            int shift = hdr & 0x0F; if (shift > 12) shift = 9;
            int filt = (hdr >> 4) & 3;
            int ch = stereo ? (b & 1) : 0;
            for (int i = 0; i < 28; i++) {
                int nib = (grp[16 + i * 4 + (b >> 1)] >> (4 * (b & 1))) & 0xF;
                int s = (int16_t)(nib << 12) >> shift;
                s += (cd->xa_old[ch] * xa_pos[filt] + cd->xa_older[ch] * xa_neg[filt] + 32) >> 6;
                if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
                cd->xa_older[ch] = cd->xa_old[ch];
                cd->xa_old[ch] = (int16_t)s;
                out[b][i] = (int16_t)s;
            }
        }
        if (stereo) {
            for (int pair = 0; pair < 4; pair++)
                for (int i = 0; i < 28; i++) xa_push(cd, out[pair * 2][i], out[pair * 2 + 1][i]);
        } else {
            for (int b = 0; b < 8; b++)
                for (int i = 0; i < 28; i++) xa_push(cd, out[b][i], out[b][i]);
        }
    }
}

// Produces `num_samples` frames at 44100 Hz; returns how many were available (rest is silence)
int ps1_cdrom_read_xa_samples(PS1CDROM* cd, int16_t* out, int num_samples) {
    int produced = 0;
    uint32_t step = cd->xa_rate ? (uint32_t)(((uint64_t)cd->xa_rate << 16) / 44100) : 0;
    while (produced < num_samples) {
        if (cd->xa_write - cd->xa_read < 2) break;
        uint32_t i0 = cd->xa_read & (XA_RING_FRAMES - 1), i1 = (cd->xa_read + 1) & (XA_RING_FRAMES - 1);
        for (int c = 0; c < 2; c++) {
            int32_t a = cd->xa_ring[i0 * 2 + c], b = cd->xa_ring[i1 * 2 + c];
            out[produced * 2 + c] = (int16_t)(a + (((b - a) * (int32_t)cd->xa_frac) >> 16));
        }
        produced++;
        cd->xa_frac += step;
        while (cd->xa_frac >= 0x10000) { cd->xa_frac -= 0x10000; cd->xa_read++; }
    }
    return produced;
}

// Reads the next sector from the image into the controller's sector buffer.
// Returns false if the sector must be dropped (XA audio, read error).
static bool cd_fetch_sector(PS1CDROM* cd) {
    if (!cd->image_file) return false;
    bool wide = (cd->mode & 0x20) != 0; // 0x924 bytes (from after sync) vs 0x800
    uint32_t off = cd->is_raw ? (wide ? 12 : 24) : 0;
    uint32_t len = wide ? (cd->is_raw ? 2340 : 2048) : CDROM_SECTOR_SIZE_DATA;

    if (cd->is_raw && (cd->mode & 0x40)) {
        // XA-ADPCM enabled: audio sectors (submode bit 2) go to the decoder, not the host
        uint8_t raw[CDROM_SECTOR_SIZE_RAW];
        fseek(cd->image_file, (long)cd->current_lba * cd->sector_size, SEEK_SET);
        if (fread(raw, 1, sizeof(raw), cd->image_file) == sizeof(raw) && (raw[18] & 0x04)) {
            bool match = !(cd->mode & 0x08) || (raw[16] == cd->filter_file && raw[17] == cd->filter_channel);
            if (match) cd_decode_xa(cd, raw);
            return false;
        }
    }

    fseek(cd->image_file, (long)cd->current_lba * cd->sector_size + off, SEEK_SET);
    size_t r = fread(cd->sector_buf, 1, len, cd->image_file);
    if (r == 0) return false;
    cd->sector_len = (int)r;
    return true;
}

void ps1_cdrom_tick(PS1Context* ctx, uint32_t cycles) {
    PS1CDROM* cd = &ctx->cdrom;

    // Command responses. Only one interrupt can be pending: a response waits
    // until the previous one has been acknowledged.
    CDEvent* e0 = &cd->ev[0];
    CDEvent* e1 = &cd->ev[1];
    if (e0->valid) {
        e0->delay = e0->delay > cycles ? e0->delay - cycles : 0;
        if (e0->delay == 0 && cd->irq_flag == 0) {
            cd_post(ctx, e0);
            cd->busy = false;
        }
    }
    if (!e0->valid && e1->valid) {
        e1->delay = e1->delay > cycles ? e1->delay - cycles : 0;
        if (e1->delay == 0 && cd->irq_flag == 0) {
            cd_post(ctx, e1);
        }
    }

    // Sector stream
    if (cd->reading && cd->image_file) {
        if (cd->sector_cycles > cycles) {
            cd->sector_cycles -= cycles;
        } else {
            cd->sector_cycles = 0;
            if (cd->irq_flag == 0 && !e0->valid) {
                if (cd_fetch_sector(cd)) {
                    cd->last_read_lba = cd->current_lba;
                    cd->response_fifo[0] = cd->status;
                    cd->response_count = 1;
                    cd->response_read_pos = 0;
                    cd->irq_flag = 1; // INT1: sector ready
                    PS1_LOG(2, "[CDROM] Sector ready: LBA %u (len=%d, mode=0x%02X)\n", cd->current_lba, cd->sector_len, cd->mode);
                    cd_update_irq(ctx);
                }
                cd->current_lba++;
                cd->sector_cycles = cd_sector_period(cd);
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
            ret = cd->reg_index |
                  (cd->param_count == 0 ? (1 << 3) : 0) |        // PRMEMPT
                  (cd->param_count < 16 ? (1 << 4) : 0) |        // PRMWRDY
                  (cd->response_count > cd->response_read_pos ? (1 << 5) : 0) | // RSLRRDY
                  (cd->data_bytes_avail > 0 ? (1 << 6) : 0) |    // DRQSTS
                  (cd->busy ? (1 << 7) : 0);                     // BUSYSTS
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
        case 3: // Index 0/2 = Interrupt Enable, Index 1/3 = Interrupt Flag
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
                if (cd->busy) {
                    PS1_LOG(1, "[CDROM] Command 0x%02X written while busy: ignored\n", val);
                } else {
                    handle_cdrom_command(ctx, val);
                }
            }
            break;
        case 2:
            if (cd->reg_index == 0) {
                if (cd->param_count < 16) cd->param_fifo[cd->param_count++] = val;
            } else if (cd->reg_index == 1) {
                cd->irq_enable = val & 0x1F;
                cd_update_irq(ctx); // A flag that is already pending may now be visible
            }
            break;
        case 3:
            if (cd->reg_index == 0) {
                // Request register: bit 7 (BFRD) loads the sector buffer into the data FIFO,
                // clearing it resets the FIFO
                if (val & 0x80) {
                    if (cd->data_bytes_avail == 0 && cd->sector_len > 0) {
                        memcpy(cd->data_fifo, cd->sector_buf, cd->sector_len);
                        cd->data_bytes_avail = cd->sector_len;
                        cd->data_read_pos = 0;
                    }
                } else {
                    cd->data_bytes_avail = 0;
                    cd->data_read_pos = 0;
                }
            } else if (cd->reg_index == 1) {
                cd->irq_flag &= ~(val & 0x1F);
                if (val & 0x40) cd->param_count = 0; // CLRPRM
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

    // Strip leading device prefix (e.g. "sim:", "cdrom:", "cd:", etc.)
    const char* p = pattern;
    if (strncasecmp(p, "sim:", 4) == 0) p += 4;
    if (strncasecmp(p, "cdrom:", 6) == 0) p += 6;
    if (strncasecmp(p, "cd:", 3) == 0) p += 3;
    while (*p == '\\' || *p == '/') p++;

    // Extract basename fallback (e.g. "c:\bin\dra.bin" -> "dra.bin")
    const char* last_slash = strrchr(p, '\\');
    const char* last_fwd = strrchr(p, '/');
    const char* base = p;
    if (last_slash && last_slash >= base) base = last_slash + 1;
    if (last_fwd && last_fwd >= base) base = last_fwd + 1;

    // Strip trailing version from pattern if present
    char clean_pat[64];
    strncpy(clean_pat, p, 63);
    clean_pat[63] = '\0';
    char* pat_semi = strchr(clean_pat, ';');
    if (pat_semi) *pat_semi = '\0';

    char clean_base[64];
    strncpy(clean_base, base, 63);
    clean_base[63] = '\0';
    char* base_semi = strchr(clean_base, ';');
    if (base_semi) *base_semi = '\0';

    return strcasecmp(clean_pat, clean_entry) == 0 || strcasecmp(clean_base, clean_entry) == 0;
}

static bool search_iso_dir(PS1CDROM* cdrom, uint32_t dir_lba, uint32_t dir_size, const char* filename, PS1ISOFile* out_file, int depth) {
    if (depth > 4) return false;
    uint32_t sectors = (dir_size + 2047) / 2048;
    uint8_t sec_buf[2048];

    // Pass 1: search files in current directory
    for (uint32_t s = 0; s < sectors; s++) {
        if (!read_iso_sector(cdrom, dir_lba + s, sec_buf)) break;
        uint32_t pos = 0;
        while (pos < 2048) {
            uint8_t rec_len = sec_buf[pos];
            if (rec_len == 0) break;

            uint32_t file_lba = *(const uint32_t*)&sec_buf[pos + 2];
            uint32_t file_size = *(const uint32_t*)&sec_buf[pos + 10];
            uint8_t flags = sec_buf[pos + 25];
            uint8_t name_len = sec_buf[pos + 32];
            const char* name_ptr = (const char*)&sec_buf[pos + 33];
            bool is_dir = (flags & 2) != 0;

            if (name_len > 0 && !is_dir && iso_name_match(filename, name_ptr, name_len)) {
                out_file->lba = file_lba;
                out_file->size = file_size;
                out_file->is_dir = false;
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

    // Pass 2: recurse into subdirectories
    for (uint32_t s = 0; s < sectors; s++) {
        if (!read_iso_sector(cdrom, dir_lba + s, sec_buf)) break;
        uint32_t pos = 0;
        while (pos < 2048) {
            uint8_t rec_len = sec_buf[pos];
            if (rec_len == 0) break;

            uint32_t sub_lba = *(const uint32_t*)&sec_buf[pos + 2];
            uint32_t sub_size = *(const uint32_t*)&sec_buf[pos + 10];
            uint8_t flags = sec_buf[pos + 25];
            uint8_t name_len = sec_buf[pos + 32];
            const char* name_ptr = (const char*)&sec_buf[pos + 33];
            bool is_dir = (flags & 2) != 0;

            if (is_dir && name_len > 0 && name_ptr[0] != 0 && name_ptr[0] != 1 && sub_lba != dir_lba) {
                if (search_iso_dir(cdrom, sub_lba, sub_size, filename, out_file, depth + 1)) {
                    return true;
                }
            }
            pos += rec_len;
        }
    }

    return false;
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

    return search_iso_dir(cdrom, root_lba, root_size, filename, out_file, 0);
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
