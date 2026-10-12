#import <AudioToolbox/AudioToolbox.h>
#include "mimips1_runtime.h"

#define NUM_AUDIO_BUFFERS 3
#define AUDIO_BUFFER_SIZE 4096 // samples per buffer

typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[NUM_AUDIO_BUFFERS];
    PS1Context* context;
    bool running;
} MacOSAudioState;

static MacOSAudioState g_audio_state;

static void audio_callback(void* userData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    MacOSAudioState* state = (MacOSAudioState*)userData;
    if (!state || !state->context) return;

    int samples = inBuffer->mAudioDataBytesCapacity / (2 * sizeof(int16_t));
    int16_t* dst = (int16_t*)inBuffer->mAudioData;

    // 1. Generate synthesized SPU voice audio
    ps1_spu_generate_samples(&state->context->spu, dst, samples);

    // 2. Mix CD-DA digital audio track if CD-ROM is playing
    if (state->context->cdrom.playing && state->context->cdrom.image_file) {
        int16_t cdda_buf[AUDIO_BUFFER_SIZE * 2];
        int to_read = samples < AUDIO_BUFFER_SIZE ? samples : AUDIO_BUFFER_SIZE;
        int got = ps1_cdrom_read_cdda_samples(&state->context->cdrom, cdda_buf, to_read);

        int cd_vol_l = state->context->spu.cd_vol_left;
        int cd_vol_r = state->context->spu.cd_vol_right;
        if (cd_vol_l == 0 && cd_vol_r == 0) {
            cd_vol_l = 0x7FFF;
            cd_vol_r = 0x7FFF;
        }

        for (int i = 0; i < got; i++) {
            int32_t cl = (cdda_buf[i * 2 + 0] * cd_vol_l) >> 15;
            int32_t cr = (cdda_buf[i * 2 + 1] * cd_vol_r) >> 15;
            int32_t out_l = dst[i * 2 + 0] + cl;
            int32_t out_r = dst[i * 2 + 1] + cr;
            if (out_l > 32767) out_l = 32767; else if (out_l < -32768) out_l = -32768;
            if (out_r > 32767) out_r = 32767; else if (out_r < -32768) out_r = -32768;
            dst[i * 2 + 0] = (int16_t)out_l;
            dst[i * 2 + 1] = (int16_t)out_r;
        }
    }

    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

bool macos_audio_init(PS1Context* ctx) {
    g_audio_state.context = ctx;

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
        AudioQueueStop(g_audio_state.queue, true);
        AudioQueueDispose(g_audio_state.queue, true);
        g_audio_state.running = false;
    }
}
