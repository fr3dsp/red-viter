#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "vb_set.h"
#include "vb_sound.h"
#include "vb_types.h"
#include "vita_ui.h"

#define OUT_RATE 48000
#define OUT_GRAIN 512
#define OUT_FRAMES (SAMPLE_COUNT * OUT_RATE / SAMPLE_RATE)
#define RING_FRAMES 16384
#define RING_MASK (RING_FRAMES - 1)
#define START_FRAMES 2048
#define TARGET_FRAMES 2560
#define MAX_QUEUED_FRAMES 6144
#define RATE_ONE 65536
#define RATE_LIMIT 1536

static int audioPort = -1;
static SceUID audioThread = -1;
static volatile int audioRunning;
static volatile int audioPaused;
static uint32_t audioEpoch;
static int16_t ring[RING_FRAMES * 2] __attribute__((aligned(64)));
static uint32_t ringWrite;
static uint32_t ringRead;

static int16_t read_sample(uint32_t index, unsigned ch) {
    return ring[((index & RING_MASK) * 2) + ch];
}

static void apply_fade(int16_t *block, unsigned frames) {
    unsigned length = frames < 64 ? frames : 64;
    for (unsigned i = 0; i < length; i++) {
        int32_t gain = (int32_t)(i + 1);
        block[i * 2] = (int16_t)((int32_t)block[i * 2] * gain / (int32_t)length);
        block[i * 2 + 1] = (int16_t)((int32_t)block[i * 2 + 1] * gain / (int32_t)length);
    }
}

