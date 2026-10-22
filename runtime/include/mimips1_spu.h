#ifndef MIMIPS1_SPU_H
#define MIMIPS1_SPU_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_NUM_VOICES 24

typedef enum {
    ADSR_PHASE_ATTACK,
    ADSR_PHASE_DECAY,
    ADSR_PHASE_SUSTAIN,
    ADSR_PHASE_RELEASE,
    ADSR_PHASE_OFF
} SPUAdsrPhase;

typedef struct {
    uint16_t vol_left_reg;      // raw volume registers (bit15 = sweep mode)
    uint16_t vol_right_reg;
    uint16_t sample_rate;       // Pitch (0x1000 = 44100Hz)
    uint32_t start_addr;        // Address in SPU RAM
    uint32_t current_addr;      // Start of the ADPCM block being played
    uint32_t loop_addr;

    // ADSR configuration
    uint16_t adsr_low;
    uint16_t adsr_high;
    SPUAdsrPhase adsr_phase;
    int32_t  adsr_level;        // 0..0x7FFF
    uint32_t adsr_counter;

    // ADPCM decode state: one block (28 samples) at a time plus interpolation history
    int16_t  prev1;
    int16_t  prev2;
    int16_t  block[28];
    int16_t  hist[4];           // last decoded samples, oldest first
    uint32_t pitch_counter;     // 12-bit fractional position between hist[2] and hist[3]
    uint8_t  block_pos;         // next sample of `block` to push into hist
    bool     block_valid;
    int16_t  last_out;          // for pitch modulation of the next voice

    bool     active;
} SPUVoice;

struct PS1SPU {
    uint8_t  ram[PS1_SPU_RAM_SIZE]; // 512KB Sound RAM
    SPUVoice voices[SPU_NUM_VOICES];

    uint16_t main_vol_left;
    uint16_t main_vol_right;
    int16_t  cd_vol_left;
    int16_t  cd_vol_right;

    uint32_t pmon_bits;         // pitch modulation
    uint32_t noise_bits;
    uint32_t reverb_bits;
    uint32_t endx_bits;
    uint32_t noise_level;
    uint32_t noise_counter;

    uint16_t ctrl;
    uint16_t stat;
    uint32_t transfer_addr;
    uint16_t irq_addr;
    uint16_t regs[0x200];       // raw mirror of the register file (reverb setup, unmodelled registers)

    // CoreAudio / Output audio ring buffer
    int16_t  out_buffer[4096 * 2]; // Stereo samples
    int      out_write_pos;
    int      out_read_pos;
};

void     ps1_spu_init(PS1SPU* spu);
void     ps1_spu_reset(PS1SPU* spu);
uint16_t ps1_spu_read(PS1SPU* spu, uint32_t addr);
void     ps1_spu_write(PS1SPU* spu, uint32_t addr, uint16_t val);

// Render audio samples (called by audio thread or per-frame)
void     ps1_spu_generate_samples(PS1SPU* spu, int16_t* output_stereo, int num_samples);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_SPU_H
