#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include <stdlib.h>
#include <string.h>

#include "render_thread.h"
#include "video_hard.h"
#include "vita_ui.h"

#define SLOT_COUNT 2
#define RENDER_STACK 0x80000
#define RENDER_PRIORITY 0x10000100

static RenderJob slots[SLOT_COUNT];
static int head;
static int tail;
static SceUID renderThread = -1;
static SceUID semFree = -1;
static SceUID semJobs = -1;
static volatile bool running;
static volatile bool quitting;

void render_present(void) {
    if (gpu_take_flushed()) vglSwapBuffers(GL_FALSE);
}

static int render_thread_main(SceSize args, void *argp) {
    for (;;) {
        sceKernelWaitSema(semJobs, 1, NULL);
        if (quitting) break;
        video_run_job(&slots[tail]);
        render_present();
        tail = (tail + 1) % SLOT_COUNT;
        sceKernelSignalSema(semFree, 1);
    }
    return 0;
}

static void release_resources(void) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        free(slots[i].vram);
        slots[i].vram = NULL;
    }
    if (semFree >= 0) sceKernelDeleteSema(semFree);
    if (semJobs >= 0) sceKernelDeleteSema(semJobs);
    semFree = -1;
    semJobs = -1;
}

bool render_thread_start(void) {
    if (running) return true;

    head = 0;
    tail = 0;
    quitting = false;
    memset(slots, 0, sizeof(slots));

    for (int i = 0; i < SLOT_COUNT; i++) {
        slots[i].vram = aligned_alloc(16, SNAP_VRAM_SIZE);
        if (!slots[i].vram) {
            release_resources();
            return false;
        }
        memset(slots[i].vram, 0, SNAP_VRAM_SIZE);
    }

    semFree = sceKernelCreateSema("rv_render_free", 0, SLOT_COUNT, SLOT_COUNT, NULL);
    semJobs = sceKernelCreateSema("rv_render_jobs", 0, 0, SLOT_COUNT, NULL);
    if (semFree < 0 || semJobs < 0) {
        vita_log("render semaphores failed");
        release_resources();
        return false;
    }

    renderThread = sceKernelCreateThread("rv_render", render_thread_main, RENDER_PRIORITY, RENDER_STACK, 0, 0, NULL);
    if (renderThread < 0) {
        vita_log("render thread failed: 0x%08x", renderThread);
        release_resources();
        return false;
    }

    running = true;
    sceKernelStartThread(renderThread, 0, NULL);
    return true;
}

bool render_thread_running(void) {
    return running;
}

void render_thread_sync(void) {
    if (!running) return;
    sceKernelWaitSema(semFree, SLOT_COUNT, NULL);
    sceKernelSignalSema(semFree, SLOT_COUNT);
}

void render_thread_stop(void) {
    if (!running) return;
    render_thread_sync();
    quitting = true;
    sceKernelSignalSema(semJobs, 1);
    sceKernelWaitThreadEnd(renderThread, NULL, NULL);
    sceKernelDeleteThread(renderThread);
    renderThread = -1;
    running = false;
    release_resources();
}

RenderJob *render_job_acquire(void) {
    sceKernelWaitSema(semFree, 1, NULL);
    return &slots[head];
}

void render_job_submit(RenderJob *job) {
    head = (head + 1) % SLOT_COUNT;
    __sync_synchronize();
    sceKernelSignalSema(semJobs, 1);
}
