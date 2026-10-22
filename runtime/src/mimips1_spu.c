#include "mimips1_spu.h"
#include <string.h>
#include <stdlib.h>

static const int16_t f_k0[5] = { 0, 60, 115, 98, 122 };
static const int16_t f_k1[5] = { 0, 0, -52, -55, -60 };

void ps1_spu_init(PS1SPU* spu) {
    memset(spu, 0, sizeof(PS1SPU));
    spu->main_vol_left = 0x3FFF;
    spu->main_vol_right = 0x3FFF;
    spu->noise_level = 1;
    for (int i = 0; i < SPU_NUM_VOICES; i++) spu->voices[i].adsr_phase = ADSR_PHASE_OFF;
}

void ps1_spu_reset(PS1SPU* spu) {
    ps1_spu_init(spu);
}

// Volume registers: bit15 = sweep mode (approximated by its target), otherwise a signed 15-bit value
static inline int32_t vol_from_reg(uint16_t v) {
    if (v & 0x8000) return (int16_t)(((v & 0x7F) << 8) | ((v & 0x4000) ? 0x8000 : 0)) ;
    return (int16_t)(v << 1);
}

static void key_on(PS1SPU* spu, int i) {
    SPUVoice* v = &spu->voices[i];
    v->current_addr = v->start_addr % PS1_SPU_RAM_SIZE;
    v->loop_addr = v->start_addr;
    v->pitch_counter = 0;
    v->adsr_phase = ADSR_PHASE_ATTACK;
    v->adsr_level = 0;
    v->adsr_counter = 0;
    v->active = true;
    v->prev1 = v->prev2 = 0;
    memset(v->hist, 0, sizeof(v->hist));
    v->block_valid = false;
    v->block_pos = 28;
    spu->endx_bits &= ~(1u << i);
}

static void key_off(PS1SPU* spu, int i) {
    SPUVoice* v = &spu->voices[i];
    if (v->active && v->adsr_phase != ADSR_PHASE_OFF) { v->adsr_phase = ADSR_PHASE_RELEASE; v->adsr_counter = 0; }
}

uint16_t ps1_spu_read(PS1SPU* spu, uint32_t addr) {
    uint32_t offset = addr & 0x3FF;
    if (offset < 0x180) {
        int v = offset / 16;
        int reg = (offset % 16) / 2;
        SPUVoice* voice = &spu->voices[v];
        switch (reg) {
            case 0: return voice->vol_left_reg;
            case 1: return voice->vol_right_reg;
            case 2: return voice->sample_rate;
            case 3: return (uint16_t)(voice->start_addr >> 3);
            case 4: return voice->adsr_low;
            case 5: return voice->adsr_high;
            case 6: return (uint16_t)voice->adsr_level;
            case 7: return (uint16_t)(voice->loop_addr >> 3);
        }
    }
    switch (offset) {
        case 0x180: return spu->main_vol_left;
        case 0x182: return spu->main_vol_right;
        case 0x19C: return (uint16_t)(spu->endx_bits & 0xFFFF);
        case 0x19E: return (uint16_t)(spu->endx_bits >> 16);
        case 0x1A4: return spu->irq_addr;
        case 0x1A6: return (uint16_t)(spu->transfer_addr >> 3);
        case 0x1A8: {
            uint16_t val = *(uint16_t*)&spu->ram[spu->transfer_addr & 0x7FFFE];
            spu->transfer_addr = (spu->transfer_addr + 2) & 0x7FFFF;
            return val;
        }
        case 0x1AA: return spu->ctrl;
        case 0x1AE: return spu->stat;
        case 0x1B0: return (uint16_t)spu->cd_vol_left;
        case 0x1B2: return (uint16_t)spu->cd_vol_right;
        case 0x1B8: return spu->main_vol_left;
        case 0x1BA: return spu->main_vol_right;
    }
    return spu->regs[(offset >> 1) & 0x1FF];
}

