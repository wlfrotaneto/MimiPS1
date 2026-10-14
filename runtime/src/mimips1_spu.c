#include "mimips1_spu.h"
#include <string.h>
#include <stdlib.h>

static const int16_t f_k0[5] = { 0, 60, 115, 98, 122 };
static const int16_t f_k1[5] = { 0, 0, -52, -55, -60 };

void ps1_spu_init(PS1SPU* spu) {
    memset(spu, 0, sizeof(PS1SPU));
    spu->master_vol_left = 0x3FFF;
    spu->master_vol_right = 0x3FFF;
    spu->cd_vol_left = 0x7FFF;
    spu->cd_vol_right = 0x7FFF;
}

void ps1_spu_reset(PS1SPU* spu) {
    ps1_spu_init(spu);
}

uint16_t ps1_spu_read(PS1SPU* spu, uint32_t addr) {
    uint32_t offset = addr & 0x3FF;
    if (offset < 0x180) {
        int v = offset / 16;
        int reg = (offset % 16) / 2;
        SPUVoice* voice = &spu->voices[v];
        switch (reg) {
            case 0: return (uint16_t)voice->vol_left;
            case 1: return (uint16_t)voice->vol_right;
            case 2: return voice->sample_rate;
            case 3: return (uint16_t)(voice->start_addr >> 3);
            case 4: return voice->adsr_low;
            case 5: return voice->adsr_high;
            case 6: return (uint16_t)(voice->adsr_level >> 16);
            case 7: return (uint16_t)(voice->loop_addr >> 3);
        }
    } else if (offset == 0x180) {
        return (uint16_t)spu->master_vol_left;
    } else if (offset == 0x182) {
        return (uint16_t)spu->master_vol_right;
    } else if (offset == 0x19C) {
        uint16_t s = 0;
        for (int i = 0; i < 16; i++) {
            if (spu->voices[i].active && spu->voices[i].adsr_phase != ADSR_PHASE_OFF) s |= (1 << i);
        }
        return s;
    } else if (offset == 0x19E) {
        uint16_t s = 0;
        for (int i = 0; i < 8; i++) {
            if (spu->voices[16 + i].active && spu->voices[16 + i].adsr_phase != ADSR_PHASE_OFF) s |= (1 << i);
        }
        return s;
    } else if (offset == 0x1A6) {
        return (uint16_t)(spu->transfer_addr >> 3);
    } else if (offset == 0x1A8) {
        uint16_t val = 0;
        if (spu->transfer_addr < (PS1_SPU_RAM_SIZE - 1)) {
            val = *(uint16_t*)&spu->ram[spu->transfer_addr];
            spu->transfer_addr = (spu->transfer_addr + 2) & 0x7FFFF;
        }
        return val;
    } else if (offset == 0x1AA) {
        return spu->ctrl;
    } else if (offset == 0x1AE) {
        return spu->stat;
    } else if (offset == 0x1B0) {
        return (uint16_t)spu->cd_vol_left;
    } else if (offset == 0x1B2) {
        return (uint16_t)spu->cd_vol_right;
    }
    return 0;
}

void ps1_spu_write(PS1SPU* spu, uint32_t addr, uint16_t val) {
    uint32_t offset = addr & 0x3FF;
    if (offset < 0x180) {
        int v = offset / 16;
        int reg = (offset % 16) / 2;
        SPUVoice* voice = &spu->voices[v];
        switch (reg) {
            case 0: voice->vol_left = (int16_t)val; break;
            case 1: voice->vol_right = (int16_t)val; break;
            case 2: voice->sample_rate = val; break;
            case 3: voice->start_addr = (uint32_t)val << 3; break;
            case 4: voice->adsr_low = val; break;
            case 5: voice->adsr_high = val; break;
            case 7: voice->loop_addr = (uint32_t)val << 3; break;
        }
    } else if (offset == 0x180) {
        spu->master_vol_left = (int16_t)val;
    } else if (offset == 0x182) {
        spu->master_vol_right = (int16_t)val;
    } else if (offset == 0x188) {
        // Key ON low
        for (int i = 0; i < 16; i++) {
            if (val & (1 << i)) {
                spu->voices[i].current_addr = spu->voices[i].start_addr;
                spu->voices[i].adsr_phase = ADSR_PHASE_ATTACK;
                spu->voices[i].adsr_level = 0;
                spu->voices[i].active = true;
                spu->voices[i].prev1 = 0;
                spu->voices[i].prev2 = 0;
            }
        }
    } else if (offset == 0x18A) {
        // Key ON high
        for (int i = 0; i < 8; i++) {
            if (val & (1 << i)) {
                spu->voices[16 + i].current_addr = spu->voices[16 + i].start_addr;
                spu->voices[16 + i].adsr_phase = ADSR_PHASE_ATTACK;
                spu->voices[16 + i].adsr_level = 0;
                spu->voices[16 + i].active = true;
                spu->voices[16 + i].prev1 = 0;
                spu->voices[16 + i].prev2 = 0;
            }
        }
    } else if (offset == 0x18C) {
        // Key OFF low
        for (int i = 0; i < 16; i++) {
            if (val & (1 << i)) {
                spu->voices[i].adsr_phase = ADSR_PHASE_RELEASE;
            }
        }
    } else if (offset == 0x18E) {
        // Key OFF high
        for (int i = 0; i < 8; i++) {
            if (val & (1 << i)) {
                spu->voices[16 + i].adsr_phase = ADSR_PHASE_RELEASE;
            }
        }
    } else if (offset == 0x1A6) {
        spu->transfer_addr = (uint32_t)val << 3;
    } else if (offset == 0x1A8) {
        // SPU Data FIFO write
        if (spu->transfer_addr < (PS1_SPU_RAM_SIZE - 1)) {
            *(uint16_t*)&spu->ram[spu->transfer_addr] = val;
            spu->transfer_addr = (spu->transfer_addr + 2) & 0x7FFFF;
        }
    } else if (offset == 0x1AA) {
        spu->ctrl = val;
        spu->stat = (val & 0x003F);
    } else if (offset == 0x1B0) {
        spu->cd_vol_left = (int16_t)val;
    } else if (offset == 0x1B2) {
        spu->cd_vol_right = (int16_t)val;
    }
}

