#include "mimips1_gpu.h"
#include "mimips1_runtime.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

static inline uint16_t rgb_to_ps1_color(uint8_t r, uint8_t g, uint8_t b, uint8_t mask) {
    return ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10) | ((mask & 1) << 15);
}

static inline void ps1_color_to_rgb(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = (uint8_t)(((c >> 0)  & 0x1F) << 3);
    *g = (uint8_t)(((c >> 5)  & 0x1F) << 3);
    *b = (uint8_t)(((c >> 10) & 0x1F) << 3);
}

static inline int16_t gp0_coord(uint32_t val) {
    int16_t c = (int16_t)(val & 0x7FF);
    if (c & 0x400) c |= 0xF800; // Sign-extend 11-bit to 16-bit (-1024..+1023)
    return c;
}

void ps1_gpu_init(PS1GPU* gpu) {
    memset(gpu, 0, sizeof(PS1GPU));
    gpu->status = GPUSTAT_READY_FOR_CMD | GPUSTAT_READY_FOR_DMA | GPUSTAT_READY_FOR_VRAM | GPUSTAT_DRAW_TO_DISPLAY;
    gpu->disp_w = 320;
    gpu->disp_h = 240;
    gpu->clip_x2 = 1023;
    gpu->clip_y2 = 511;
}

void ps1_gpu_reset(PS1GPU* gpu) {
    ps1_gpu_init(gpu);
}

uint32_t ps1_gpu_read_status(PS1GPU* gpu) {
    uint32_t stat = gpu->status;
    if (gpu->interlace_field) {
        stat |= (1u << 31) | (1u << 13);
    } else {
        stat &= ~((1u << 31) | (1u << 13));
    }
    return stat;
}

uint32_t ps1_gpu_read_data(PS1GPU* gpu) {
    if (gpu->transfer.active && gpu->transfer.is_read) {
        uint16_t p0 = gpu->vram[gpu->transfer.cur_y][gpu->transfer.cur_x];
        gpu->transfer.cur_x++;
        if (gpu->transfer.cur_x >= (gpu->transfer.x + gpu->transfer.w)) {
            gpu->transfer.cur_x = gpu->transfer.x;
            gpu->transfer.cur_y++;
        }
        uint16_t p1 = gpu->vram[gpu->transfer.cur_y][gpu->transfer.cur_x];
        gpu->transfer.cur_x++;
        if (gpu->transfer.cur_x >= (gpu->transfer.x + gpu->transfer.w)) {
            gpu->transfer.cur_x = gpu->transfer.x;
            gpu->transfer.cur_y++;
            if (gpu->transfer.cur_y >= (gpu->transfer.y + gpu->transfer.h)) {
                gpu->transfer.active = false;
            }
        }
        return (uint32_t)p0 | ((uint32_t)p1 << 16);
    }
    return gpu->gpu_read_latch;
}

static int get_gp0_cmd_len(uint32_t cmd) {
    uint8_t op = (cmd >> 24) & 0xFF;
    if (op == 0x00) return 1;
    if (op == 0x01) return 1;
    if (op == 0x02) return 3; // Fill Rect
    if (op >= 0x20 && op <= 0x3F) {
        bool is_quad = (op & (1 << 3)) != 0;
        bool is_textured = (op & (1 << 2)) != 0;
        bool is_gouraud = (op & (1 << 4)) != 0;
        
        int len = 1; // cmd + first vertex or color
        int num_v = is_quad ? 4 : 3;
        if (is_gouraud) {
            len = num_v * (is_textured ? 3 : 2) + (is_textured ? 0 : 0);
        } else {
            len = 1 + num_v * (is_textured ? 2 : 1);
        }
        return len;
    }
    if (op >= 0x40 && op <= 0x5F) {
        // Line
        bool is_poly = (op & (1 << 3)) != 0;
        bool is_gouraud = (op & (1 << 4)) != 0;
        if (is_poly) return 0; // Poly-line is variable-length terminated by 0x55555555
        return is_gouraud ? 4 : 3;
    }
    if (op >= 0x60 && op <= 0x7F) {
        // Rectangles / Sprites
        bool is_textured = (op & (1 << 2)) != 0;
        uint8_t size_type = (op >> 3) & 3;
        int len = 2; // color/cmd, coord
        if (is_textured) len++;
        if (size_type == 0) len++; // variable size
        return len;
    }
    if (op >= 0x80 && op <= 0x9F) return 4; // VRAM to VRAM
    if (op >= 0xA0 && op <= 0xBF) return 3; // CPU to VRAM
    if (op >= 0xC0 && op <= 0xDF) return 3; // VRAM to CPU
    if (op >= 0xE1 && op <= 0xE6) return 1; // Environment setting
    return 1;
}

