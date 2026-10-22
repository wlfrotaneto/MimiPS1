#ifndef MIMIPS1_DMA_H
#define MIMIPS1_DMA_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DMA_MDEC_IN  = 0,
    DMA_MDEC_OUT = 1,
    DMA_GPU      = 2,
    DMA_CDROM    = 3,
    DMA_SPU      = 4,
    DMA_PIO      = 5,
    DMA_OTC      = 6
} DMAChannelID;

typedef struct {
    uint32_t madr;  // Memory Address Register
    uint32_t bcr;   // Block Control Register
    uint32_t chcr;  // Channel Control Register
    bool     active;
} DMAChannel;

struct PS1DMA {
    DMAChannel channels[7];
    uint32_t   dpcr; // Control Register
    uint32_t   dicr; // Interrupt Register
    int        last_channel;   // Debug: most recent transfer
    uint32_t   last_madr, last_bcr;
};

void     ps1_dma_init(PS1DMA* dma);
void     ps1_dma_reset(PS1DMA* dma);
uint32_t ps1_dma_read(PS1Context* ctx, uint32_t addr);
void     ps1_dma_write(PS1Context* ctx, uint32_t addr, uint32_t val);

// Execute DMA transfer
void     ps1_dma_execute(PS1Context* ctx, int channel_idx);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_DMA_H
