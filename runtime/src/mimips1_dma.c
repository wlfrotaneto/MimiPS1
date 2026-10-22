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

static void mdec_out_pump(PS1Context* ctx, DMAChannel* ch) {
    uint32_t start = ch->pending_addr;
    while (ch->pending_words && ch->pending_addr < PS1_RAM_SIZE && ctx->mdec.out_pos < ctx->mdec.out_len) {
        *(uint32_t*)&ctx->memory.ram[ch->pending_addr] = ps1_mdec_read_data(&ctx->mdec);
        ch->pending_addr += 4;
        ch->pending_words--;
    }
    if (ch->pending_addr > start) ps1_native_check_range(ctx, start, ch->pending_addr - start);
}

void ps1_dma_execute(PS1Context* ctx, int channel_idx) {
    DMAChannel* ch = &ctx->dma.channels[channel_idx];
    uint32_t sync_mode = (ch->chcr >> 9) & 0x03;
    ctx->dma.last_channel = channel_idx;
    ctx->dma.last_madr = ch->madr;
    ctx->dma.last_bcr = ch->bcr;
    uint32_t words_moved = 0;
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

                words_moved += packet_words + 1;
                for (uint32_t i = 1; i <= packet_words; i++) {
                    uint32_t data_addr = (cur_addr + (i * 4)) & 0x1FFFFC;
                    if (data_addr < PS1_RAM_SIZE) {
                        uint32_t word = *(uint32_t*)&ctx->memory.ram[data_addr];
                        ps1_gpu_write_gp0(&ctx->gpu, word);
                    }
                }
                if (next_addr == 0x00FFFFFF || next_addr == 0 || (next_addr & 0x00800000) != 0 || header == 0) break;
                cur_addr = next_addr & 0x1FFFFC;
            }
        } else {
            // Block Mode (sync_mode == 0: immediate block, sync_mode == 1: slice/request)
            uint32_t bs = ch->bcr & 0xFFFF;
            if (bs == 0) bs = 0x10000;
            uint32_t word_count;
            if (sync_mode == 1) {
                uint32_t ba = ch->bcr >> 16;
                if (ba == 0) ba = 1;
                word_count = bs * ba;
            } else {
                word_count = bs;
            }
            uint32_t addr = ch->madr & 0x1FFFFC;
            bool to_ram = (ch->chcr & 1) == 0;
            words_moved = word_count;

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
        words_moved = count;

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
        uint32_t count = ch->bcr & 0xFFFF;
        if (count == 0) count = 0x800 / 4; // Hardware quirk: BCR=0 transfers 0x800 bytes
        words_moved = count * 6; // CD DMA is slow (~24 cycles/word)
        uint32_t addr = ch->madr & 0x1FFFFC;
        for (uint32_t i = 0; i < count && addr < PS1_RAM_SIZE; i++) {
            if (ctx->cdrom.data_bytes_avail >= 4) {
                uint32_t w = *(uint32_t*)&ctx->cdrom.data_fifo[ctx->cdrom.data_read_pos];
                ctx->cdrom.data_read_pos += 4;
                ctx->cdrom.data_bytes_avail -= 4;
                *(uint32_t*)&ctx->memory.ram[addr] = w;
            }
            addr += 4;
        }
    }
    // SPU DMA Channel (4)
    else if (channel_idx == DMA_SPU) {
        uint32_t count = (ch->bcr & 0xFFFF) * (ch->bcr >> 16 ? (ch->bcr >> 16) : 1);
        words_moved = count * 4;
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
    // MDEC DMA Channels (0 = RAM -> MDEC, 1 = MDEC -> RAM)
    else if (channel_idx == DMA_MDEC_IN || channel_idx == DMA_MDEC_OUT) {
        uint32_t bs = ch->bcr & 0xFFFF;
        uint32_t ba = ch->bcr >> 16;
        uint32_t words = (sync_mode == 1) ? bs * (ba ? ba : 1) : bs;
        if (words == 0) words = 0x10000;
        words_moved = words * 2;
        uint32_t addr = ch->madr & 0x1FFFFC;
        if (channel_idx == DMA_MDEC_IN) {
            for (uint32_t i = 0; i < words && addr < PS1_RAM_SIZE; i++, addr += 4) {
                ps1_mdec_write_command(&ctx->mdec, *(uint32_t*)&ctx->memory.ram[addr]);
            }
        } else {
            // MDEC-out is request driven: it moves data as the decoder produces it and
            // waits otherwise. The transfer may be armed before the input has been decoded.
            ch->pending_words = words;
            ch->pending_addr = addr;
            mdec_out_pump(ctx, ch);
            if (ch->pending_words) {
                ch->waiting = true;
                ch->chcr |= (1u << 24);
                ch->chcr &= ~(1u << 28);
                return;   // completion is reported once all words have been delivered
            }
        }
        PS1_LOG(2, "[MDEC] DMA%d words=%u out_len=%u out_pos=%u\n", channel_idx, words, ctx->mdec.out_len, ctx->mdec.out_pos);
    }

    // Transfers that wrote RAM must not leave stale recompiled code behind
    if (channel_idx == DMA_CDROM || channel_idx == DMA_GPU || channel_idx == DMA_OTC) {
        uint32_t span = (words_moved ? words_moved : 1) * 4;
        if (channel_idx == DMA_CDROM) span = (words_moved / 6) * 4;
        ps1_native_check_range(ctx, ch->madr & 0x1FFFFC, span ? span : 4);
    }

    // The data has moved, but completion (busy flag, DICR flag, IRQ3) is reported after the
    // time the hardware would need. Guests routinely finish their own bookkeeping after
    // kicking a transfer, so reporting completion immediately would run their callback first.
    ch->chcr |= (1u << 24);
    ch->chcr &= ~(1u << 28);
    ch->completing = true;
    ch->done_cycles = words_moved + 40;
}

