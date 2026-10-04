#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "vb_set.h"
#include "vb_sound.h"
#include "vb_types.h"
#include "vita_ui.h"

#define OUT_RATE 48000
#define OUT_GRAIN 960
#define OUT_FRAMES (SAMPLE_COUNT * OUT_RATE / SAMPLE_RATE)
#define RING_FRAMES 8192
#define RING_MASK (RING_FRAMES - 1)
#define PREBUFFER_FRAMES (OUT_GRAIN * 2)
#define MAX_QUEUED (OUT_GRAIN * 4)

static int audioPort = -1;
static SceUID audioThread = -1;
static volatile int audioRunning;
static volatile int audioPaused;
static volatile int audioPrimed;
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ringWrite;
static volatile uint32_t ringRead;

static int audio_thread_main(SceSize args, void *argp) {
    int16_t block[OUT_GRAIN * 2];
    while (audioRunning) {
        uint32_t w = ringWrite;
        uint32_t r = ringRead;
        uint32_t avail = w - r;
        if (audioPaused) {
            memset(block, 0, sizeof(block));
            ringRead = w;
            audioPrimed = 0;
        } else if (!audioPrimed) {
            if (avail < PREBUFFER_FRAMES) {
                sceKernelDelayThread(1000);
                continue;
            }
            audioPrimed = 1;
            continue;
        } else {
            uint32_t n = avail < OUT_GRAIN ? avail : OUT_GRAIN;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t idx = (r + i) & RING_MASK;
                block[i * 2] = ring[idx * 2];
                block[i * 2 + 1] = ring[idx * 2 + 1];
            }
            if (n < OUT_GRAIN) memset(block + n * 2, 0, (OUT_GRAIN - n) * 2 * sizeof(int16_t));
            __sync_synchronize();
            ringRead = r + n;
        }
        sceAudioOutOutput(audioPort, block);
    }
    return 0;
}

bool sound_init_backend(int16_t *wavebufs[]) {
    audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audioPort < 0) {
        vita_log("audio open failed: 0x%08x", audioPort);
        tVBOpt.SOUND = 0;
        return false;
    }

    int vols[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    sceAudioOutSetVolume(audioPort, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);

    ringWrite = 0;
    ringRead = 0;
    audioPaused = 0;
    audioPrimed = 0;
    audioRunning = 1;
    audioThread = sceKernelCreateThread("rv_audio", audio_thread_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (audioThread < 0) {
        vita_log("audio thread failed: 0x%08x", audioThread);
        sceAudioOutReleasePort(audioPort);
        audioPort = -1;
        audioRunning = 0;
        tVBOpt.SOUND = 0;
        return false;
    }
    sceKernelStartThread(audioThread, 0, NULL);
    return true;
}

void sound_close_backend(void) {
    if (audioPort < 0) return;
    audioRunning = 0;
    if (audioThread >= 0) {
        sceKernelWaitThreadEnd(audioThread, NULL, NULL);
        sceKernelDeleteThread(audioThread);
        audioThread = -1;
    }
    sceAudioOutReleasePort(audioPort);
    audioPort = -1;
}

void sound_pause_backend(void) {
    audioPaused = 1;
    audioPrimed = 0;
}

void sound_resume_backend(void) {
    ringRead = ringWrite;
    audioPrimed = 0;
    audioPaused = 0;
}

bool sound_push_backend(int16_t *buf) {
    if (audioPort < 0) return false;
    uint32_t w = ringWrite;
    uint32_t queued = w - ringRead;
    if (queued + OUT_FRAMES > RING_FRAMES) return false;
    if (queued > MAX_QUEUED) return false;

    for (int i = 0; i < OUT_FRAMES; i++) {
        uint32_t scaled = (uint32_t)i * SAMPLE_COUNT;
        int idx = (int)(scaled / OUT_FRAMES);
        int next = idx + 1 < SAMPLE_COUNT ? idx + 1 : idx;
        int frac = (int)(scaled % OUT_FRAMES);
        uint32_t o = (w + i) & RING_MASK;
        for (int ch = 0; ch < 2; ch++) {
            int a = buf[idx * 2 + ch];
            int b = buf[next * 2 + ch];
            ring[o * 2 + ch] = (int16_t)(a + (b - a) * frac / OUT_FRAMES);
        }
    }
    __sync_synchronize();
    ringWrite = w + OUT_FRAMES;
    return true;
}
