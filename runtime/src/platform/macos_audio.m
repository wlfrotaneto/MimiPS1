#import <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include "mimips1_runtime.h"

#define NUM_AUDIO_BUFFERS 3
#define AUDIO_BUFFER_SIZE 512 // Reduced to 512 frames (~11.6ms) for lowest latency
#define AUDIO_RING_CAPACITY 16384 // frames (stereo)
#define AUDIO_RING_MASK (AUDIO_RING_CAPACITY - 1)
#define AUDIO_MAX_LATENCY_FRAMES 4410 // ~6 video frames at 44.1kHz (~100ms headroom)

typedef struct {
    int16_t buffer[AUDIO_RING_CAPACITY * 2]; // 16384 stereo frames (L, R)
    _Atomic size_t write_idx;
    _Atomic size_t read_idx;
    _Atomic uint64_t underrun_count;
    int16_t last_sample_l;
    int16_t last_sample_r;
} AudioRingBuffer;

typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[NUM_AUDIO_BUFFERS];
    PS1Context* context;
    bool running;
    AudioRingBuffer ring;
    double samples_due;
} MacOSAudioState;

static MacOSAudioState g_audio_state;

static inline void audio_ring_push(AudioRingBuffer* ring, const int16_t* src, int count) {
    if (count <= 0) return;

    size_t w = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&ring->read_idx, memory_order_acquire);

    size_t occupied = (w >= r) ? (w - r) : 0;
    size_t space = (AUDIO_RING_CAPACITY > occupied) ? (AUDIO_RING_CAPACITY - occupied) : 0;
    if ((size_t)count > space) {
        count = (int)space;
    }

    for (int i = 0; i < count; i++) {
        size_t idx = (w + i) & AUDIO_RING_MASK;
        ring->buffer[idx * 2 + 0] = src[i * 2 + 0];
        ring->buffer[idx * 2 + 1] = src[i * 2 + 1];
    }

    atomic_store_explicit(&ring->write_idx, w + count, memory_order_release);
}

static inline int audio_ring_pop(AudioRingBuffer* ring, int16_t* dst, int count) {
    size_t r = atomic_load_explicit(&ring->read_idx, memory_order_relaxed);
    size_t w = atomic_load_explicit(&ring->write_idx, memory_order_acquire);

    size_t available = (w >= r) ? (w - r) : 0;
    // Consumer solely adjusts read_idx to discard excess if producer gets too far ahead
    if (available > AUDIO_MAX_LATENCY_FRAMES) {
        size_t drop = available - AUDIO_MAX_LATENCY_FRAMES;
        r += drop;
        available = AUDIO_MAX_LATENCY_FRAMES;
    }

    int to_read = (count < (int)available) ? count : (int)available;
    for (int i = 0; i < to_read; i++) {
        size_t idx = (r + i) & AUDIO_RING_MASK;
        dst[i * 2 + 0] = ring->buffer[idx * 2 + 0];
        dst[i * 2 + 1] = ring->buffer[idx * 2 + 1];
    }

    if (to_read > 0) {
        ring->last_sample_l = dst[(to_read - 1) * 2 + 0];
        ring->last_sample_r = dst[(to_read - 1) * 2 + 1];
    }
    atomic_store_explicit(&ring->read_idx, r + to_read, memory_order_release);

    return to_read;
}