static void dma_complete(PS1Context* ctx, int channel_idx) {
    DMAChannel* ch = &ctx->dma.channels[channel_idx];
    ch->chcr &= ~(1u << 24); // Clear busy flag
    ch->completing = false;

    // Deliver SPU completion event if applicable (HLE kernel only)
    if (channel_idx == DMA_SPU && !ctx->bios_boot) {
        ps1_bios_deliver_event(ctx, 0xF0000009, 0x0020);
    }

    // Channel interrupt flag (bit 24 + ch) is only latched when that channel's enable bit (16 + ch) is set
    if ((ctx->dma.dicr >> (16 + channel_idx)) & 1) {
        ctx->dma.dicr |= (1u << (24 + channel_idx));
    }

    bool was_master = (ctx->dma.dicr & (1u << 31)) != 0;
    bool force_irq = (ctx->dma.dicr & (1 << 15)) != 0;
    bool master_en = (ctx->dma.dicr & (1 << 23)) != 0;
    uint32_t enables = (ctx->dma.dicr >> 16) & 0x7F;
    uint32_t flags = (ctx->dma.dicr >> 24) & 0x7F;
    if (force_irq || (master_en && ((enables & flags) != 0))) {
        ctx->dma.dicr |= (1u << 31);
        if (!was_master) ps1_trigger_irq(ctx, IRQ_DMA);
    } else {
        ctx->dma.dicr &= ~(1u << 31);
    }
}

void ps1_dma_tick(PS1Context* ctx, uint32_t cycles) {
    for (int i = 0; i < 7; i++) {
        DMAChannel* ch = &ctx->dma.channels[i];
        if (ch->waiting) {
            mdec_out_pump(ctx, ch);
            if (ch->pending_words == 0) {
                ch->waiting = false;
                ch->completing = true;
                ch->done_cycles = 64;
            }
            continue;
        }
        if (!ch->completing) continue;
        if (ch->done_cycles > cycles) {
            ch->done_cycles -= cycles;
        } else {
            ch->done_cycles = 0;
            dma_complete(ctx, i);
        }
    }
}