static int audio_thread_main(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int16_t block[OUT_GRAIN * 2] __attribute__((aligned(64)));
    int16_t previous[OUT_GRAIN * 2] __attribute__((aligned(64)));
    memset(previous, 0, sizeof(previous));
    uint32_t epochSeen = __atomic_load_n(&audioEpoch, __ATOMIC_ACQUIRE);
    uint32_t phase = 0;
    int32_t step = RATE_ONE;
    bool primed = false;
    bool first = true;
    bool havePrevious = false;

    while (__atomic_load_n(&audioRunning, __ATOMIC_ACQUIRE)) {
        uint32_t epoch = __atomic_load_n(&audioEpoch, __ATOMIC_ACQUIRE);
        bool paused = __atomic_load_n(&audioPaused, __ATOMIC_ACQUIRE) != 0;
        if (epoch != epochSeen || paused) {
            epochSeen = epoch;
            primed = false;
            first = true;
            havePrevious = false;
            phase = 0;
            step = RATE_ONE;
            __atomic_store_n(&ringRead, __atomic_load_n(&ringWrite, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        }

        uint32_t r = __atomic_load_n(&ringRead, __ATOMIC_RELAXED);
        uint32_t w = __atomic_load_n(&ringWrite, __ATOMIC_ACQUIRE);
        uint32_t available = w - r;
        if (!paused && !primed && available >= START_FRAMES) {
            primed = true;
            first = true;
        }

        if (paused || !primed) {
            memset(block, 0, sizeof(block));
        } else {
            int32_t correction = ((int32_t)available - TARGET_FRAMES) * 2;
            if (correction > RATE_LIMIT) correction = RATE_LIMIT;
            if (correction < -RATE_LIMIT) correction = -RATE_LIMIT;
            int32_t wanted = RATE_ONE + correction;
            step += (wanted - step) / 8;

            uint32_t pos = phase;
            uint32_t produced = 0;
            for (; produced < OUT_GRAIN; produced++) {
                uint32_t base = pos >> 16;
                if (base + 1 >= available) break;
                uint32_t frac = pos & 0xffff;
                for (unsigned ch = 0; ch < 2; ch++) {
                    int32_t a = read_sample(r + base, ch);
                    int32_t b = read_sample(r + base + 1, ch);
                    block[produced * 2 + ch] = (int16_t)(a + ((int64_t)(b - a) * (int32_t)frac) / RATE_ONE);
                }
                pos += (uint32_t)step;
            }

            if (produced < OUT_GRAIN) {
                for (uint32_t i = produced; i < OUT_GRAIN; i++) {
                    for (unsigned ch = 0; ch < 2; ch++) {
                        int32_t held = havePrevious ? previous[i * 2 + ch] : 0;
                        if (produced) held = block[(produced - 1) * 2 + ch];
                        uint32_t remain = OUT_GRAIN - i;
                        block[i * 2 + ch] = (int16_t)(held * (int32_t)remain / (int32_t)(OUT_GRAIN - produced));
                    }
                }
                primed = false;
                first = true;
                havePrevious = false;
            } else if (first) {
                apply_fade(block, OUT_GRAIN);
                first = false;
            }
            uint32_t consumed = pos >> 16;
            if (consumed > available) consumed = available;
            if (epochSeen == __atomic_load_n(&audioEpoch, __ATOMIC_ACQUIRE) &&
                !__atomic_load_n(&audioPaused, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&ringRead, r + consumed, __ATOMIC_RELEASE);
                phase = pos & 0xffff;
            } else {
                memset(block, 0, sizeof(block));
                primed = false;
            }
            if (produced == OUT_GRAIN) {
                memcpy(previous, block, sizeof(previous));
                havePrevious = true;
            }
        }
        if (sceAudioOutOutput(audioPort, block) < 0) break;
    }
    return 0;
}

bool sound_init_backend(int16_t *wavebufs[]) {
    (void)wavebufs;
    audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audioPort < 0) {
        tVBOpt.SOUND = 0;
        return false;
    }

    int volumes[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    sceAudioOutSetVolume(audioPort, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, volumes);
    __atomic_store_n(&ringWrite, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ringRead, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&audioEpoch, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&audioPaused, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&audioRunning, 1, __ATOMIC_RELEASE);
    audioThread = sceKernelCreateThread("rv_audio", audio_thread_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (audioThread < 0) {
        __atomic_store_n(&audioRunning, 0, __ATOMIC_RELEASE);
        sceAudioOutReleasePort(audioPort);
        audioPort = -1;
        tVBOpt.SOUND = 0;
        return false;
    }
    if (sceKernelStartThread(audioThread, 0, NULL) < 0) {
        __atomic_store_n(&audioRunning, 0, __ATOMIC_RELEASE);
        sceKernelDeleteThread(audioThread);
        audioThread = -1;
        sceAudioOutReleasePort(audioPort);
        audioPort = -1;
        tVBOpt.SOUND = 0;
        return false;
    }
    return true;
}

void sound_close_backend(void) {
    if (audioPort < 0) return;
    __atomic_store_n(&audioRunning, 0, __ATOMIC_RELEASE);
    if (audioThread >= 0) {
        sceKernelWaitThreadEnd(audioThread, NULL, NULL);
        sceKernelDeleteThread(audioThread);
        audioThread = -1;
    }
    sceAudioOutReleasePort(audioPort);
    audioPort = -1;
}

void sound_pause_backend(void) {
    __atomic_store_n(&audioPaused, 1, __ATOMIC_RELEASE);
}

void sound_resume_backend(void) {
    __atomic_add_fetch(&audioEpoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&audioPaused, 0, __ATOMIC_RELEASE);
}

bool sound_push_backend(int16_t *buf) {
    if (audioPort < 0 || __atomic_load_n(&audioPaused, __ATOMIC_ACQUIRE)) return false;
    uint32_t w = __atomic_load_n(&ringWrite, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&ringRead, __ATOMIC_ACQUIRE);
    uint32_t available = w - r;
    if (available > MAX_QUEUED_FRAMES || available + OUT_FRAMES > RING_FRAMES) return false;

    for (uint32_t i = 0; i < OUT_FRAMES; ++i) {
        uint32_t numerator = i * SAMPLE_COUNT;
        uint32_t index = numerator / OUT_FRAMES;
        uint32_t next = index + 1 < SAMPLE_COUNT ? index + 1 : index;
        int32_t frac = (int32_t)(numerator % OUT_FRAMES);
        uint32_t dest = ((w + i) & RING_MASK) * 2;
        for (unsigned ch = 0; ch < 2; ch++) {
            int32_t a = buf[index * 2 + ch];
            int32_t b = buf[next * 2 + ch];
            ring[dest + ch] = (int16_t)(a + ((b - a) * frac) / OUT_FRAMES);
        }
    }
    __atomic_store_n(&ringWrite, w + OUT_FRAMES, __ATOMIC_RELEASE);
    return true;
}