static void blend_pixel(PS1GPU* gpu, uint16_t* dst, uint8_t fr, uint8_t fg, uint8_t fb, bool semi_trans) {
    if (!semi_trans) {
        *dst = rgb_to_ps1_color(fr, fg, fb, 0);
        return;
    }

    uint8_t br, bg, bb;
    ps1_color_to_rgb(*dst, &br, &bg, &bb);

    int r = 0, g = 0, b = 0;
    switch (gpu->semi_trans_mode) {
        case 0: // 0.5 * B + 0.5 * F
            r = (br >> 1) + (fr >> 1);
            g = (bg >> 1) + (fg >> 1);
            b = (bb >> 1) + (fb >> 1);
            break;
        case 1: // 1.0 * B + 1.0 * F
            r = br + fr;
            g = bg + fg;
            b = bb + fb;
            break;
        case 2: // 1.0 * B - 1.0 * F
            r = br - fr;
            g = bg - fg;
            b = bb - fb;
            break;
        case 3: // 1.0 * B + 0.25 * F
            r = br + (fr >> 2);
            g = bg + (fg >> 2);
            b = bb + (fb >> 2);
            break;
    }

    r = MIN(MAX(r, 0), 255);
    g = MIN(MAX(g, 0), 255);
    b = MIN(MAX(b, 0), 255);
    *dst = rgb_to_ps1_color((uint8_t)r, (uint8_t)g, (uint8_t)b, 1);
}

uint16_t ps1_gpu_sample_texture(PS1GPU* gpu, int u, int v) {
    u = (u & 0xFF);
    v = (v & 0xFF);
    u = (u & ~(gpu->tex_win_mask_x * 8)) | ((gpu->tex_win_off_x & gpu->tex_win_mask_x) * 8);
    v = (v & ~(gpu->tex_win_mask_y * 8)) | ((gpu->tex_win_off_y & gpu->tex_win_mask_y) * 8);

    int tex_x = (gpu->tex_page_x * 64) + (u & 0xFF);
    int tex_y = (gpu->tex_page_y * 256) + (v & 0xFF);

    if (gpu->tex_color_mode == 0) {
        // 4-bit CLUT
        int fb_x = (gpu->tex_page_x * 64) + ((u & 0xFF) / 4);
        uint16_t raw_word = gpu->vram[tex_y % 512][fb_x % 1024];
        int shift = ((u & 3) * 4);
        uint8_t clut_idx = (raw_word >> shift) & 0x0F;
        return gpu->vram[gpu->clut_y % 512][(gpu->clut_x + clut_idx) % 1024];
    } else if (gpu->tex_color_mode == 1) {
        // 8-bit CLUT
        int fb_x = (gpu->tex_page_x * 64) + ((u & 0xFF) / 2);
        uint16_t raw_word = gpu->vram[tex_y % 512][fb_x % 1024];
        uint8_t clut_idx = (u & 1) ? (raw_word >> 8) : (raw_word & 0xFF);
        return gpu->vram[gpu->clut_y % 512][(gpu->clut_x + clut_idx) % 1024];
    } else {
        // 15-bit direct
        return gpu->vram[tex_y % 512][tex_x % 1024];
    }
}

// Barycentric triangle rasterizer
void ps1_gpu_draw_triangle(PS1GPU* gpu, const PS1Vertex* v0, const PS1Vertex* v1, const PS1Vertex* v2, bool textured, bool gouraud, bool semi_trans, bool raw_texture) {
    int min_x = MIN(v0->x, MIN(v1->x, v2->x));
    int max_x = MAX(v0->x, MAX(v1->x, v2->x));
    int min_y = MIN(v0->y, MIN(v1->y, v2->y));
    int max_y = MAX(v0->y, MAX(v1->y, v2->y));

    // Clip to drawing area
    min_x = MAX(min_x, gpu->clip_x1);
    max_x = MIN(max_x, gpu->clip_x2);
    min_y = MAX(min_y, gpu->clip_y1);
    max_y = MIN(max_y, gpu->clip_y2);

    int area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
    if (area == 0) return;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int w0 = (v1->x - x) * (v2->y - y) - (v1->y - y) * (v2->x - x);
            int w1 = (v2->x - x) * (v0->y - y) - (v2->y - y) * (v0->x - x);
            int w2 = (v0->x - x) * (v1->y - y) - (v0->y - y) * (v1->x - x);

            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                
                uint8_t r = v0->r, g = v0->g, b = v0->b;
                if (gouraud) {
                    float fw0 = (float)w0 / area;
                    float fw1 = (float)w1 / area;
                    float fw2 = (float)w2 / area;
                    r = (uint8_t)(v0->r * fw0 + v1->r * fw1 + v2->r * fw2);
                    g = (uint8_t)(v0->g * fw0 + v1->g * fw1 + v2->g * fw2);
                    b = (uint8_t)(v0->b * fw0 + v1->b * fw1 + v2->b * fw2);
                }

                bool is_semi = semi_trans;
                if (textured) {
                    float fw0 = (float)w0 / area;
                    float fw1 = (float)w1 / area;
                    float fw2 = (float)w2 / area;
                    int u = (int)(v0->u * fw0 + v1->u * fw1 + v2->u * fw2 + 0.5f);
                    int v = (int)(v0->v * fw0 + v1->v * fw1 + v2->v * fw2 + 0.5f);
                    uint16_t tex_c = ps1_gpu_sample_texture(gpu, u, v);
                    if (tex_c == 0) continue; // Transparent color key

                    uint8_t tr, tg, tb;
                    ps1_color_to_rgb(tex_c, &tr, &tg, &tb);
                    if (!raw_texture) {
                        int mr = ((int)r * (int)tr) >> 7;
                        int mg = ((int)g * (int)tg) >> 7;
                        int mb = ((int)b * (int)tb) >> 7;
                        r = (uint8_t)MIN(255, mr);
                        g = (uint8_t)MIN(255, mg);
                        b = (uint8_t)MIN(255, mb);
                    } else {
                        r = tr;
                        g = tg;
                        b = tb;
                    }
                    is_semi = semi_trans && ((tex_c & 0x8000) != 0);
                }

                blend_pixel(gpu, &gpu->vram[y % 512][x % 1024], r, g, b, is_semi);
            }
        }
    }
}

