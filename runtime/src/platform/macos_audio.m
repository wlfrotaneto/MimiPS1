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
    ps1_spu_generate_samples(&state->context->spu, (int16_t*)inBuffer->mAudioData, samples);
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
