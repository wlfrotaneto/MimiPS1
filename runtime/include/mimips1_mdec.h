#ifndef MIMIPS1_MDEC_H
#define MIMIPS1_MDEC_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDEC_OUT_CAPACITY (1024 * 1024)  // enough for several 24-bit macroblock rows

struct PS1MDEC {
    // Control / status
    uint32_t control;          // last write to 0x1F801824
    bool     dma_in_enable;
    bool     dma_out_enable;

    // Tables loaded with commands 2 and 3
    uint8_t  quant[2][64];     // [0] luminance, [1] chrominance
    int16_t  scale[64];

    // Command in progress
    uint32_t cmd;              // 0 = none, else command id (1 decode, 2 quant, 3 scale)
    uint32_t cmd_word;
    uint32_t params_left;      // words still expected
    uint32_t params_total;
    uint16_t rl[0x10000 * 2];  // run-length halfwords of the current decode command
    uint32_t rl_len;
    uint8_t  tbl_bytes[128];
    uint32_t tbl_len;

    // Decoded output FIFO (bytes)
    uint8_t  out[MDEC_OUT_CAPACITY];
    uint32_t out_len;
    uint32_t out_pos;
    uint32_t out_depth;        // 0 = 4bit, 1 = 8bit, 2 = 24bit, 3 = 15bit
};

void     ps1_mdec_init(PS1MDEC* m);
void     ps1_mdec_reset(PS1MDEC* m);
void     ps1_mdec_write_command(PS1MDEC* m, uint32_t word);   // 0x1F801820
void     ps1_mdec_write_control(PS1MDEC* m, uint32_t word);   // 0x1F801824
uint32_t ps1_mdec_read_data(PS1MDEC* m);                      // 0x1F801820
uint32_t ps1_mdec_read_status(PS1MDEC* m);                    // 0x1F801824

// DMA helpers: words moved in either direction
void     ps1_mdec_dma_in(PS1MDEC* m, const uint32_t* words, uint32_t count);
uint32_t ps1_mdec_dma_out(PS1MDEC* m, uint32_t* words, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_MDEC_H
