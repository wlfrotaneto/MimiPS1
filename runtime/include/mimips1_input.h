#ifndef MIMIPS1_INPUT_H
#define MIMIPS1_INPUT_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PS1RumbleCallback)(int pad_idx, uint8_t small_motor, uint8_t large_motor, void* userdata);

typedef struct {
    uint16_t buttons;       // Bitmask of pressed buttons (PS1Button)
    uint8_t  left_stick_x;  // 0-255 (128 centered)
    uint8_t  left_stick_y;
    uint8_t  right_stick_x;
    uint8_t  right_stick_y;
    bool     is_analog;
    uint8_t  rumble_small;  // 0x00 or 0x01
    uint8_t  rumble_large;  // 0x00..0xFF
} PS1Controller;

struct PS1Input {
    PS1Controller pads[2];
    
    // Serial communication shift register state
    uint8_t  tx_data;
    uint8_t  rx_data;
    uint16_t stat;
    uint16_t mode;
    uint16_t ctrl;
    uint16_t baud;

    int      pad_state;
    int      pad_byte_idx;

    // Byte transfer in flight: completes (RX ready, ACK/IRQ7) after xfer_cycles
    bool     xfer_busy;
    bool     xfer_ack;          // Device pulses /ACK at the end of this byte
    uint32_t xfer_cycles;
    uint32_t ack_cycles;        // /ACK stays low (DSR set) only briefly after a byte

    // A press shorter than the game's polling interval would be lost: releases are deferred until
    // the button has been down for a few video frames
    uint32_t vblank_count;
    uint32_t press_at[2][16];
    uint16_t release_pending[2];
    uint16_t phys[2];           // physical (unstretched) button state
    uint16_t queued[2];         // a tap waiting for the previous one to finish
    uint32_t gap_until[2][16];

    // Memory card transaction (81h ... on the selected port)
    void*    cards;             // PS1CardManager*, set by the runtime
    int      card_slot;
    bool     in_card_xfer;      // the byte being exchanged belongs to a memory card transaction
    uint8_t  card_cmd;          // 'R', 'W' or 'S'
    int      card_idx;          // byte index within the transaction
    uint16_t card_frame;
    uint8_t  card_buf[128];
    uint8_t  card_chk;
    uint8_t  card_flag;         // 0x08 until the first write since power on
    uint8_t  card_prev_tx;      // previous byte sent by the host (echoed during writes)
    uint8_t  card_rx_chk;
    uint64_t card_idle_cycles;  // since the last write; the image is flushed to disk once idle
    bool     card_pending_save;

    PS1RumbleCallback rumble_cb;
    void*             rumble_userdata;
};

void     ps1_input_init(PS1Input* input);
void     ps1_input_reset(PS1Input* input);
void     ps1_input_vblank(PS1Input* input);   // once per video frame: applies deferred releases
void     ps1_input_set_button(PS1Input* input, int pad_idx, PS1Button btn, bool pressed);
void     ps1_input_set_stick(PS1Input* input, int pad_idx, bool right_stick, uint8_t x, uint8_t y);
void     ps1_input_set_analog(PS1Input* input, int pad_idx, bool enabled);
void     ps1_input_set_rumble_callback(PS1Input* input, PS1RumbleCallback cb, void* userdata);

uint16_t ps1_input_read16(PS1Input* input, uint32_t addr);
void     ps1_input_write16(PS1Input* input, uint32_t addr, uint16_t val);
void     ps1_input_tick(PS1Context* ctx, uint32_t cycles);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_INPUT_H
