// Motion decoder (MDEC): decodes the PlayStation's JPEG-like macroblock stream.
//
// Pipeline per 8x8 block: run-length/VLC halfwords -> dequantisation (zig-zag order)
// -> two-pass IDCT using the game-supplied scale table -> YCbCr to RGB for the
// 16x16 macroblock (Cr, Cb, Y1..Y4 block order).
#include "mimips1_mdec.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

void ps1_mdec_init(PS1MDEC* m) {
    memset(m, 0, sizeof(*m));
    m->params_left = 0;
}

void ps1_mdec_reset(PS1MDEC* m) {
    m->control = 0;
    m->dma_in_enable = m->dma_out_enable = false;
    m->cmd = 0;
    m->params_left = 0;
    m->rl_len = 0;
    m->tbl_len = 0;
    m->out_len = m->out_pos = 0;
}

static inline int32_t sext10(uint32_t v) { return (int32_t)(v << 22) >> 22; }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Decodes one block's run-length data into 64 dequantised coefficients.
// Returns the number of halfwords consumed.
static uint32_t decode_block_rl(const PS1MDEC* m, const uint16_t* hw, uint32_t avail, const uint8_t* qt, int32_t* coef) {
    uint32_t pos = 0;
    memset(coef, 0, 64 * sizeof(int32_t));

    while (pos < avail && hw[pos] == 0xFE00) pos++;   // padding before the block
    if (pos >= avail) return pos;

    uint32_t first = hw[pos++];
    uint32_t qscale = (first >> 10) & 63;
    int32_t dc = sext10(first & 0x3FF);

    int32_t val = (qscale == 0) ? dc * 2 : dc * qt[0];
    coef[0] = clampi(val, -0x400, 0x3FF);

    uint32_t n = 0;
    while (pos < avail) {
        uint32_t code = hw[pos++];
        if (code == 0xFE00) break;                    // end of block
        n += ((code >> 10) & 63) + 1;
        if (n > 63) break;
        int32_t level = sext10(code & 0x3FF);
        int32_t v = (qscale == 0) ? level * 2 : (level * (int32_t)qt[n] * (int32_t)qscale + 4) / 8;
        coef[zigzag[n]] = clampi(v, -0x400, 0x3FF);
    }
    return pos;
}

// Two-pass 8x8 inverse DCT with the scale table from command 3 (2^15 fixed point).
// An all-DC block of value D yields D/8 per pixel, as in the hardware.
static void idct(const PS1MDEC* m, const int32_t* coef, int32_t* out) {
    int32_t tmp[64];
    for (int v = 0; v < 8; v++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++) sum += (int64_t)coef[v * 8 + u] * m->scale[u * 8 + x];
            tmp[v * 8 + x] = (int32_t)((sum + 0x8000) >> 16);
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int v = 0; v < 8; v++) sum += (int64_t)tmp[v * 8 + x] * m->scale[v * 8 + y];
            out[y * 8 + x] = clampi((int32_t)((sum + 0x8000) >> 16), -128, 127);
        }
    }
}

static void push_out(PS1MDEC* m, uint8_t b) {
    if (m->out_len < MDEC_OUT_CAPACITY) m->out[m->out_len++] = b;
}

