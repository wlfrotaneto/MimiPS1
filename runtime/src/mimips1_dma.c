#include "mimips1_dma.h"
#include "mimips1_runtime.h"
#include <string.h>
#include <stdio.h>

void ps1_dma_init(PS1DMA* dma) {
    memset(dma, 0, sizeof(PS1DMA));
    dma->dpcr = 0x07654321; // Default DMA priority
}

void ps1_dma_reset(PS1DMA* dma) {
    ps1_dma_init(dma);
}

uint32_t ps1_dma_read(PS1Context* ctx, uint32_t addr) {
    int channel = (addr - 0x1F801080) / 0x10;
    int reg = (addr & 0x0F);

    if (channel < 7) {
        if (reg == 0x00) return ctx->dma.channels[channel].madr;
        if (reg == 0x04) return ctx->dma.channels[channel].bcr;
        if (reg == 0x08) return ctx->dma.channels[channel].chcr;
    } else if (addr == 0x1F8010F0) {
        return ctx->dma.dpcr;
    } else if (addr == 0x1F8010F4) {
        return ctx->dma.dicr;
    }
    return 0;
}

void ps1_dma_write(PS1Context* ctx, uint32_t addr, uint32_t val) {
    int channel = (addr - 0x1F801080) / 0x10;
    int reg = (addr & 0x0F);

    if (channel < 7) {
        if (reg == 0x00) ctx->dma.channels[channel].madr = val & 0x00FFFFFF;
        else if (reg == 0x04) ctx->dma.channels[channel].bcr = val;
        else if (reg == 0x08) {
            ctx->dma.channels[channel].chcr = val;
            if ((val & (1 << 24)) || (val & (1 << 28))) {
                ps1_dma_execute(ctx, channel);
            }
        }
    } else if (addr == 0x1F8010F0) {
        ctx->dma.dpcr = val;
    } else if (addr == 0x1F8010F4) {
        // Bits 0-23: writable (enables, force IRQ, etc.)
        // Bits 24-30: write-1-to-clear (acknowledge channel IRQ flags)
        uint32_t current_flags = (ctx->dma.dicr >> 24) & 0x7F;
        uint32_t ack_flags = (val >> 24) & 0x7F;
        current_flags &= ~ack_flags;

        ctx->dma.dicr = (val & 0x00FFFFFF) | (current_flags << 24);

        // Recalculate Master Interrupt Flag (bit 31)
        bool force_irq = (ctx->dma.dicr & (1 << 15)) != 0;
        bool master_en = (ctx->dma.dicr & (1 << 23)) != 0;
        uint32_t enables = (ctx->dma.dicr >> 16) & 0x7F;
        uint32_t flags = (ctx->dma.dicr >> 24) & 0x7F;
        if (force_irq || (master_en && ((enables & flags) != 0))) {
            ctx->dma.dicr |= (1u << 31);
        } else {
            ctx->dma.dicr &= ~(1u << 31);
        }
    }
}

