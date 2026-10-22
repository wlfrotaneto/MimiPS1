#include "mimips1_input.h"
#include "mimips1_runtime.h"
#include "mimips1_card.h"
#include <string.h>

void ps1_input_init(PS1Input* input) {
    memset(input, 0, sizeof(PS1Input));
    input->stat = 0x0005; // TX Ready / RX FIFO empty
    input->card_flag = 0x08;
    for (int i = 0; i < 2; i++) {
        input->pads[i].buttons = 0xFFFF; // Active low: all released
        input->pads[i].left_stick_x = 128;
        input->pads[i].left_stick_y = 128;
        input->pads[i].right_stick_x = 128;
        input->pads[i].right_stick_y = 128;
    }
}

void ps1_input_reset(PS1Input* input) {
    ps1_input_init(input);
}

#define MIN_PRESS_FRAMES 3

static int btn_index(PS1Button btn) {
    for (int i = 0; i < 16; i++) if ((uint16_t)btn == (1u << i)) return i;
    return -1;
}

void ps1_input_set_button(PS1Input* input, int pad_idx, PS1Button btn, bool pressed) {
    if (pad_idx < 0 || pad_idx > 1) return;
    int bi = btn_index(btn);
    if (pressed) {
        if (bi >= 0 && (input->pads[pad_idx].buttons & (uint16_t)btn)) input->press_at[pad_idx][bi] = input->vblank_count; // newly down
        if (bi >= 0) input->release_pending[pad_idx] &= ~(uint16_t)btn;
        input->pads[pad_idx].buttons &= ~(uint16_t)btn; // Active low
    } else if (!(input->pads[pad_idx].buttons & (uint16_t)btn)) {          // currently down
        if (bi >= 0 && input->vblank_count - input->press_at[pad_idx][bi] < MIN_PRESS_FRAMES) {
            input->release_pending[pad_idx] |= (uint16_t)btn;               // too short: release later
        } else {
            input->pads[pad_idx].buttons |= (uint16_t)btn;
        }
    }
}

void ps1_input_vblank(PS1Input* input) {
    input->vblank_count++;
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < 16; i++) {
            uint16_t bit = (uint16_t)(1u << i);
            if ((input->release_pending[p] & bit) && input->vblank_count - input->press_at[p][i] >= MIN_PRESS_FRAMES) {
                input->release_pending[p] &= ~bit;
                input->pads[p].buttons |= bit;
            }
        }
    }
}

void ps1_input_set_stick(PS1Input* input, int pad_idx, bool right_stick, uint8_t x, uint8_t y) {
    if (pad_idx < 0 || pad_idx > 1) return;
    if (right_stick) {
        input->pads[pad_idx].right_stick_x = x;
        input->pads[pad_idx].right_stick_y = y;
    } else {
        input->pads[pad_idx].left_stick_x = x;
        input->pads[pad_idx].left_stick_y = y;
    }
}

void ps1_input_set_analog(PS1Input* input, int pad_idx, bool enabled) {
    if (pad_idx < 0 || pad_idx > 1) return;
    input->pads[pad_idx].is_analog = enabled;
}

void ps1_input_set_rumble_callback(PS1Input* input, PS1RumbleCallback cb, void* userdata) {
    input->rumble_cb = cb;
    input->rumble_userdata = userdata;
}

// ---------------------------------------------------------------------------------------------
// Memory card (selected with 81h instead of 01h)