void ps1_gpu_clear_vram(PS1GPU* gpu, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) {
    uint16_t color = rgb_to_ps1_color(r, g, b, 0);
    int x1 = x & 0x3F0; // 16-pixel aligned
    int y1 = y & 0x1FF;
    int cw = ((w & 0x3FF) + 0x0F) & ~0x0F;
    if (cw == 0 && w > 0) cw = 1024;
    int ch = h & 0x1FF;
    if (ch == 0 && h > 0) ch = 512;
    if (cw == 0 || ch == 0) return;

    for (int cy = 0; cy < ch; cy++) {
        for (int cx = 0; cx < cw; cx++) {
            gpu->vram[(y1 + cy) & 0x1FF][(x1 + cx) & 0x3FF] = color;
        }
    }
}

void ps1_gpu_draw_rect(PS1GPU* gpu, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t u, uint8_t v, bool textured, bool semi_trans, bool raw_texture) {
    int draw_x = x + gpu->draw_offset_x;
    int draw_y = y + gpu->draw_offset_y;
    int x1 = MAX(draw_x, gpu->clip_x1);
    int y1 = MAX(draw_y, gpu->clip_y1);
    int x2 = MIN(draw_x + w, gpu->clip_x2 + 1);
    int y2 = MIN(draw_y + h, gpu->clip_y2 + 1);

    for (int cy = y1; cy < y2; cy++) {
        for (int cx = x1; cx < x2; cx++) {
            uint8_t pr = r, pg = g, pb = b;
            bool is_semi = semi_trans;
            if (textured) {
                int cur_u = (u + (cx - draw_x)) & 0xFF;
                int cur_v = (v + (cy - draw_y)) & 0xFF;
                uint16_t tex = ps1_gpu_sample_texture(gpu, cur_u, cur_v);
                if (tex == 0) continue;
                uint8_t tr, tg, tb;
                ps1_color_to_rgb(tex, &tr, &tg, &tb);
                if (!raw_texture) {
                    int mr = ((int)pr * (int)tr) >> 7;
                    int mg = ((int)pg * (int)tg) >> 7;
                    int mb = ((int)pb * (int)tb) >> 7;
                    pr = (uint8_t)MIN(255, mr);
                    pg = (uint8_t)MIN(255, mg);
                    pb = (uint8_t)MIN(255, mb);
                } else {
                    pr = tr;
                    pg = tg;
                    pb = tb;
                }
                is_semi = semi_trans && ((tex & 0x8000) != 0);
            }
            blend_pixel(gpu, &gpu->vram[cy & 0x1FF][cx & 0x3FF], pr, pg, pb, is_semi);
        }
    }
}

