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

    PS1RumbleCallback rumble_cb;
    void*             rumble_userdata;
};

void     ps1_input_init(PS1Input* input);
void     ps1_input_reset(PS1Input* input);
void     ps1_input_set_button(PS1Input* input, int pad_idx, PS1Button btn, bool pressed);
void     ps1_input_set_stick(PS1Input* input, int pad_idx, bool right_stick, uint8_t x, uint8_t y);
void     ps1_input_set_analog(PS1Input* input, int pad_idx, bool enabled);
void     ps1_input_set_rumble_callback(PS1Input* input, PS1RumbleCallback cb, void* userdata);

uint16_t ps1_input_read16(PS1Input* input, uint32_t addr);
void     ps1_input_write16(PS1Input* input, uint32_t addr, uint16_t val);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_INPUT_H