void ps1_spu_write(PS1SPU* spu, uint32_t addr, uint16_t val) {
    uint32_t offset = addr & 0x3FF;
    spu->regs[(offset >> 1) & 0x1FF] = val;
    if (offset < 0x180) {
        int v = offset / 16;
        int reg = (offset % 16) / 2;
        SPUVoice* voice = &spu->voices[v];
        switch (reg) {
            case 0: voice->vol_left_reg = val; break;
            case 1: voice->vol_right_reg = val; break;
            case 2: voice->sample_rate = val; break;
            case 3: voice->start_addr = ((uint32_t)val << 3) & 0x7FFFF; break;
            case 4: voice->adsr_low = val; break;
            case 5: voice->adsr_high = val; break;
            case 6: voice->adsr_level = val & 0x7FFF; break;
            case 7: voice->loop_addr = ((uint32_t)val << 3) & 0x7FFFF; break;
        }
        return;
    }
    switch (offset) {
        case 0x180: spu->main_vol_left = val; break;
        case 0x182: spu->main_vol_right = val; break;
        case 0x188: for (int i = 0; i < 16; i++) if (val & (1 << i)) key_on(spu, i); break;
        case 0x18A: for (int i = 0; i < 8; i++) if (val & (1 << i)) key_on(spu, 16 + i); break;
        case 0x18C: for (int i = 0; i < 16; i++) if (val & (1 << i)) key_off(spu, i); break;
        case 0x18E: for (int i = 0; i < 8; i++) if (val & (1 << i)) key_off(spu, 16 + i); break;
        case 0x190: spu->pmon_bits = (spu->pmon_bits & 0xFFFF0000u) | val; break;
        case 0x192: spu->pmon_bits = (spu->pmon_bits & 0x0000FFFFu) | ((uint32_t)val << 16); break;
        case 0x194: spu->noise_bits = (spu->noise_bits & 0xFFFF0000u) | val; break;
        case 0x196: spu->noise_bits = (spu->noise_bits & 0x0000FFFFu) | ((uint32_t)val << 16); break;
        case 0x198: spu->reverb_bits = (spu->reverb_bits & 0xFFFF0000u) | val; break;
        case 0x19A: spu->reverb_bits = (spu->reverb_bits & 0x0000FFFFu) | ((uint32_t)val << 16); break;
        case 0x1A4: spu->irq_addr = val; break;
        case 0x1A6: spu->transfer_addr = ((uint32_t)val << 3) & 0x7FFFF; break;
        case 0x1A8:
            *(uint16_t*)&spu->ram[spu->transfer_addr & 0x7FFFE] = val;
            spu->transfer_addr = (spu->transfer_addr + 2) & 0x7FFFF;
            break;
        case 0x1AA:
            spu->ctrl = val;
            spu->stat = (spu->stat & ~0x3Fu) | (val & 0x3F);
            break;
        case 0x1B0: spu->cd_vol_left = (int16_t)val; break;
        case 0x1B2: spu->cd_vol_right = (int16_t)val; break;
    }
}

// ---------------------------------------------------------------------------------------------
// ADPCM

static void decode_block(PS1SPU* spu, SPUVoice* voice) {
    uint32_t base = voice->current_addr & ~15u;
    uint8_t head = spu->ram[base % PS1_SPU_RAM_SIZE];
    uint8_t shift = head & 0x0F;
    uint8_t filter = (head >> 4) & 0x07;
    if (filter > 4) filter = 4;
    if (shift > 12) shift = 9;      // invalid shifts behave like 9 on hardware
    for (int i = 0; i < 28; i++) {
        uint8_t byte = spu->ram[(base + 2 + i / 2) % PS1_SPU_RAM_SIZE];
        int32_t nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += ((int32_t)voice->prev1 * f_k0[filter] + (int32_t)voice->prev2 * f_k1[filter] + 32) >> 6;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        voice->prev2 = voice->prev1;
        voice->prev1 = (int16_t)s;
        voice->block[i] = (int16_t)s;
    }
    voice->block_valid = true;
    voice->block_pos = 0;
}

// Moves to the block following the one that was just consumed (honouring loop flags)
static void advance_block(PS1SPU* spu, int vi, SPUVoice* voice) {
    uint32_t base = voice->current_addr & ~15u;
    uint8_t flags = spu->ram[(base + 1) % PS1_SPU_RAM_SIZE];
    if (flags & 4) voice->loop_addr = base;
    if (flags & 1) {
        spu->endx_bits |= 1u << vi;
        if (flags & 2) {
            voice->current_addr = voice->loop_addr;
        } else {
            voice->active = false;
            voice->adsr_phase = ADSR_PHASE_OFF;
            voice->adsr_level = 0;
            return;
        }
    } else {
        voice->current_addr = (base + 16) % PS1_SPU_RAM_SIZE;
    }
    voice->block_valid = false;
}

static void next_sample(PS1SPU* spu, int vi, SPUVoice* voice) {
    if (!voice->block_valid) decode_block(spu, voice);
    int16_t s = voice->block[voice->block_pos++];
    voice->hist[0] = voice->hist[1]; voice->hist[1] = voice->hist[2];
    voice->hist[2] = voice->hist[3]; voice->hist[3] = s;
    if (voice->block_pos >= 28) advance_block(spu, vi, voice);
}

// ---------------------------------------------------------------------------------------------
// Envelope

static void adsr_step(SPUVoice* v, int shift, int step, bool decrease, bool exp) {
    int32_t level = v->adsr_level;
    int32_t cycles_shift = shift > 11 ? shift - 11 : 0;
    int32_t adsr_step = decrease ? (-8 + step) : (7 - step);
    adsr_step <<= (shift < 11 ? 11 - shift : 0);
    uint32_t cycles = 1u << cycles_shift;
    if (exp && !decrease && level > 0x6000) cycles <<= 2;
    if (exp && decrease) adsr_step = (adsr_step * level) >> 15;
    if (++v->adsr_counter < cycles) return;
    v->adsr_counter = 0;
    level += adsr_step;
    if (level < 0) level = 0; else if (level > 0x7FFF) level = 0x7FFF;
    v->adsr_level = level;
}

