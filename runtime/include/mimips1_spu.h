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
    int16_t  vol_left;
    int16_t  vol_right;
    uint16_t sample_rate;       // Pitch (0x1000 = 44100Hz)
    uint32_t start_addr;        // Address in SPU RAM
    uint32_t current_addr;
    uint32_t loop_addr;
    
    // ADSR configuration
    uint16_t adsr_low;
    uint16_t adsr_high;
    SPUAdsrPhase adsr_phase;
    int32_t  adsr_level;

    // ADPCM decode state
    int16_t  prev1;
    int16_t  prev2;
    uint32_t sample_counter;    // Fixed-point counter for pitch resampling
    uint8_t  sample_in_block;   // 0..27 samples within 16-byte block
    int16_t  current_decoded_sample;

    bool     key_on;
    bool     key_off;
    bool     looping;
    bool     active;
} SPUVoice;

struct PS1SPU {
    uint8_t  ram[PS1_SPU_RAM_SIZE]; // 512KB Sound RAM
    SPUVoice voices[SPU_NUM_VOICES];
    
    int16_t  master_vol_left;
    int16_t  master_vol_right;
    int16_t  cd_vol_left;
    int16_t  cd_vol_right;

    uint32_t key_on_bits;
    uint32_t key_off_bits;
    uint32_t voice_status;

    uint16_t ctrl;
    uint16_t stat;
    uint32_t transfer_addr;
    uint16_t irq_addr;
    
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