static void audio_callback(void* userData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    MacOSAudioState* state = (MacOSAudioState*)userData;
    if (!state) return;

    int frames_needed = inBuffer->mAudioDataBytesCapacity / (2 * sizeof(int16_t));
    int16_t* dst = (int16_t*)inBuffer->mAudioData;

    int got = audio_ring_pop(&state->ring, dst, frames_needed);
    if (got < frames_needed) {
        atomic_fetch_add_explicit(&state->ring.underrun_count, 1, memory_order_relaxed);
        for (int i = got; i < frames_needed; i++) {
            dst[i * 2 + 0] = state->ring.last_sample_l;
            dst[i * 2 + 1] = state->ring.last_sample_r;
        }
    }

    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

void macos_audio_tick(PS1Context* ctx, uint32_t delta_cycles) {
    if (!g_audio_state.running || !ctx) return;

    g_audio_state.samples_due += (double)delta_cycles * (44100.0 / (double)PS1_CPU_CLOCK_HZ);

    while (g_audio_state.samples_due >= 32.0) {
        int chunk = (int)g_audio_state.samples_due;
        if (chunk > 256) chunk = 256;
        g_audio_state.samples_due -= (double)chunk;

        int16_t mix_buf[256 * 2];
        ps1_spu_generate_samples(&ctx->spu, mix_buf, chunk);

        // XA-ADPCM streams (movies, music) decoded by the CD controller
        if (!ctx->cdrom.muted && (ctx->spu.ctrl & 1)) {
            int16_t xa_buf[256 * 2];
            int got = ps1_cdrom_read_xa_samples(&ctx->cdrom, xa_buf, chunk);
            int vl = ctx->spu.cd_vol_left ? ctx->spu.cd_vol_left : 0x7FFF;
            int vr = ctx->spu.cd_vol_right ? ctx->spu.cd_vol_right : 0x7FFF;
            for (int i = 0; i < got; i++) {
                int32_t l = mix_buf[i * 2] + ((xa_buf[i * 2] * vl) >> 15);
                int32_t r = mix_buf[i * 2 + 1] + ((xa_buf[i * 2 + 1] * vr) >> 15);
                mix_buf[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                mix_buf[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
        }

        // Mix CD-DA digital audio track if CD-ROM is playing
        if (ctx->cdrom.playing && ctx->cdrom.image_file) {
            int16_t cdda_buf[256 * 2];
            int got = ps1_cdrom_read_cdda_samples(&ctx->cdrom, cdda_buf, chunk);

            int cd_vol_l = ctx->spu.cd_vol_left;
            int cd_vol_r = ctx->spu.cd_vol_right;
            if (cd_vol_l == 0 && cd_vol_r == 0) {
                cd_vol_l = 0x7FFF;
                cd_vol_r = 0x7FFF;
            }

            for (int i = 0; i < got; i++) {
                int32_t cl = (cdda_buf[i * 2 + 0] * cd_vol_l) >> 15;
                int32_t cr = (cdda_buf[i * 2 + 1] * cd_vol_r) >> 15;
                int32_t out_l = mix_buf[i * 2 + 0] + cl;
                int32_t out_r = mix_buf[i * 2 + 1] + cr;
                if (out_l > 32767) out_l = 32767; else if (out_l < -32768) out_l = -32768;
                if (out_r > 32767) out_r = 32767; else if (out_r < -32768) out_r = -32768;
                mix_buf[i * 2 + 0] = (int16_t)out_l;
                mix_buf[i * 2 + 1] = (int16_t)out_r;
            }
        }

        static int stats = -1; static int peak, spu_peak, frames_acc;
        if (stats < 0) stats = getenv("MIMIPS1_AUDIO_STATS") != NULL;
        if (stats) {
            for (int i = 0; i < chunk * 2; i++) { int a = abs(mix_buf[i]); if (a > peak) peak = a; }
            frames_acc += chunk;
            if (frames_acc >= 44100) {
                int voices = 0; for (int v = 0; v < 24; v++) voices += ctx->spu.voices[v].active;
                printf("[Audio] peak=%d active_voices=%d xa_queued=%u spucnt=%04X\n", peak, voices, ctx->cdrom.xa_write - ctx->cdrom.xa_read, ctx->spu.ctrl);
                fflush(stdout); peak = 0; frames_acc = 0;
            }
        }
        audio_ring_push(&g_audio_state.ring, mix_buf, chunk);
    }
}

uint64_t macos_audio_get_underruns(void) {
    return atomic_load_explicit(&g_audio_state.ring.underrun_count, memory_order_relaxed);
}

bool macos_audio_init(PS1Context* ctx) {
    g_audio_state.context = ctx;

    atomic_store(&g_audio_state.ring.write_idx, 0);
    atomic_store(&g_audio_state.ring.read_idx, 0);
    atomic_store(&g_audio_state.ring.underrun_count, 0);
    g_audio_state.ring.last_sample_l = 0;
    g_audio_state.ring.last_sample_r = 0;
    g_audio_state.samples_due = 0.0;

    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate       = 44100.0;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel   = 16;
    format.mChannelsPerFrame = 2;
    format.mBytesPerFrame    = 4;
    format.mFramesPerPacket  = 1;
    format.mBytesPerPacket   = 4;

    OSStatus status = AudioQueueNewOutput(&format, audio_callback, &g_audio_state, NULL, NULL, 0, &g_audio_state.queue);
    if (status != noErr) {
        return false;
    }

    uint32_t buffer_size_bytes = AUDIO_BUFFER_SIZE * 4;
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        AudioQueueAllocateBuffer(g_audio_state.queue, buffer_size_bytes, &g_audio_state.buffers[i]);
        g_audio_state.buffers[i]->mAudioDataByteSize = buffer_size_bytes;
        memset(g_audio_state.buffers[i]->mAudioData, 0, buffer_size_bytes);
        AudioQueueEnqueueBuffer(g_audio_state.queue, g_audio_state.buffers[i], 0, NULL);
    }

    AudioQueueStart(g_audio_state.queue, NULL);
    g_audio_state.running = true;
    return true;
}

void macos_audio_shutdown(void) {
    if (g_audio_state.running) {
        g_audio_state.running = false;
        AudioQueueStop(g_audio_state.queue, true);
        AudioQueueDispose(g_audio_state.queue, true);
        g_audio_state.context = NULL;
    }
}