void ps1_dma_execute(PS1Context* ctx, int channel_idx) {
    DMAChannel* ch = &ctx->dma.channels[channel_idx];
    uint32_t sync_mode = (ch->chcr >> 9) & 0x03;
    PS1_LOG(2, "[DMA] execute ch=%d sync=%u madr=0x%08X bcr=0x%08X chcr=0x%08X\n", channel_idx, sync_mode, ch->madr, ch->bcr, ch->chcr);

    // GPU DMA Channel (2)
    if (channel_idx == DMA_GPU) {
        if (sync_mode == 2) {
            // Linked List Mode (Ordering Tables)
            uint32_t cur_addr = ch->madr & 0x1FFFFC;
            int max_packets = 65536;
            while (cur_addr < (PS1_RAM_SIZE - 4) && --max_packets > 0) {
                uint32_t header = *(uint32_t*)&ctx->memory.ram[cur_addr];
                uint32_t packet_words = (header >> 24) & 0xFF;
                uint32_t next_addr = header & 0x00FFFFFF;

                if (!ctx->gpu.transfer.active) {
                    ctx->gpu.gp0_fifo_len = 0;
                }

                for (uint32_t i = 1; i <= packet_words; i++) {
                    uint32_t data_addr = (cur_addr + (i * 4)) & 0x1FFFFC;
                    if (data_addr < PS1_RAM_SIZE) {
                        uint32_t word = *(uint32_t*)&ctx->memory.ram[data_addr];
                        ps1_gpu_write_gp0(&ctx->gpu, word);
                    }
                }
                if (next_addr == 0x00FFFFFF || (next_addr & 0x800000) != 0 || next_addr == 0) break;
                cur_addr = next_addr & 0x1FFFFC;
            }
        } else {
            // Block Mode
            uint32_t word_count = (ch->bcr & 0xFFFF) * (ch->bcr >> 16 ? (ch->bcr >> 16) : 1);
            uint32_t addr = ch->madr & 0x1FFFFC;
            bool to_ram = (ch->chcr & 1) == 0;

            for (uint32_t i = 0; i < word_count && addr < PS1_RAM_SIZE; i++) {
                if (to_ram) {
                    *(uint32_t*)&ctx->memory.ram[addr] = ps1_gpu_read_data(&ctx->gpu);
                } else {
                    ps1_gpu_write_gp0(&ctx->gpu, *(uint32_t*)&ctx->memory.ram[addr]);
                }
                addr = (ch->chcr & (1 << 1)) ? (addr - 4) : (addr + 4);
            }
        }
    }
    // OTC DMA Channel (6) - Clear Ordering Table
    else if (channel_idx == DMA_OTC) {
        uint32_t count = ch->bcr & 0xFFFF;
        uint32_t addr = ch->madr & 0x1FFFFC;

        for (uint32_t i = 0; i < count && addr < PS1_RAM_SIZE; i++) {
            if (i == (count - 1)) {
                *(uint32_t*)&ctx->memory.ram[addr] = 0x00FFFFFF; // End marker
            } else {
                *(uint32_t*)&ctx->memory.ram[addr] = (addr - 4) & 0x00FFFFFF;
            }
            addr -= 4;
        }
    }
    // CD-ROM DMA Channel (3)
    else if (channel_idx == DMA_CDROM) {
        uint32_t count = (ch->bcr & 0xFFFF) * (ch->bcr >> 16 ? (ch->bcr >> 16) : 1);
        uint32_t addr = ch->madr & 0x1FFFFC;
        PS1_LOG(2, "[DMA3] madr=0x%08X count=%u avail=%d pos=%d\n", addr, count, ctx->cdrom.data_bytes_avail, ctx->cdrom.data_read_pos);
        for (uint32_t i = 0; i < count && addr < PS1_RAM_SIZE; i++) {
            if (ctx->cdrom.data_bytes_avail >= 4) {
                uint32_t w = *(uint32_t*)&ctx->cdrom.data_fifo[ctx->cdrom.data_read_pos];
                ctx->cdrom.data_read_pos += 4;
                ctx->cdrom.data_bytes_avail -= 4;
                *(uint32_t*)&ctx->memory.ram[addr] = w;
                uint32_t fn_idx = addr >> 2;
                if (fn_idx < PS1_FUNC_TABLE_SIZE) {
                    ctx->cpu.func_table[fn_idx] = NULL;
                }
                if (i == 0) PS1_LOG(2, "[DMA3 First Word] 0x%08X\n", w);
            }
            addr += 4;
        }
    }
    // SPU DMA Channel (4)
    else if (channel_idx == DMA_SPU) {
        uint32_t count = (ch->bcr & 0xFFFF) * (ch->bcr >> 16 ? (ch->bcr >> 16) : 1);
        uint32_t addr = ch->madr & 0x1FFFFC;
        bool to_spu = (ch->chcr & 1) != 0;
        for (uint32_t i = 0; i < count && addr < (PS1_RAM_SIZE - 3); i++) {
            if (to_spu) {
                uint16_t w0 = *(uint16_t*)&ctx->memory.ram[addr];
                uint16_t w1 = *(uint16_t*)&ctx->memory.ram[addr + 2];
                if (ctx->spu.transfer_addr < (PS1_SPU_RAM_SIZE - 3)) {
                    *(uint16_t*)&ctx->spu.ram[ctx->spu.transfer_addr] = w0;
                    *(uint16_t*)&ctx->spu.ram[ctx->spu.transfer_addr + 2] = w1;
                    ctx->spu.transfer_addr = (ctx->spu.transfer_addr + 4) & 0x7FFFF;
                }
            }
            addr += 4;
        }
    }
    // MDEC DMA Channels (0, 1)
    else if (channel_idx == DMA_MDEC_IN || channel_idx == DMA_MDEC_OUT) {
        // Dummy MDEC transfer completion
    }

    // Finish DMA
    ch->chcr &= ~(1 << 24); // Clear busy flag
    ch->chcr &= ~(1 << 28); // Clear manual trigger flag

    // Deliver SPU completion event if applicable
    if (channel_idx == DMA_SPU) {
        ps1_bios_deliver_event(ctx, 0xF0000009, 0x0020);
    }

    // Set channel interrupt flag in DICR (bit 24 + channel_idx)
    ctx->dma.dicr |= (1u << (24 + channel_idx));

    // Recalculate Master Interrupt Flag (bit 31)
    bool force_irq = (ctx->dma.dicr & (1 << 15)) != 0;
    bool master_en = (ctx->dma.dicr & (1 << 23)) != 0;
    uint32_t enables = (ctx->dma.dicr >> 16) & 0x7F;
    uint32_t flags = (ctx->dma.dicr >> 24) & 0x7F;
    if (force_irq || (master_en && ((enables & flags) != 0))) {
        ctx->dma.dicr |= (1u << 31);
        ps1_trigger_irq(ctx, IRQ_DMA);
    }
}