static void adsr_tick(SPUVoice* v) {
    uint16_t lo = v->adsr_low, hi = v->adsr_high;
    switch (v->adsr_phase) {
        case ADSR_PHASE_ATTACK:
            adsr_step(v, (lo >> 10) & 0x1F, (lo >> 8) & 3, false, (lo & 0x8000) != 0);
            if (v->adsr_level >= 0x7FFF) v->adsr_phase = ADSR_PHASE_DECAY;
            break;
        case ADSR_PHASE_DECAY:
            adsr_step(v, (lo >> 4) & 0xF, 0, true, true);
            if (v->adsr_level <= (int32_t)(((lo & 0xF) + 1) * 0x800)) v->adsr_phase = ADSR_PHASE_SUSTAIN;
            break;
        case ADSR_PHASE_SUSTAIN:
            adsr_step(v, (hi >> 8) & 0x1F, (hi >> 6) & 3, (hi & 0x4000) != 0, (hi & 0x8000) != 0);
            break;
        case ADSR_PHASE_RELEASE:
            adsr_step(v, hi & 0x1F, 0, true, (hi & 0x20) != 0);
            if (v->adsr_level <= 0) { v->adsr_phase = ADSR_PHASE_OFF; v->active = false; }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Mixing

static inline int16_t interpolate(const int16_t h[4], uint32_t frac) {
    // 4-point Catmull-Rom between h[1] and h[2]... history keeps the newest sample in h[3], so the
    // segment being played is h[2] -> h[3] with h[1] and the extrapolated h[3] as outer points
    float t = (float)frac / 4096.0f;
    float p0 = h[1], p1 = h[2], p2 = h[3], p3 = h[3];
    float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    float c = -0.5f * p0 + 0.5f * p2;
    float r = ((a * t + b) * t + c) * t + p1;
    if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
    return (int16_t)r;
}

static void noise_tick(PS1SPU* spu) {
    uint32_t ctrl = spu->ctrl;
    int shift = (ctrl >> 10) & 0xF, step = ((ctrl >> 8) & 3) + 4;
    spu->noise_counter += step;
    uint32_t limit = 0x20000u >> shift;
    if (spu->noise_counter >= limit) {
        spu->noise_counter = 0;
        uint32_t l = spu->noise_level;
        uint32_t fb = ((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1;
        spu->noise_level = ((l << 1) | fb) & 0xFFFF;
    }
}

void ps1_spu_generate_samples(PS1SPU* spu, int16_t* output_stereo, int num_samples) {
    bool enabled = (spu->ctrl & 0x8000) != 0;
    bool unmuted = (spu->ctrl & 0x4000) != 0;

    for (int s = 0; s < num_samples; s++) {
        int32_t mix_l = 0, mix_r = 0;
        noise_tick(spu);
        int16_t prev_out = 0;

        for (int vi = 0; vi < SPU_NUM_VOICES; vi++) {
            SPUVoice* v = &spu->voices[vi];
            if (!v->active || v->adsr_phase == ADSR_PHASE_OFF) { v->last_out = 0; prev_out = 0; continue; }

            int16_t sample;
            if (spu->noise_bits & (1u << vi)) sample = (int16_t)spu->noise_level;
            else sample = interpolate(v->hist, v->pitch_counter);

            adsr_tick(v);
            int32_t amp = (sample * v->adsr_level) >> 15;
            v->last_out = (int16_t)amp;
            mix_l += (amp * vol_from_reg(v->vol_left_reg)) >> 15;
            mix_r += (amp * vol_from_reg(v->vol_right_reg)) >> 15;

            uint32_t pitch = v->sample_rate;
            if (vi > 0 && (spu->pmon_bits & (1u << vi))) {
                int32_t factor = prev_out + 0x8000;
                pitch = (uint32_t)(((int32_t)(int16_t)pitch * factor) >> 15) & 0xFFFF;
            }
            if (pitch > 0x3FFF) pitch = 0x3FFF;
            v->pitch_counter += pitch;
            while (v->pitch_counter >= 0x1000 && v->active) {
                v->pitch_counter -= 0x1000;
                next_sample(spu, vi, v);
            }
            prev_out = v->last_out;
        }

        mix_l = (mix_l * vol_from_reg(spu->main_vol_left)) >> 15;
        mix_r = (mix_r * vol_from_reg(spu->main_vol_right)) >> 15;
        if (!enabled || !unmuted) { mix_l = 0; mix_r = 0; }
        if (mix_l > 32767) mix_l = 32767; else if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767) mix_r = 32767; else if (mix_r < -32768) mix_r = -32768;
        output_stereo[s * 2 + 0] = (int16_t)mix_l;
        output_stereo[s * 2 + 1] = (int16_t)mix_r;
    }
}