static int16_t decode_adpcm_sample(SPUVoice* voice, uint8_t nibble, uint8_t shift, uint8_t filter) {
    int32_t sample = (int8_t)(nibble << 4) >> 4; // Sign-extend 4-bit
    sample = (sample << (12 - shift));

    int32_t f = ((int32_t)voice->prev1 * f_k0[filter] + (int32_t)voice->prev2 * f_k1[filter]) >> 6;
    sample += f;
    if (sample > 32767) sample = 32767;
    else if (sample < -32768) sample = -32768;

    voice->prev2 = voice->prev1;
    voice->prev1 = (int16_t)sample;
    return (int16_t)sample;
}

void ps1_spu_generate_samples(PS1SPU* spu, int16_t* output_stereo, int num_samples) {
    memset(output_stereo, 0, num_samples * 2 * sizeof(int16_t));

    for (int s = 0; s < num_samples; s++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;

        for (int v = 0; v < SPU_NUM_VOICES; v++) {
            SPUVoice* voice = &spu->voices[v];
            if (!voice->active || voice->adsr_phase == ADSR_PHASE_OFF) continue;

            // ADSR update
            if (voice->adsr_phase == ADSR_PHASE_ATTACK) {
                voice->adsr_level += 0x800;
                if (voice->adsr_level >= 0x7FFF) {
                    voice->adsr_level = 0x7FFF;
                    voice->adsr_phase = ADSR_PHASE_DECAY;
                }
            } else if (voice->adsr_phase == ADSR_PHASE_DECAY) {
                voice->adsr_level -= 0x200;
                if (voice->adsr_level <= 0x4000) {
                    voice->adsr_level = 0x4000;
                    voice->adsr_phase = ADSR_PHASE_SUSTAIN;
                }
            } else if (voice->adsr_phase == ADSR_PHASE_RELEASE) {
                voice->adsr_level -= 0x400;
                if (voice->adsr_level <= 0) {
                    voice->adsr_level = 0;
                    voice->adsr_phase = ADSR_PHASE_OFF;
                    voice->active = false;
                    continue;
                }
            }

            // Simple sample synthesis
            uint32_t block_addr = (voice->current_addr & ~15) % PS1_SPU_RAM_SIZE;
            uint8_t shift = spu->ram[block_addr] & 0x0F;
            uint8_t filter = (spu->ram[block_addr] >> 4) & 0x07;
            if (filter > 4) filter = 0;

            int nibble_idx = (voice->current_addr % 14) * 2;
            uint8_t byte = spu->ram[(block_addr + 2 + (nibble_idx / 2)) % PS1_SPU_RAM_SIZE];
            uint8_t nibble = (nibble_idx & 1) ? (byte >> 4) : (byte & 0x0F);

            int16_t sample = decode_adpcm_sample(voice, nibble, shift, filter);
            int32_t amp = (sample * voice->adsr_level) >> 15;

            mix_l += (amp * voice->vol_left) >> 15;
            mix_r += (amp * voice->vol_right) >> 15;

            voice->sample_counter += voice->sample_rate;
            if (voice->sample_counter >= 0x1000) {
                voice->sample_counter -= 0x1000;
                voice->current_addr++;
                if ((voice->current_addr % 16) == 0) {
                    uint8_t flags = spu->ram[(voice->current_addr - 15) % PS1_SPU_RAM_SIZE];
                    if (flags & 1) { // Loop end
                        voice->current_addr = voice->loop_addr;
                    }
                }
            }
        }

        mix_l = (mix_l * spu->master_vol_left) >> 15;
        mix_r = (mix_r * spu->master_vol_right) >> 15;

        if (mix_l > 32767) mix_l = 32767; else if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767) mix_r = 32767; else if (mix_r < -32768) mix_r = -32768;

        output_stereo[s * 2 + 0] = (int16_t)mix_l;
        output_stereo[s * 2 + 1] = (int16_t)mix_r;
    }
}
