#include <string.h>

#include "render_thread.h"
#include "vb_dsp.h"
#include "video_hard.h"
#include "v810_mem.h"
#include "vb_set.h"

VB_DSPCACHE tDSPCACHE;
VB_STATE *rvb;
VB_DSPCACHE *rdsp;

int eye_count = 2;

static int g_displayed_fb = 0;
static int vip_displayed_fb = 0;
static HWORD old_brt[3];
static RenderJob syncJob;

void video_init(void) {
    video_hard_init();
    setup_brightness_lut();
    rvb = vb_state;
    rdsp = &tDSPCACHE;
    render_thread_start();
}

static void flush_core(bool default_for_both) {
    gpu_flush(default_for_both, g_displayed_fb, vip_displayed_fb);
    rdsp->ColumnTableInvalid = false;
    rdsp->BrtPALMod = false;
}

static void render_core(const RenderJob *job) {
    int displayed_fb = job->displayed_fb;
    bool antiflicker = job->antiflicker;
    bool should_flush = job->should_flush;

    gpu_reset_vip_download();

    HWORD new_brt[3] = {rvb->tVIPREG.BRTA, rvb->tVIPREG.BRTB, rvb->tVIPREG.BRTC};
    if (antiflicker) {
        rvb->tVIPREG.BRTA = old_brt[0];
        rvb->tVIPREG.BRTB = old_brt[1];
        rvb->tVIPREG.BRTC = old_brt[2];
        flush_core(false);
        rvb->tVIPREG.BRTA = new_brt[0];
        rvb->tVIPREG.BRTB = new_brt[1];
        rvb->tVIPREG.BRTC = new_brt[2];
    }
    memcpy(old_brt, new_brt, sizeof(new_brt));

    if (CHECK_GAMEID("01VREE") || CHECK_GAMEID("E4VREJ")) {
        tVBOpt.VIP_OVER_SOFT = ((WORLD *)(rvb->V810_DISPLAY_RAM.off + 0x3d800))[12].head == 0;
    }

    g_displayed_fb = displayed_fb;
    vip_displayed_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : 0;

    eye_count = tVBOpt.ANAGLYPH || tVBOpt.RENDERMODE == RM_TOCPU ? 2 : 1;

    if (tVBOpt.RENDERMODE == RM_TOGPU || tVBOpt.RENDERMODE == RM_TOCPU || tVBOpt.RENDERMODE == RM_CPUONLY) {
        gpu_soft_to_texture(displayed_fb);
    }

    if (tVBOpt.DOUBLE_BUFFER) {
        if (antiflicker) gpu_blend_antiflicker();
        if (should_flush) flush_core(false);
        if (antiflicker) gpu_blend_default();
    }

    if (rvb->tVIPREG.XPCTRL & XPEN) {
        if (rdsp->CharCacheInvalid) {
            if (tVBOpt.RENDERMODE != RM_CPUONLY) update_texture_cache_hard();
            else update_texture_cache_soft();
        }

        if (tVBOpt.RENDERMODE != RM_CPUONLY) {
            video_hard_render(tVBOpt.DOUBLE_BUFFER ? !displayed_fb : 0);
        } else {
            video_soft_render(!displayed_fb);
        }

        rdsp->CharCacheInvalid = false;
        memset(rdsp->CharacterCache, 0, sizeof(rdsp->CharacterCache));
    }

    if (!tVBOpt.DOUBLE_BUFFER) {
        if (antiflicker) gpu_blend_antiflicker();
        if (should_flush) flush_core(false);
        if (antiflicker) gpu_blend_default();
    }
}

void video_run_job(RenderJob *job) {
    rvb = &job->vb;
    rdsp = &job->dsp;
    if (job->flush_only) flush_core(job->default_for_both);
    else render_core(job);
}

static bool async_allowed(void) {
    return render_thread_running() && tVBOpt.RENDERMODE == RM_TOGPU;
}

static void snapshot_state(RenderJob *job) {
    job->vb = *vb_state;
    job->dsp = tDSPCACHE;
    memcpy(job->vram + 0x3dc00, (const uint8_t *)vb_state->V810_DISPLAY_RAM.off + 0x3dc00, 0x400);
    job->vb.V810_DISPLAY_RAM.off = (size_t)job->vram;
}

static void snapshot_vram(RenderJob *job, int displayed_fb, bool copy_framebuffers) {
    const uint8_t *src = (const uint8_t *)vb_state->V810_DISPLAY_RAM.off;
    memcpy(job->vram + 0x20000, src + 0x20000, 0x20000);
    for (int i = 0; i < 4; i++) {
        size_t chars = 0x6000 + 0x8000 * i;
        memcpy(job->vram + chars, src + chars, 0x2000);
    }
    if (copy_framebuffers) {
        for (int eye = 0; eye < 2; eye++) {
            size_t fb = 0x10000 * eye + 0x8000 * displayed_fb;
            memcpy(job->vram + fb, src + fb, 0x6000);
        }
    }
}

void video_render(int displayed_fb, bool on_time) {
    bool antiflicker = gpu_antiflicker_allowed() && tVBOpt.ANTIFLICKER && on_time;
    bool soft_dirty = tDSPCACHE.DDSPDataState[displayed_fb] == CPU_WROTE;
    bool xp_enabled = (vb_state->tVIPREG.XPCTRL & XPEN) != 0;
    bool should_flush = antiflicker || xp_enabled || soft_dirty || tDSPCACHE.ColumnTableInvalid || tDSPCACHE.BrtPALMod;

    if (async_allowed()) {
        RenderJob *job = render_job_acquire();
        job->flush_only = false;
        job->antiflicker = antiflicker;
        job->should_flush = should_flush;
        job->on_time = on_time;
        job->displayed_fb = displayed_fb;
        snapshot_state(job);
        snapshot_vram(job, displayed_fb, soft_dirty);

        if (soft_dirty) tDSPCACHE.DDSPDataState[displayed_fb] = GPU_WROTE;
        if (xp_enabled) {
            tDSPCACHE.CharCacheInvalid = false;
            memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
        }
        if (should_flush) {
            tDSPCACHE.ColumnTableInvalid = false;
            tDSPCACHE.BrtPALMod = false;
        }

        render_job_submit(job);
        return;
    }

    render_thread_sync();
    rvb = vb_state;
    rdsp = &tDSPCACHE;
    syncJob.flush_only = false;
    syncJob.antiflicker = antiflicker;
    syncJob.should_flush = should_flush;
    syncJob.on_time = on_time;
    syncJob.displayed_fb = displayed_fb;
    render_core(&syncJob);
    render_present();
}

void video_flush(bool default_for_both) {
    if (async_allowed()) {
        RenderJob *job = render_job_acquire();
        job->flush_only = true;
        job->default_for_both = default_for_both;
        snapshot_state(job);
        tDSPCACHE.ColumnTableInvalid = false;
        tDSPCACHE.BrtPALMod = false;
        render_job_submit(job);
        return;
    }

    render_thread_sync();
    rvb = vb_state;
    rdsp = &tDSPCACHE;
    flush_core(default_for_both);
    render_present();
}

void video_quit(void) {
    render_thread_stop();
    gpu_quit();
}