static void run_decode(PS1MDEC* m) {
    uint32_t depth = (m->cmd_word >> 27) & 3;
    bool is_signed = (m->cmd_word >> 26) & 1;
    uint32_t bit15 = ((m->cmd_word >> 25) & 1) ? 0x8000u : 0;
    m->out_depth = depth;

    // Consumed data in the output FIFO is dropped when a new command starts
    if (m->out_pos >= m->out_len) m->out_len = m->out_pos = 0;

    const uint16_t* hw = m->rl;
    uint32_t pos = 0, n = m->rl_len;
    int32_t coef[64], blk[6][64];

    while (pos < n) {
        // Skip trailing padding so a stream that ends in 0xFE00 fill does not emit empty macroblocks
        uint32_t probe = pos;
        while (probe < n && hw[probe] == 0xFE00) probe++;
        if (probe >= n) break;

        if (depth >= 2) {
            // Colour: Cr, Cb, Y1, Y2, Y3, Y4
            for (int b = 0; b < 6; b++) {
                const uint8_t* qt = m->quant[(b < 2) ? 1 : 0];
                pos += decode_block_rl(m, hw + pos, n - pos, qt, coef);
                idct(m, coef, blk[b]);
            }
            for (int y = 0; y < 16; y++) {
                for (int x = 0; x < 16; x++) {
                    int yb = (y >> 3) * 2 + (x >> 3);
                    int32_t Y = blk[2 + yb][(y & 7) * 8 + (x & 7)];
                    int ci = (y >> 1) * 8 + (x >> 1);
                    // Cr = block 0, Cb = block 1
                    int32_t Cr = blk[0][((y >> 1) & 7) * 8 + ((x >> 1) & 7)];
                    int32_t Cb = blk[1][((y >> 1) & 7) * 8 + ((x >> 1) & 7)];
                    (void)ci;
                    int32_t R = clampi(Y + ((1436 * Cr) >> 10), -128, 127);
                    int32_t G = clampi(Y - ((352 * Cb + 731 * Cr) >> 10), -128, 127);
                    int32_t B = clampi(Y + ((1815 * Cb) >> 10), -128, 127);
                    if (!is_signed) { R += 128; G += 128; B += 128; }
                    if (depth == 2) {
                        push_out(m, (uint8_t)R); push_out(m, (uint8_t)G); push_out(m, (uint8_t)B);
                    } else {
                        uint32_t px = (uint32_t)((R & 0xFF) >> 3) | (uint32_t)(((G & 0xFF) >> 3) << 5) |
                                      (uint32_t)(((B & 0xFF) >> 3) << 10) | bit15;
                        push_out(m, (uint8_t)px); push_out(m, (uint8_t)(px >> 8));
                    }
                }
            }
        } else {
            // Monochrome: one Y block per macroblock
            pos += decode_block_rl(m, hw + pos, n - pos, m->quant[0], coef);
            idct(m, coef, blk[0]);
            if (depth == 1) {
                for (int i = 0; i < 64; i++) push_out(m, (uint8_t)(is_signed ? blk[0][i] : blk[0][i] + 128));
            } else {
                for (int i = 0; i < 64; i += 2) {
                    uint8_t a = (uint8_t)((is_signed ? blk[0][i] : blk[0][i] + 128) >> 4);
                    uint8_t b = (uint8_t)((is_signed ? blk[0][i + 1] : blk[0][i + 1] + 128) >> 4);
                    push_out(m, (uint8_t)(a | (b << 4)));
                }
            }
        }
    }
}