void ps1_gpu_draw_line(PS1GPU* gpu, const PS1Vertex* v0, const PS1Vertex* v1, bool gouraud, bool semi_trans) {
    // Bresenham's line algorithm
    int x0 = v0->x + gpu->draw_offset_x;
    int y0 = v0->y + gpu->draw_offset_y;
    int x1 = v1->x + gpu->draw_offset_x;
    int y1 = v1->y + gpu->draw_offset_y;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        if (x0 >= gpu->clip_x1 && x0 <= gpu->clip_x2 && y0 >= gpu->clip_y1 && y0 <= gpu->clip_y2) {
            blend_pixel(gpu, &gpu->vram[y0 % 512][x0 % 1024], v0->r, v0->g, v0->b, semi_trans);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint32_t s_gp0_stats[256];
static int s_logged_polys = 0;
static int s_gp0_stats_enabled = -1;

static inline void check_gp0_stats_env(void) {
    if (s_gp0_stats_enabled < 0) {
        const char* e = getenv("MIMIPS1_GP0_STATS");
        s_gp0_stats_enabled = (e && atoi(e) == 1) ? 1 : 0;
    }
}

static void execute_gp0_packet(PS1GPU* gpu) {
    check_gp0_stats_env();
    uint8_t op = (gpu->gp0_fifo[0] >> 24) & 0xFF;
    if (s_gp0_stats_enabled) {
        s_gp0_stats[op]++;
    }
    
    // Fill Rectangle in VRAM
    if (op == 0x02) {
        uint8_t r = (uint8_t)(gpu->gp0_fifo[0] & 0xFF);
        uint8_t g = (uint8_t)((gpu->gp0_fifo[0] >> 8) & 0xFF);
        uint8_t b = (uint8_t)((gpu->gp0_fifo[0] >> 16) & 0xFF);
        int16_t x = (int16_t)(gpu->gp0_fifo[1] & 0xFFFF);
        int16_t y = (int16_t)(gpu->gp0_fifo[1] >> 16);
        int16_t w = (int16_t)(gpu->gp0_fifo[2] & 0xFFFF);
        int16_t h = (int16_t)(gpu->gp0_fifo[2] >> 16);
        ps1_gpu_clear_vram(gpu, x, y, w, h, r, g, b);
        PS1_LOG(2, "[GPU 02] Clear VRAM: (%d, %d)-(%dx%d) color=(%d,%d,%d)\n", x, y, w, h, r, g, b);
        return;
    }

    // Polygons
    if (op >= 0x20 && op <= 0x3F) {
        bool is_quad = (op & (1 << 3)) != 0;
        bool is_textured = (op & (1 << 2)) != 0;
        bool is_gouraud = (op & (1 << 4)) != 0;
        bool semi_trans = (op & (1 << 1)) != 0;

        PS1Vertex v[4];
        memset(v, 0, sizeof(v));

        int idx = 0;
        uint8_t cr = (uint8_t)(gpu->gp0_fifo[0] & 0xFF);
        uint8_t cg = (uint8_t)((gpu->gp0_fifo[0] >> 8) & 0xFF);
        uint8_t cb = (uint8_t)((gpu->gp0_fifo[0] >> 16) & 0xFF);

        int num_v = is_quad ? 4 : 3;
        for (int i = 0; i < num_v; i++) {
            if (is_gouraud) {
                if (i > 0) {
                    uint32_t c = gpu->gp0_fifo[idx++];
                    cr = (uint8_t)(c & 0xFF);
                    cg = (uint8_t)((c >> 8) & 0xFF);
                    cb = (uint8_t)((c >> 16) & 0xFF);
                } else {
                    idx++;
                }
            } else if (i == 0) {
                idx++;
            }

            v[i].r = cr; v[i].g = cg; v[i].b = cb;
            uint32_t pos = gpu->gp0_fifo[idx++];
            v[i].x = gp0_coord(pos & 0xFFFF) + gpu->draw_offset_x;
            v[i].y = gp0_coord(pos >> 16) + gpu->draw_offset_y;

            if (is_textured) {
                uint32_t uv = gpu->gp0_fifo[idx++];
                v[i].u = (uint8_t)(uv & 0xFF);
                v[i].v = (uint8_t)((uv >> 8) & 0xFF);
                if (i == 0) {
                    gpu->clut_x = (uint16_t)(((uv >> 16) & 0x3F) * 16);
                    gpu->clut_y = (uint16_t)((uv >> 22) & 0x1FF);
                } else if (i == 1) {
                    uint16_t tpage = (uint16_t)((uv >> 16) & 0x1FF);
                    gpu->tex_page_x = tpage & 0x0F;
                    gpu->tex_page_y = (tpage >> 4) & 0x01;
                    gpu->semi_trans_mode = (uint8_t)((tpage >> 5) & 0x03);
                    gpu->tex_color_mode = (uint8_t)((tpage >> 7) & 0x03);
                    gpu->status = (gpu->status & ~0x01FFu) | (tpage & 0x01FFu);
                }
            }
        }

        if (s_gp0_stats_enabled && s_logged_polys < 5) {
            s_logged_polys++;
            printf("[GP0 Poly #%d Op=0x%02X] %s %s vertices:", s_logged_polys, op, is_quad ? "Quad" : "Tri", is_textured ? "Textured" : "Flat");
            for (int i = 0; i < num_v; i++) {
                printf(" v%d=(%d, %d)", i, v[i].x, v[i].y);
            }
            printf("\n");
        }

        bool raw_texture = (op & 1) != 0;
        ps1_gpu_draw_triangle(gpu, &v[0], &v[1], &v[2], is_textured, is_gouraud, semi_trans, raw_texture);
        if (is_quad) {
            ps1_gpu_draw_triangle(gpu, &v[1], &v[2], &v[3], is_textured, is_gouraud, semi_trans, raw_texture);
        }
        return;
    }

    // Lines
    if (op >= 0x40 && op <= 0x5F) {
        bool is_poly = (op & (1 << 3)) != 0;
        bool is_gouraud = (op & (1 << 4)) != 0;
        bool semi_trans = (op & (1 << 1)) != 0;

        if (!is_poly) {
            PS1Vertex v0, v1;
            memset(&v0, 0, sizeof(v0));
            memset(&v1, 0, sizeof(v1));
            uint8_t cr = (uint8_t)(gpu->gp0_fifo[0] & 0xFF);
            uint8_t cg = (uint8_t)((gpu->gp0_fifo[0] >> 8) & 0xFF);
            uint8_t cb = (uint8_t)((gpu->gp0_fifo[0] >> 16) & 0xFF);
            v0.r = cr; v0.g = cg; v0.b = cb;
            uint32_t pos0 = gpu->gp0_fifo[1];
            v0.x = gp0_coord(pos0 & 0xFFFF);
            v0.y = gp0_coord(pos0 >> 16);
            int idx = 2;
            if (is_gouraud) {
                uint32_t c1 = gpu->gp0_fifo[idx++];
                v1.r = (uint8_t)(c1 & 0xFF);
                v1.g = (uint8_t)((c1 >> 8) & 0xFF);
                v1.b = (uint8_t)((c1 >> 16) & 0xFF);
            } else {
                v1.r = cr; v1.g = cg; v1.b = cb;
            }
            uint32_t pos1 = gpu->gp0_fifo[idx++];
            v1.x = gp0_coord(pos1 & 0xFFFF);
            v1.y = gp0_coord(pos1 >> 16);
            ps1_gpu_draw_line(gpu, &v0, &v1, is_gouraud, semi_trans);
        } else {
            // Poly-line (variable length)
            PS1Vertex v_prev, v_curr;
            memset(&v_prev, 0, sizeof(v_prev));
            memset(&v_curr, 0, sizeof(v_curr));
            uint8_t cr = (uint8_t)(gpu->gp0_fifo[0] & 0xFF);
            uint8_t cg = (uint8_t)((gpu->gp0_fifo[0] >> 8) & 0xFF);
            uint8_t cb = (uint8_t)((gpu->gp0_fifo[0] >> 16) & 0xFF);
            v_prev.r = cr; v_prev.g = cg; v_prev.b = cb;
            uint32_t pos0 = gpu->gp0_fifo[1];
            v_prev.x = gp0_coord(pos0 & 0xFFFF);
            v_prev.y = gp0_coord(pos0 >> 16);

            int idx = 2;
            while (idx < gpu->gp0_fifo_len) {
                if ((gpu->gp0_fifo[idx] & 0xF000F000) == 0x50005000 || gpu->gp0_fifo[idx] == 0x55555555) {
                    break;
                }
                if (is_gouraud) {
                    if (idx + 1 >= gpu->gp0_fifo_len) break;
                    uint32_t c = gpu->gp0_fifo[idx++];
                    v_curr.r = (uint8_t)(c & 0xFF);
                    v_curr.g = (uint8_t)((c >> 8) & 0xFF);
                    v_curr.b = (uint8_t)((c >> 16) & 0xFF);
                } else {
                    v_curr.r = cr; v_curr.g = cg; v_curr.b = cb;
                }
                uint32_t pos = gpu->gp0_fifo[idx++];
                v_curr.x = gp0_coord(pos & 0xFFFF);
                v_curr.y = gp0_coord(pos >> 16);
                ps1_gpu_draw_line(gpu, &v_prev, &v_curr, is_gouraud, semi_trans);
                v_prev = v_curr;
            }
        }
        return;
    }

    // Rectangles / Sprites
    if (op >= 0x60 && op <= 0x7F) {
        bool is_textured = (op & (1 << 2)) != 0;
        bool semi_trans = (op & (1 << 1)) != 0;
        uint8_t r = (uint8_t)(gpu->gp0_fifo[0] & 0xFF);
        uint8_t g = (uint8_t)((gpu->gp0_fifo[0] >> 8) & 0xFF);
        uint8_t b = (uint8_t)((gpu->gp0_fifo[0] >> 16) & 0xFF);

        int16_t x = gp0_coord(gpu->gp0_fifo[1] & 0xFFFF);
        int16_t y = gp0_coord(gpu->gp0_fifo[1] >> 16);
        uint8_t u = 0, v = 0;
        int idx = 2;
        if (is_textured) {
            u = (uint8_t)(gpu->gp0_fifo[idx] & 0xFF);
            v = (uint8_t)((gpu->gp0_fifo[idx] >> 8) & 0xFF);
            gpu->clut_x = (uint16_t)(((gpu->gp0_fifo[idx] >> 16) & 0x3F) * 16);
            gpu->clut_y = (uint16_t)((gpu->gp0_fifo[idx] >> 22) & 0x1FF);
            idx++;
        }

        uint8_t size_type = (op >> 3) & 3;
        int16_t w = 1, h = 1;
        if (size_type == 1) { w = 1; h = 1; }
        else if (size_type == 2) { w = 8; h = 8; }
        else if (size_type == 3) { w = 16; h = 16; }
        else {
            w = (int16_t)(gpu->gp0_fifo[idx] & 0x3FF);
            h = (int16_t)((gpu->gp0_fifo[idx] >> 16) & 0x1FF);
        }

        bool raw_texture = (op & 1) != 0;
        ps1_gpu_draw_rect(gpu, x, y, w, h, r, g, b, u, v, is_textured, semi_trans, raw_texture);
        return;
    }

    // VRAM to VRAM Copy (Move Image)
    if (op >= 0x80 && op <= 0x9F) {
        uint16_t sx = (uint16_t)(gpu->gp0_fifo[1] & 0x3FF);
        uint16_t sy = (uint16_t)((gpu->gp0_fifo[1] >> 16) & 0x1FF);
        uint16_t dx = (uint16_t)(gpu->gp0_fifo[2] & 0x3FF);
        uint16_t dy = (uint16_t)((gpu->gp0_fifo[2] >> 16) & 0x1FF);
        uint16_t w  = (uint16_t)(gpu->gp0_fifo[3] & 0x3FF);
        uint16_t h  = (uint16_t)((gpu->gp0_fifo[3] >> 16) & 0x1FF);
        if (w == 0) w = 1024;
        if (h == 0) h = 512;
        for (uint16_t y = 0; y < h; y++) {
            for (uint16_t x = 0; x < w; x++) {
                gpu->vram[(dy + y) & 0x1FF][(dx + x) & 0x3FF] =
                    gpu->vram[(sy + y) & 0x1FF][(sx + x) & 0x3FF];
            }
        }
        return;
    }

    // CPU to VRAM
    if (op >= 0xA0 && op <= 0xBF) {
        gpu->transfer.x = (uint16_t)(gpu->gp0_fifo[1] & 0x3FF);
        gpu->transfer.y = (uint16_t)((gpu->gp0_fifo[1] >> 16) & 0x1FF);
        uint16_t tw = (uint16_t)(gpu->gp0_fifo[2] & 0x3FF);
        uint16_t th = (uint16_t)((gpu->gp0_fifo[2] >> 16) & 0x1FF);
        gpu->transfer.w = (tw == 0) ? 1024 : tw;
        gpu->transfer.h = (th == 0) ? 512 : th;
        gpu->transfer.cur_x = gpu->transfer.x;
        gpu->transfer.cur_y = gpu->transfer.y;
        gpu->transfer.active = true;
        gpu->transfer.is_read = false;
        return;
    }

    // VRAM to CPU (StoreImage)
    if (op >= 0xC0 && op <= 0xDF) {
        gpu->transfer.x = (uint16_t)(gpu->gp0_fifo[1] & 0x3FF);
        gpu->transfer.y = (uint16_t)((gpu->gp0_fifo[1] >> 16) & 0x1FF);
        uint16_t tw = (uint16_t)(gpu->gp0_fifo[2] & 0x3FF);
        uint16_t th = (uint16_t)((gpu->gp0_fifo[2] >> 16) & 0x1FF);
        gpu->transfer.w = (tw == 0) ? 1024 : tw;
        gpu->transfer.h = (th == 0) ? 512 : th;
        gpu->transfer.cur_x = gpu->transfer.x;
        gpu->transfer.cur_y = gpu->transfer.y;
        gpu->transfer.active = true;
        gpu->transfer.is_read = true;
        return;
    }

    // Draw Mode / Texpage
    if (op == 0xE1) {
        uint32_t val = gpu->gp0_fifo[0];
        gpu->tex_page_x = (uint16_t)(val & 0x0F);
        gpu->tex_page_y = (uint16_t)((val >> 4) & 0x01);
        gpu->semi_trans_mode = (uint8_t)((val >> 5) & 0x03);
        gpu->tex_color_mode = (uint8_t)((val >> 7) & 0x03);
        gpu->dither_enable = (val & (1 << 9)) != 0;
        gpu->tex_disable = (val & (1 << 11)) != 0;
        gpu->status = (gpu->status & ~0x07FFu) | (val & 0x07FFu);
        return;
    }

    // Texture Window Setting
    if (op == 0xE2) {
        uint32_t val = gpu->gp0_fifo[0];
        gpu->tex_win_mask_x = (uint8_t)(val & 0x1F);
        gpu->tex_win_mask_y = (uint8_t)((val >> 5) & 0x1F);
        gpu->tex_win_off_x  = (uint8_t)((val >> 10) & 0x1F);
        gpu->tex_win_off_y  = (uint8_t)((val >> 15) & 0x1F);
        return;
    }

    // Draw Area Top-Left
    if (op == 0xE3) {
        uint32_t val = gpu->gp0_fifo[0];
        gpu->clip_x1 = (int16_t)(val & 0x3FF);
        gpu->clip_y1 = (int16_t)((val >> 10) & 0x1FF);
        PS1_LOG(2, "[GPU] E3 Set Clip Top-Left: (%d, %d)\n", gpu->clip_x1, gpu->clip_y1);
        return;
    }

    // Draw Area Bottom-Right
    if (op == 0xE4) {
        uint32_t val = gpu->gp0_fifo[0];
        gpu->clip_x2 = (int16_t)(val & 0x3FF);
        gpu->clip_y2 = (int16_t)((val >> 10) & 0x1FF);
        PS1_LOG(2, "[GPU] E4 Set Clip Bottom-Right: (%d, %d)\n", gpu->clip_x2, gpu->clip_y2);
        return;
    }

    // Draw Offset
    if (op == 0xE5) {
        uint32_t val = gpu->gp0_fifo[0];
        int16_t ox = (int16_t)(val & 0x7FF);
        int16_t oy = (int16_t)((val >> 11) & 0x7FF);
        if (ox & 0x400) ox |= 0xF800; // Sign extend 11-bit
        if (oy & 0x400) oy |= 0xF800;
        gpu->draw_offset_x = ox;
        gpu->draw_offset_y = oy;
        return;
    }

    // Mask Bit Setting
    if (op == 0xE6) {
        uint32_t val = gpu->gp0_fifo[0];
        gpu->status = (gpu->status & ~(3 << 11)) | ((val & 3) << 11);
        return;
    }
}

void ps1_gpu_write_gp0(PS1GPU* gpu, uint32_t val) {
    if (gpu->transfer.active && !gpu->transfer.is_read) {
        uint16_t p0 = (uint16_t)(val & 0xFFFF);
        uint16_t p1 = (uint16_t)(val >> 16);
        
        gpu->vram[gpu->transfer.cur_y % 512][gpu->transfer.cur_x % 1024] = p0;
        gpu->transfer.cur_x++;
        if (gpu->transfer.cur_x >= (gpu->transfer.x + gpu->transfer.w)) {
            gpu->transfer.cur_x = gpu->transfer.x;
            gpu->transfer.cur_y++;
            if (gpu->transfer.cur_y >= (gpu->transfer.y + gpu->transfer.h)) {
                gpu->transfer.active = false;
                return;
            }
        }
        
        gpu->vram[gpu->transfer.cur_y % 512][gpu->transfer.cur_x % 1024] = p1;
        gpu->transfer.cur_x++;
        if (gpu->transfer.cur_x >= (gpu->transfer.x + gpu->transfer.w)) {
            gpu->transfer.cur_x = gpu->transfer.x;
            gpu->transfer.cur_y++;
            if (gpu->transfer.cur_y >= (gpu->transfer.y + gpu->transfer.h)) {
                gpu->transfer.active = false;
            }
        }
        return;
    }

    if (gpu->gp0_fifo_len == 0) {
        gpu->gp0_cmd = val;
        gpu->gp0_words_needed = get_gp0_cmd_len(val);
    }

    uint8_t op = (gpu->gp0_cmd >> 24) & 0xFF;
    bool is_polyline = (op >= 0x48 && op <= 0x4F) || (op >= 0x58 && op <= 0x5F);

    if (is_polyline && gpu->gp0_fifo_len >= 3) {
        if ((val & 0xF000F000) == 0x50005000 || val == 0x55555555) {
            execute_gp0_packet(gpu);
            gpu->gp0_fifo_len = 0;
            return;
        }
    }

    if (gpu->gp0_fifo_len < 64) {
        gpu->gp0_fifo[gpu->gp0_fifo_len++] = val;
    }
    if (!is_polyline && gpu->gp0_fifo_len >= gpu->gp0_words_needed) {
        execute_gp0_packet(gpu);
        gpu->gp0_fifo_len = 0;
    } else if (is_polyline && gpu->gp0_fifo_len >= 64) {
        execute_gp0_packet(gpu);
        gpu->gp0_fifo_len = 0;
    }
}

void ps1_gpu_write_gp1(PS1GPU* gpu, uint32_t val) {
    uint8_t cmd = (val >> 24) & 0xFF;
    switch (cmd) {
        case 0x00: // Reset GPU
            ps1_gpu_reset(gpu);
            break;
        case 0x01: // Reset FIFO
            gpu->gp0_fifo_len = 0;
            break;
        case 0x02: // Acknowledge GPU Interrupt
            gpu->status &= ~GPUSTAT_INTERRUPT_REQ;
            break;
        case 0x03: // Display Enable (val & 1: 1=Disable/blank, 0=Enable)
            if (val & 1) gpu->status |= GPUSTAT_DISPLAY_ENABLE;
            else gpu->status &= ~GPUSTAT_DISPLAY_ENABLE;
            gpu->disp_enable = (val & 1) == 0;
            break;
        case 0x04: // DMA Direction / Data Request
            gpu->status = (gpu->status & ~GPUSTAT_DMA_DIR) | ((val & 3) << 29);
            if ((val & 3) != 0) {
                gpu->status |= GPUSTAT_DMA_DATA_REQ;
            } else {
                gpu->status &= ~GPUSTAT_DMA_DATA_REQ;
            }
            break;
        case 0x05: // Display Start
            gpu->disp_x = (int16_t)(val & 0x3FE); // 2-pixel aligned in hardware
            gpu->disp_y = (int16_t)((val >> 10) & 0x1FF);
            memcpy(gpu->front_vram, gpu->vram, sizeof(gpu->vram));
            gpu->front_disp_x = gpu->disp_x;
            gpu->front_disp_y = gpu->disp_y;
            gpu->front_disp_w = gpu->disp_w;
            gpu->front_disp_h = gpu->disp_h;
            break;
        case 0x06: // Horizontal Display Range
            break;
        case 0x07: // Vertical Display Range (bits 0..9 = Y1, bits 10..19 = Y2)
            gpu->disp_y1 = (int16_t)(val & 0x3FF);
            gpu->disp_y2 = (int16_t)((val >> 10) & 0x3FF);
            break;
        case 0x08: // Display Mode
            {
                int hres_code = (val & 3) | ((val & (1 << 6)) >> 4);
                if (hres_code == 0) gpu->disp_w = 256;
                else if (hres_code == 1) gpu->disp_w = 320;
                else if (hres_code == 2) gpu->disp_w = 512;
                else if (hres_code == 3) gpu->disp_w = 640;
                else if (hres_code == 4) gpu->disp_w = 368;
                
                gpu->disp_h = (val & (1 << 2)) ? 480 : 240;
                gpu->is_pal = (val & (1 << 3)) != 0;
                gpu->is_24bit = (val & (1 << 4)) != 0;

                // Update GPUSTAT bits 16..22
                gpu->status = (gpu->status & ~(0x7F << 16)) | ((val & 0x7F) << 16);
            }
            break;
        case 0x10: // Get GPU Info
            switch (val & 0x0F) {
                case 0x02:
                    gpu->gpu_read_latch = ((uint32_t)gpu->tex_win_mask_x) |
                                          ((uint32_t)gpu->tex_win_mask_y << 5) |
                                          ((uint32_t)gpu->tex_win_off_x << 10) |
                                          ((uint32_t)gpu->tex_win_off_y << 15);
                    break;
                case 0x03:
                    gpu->gpu_read_latch = ((uint32_t)gpu->clip_x1 & 0x3FF) |
                                          (((uint32_t)gpu->clip_y1 & 0x1FF) << 10);
                    break;
                case 0x04:
                    gpu->gpu_read_latch = ((uint32_t)gpu->clip_x2 & 0x3FF) |
                                          (((uint32_t)gpu->clip_y2 & 0x1FF) << 10);
                    break;
                case 0x05:
                    gpu->gpu_read_latch = ((uint32_t)gpu->draw_offset_x & 0x7FF) |
                                          (((uint32_t)gpu->draw_offset_y & 0x7FF) << 11);
                    break;
                case 0x07:
                    gpu->gpu_read_latch = 2; // GPU Version 2 (standard retail PS1 GPU)
                    break;
                default:
                    gpu->gpu_read_latch = 0;
                    break;
            }
            break;
    }
}

void ps1_gpu_vblank(PS1GPU* gpu) {
    memcpy(gpu->front_vram, gpu->vram, sizeof(gpu->vram));
    gpu->front_disp_x = gpu->disp_x;
    gpu->front_disp_y = gpu->disp_y;
    gpu->front_disp_w = gpu->disp_w;
    gpu->front_disp_h = gpu->disp_h;

    check_gp0_stats_env();
    if (!s_gp0_stats_enabled) return;

    static int s_vblank_count = 0;
    s_vblank_count++;
    if (s_vblank_count >= 60) {
        s_vblank_count = 0;
        printf("\n=== [GP0 Histogram (60 VBlanks)] ===\n");
        bool any = false;
        for (int i = 0; i < 256; i++) {
            if (s_gp0_stats[i] > 0) {
                printf("  Op 0x%02X: %u\n", i, s_gp0_stats[i]);
                any = true;
            }
        }
        if (!any) {
            printf("  (no GP0 commands)\n");
        }
        printf("=====================================\n\n");
        memset(s_gp0_stats, 0, sizeof(s_gp0_stats));
        s_logged_polys = 0;
    }
}
