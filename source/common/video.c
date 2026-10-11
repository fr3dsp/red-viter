#include <string.h>

#include "vb_dsp.h"
#include "video_hard.h"
#include "v810_mem.h"
#include "vb_set.h"

VB_DSPCACHE tDSPCACHE;

int eye_count = 2;

static int g_displayed_fb = 0;
static int vip_displayed_fb = 0;
static HWORD old_brt[3];

void video_init(void) {
    video_hard_init();
    setup_brightness_lut();
}

static void flush_core(bool default_for_both) {
    gpu_flush(default_for_both, g_displayed_fb, vip_displayed_fb);
    tDSPCACHE.ColumnTableInvalid = false;
    tDSPCACHE.BrtPALMod = false;
}

static void render_core(int displayed_fb, bool antiflicker, bool should_flush) {
    gpu_reset_vip_download();

    HWORD new_brt[3] = {vb_state->tVIPREG.BRTA, vb_state->tVIPREG.BRTB, vb_state->tVIPREG.BRTC};
    if (antiflicker) {
        vb_state->tVIPREG.BRTA = old_brt[0];
        vb_state->tVIPREG.BRTB = old_brt[1];
        vb_state->tVIPREG.BRTC = old_brt[2];
        flush_core(false);
        vb_state->tVIPREG.BRTA = new_brt[0];
        vb_state->tVIPREG.BRTB = new_brt[1];
        vb_state->tVIPREG.BRTC = new_brt[2];
    }
    memcpy(old_brt, new_brt, sizeof(new_brt));

    if (CHECK_GAMEID("01VREE") || CHECK_GAMEID("E4VREJ")) {
        tVBOpt.VIP_OVER_SOFT = ((WORLD *)(vb_state->V810_DISPLAY_RAM.off + 0x3d800))[12].head == 0;
    }

    g_displayed_fb = displayed_fb;
    vip_displayed_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : 0;

    eye_count = tVBOpt.RENDERMODE == RM_TOCPU ? 2 : 1;

    if (tVBOpt.RENDERMODE == RM_TOGPU || tVBOpt.RENDERMODE == RM_TOCPU || tVBOpt.RENDERMODE == RM_CPUONLY) {
        gpu_soft_to_texture(displayed_fb);
    }

    if (tVBOpt.DOUBLE_BUFFER) {
        if (antiflicker) gpu_blend_antiflicker();
        if (should_flush) flush_core(false);
        if (antiflicker) gpu_blend_default();
    }

    if (vb_state->tVIPREG.XPCTRL & XPEN) {
        if (tDSPCACHE.CharCacheInvalid) {
            if (tVBOpt.RENDERMODE != RM_CPUONLY) update_texture_cache_hard();
            else update_texture_cache_soft();
        }

        if (tVBOpt.RENDERMODE != RM_CPUONLY) {
            video_hard_render(tVBOpt.DOUBLE_BUFFER ? !displayed_fb : 0);
        } else {
            video_soft_render(!displayed_fb);
        }

        tDSPCACHE.CharCacheInvalid = false;
        memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
    }

    if (!tVBOpt.DOUBLE_BUFFER) {
        if (antiflicker) gpu_blend_antiflicker();
        if (should_flush) flush_core(false);
        if (antiflicker) gpu_blend_default();
    }
}

void video_render(int displayed_fb, bool on_time) {
    bool antiflicker = gpu_antiflicker_allowed() && tVBOpt.ANTIFLICKER && on_time;
    bool soft_dirty = tDSPCACHE.DDSPDataState[displayed_fb] == CPU_WROTE;
    bool xp_enabled = (vb_state->tVIPREG.XPCTRL & XPEN) != 0;
    bool should_flush = antiflicker || xp_enabled || soft_dirty || tDSPCACHE.ColumnTableInvalid || tDSPCACHE.BrtPALMod;

    render_core(displayed_fb, antiflicker, should_flush);
    gpu_present();
}

void video_flush(bool default_for_both) {
    flush_core(default_for_both);
    gpu_present();
}

void video_quit(void) {
    gpu_quit();
}