static void debug_dump_frame(const PS1MDEC* m) {
    static int dumped = 0;
    const char* e = getenv("MIMIPS1_MDEC_DUMP");
    if (!e || dumped >= atoi(e) || m->out_depth < 2) return;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/mdec_%d.ppm", dumped++);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t bpp = m->out_depth == 2 ? 3 : 2;
    uint32_t w = 16, h = m->out_len / (16 * bpp * 1); // macroblocks come out one after another
    // Output is macroblock-ordered (16x16); lay macroblocks left to right, 20 per row, for viewing
    uint32_t mbs = m->out_len / (256 * bpp);
    uint32_t cols = 20, rows = (mbs + cols - 1) / cols;
    (void)w; (void)h;
    fprintf(f, "P6\n%u %u\n255\n", cols * 16, rows * 16);
    for (uint32_t ry = 0; ry < rows * 16; ry++) {
        for (uint32_t rx = 0; rx < cols * 16; rx++) {
            uint32_t mb = (ry / 16) * cols + (rx / 16);
            uint32_t px = (ry % 16) * 16 + (rx % 16);
            uint8_t rgb[3] = {0, 0, 0};
            if (mb < mbs) {
                const uint8_t* p = m->out + (mb * 256 + px) * bpp;
                if (bpp == 3) { rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; }
                else { uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); rgb[0] = (uint8_t)((v & 31) << 3); rgb[1] = (uint8_t)(((v >> 5) & 31) << 3); rgb[2] = (uint8_t)(((v >> 10) & 31) << 3); }
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void finish_command(PS1MDEC* m) {
    if (m->cmd == 1) {
        if (getenv("MIMIPS1_MDEC_LOG")) {
            uint32_t nz = 0;
            for (uint32_t i = 0; i < m->rl_len; i++) if (m->rl[i] != 0xFE00) nz++;
            printf("[MDEC] decode cmd=%08X rl_len=%u non-EOB=%u first: %04X %04X %04X %04X %04X %04X %04X %04X quant0=%u scale0=%04X\n",
                   m->cmd_word, m->rl_len, nz, m->rl[0], m->rl[1], m->rl[2], m->rl[3], m->rl[4], m->rl[5], m->rl[6], m->rl[7],
                   m->quant[0][0], (uint16_t)m->scale[0]);
        }
        run_decode(m);
        if (getenv("MIMIPS1_MDEC_LOG")) printf("[MDEC]   -> out_len=%u depth=%u first bytes: %02X %02X %02X %02X %02X %02X\n", m->out_len, m->out_depth, m->out[0], m->out[1], m->out[2], m->out[3], m->out[4], m->out[5]);
        debug_dump_frame(m);
    } else if (m->cmd == 2) {
        memset(m->quant, 0, sizeof(m->quant));
        memcpy(m->quant[0], m->tbl_bytes, 64);
        if (m->tbl_len >= 128) memcpy(m->quant[1], m->tbl_bytes + 64, 64);
        else memcpy(m->quant[1], m->tbl_bytes, 64);
    } else if (m->cmd == 3) {
        for (int i = 0; i < 64; i++) m->scale[i] = (int16_t)(m->tbl_bytes[i * 2] | (m->tbl_bytes[i * 2 + 1] << 8));
    }
    m->cmd = 0;
}

void ps1_mdec_write_command(PS1MDEC* m, uint32_t word) {
    if (m->cmd == 0) {
        uint32_t id = word >> 29;
        m->cmd_word = word;
        if (id == 1) {
            m->cmd = 1;
            m->params_total = m->params_left = word & 0xFFFF;
            m->rl_len = 0;
        } else if (id == 2) {
            m->cmd = 2;
            m->params_total = m->params_left = (word & 1) ? 32 : 16;
            m->tbl_len = 0;
        } else if (id == 3) {
            m->cmd = 3;
            m->params_total = m->params_left = 32;
            m->tbl_len = 0;
        } else {
            return; // NOP / unknown
        }
        if (m->params_left == 0) finish_command(m);
        return;
    }

    if (m->cmd == 1) {
        if (m->rl_len + 2 <= sizeof(m->rl) / sizeof(m->rl[0])) {
            m->rl[m->rl_len++] = (uint16_t)word;
            m->rl[m->rl_len++] = (uint16_t)(word >> 16);
        }
    } else {
        if (m->tbl_len + 4 <= sizeof(m->tbl_bytes)) {
            m->tbl_bytes[m->tbl_len++] = (uint8_t)word;
            m->tbl_bytes[m->tbl_len++] = (uint8_t)(word >> 8);
            m->tbl_bytes[m->tbl_len++] = (uint8_t)(word >> 16);
            m->tbl_bytes[m->tbl_len++] = (uint8_t)(word >> 24);
        }
    }
    if (--m->params_left == 0) finish_command(m);
}

void ps1_mdec_write_control(PS1MDEC* m, uint32_t word) {
    if (word & 0x80000000u) ps1_mdec_reset(m);
    m->control = word;
    m->dma_in_enable = (word >> 30) & 1;
    m->dma_out_enable = (word >> 29) & 1;
}

uint32_t ps1_mdec_read_data(PS1MDEC* m) {
    uint32_t w = 0;
    for (int i = 0; i < 4 && m->out_pos < m->out_len; i++) w |= (uint32_t)m->out[m->out_pos++] << (8 * i);
    return w;
}

uint32_t ps1_mdec_read_status(PS1MDEC* m) {
    uint32_t s = 0;
    bool out_empty = m->out_pos >= m->out_len;
    if (out_empty) s |= 1u << 31;
    if (m->cmd != 0) s |= 1u << 29;                       // command busy while parameters are pending
    if (m->dma_in_enable && m->cmd != 0) s |= 1u << 28;   // data-in request
    if (m->dma_out_enable && !out_empty) s |= 1u << 27;   // data-out request
    s |= (m->out_depth & 3) << 25;
    s |= (m->cmd != 0 && m->params_left) ? ((m->params_left - 1) & 0xFFFF) : 0xFFFF;
    return s;
}

void ps1_mdec_dma_in(PS1MDEC* m, const uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) ps1_mdec_write_command(m, words[i]);
}

uint32_t ps1_mdec_dma_out(PS1MDEC* m, uint32_t* words, uint32_t count) {
    uint32_t n = 0;
    while (n < count && m->out_pos < m->out_len) words[n++] = ps1_mdec_read_data(m);
    return n;
}