static uint8_t card_exchange(PS1Input* in, uint8_t tx, bool* more) {
    PS1CardManager* mgr = (PS1CardManager*)in->cards;
    int slot = in->card_slot;
    uint8_t rx = 0xFF;
    *more = true;
    int i = in->card_idx++;

    if (i == 0) { // command byte
        in->card_cmd = tx;
        if (tx != 'R' && tx != 'W' && tx != 'S') { in->pad_state = 0; *more = false; return 0xFF; }
        return in->card_flag;
    }
    switch (in->card_cmd) {
    case 'S': // Get ID
        {
            static const uint8_t id[] = { 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80 };
            if (i - 1 < (int)sizeof(id)) rx = id[i - 1];
            if (i - 1 >= (int)sizeof(id) - 1) { in->pad_state = 0; *more = false; }
            return rx;
        }
    case 'R':
        if (i == 1) return 0x5A;
        if (i == 2) return 0x5D;
        if (i == 3) { in->card_frame = (uint16_t)(tx << 8); return 0x00; }
        if (i == 4) {
            in->card_frame |= tx;
            in->card_chk = (uint8_t)(in->card_frame >> 8) ^ (uint8_t)in->card_frame;
            if (in->card_frame >= PS1_CARD_NUM_FRAMES) { in->pad_state = 0; *more = false; return (uint8_t)(in->card_frame >> 8); }
            memset(in->card_buf, 0, sizeof(in->card_buf));
            ps1_card_read_frame(mgr, slot, in->card_frame, in->card_buf);
            return (uint8_t)(in->card_frame >> 8);
        }
        if (i == 5) return 0x5C;
        if (i == 6) return 0x5D;
        if (i == 7) return (uint8_t)(in->card_frame >> 8);
        if (i == 8) return (uint8_t)in->card_frame;
        if (i < 9 + 128) { uint8_t b = in->card_buf[i - 9]; in->card_chk ^= b; return b; }
        if (i == 9 + 128) return in->card_chk;
        in->pad_state = 0; *more = false;
        return 'G';
    case 'W':
        if (i == 1) return 0x5A;
        if (i == 2) return 0x5D;
        if (i == 3) { in->card_frame = (uint16_t)(tx << 8); return 0x00; }
        if (i == 4) { in->card_frame |= tx; in->card_chk = (uint8_t)(in->card_frame >> 8) ^ (uint8_t)in->card_frame; return (uint8_t)(in->card_frame >> 8); }
        if (i < 5 + 128) { in->card_buf[i - 5] = tx; in->card_chk ^= tx; return in->card_prev_tx; }
        if (i == 5 + 128) { in->card_rx_chk = tx; return in->card_prev_tx; }   // received checksum
        if (i == 6 + 128) return 0x5C;
        if (i == 7 + 128) return 0x5D;
        {
            uint8_t status = 'G';
            if (in->card_frame >= PS1_CARD_NUM_FRAMES) status = 0xFF;
            else if (in->card_rx_chk != in->card_chk) status = 'N';
            else {
                PS1_LOG(2, "[Card] write frame %u\n", in->card_frame);
                ps1_card_write_frame(mgr, slot, in->card_frame, in->card_buf);
                in->card_flag = 0x00;
                in->card_pending_save = true;
                in->card_idle_cycles = 0;
            }
            in->pad_state = 0; *more = false;
            return status;
        }
    }
    in->pad_state = 0; *more = false;
    return rx;
}

static uint8_t exchange_byte(PS1Input* input, uint8_t tx) {
    uint8_t rx = 0xFF;
    int pad_idx = (input->ctrl & (1 << 13)) ? 1 : 0;
    PS1Controller* p = &input->pads[pad_idx];

    switch (input->pad_state) {
        case 0: // Idle
            if (tx == 0x01) { // Select controller
                input->pad_state = 1;
                rx = 0xFF;
            }
            else if (tx == 0x81) { // Memory card
                PS1CardManager* mgr = (PS1CardManager*)input->cards;
                if (mgr && mgr->cards[pad_idx].loaded && !getenv("MIMIPS1_NOCARD")) {
                    PS1_LOG(3, "[Card] port %d selected\n", pad_idx);
                    input->pad_state = 20;
                    input->card_slot = pad_idx;
                    input->card_idx = 0;
                    rx = 0xFF;
                }
            }
            // anything else: no device answers, so no ACK
            break;
        case 20: { // memory card transaction
            bool more;
            rx = card_exchange(input, tx, &more);
            PS1_LOG(3, "[CardIO] %c i=%d tx=%02X rx=%02X\n", input->card_cmd, input->card_idx - 1, tx, rx);
            (void)more;
            input->card_prev_tx = tx;
            break;
        }
        case 1:
            if (tx == 0x42) { // Poll
                input->pad_state = 2;
                rx = p->is_analog ? 0x73 : 0x41; // Pad ID
            } else {
                input->pad_state = 0;
            }
            break;
        case 2:
            rx = 0x5A; // Acknowledge byte
            input->pad_state = 3;
            break;
        case 3:
            rx = (uint8_t)(p->buttons & 0xFF); // Buttons low byte
            if (p->is_analog) {
                p->rumble_small = (tx == 0x01) ? 1 : 0;
            }
            input->pad_state = 4;
            break;
        case 4:
            rx = (uint8_t)((p->buttons >> 8) & 0xFF); // Buttons high byte
            if (p->is_analog) {
                p->rumble_large = tx;
                if (input->rumble_cb) {
                    input->rumble_cb(pad_idx, p->rumble_small, p->rumble_large, input->rumble_userdata);
                }
                input->pad_state = 5;
            } else {
                input->pad_state = 0;
            }
            break;
        case 5: rx = p->right_stick_x; input->pad_state = 6; break;
        case 6: rx = p->right_stick_y; input->pad_state = 7; break;
        case 7: rx = p->left_stick_x; input->pad_state = 8; break;
        case 8: rx = p->left_stick_y; input->pad_state = 0; break;
        default:
            input->pad_state = 0;
            break;
    }
    return rx;
}

