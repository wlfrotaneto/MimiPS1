#include "mimips1_input.h"
#include "mimips1_runtime.h"
#include <string.h>

void ps1_input_init(PS1Input* input) {
    memset(input, 0, sizeof(PS1Input));
    input->stat = 0x0005; // TX Ready / RX FIFO empty
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

void ps1_input_set_button(PS1Input* input, int pad_idx, PS1Button btn, bool pressed) {
    if (pad_idx < 0 || pad_idx > 1) return;
    if (pressed) {
        input->pads[pad_idx].buttons &= ~(uint16_t)btn; // Active low
    } else {
        input->pads[pad_idx].buttons |= (uint16_t)btn;
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
            // 0x81 (memory card) and anything else: no device answers, so no ACK
            break;
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
    if (!in->xfer_busy) return;
    if (in->xfer_cycles > cycles) {
        in->xfer_cycles -= cycles;
        return;
    }
    in->xfer_busy = false;
    in->stat |= 0x0001 | 0x0004 | 0x0002;   // TX ready, RX byte available
    if (in->xfer_ack) {
        in->stat |= 0x0080 | 0x0200;        // DSR (/ACK) and IRQ flag
        if (in->ctrl & (1 << 12)) {         // DSR interrupt enable
            ps1_trigger_irq(ctx, IRQ_CONTROLLER);
        }
    }
}
