#include <string.h>

#include "render_thread.h"

static RenderJob job;
static uint8_t vram[SNAP_VRAM_SIZE] __attribute__((aligned(64)));

void render_present(void) {
    if (gpu_take_flushed()) vglSwapBuffers(GL_FALSE);
}

bool render_thread_start(void) {
    memset(&job, 0, sizeof(job));
    memset(vram, 0, sizeof(vram));
    job.vram = vram;
    return true;
}

bool render_thread_running(void) {
    return false;
}

void render_thread_sync(void) {
}

void render_thread_stop(void) {
}

RenderJob *render_job_acquire(void) {
    job.vram = vram;
    return &job;
}

void render_job_submit(RenderJob *submitted) {
    video_run_job(submitted);
    render_present();
}
