#ifndef RENDER_THREAD_H
#define RENDER_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "v810_mem.h"
#include "vb_dsp.h"

#define SNAP_VRAM_SIZE 0x40000

typedef struct {
    bool flush_only;
    bool default_for_both;
    bool antiflicker;
    bool should_flush;
    bool on_time;
    int displayed_fb;
    VB_STATE vb;
    VB_DSPCACHE dsp;
    uint8_t *vram;
} RenderJob;

extern VB_STATE *rvb;
extern VB_DSPCACHE *rdsp;

bool render_thread_start(void);
void render_thread_stop(void);
void render_thread_sync(void);
bool render_thread_running(void);
RenderJob *render_job_acquire(void);
void render_job_submit(RenderJob *job);
void render_present(void);
void video_run_job(RenderJob *job);

#endif