// A byte takes ~8 bit times at the programmed baud rate; the pad then pulses /ACK
// on every byte except the last of a transaction.
#define SIO_BYTE_CYCLES 1400

uint16_t ps1_input_read16(PS1Input* input, uint32_t addr) {
    uint32_t reg = addr & 0x0F;
    if (reg == 0x00) { // RX Data (reading pops the FIFO)
        input->stat &= ~0x0002;
        return input->rx_data;
    } else if (reg == 0x04) { // STAT
        return input->stat;
    } else if (reg == 0x08) { // MODE
        return input->mode;
    } else if (reg == 0x0A) { // CTRL
        return input->ctrl;
    } else if (reg == 0x0E) { // BAUD
        return input->baud;
    }
    return 0;
}

void ps1_input_write16(PS1Input* input, uint32_t addr, uint16_t val) {
    uint32_t reg = addr & 0x0F;
    if (reg == 0x00) { // TX Data
        if (!(input->ctrl & 0x0001)) return; // TXEN off
        input->tx_data = (uint8_t)val;
        input->rx_data = exchange_byte(input, input->tx_data);
        input->xfer_ack = (input->pad_state != 0);
        input->xfer_busy = true;
        input->xfer_cycles = SIO_BYTE_CYCLES;
        input->stat &= ~(0x0001 | 0x0004 | 0x0080); // TX busy, DSR released
    } else if (reg == 0x08) {
        input->mode = val;
    } else if (reg == 0x0A) {
        input->ctrl = val & ~0x0050; // Bits 4 (ACK) and 6 (RESET) are commands, not stored
        if (val & (1 << 4)) { // Acknowledge: clears IRQ and error flags
            input->stat &= ~(0x0008 | 0x0010 | 0x0020 | 0x0200);
        }
        if (val & (1 << 6)) { // Reset
            input->pad_state = 0;
            input->xfer_busy = false;
            input->stat = 0x0005;
            input->ctrl = 0;
            input->mode = 0;
        }
        if (!(val & 0x0002)) { // DTR released: deselects the device
            input->pad_state = 0;
        }
    } else if (reg == 0x0E) {
        input->baud = val;
    }
}

void ps1_input_tick(PS1Context* ctx, uint32_t cycles) {
    PS1Input* in = &ctx->input;
    if (in->ack_cycles) {
        if (in->ack_cycles > cycles) in->ack_cycles -= cycles;
        else { in->ack_cycles = 0; in->stat &= ~0x0080; }   // /ACK released
    }
    if (in->card_pending_save) {
        in->card_idle_cycles += cycles;
        if (in->card_idle_cycles > PS1_CPU_CLOCK_HZ) { // a second without writes: flush to disk
            in->card_pending_save = false;
            ps1_card_save(&ctx->card_mgr, in->card_slot);
        }
    }
    if (!in->xfer_busy) return;
    if (in->xfer_cycles > cycles) {
        in->xfer_cycles -= cycles;
        return;
    }
    in->xfer_busy = false;
    in->stat |= 0x0001 | 0x0004 | 0x0002;   // TX ready, RX byte available
    if (in->xfer_ack) {
        in->stat |= 0x0080 | 0x0200;        // DSR (/ACK) and IRQ flag
        in->ack_cycles = 300;
        if (in->ctrl & (1 << 12)) {         // DSR interrupt enable
            ps1_trigger_irq(ctx, IRQ_CONTROLLER);
        }
    }
}
