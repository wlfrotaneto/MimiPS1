#ifndef MIMIPS1_GPU_H
#define MIMIPS1_GPU_H

#include "mimips1_types.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

// GPUSTAT Register Bits
#define GPUSTAT_TEX_PAGE_X_BASE     (0xF << 0)
#define GPUSTAT_TEX_PAGE_Y_BASE     (1 << 4)
#define GPUSTAT_SEMI_TRANS_MODE     (3 << 5)
#define GPUSTAT_TEX_PAGE_COLORS     (3 << 7)
#define GPUSTAT_DITHER_ENABLE       (1 << 9)
#define GPUSTAT_DRAW_TO_DISPLAY     (1 << 10)
#define GPUSTAT_MASK_BIT            (1 << 11)
#define GPUSTAT_PRESERVE_MASKED     (1 << 12)
#define GPUSTAT_INTERLACE_FIELD     (1 << 13)
#define GPUSTAT_REVERSE_FLAG        (1 << 14)
#define GPUSTAT_TEX_DISABLE         (1 << 15)
#define GPUSTAT_HORIZ_RES2          (1 << 16)
#define GPUSTAT_HORIZ_RES1          (3 << 17)
#define GPUSTAT_VERT_RES            (1 << 19)
#define GPUSTAT_PAL_MODE            (1 << 20)
#define GPUSTAT_COLOR_DEPTH_24BPP   (1 << 21)
#define GPUSTAT_VERTICAL_INTERLACE  (1 << 22)
#define GPUSTAT_DISPLAY_ENABLE      (1 << 23)
#define GPUSTAT_INTERRUPT_REQ       (1 << 24)
#define GPUSTAT_DMA_DATA_REQ        (1 << 25)
#define GPUSTAT_READY_FOR_CMD       (1 << 26)
#define GPUSTAT_READY_FOR_VRAM      (1 << 27)
#define GPUSTAT_READY_FOR_DMA       (1 << 28)
#define GPUSTAT_DMA_DIR             (3 << 29)

// Semi-transparency blend modes:
// 0: 0.5 * B + 0.5 * F
// 1: 1.0 * B + 1.0 * F
// 2: 1.0 * B - 1.0 * F
// 3: 1.0 * B + 0.25 * F

typedef struct {
    int16_t x, y;
    uint8_t u, v;
    uint8_t r, g, b;
} PS1Vertex;

typedef struct {
    uint16_t vram[PS1_VRAM_HEIGHT][PS1_VRAM_WIDTH];
    bool     is_24bit;
    int16_t  disp_x, disp_y, disp_w, disp_h;
    int16_t  disp_y1, disp_y2;
} PS1GPUFrame;

struct PS1GPU {
    uint16_t vram[PS1_VRAM_HEIGHT][PS1_VRAM_WIDTH]; // 1024 x 512 16-bit Framebuffer
    
    // Status and Command processing
    uint32_t status;
    uint32_t gpu_read_latch;
    uint32_t gp0_fifo[64];
    int      gp0_fifo_len;
    int      gp0_words_needed;
    uint32_t gp0_cmd;

    // Drawing settings
    int16_t  draw_offset_x;
    int16_t  draw_offset_y;
    int16_t  clip_x1, clip_y1;
    int16_t  clip_x2, clip_y2;
    
    // Texture window & page
    uint16_t tex_page_x;
    uint16_t tex_page_y;
    uint8_t  tex_color_mode; // 0: 4-bit, 1: 8-bit, 2: 15-bit direct
    uint8_t  semi_trans_mode;
    uint16_t clut_x, clut_y;
    bool     tex_disable;
    bool     dither_enable;
    uint8_t  tex_win_mask_x, tex_win_mask_y;
    uint8_t  tex_win_off_x, tex_win_off_y;

    // Display Area
    int16_t  disp_x, disp_y;
    int16_t  disp_w, disp_h;
    int16_t  disp_y1, disp_y2;
    bool     disp_enable;
    bool     is_pal;
    bool     is_24bit;
    bool     interlace_field;

    // Double buffering snapshot & display crop
    // The emulation thread publishes a snapshot per video frame (triple buffered, lock free);
    // the presenter acquires the latest one and reads it without ever seeing a half-copied frame.
    PS1GPUFrame frames[3];
    _Atomic int frame_latest;   // index of the newest complete frame, -1 if none yet
    _Atomic int frame_reading;  // index the presenter is using, -1 if none

    // VRAM Image transfer state
    struct {
        uint16_t x, y, w, h;
        uint16_t cur_x, cur_y;
        bool     active;
        bool     is_read;
    } transfer;
};

void ps1_gpu_init(PS1GPU* gpu);
// Presenter side: newest published frame (NULL before the first); release it when done
const PS1GPUFrame* ps1_gpu_acquire_frame(PS1GPU* gpu);
void ps1_gpu_release_frame(PS1GPU* gpu);
void ps1_gpu_reset(PS1GPU* gpu);

// Port Access
uint32_t ps1_gpu_read_status(PS1GPU* gpu);
uint32_t ps1_gpu_read_data(PS1GPU* gpu);
void     ps1_gpu_write_gp0(PS1GPU* gpu, uint32_t val);
void     ps1_gpu_write_gp1(PS1GPU* gpu, uint32_t val);
void     ps1_gpu_vblank(PS1GPU* gpu);

// Rasterizer Primitives
void ps1_gpu_draw_triangle(PS1GPU* gpu, const PS1Vertex* v0, const PS1Vertex* v1, const PS1Vertex* v2, bool textured, bool gouraud, bool semi_trans, bool raw_texture);
void ps1_gpu_draw_rect(PS1GPU* gpu, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t u, uint8_t v, bool textured, bool semi_trans, bool raw_texture);
void ps1_gpu_draw_line(PS1GPU* gpu, const PS1Vertex* v0, const PS1Vertex* v1, bool gouraud, bool semi_trans);
void ps1_gpu_clear_vram(PS1GPU* gpu, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b);

// Texture pixel sampling
uint16_t ps1_gpu_sample_texture(PS1GPU* gpu, int u, int v);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_GPU_H
