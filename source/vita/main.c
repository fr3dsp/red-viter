#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "inih/ini.h"
#include "drc_core.h"
#include "render_thread.h"
#include "v810_cpu.h"
#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_gui.h"
#include "vb_set.h"
#include "vb_sound.h"
#include "video_hard.h"
#include "vita_ui.h"

int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;

#if !DRC_AVAILABLE
int drc_handleInterrupts(WORD cpsr, WORD *PC) { return 0; }
void drc_relocTable(void) {}
#endif

#define FRAME_US 20000
#define STICK_DEAD 60
#define MAX_ENTRIES 4096
#define CFG_PATH CONFIG_DIR "/vita.cfg"
#define BTN_L (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)
#define BTN_R (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)
#define BROWSER_ROWS 16
#define BROWSER_ROW_H 24
#define MENU_PX 130
#define MENU_PW 700
#define MENU_ROW_H 24

enum {
    ACT_RESUME,
    ACT_BROWSER,
    ACT_QUIT,
};

typedef struct {
    char name[256];
    bool dir;
} Entry;

typedef struct {
    const char *name;
    bool multicol;
    int multiid;
    int tint;
} TintPreset;

static const TintPreset tintPresets[] = {
    {"Red", false, 0, 0x0000ff},
    {"White", false, 0, 0xffffff},
    {"Green", false, 0, 0x00ff00},
    {"Amber", false, 0, 0x0080ff},
    {"Palette A", true, 1, 0x0000ff},
    {"Palette B", true, 2, 0x0000ff},
    {"Palette C", true, 3, 0x0000ff},
};
#define TINT_COUNT (int)(sizeof(tintPresets) / sizeof(tintPresets[0]))

static const char *scaleNames[] = {"2x (768x448)", "Fit height", "Stretch"};
#define SCALE_COUNT 3

static int viewScale = 0;
static bool viewSmooth = false;
static int stateSlot = 0;

static unsigned int heldPrev;
static int repeatFrames;
static volatile int resumePending;
static volatile int callbackRunning;
static SceUID callbackThread = -1;

static int power_callback(int notifyId, int notifyCount, int powerInfo, void *common) {
    (void)notifyId;
    (void)notifyCount;
    (void)common;
    if (powerInfo & (SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_AFTER_SYSTEM_RESUME)) {
        drc_reopen_vm();
        resumePending = 1;
    }
    return 0;
}

static int callback_thread_main(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUID cbid = sceKernelCreateCallback("rv_power", 0, power_callback, NULL);
    if (cbid < 0) return cbid;
    int r = scePowerRegisterCallback(cbid);
    if (r < 0) {
        sceKernelDeleteCallback(cbid);
        return r;
    }
    while (callbackRunning) sceKernelDelayThreadCB(1000000);
    scePowerUnregisterCallback(cbid);
    sceKernelDeleteCallback(cbid);
    return 0;
}

static void callback_thread_start(void) {
    callbackRunning = 1;
    callbackThread = sceKernelCreateThread("rv_power_cb", callback_thread_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (callbackThread < 0) {
        callbackRunning = 0;
        return;
    }
    if (sceKernelStartThread(callbackThread, 0, NULL) < 0) {
        callbackRunning = 0;
        sceKernelDeleteThread(callbackThread);
        callbackThread = -1;
    }
}

static void callback_thread_stop(void) {
    if (callbackThread < 0) return;
    callbackRunning = 0;
    sceKernelWaitThreadEnd(callbackThread, NULL, NULL);
    sceKernelDeleteThread(callbackThread);
    callbackThread = -1;
}

static uint64_t now_us(void) {
    return sceKernelGetProcessTimeWide();
}

static int cfg_handler(void *user, const char *section, const char *name, const char *value) {
    if (strcmp(name, "scale") == 0) {
        int v = atoi(value);
        if (v >= 0 && v < SCALE_COUNT) viewScale = v;
    } else if (strcmp(name, "smooth") == 0) {
        viewSmooth = atoi(value) != 0;
    } else if (strcmp(name, "dynarec") == 0) {
        drc_set_enabled(atoi(value) != 0);
    }
    return 1;
}

static void load_view_config(void) {
    ini_parse(CFG_PATH, cfg_handler, NULL);
    gpu_set_present(viewScale, viewSmooth);
}

static void save_view_config(void) {
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "[view]\nscale=%d\nsmooth=%d\n[emulation]\ndynarec=%d\n", viewScale, viewSmooth, drc_get_enabled() ? 1 : 0);
    fclose(f);
}

static void read_pad(SceCtrlData *pad) {
    memset(pad, 0, sizeof(*pad));
    sceCtrlPeekBufferPositive(0, pad, 1);
}

static unsigned int pad_pressed(const SceCtrlData *pad, bool allow_repeat) {
    unsigned int pressed = pad->buttons & ~heldPrev;
    if (allow_repeat) {
        unsigned int dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
        if (pad->buttons & dirs & heldPrev) {
            repeatFrames++;
            if (repeatFrames > 20 && (repeatFrames % 4) == 0) pressed |= pad->buttons & dirs;
        } else {
            repeatFrames = 0;
        }
    }
    heldPrev = pad->buttons;
    return pressed;
}

static void wait_release(void) {
    SceCtrlData pad;
    do {
        ui_begin(0, 0, 0);
        ui_end();
        read_pad(&pad);
    } while (pad.buttons != 0);
    heldPrev = 0;
    repeatFrames = 0;
}

static bool has_rom_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".vb") == 0 || strcasecmp(dot, ".zip") == 0;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *ea = a;
    const Entry *eb = b;
    if (ea->dir != eb->dir) return ea->dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static bool at_root(const char *path) {
    size_t n = strlen(path);
    return n >= 2 && path[n - 1] == '/' && path[n - 2] == ':';
}

static void path_up(char *p) {
    size_t n = strlen(p);
    while (n > 0 && p[n - 1] == '/') p[--n] = 0;
    char *slash = strrchr(p, '/');
    if (slash) {
        if (slash == p || slash[-1] == ':') slash[1] = 0;
        else *slash = 0;
    } else {
        char *colon = strrchr(p, ':');
        if (colon) strcpy(colon + 1, "/");
    }
}

static void path_join(char *out, size_t size, const char *dir, const char *name) {
    size_t n = strlen(dir);
    snprintf(out, size, "%s%s%s", dir, (n > 0 && dir[n - 1] == '/') ? "" : "/", name);
}

static int list_dir(const char *path, Entry *entries, int max) {
    int count = 0;
    SceUID dir = sceIoDopen(path);
    if (dir < 0) return -1;
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dir, &ent) > 0 && count < max) {
        bool is_dir = SCE_S_ISDIR(ent.d_stat.st_mode);
        if (ent.d_name[0] == '.') {
            memset(&ent, 0, sizeof(ent));
            continue;
        }
        if (is_dir || has_rom_extension(ent.d_name)) {
            snprintf(entries[count].name, sizeof(entries[count].name), "%s", ent.d_name);
            entries[count].dir = is_dir;
            count++;
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dir);
    qsort(entries, count, sizeof(Entry), entry_cmp);
    return count;
}

static bool dir_exists(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0) return false;
    sceIoDclose(d);
    return true;
}

static void fit_text(char *out, size_t size, const char *in, int max_chars) {
    if ((int)strlen(in) <= max_chars) snprintf(out, size, "%s", in);
    else snprintf(out, size, "...%s", in + strlen(in) - (max_chars - 3));
}

static void fit_name(char *out, size_t size, const char *in, int max_chars) {
    if ((int)strlen(in) <= max_chars) snprintf(out, size, "%s", in);
    else snprintf(out, size, "%.*s...", max_chars - 3, in);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void draw_icon(float x, float y, bool dir) {
    if (dir) {
        ui_rect(x, y + 3, 9, 3, COL_FOLDER);
        ui_rect(x, y + 6, 20, 12, COL_FOLDER);
        ui_rect(x, y + 8, 20, 1, 0xFFD890FFu);
    } else {
        ui_rect(x + 3, y + 1, 13, 18, COL_WHITE);
        ui_rect(x + 10, y + 1, 6, 6, 0xB8B8B8FFu);
        ui_rect(x + 5, y + 10, 9, 2, COL_RED);
        ui_rect(x + 5, y + 14, 9, 2, COL_RED);
    }
}

static void message_screen(const char *title, const char *line) {
    SceCtrlData pad;
    wait_release();
    for (;;) {
        ui_begin(0.02f, 0.0f, 0.0f);
        ui_backdrop();
        ui_header(title, "");
        ui_panel(130, 130, SCREEN_W - 260, 200);
        ui_text(162, 164, 2, COL_WHITE, "%s", line);
        ui_footer();
        ui_hint(16, SCREEN_H - 30, "X", "Continue");
        ui_end();
        read_pad(&pad);
        if (pad_pressed(&pad, false) & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) break;
    }
}

static bool browse_rom(char *out_path, size_t out_size) {
    Entry *entries = malloc(sizeof(Entry) * MAX_ENTRIES);
    if (!entries) return false;

    char cwd[300];
    cwd[0] = 0;
    if (tVBOpt.LAST_ROM[0]) {
        snprintf(cwd, sizeof(cwd), "%s", tVBOpt.LAST_ROM);
        path_up(cwd);
        if (strrchr(tVBOpt.LAST_ROM, '/') == NULL) cwd[0] = 0;
    }
    if (!cwd[0] || !dir_exists(cwd)) {
        if (dir_exists(ROM_DIR)) snprintf(cwd, sizeof(cwd), "%s", ROM_DIR);
        else snprintf(cwd, sizeof(cwd), "ux0:/");
    }

    int count = 0;
    int sel = 0;
    int top = 0;
    bool reload = true;
    bool chosen = false;
    SceCtrlData pad;
    wait_release();

    for (;;) {
        if (reload) {
            count = list_dir(cwd, entries, MAX_ENTRIES);
            if (count < 0) {
                path_up(cwd);
                count = list_dir(cwd, entries, MAX_ENTRIES);
                if (count < 0) count = 0;
            }
            sel = 0;
            top = 0;
            reload = false;
        }

        const int rows = BROWSER_ROWS;
        bool has_up = !at_root(cwd);
        int total = count + (has_up ? 1 : 0);
        const float list_y = 84;
        const float list_h = SCREEN_H - 40 - 8 - list_y;
        const float rows_y = list_y + (list_h - rows * BROWSER_ROW_H) / 2;

        char path_text[120];
        fit_text(path_text, sizeof(path_text), cwd, 100);

        ui_begin(0.02f, 0.0f, 0.0f);
        ui_backdrop();
        ui_header("Red Viter", path_text);
        ui_text_right(SCREEN_W - 24, 24, 1, COL_GREY, "%d %s", count, count == 1 ? "item" : "items");
        ui_panel(12, list_y, SCREEN_W - 24, list_h);
        for (int i = 0; i < rows && top + i < total; i++) {
            int idx = top + i;
            float y = rows_y + i * BROWSER_ROW_H;
            const char *name;
            bool is_dir;
            if (has_up && idx == 0) {
                name = "Parent folder";
                is_dir = true;
            } else {
                const Entry *e = &entries[idx - (has_up ? 1 : 0)];
                name = e->name;
                is_dir = e->dir;
            }
            bool selected = idx == sel;
            if (selected) {
                ui_rect_grad(20, y, SCREEN_W - 56, BROWSER_ROW_H, COL_SEL_TOP, COL_SEL_BOT);
                ui_rect(20, y, 4, BROWSER_ROW_H, COL_ACCENT);
            }
            draw_icon(36, y + 2, is_dir);
            char shown[256];
            fit_name(shown, sizeof(shown), name, 88);
            ui_text(70, y + 4, 1, selected ? COL_WHITE : (is_dir ? COL_GREY : COL_WHITE), "%s", shown);
        }
        ui_scrollbar(SCREEN_W - 28, rows_y, rows * BROWSER_ROW_H, top, rows, total);
        if (total == 0) {
            ui_text_center(SCREEN_W / 2, 220, 2, COL_GREY, "No .vb or .zip files here");
            ui_text_center(SCREEN_W / 2, 270, 1, COL_MUTED, "Copy your ROMs to " ROM_DIR "/");
        }
        ui_footer();
        float hx = 16;
        hx = ui_hint(hx, SCREEN_H - 30, "^v", "Select");
        hx = ui_hint(hx, SCREEN_H - 30, "< >", "Page");
        hx = ui_hint(hx, SCREEN_H - 30, "X", "Open");
        hx = ui_hint(hx, SCREEN_H - 30, "O", "Back");
        ui_hint(hx, SCREEN_H - 30, "START", "Quit");
        ui_end();

        read_pad(&pad);
        unsigned int pressed = pad_pressed(&pad, true);

        if (pressed & SCE_CTRL_UP) sel = sel > 0 ? sel - 1 : (total > 0 ? total - 1 : 0);
        if (pressed & SCE_CTRL_DOWN) sel = sel + 1 < total ? sel + 1 : 0;
        if (pressed & SCE_CTRL_LEFT) sel = sel > rows ? sel - rows : 0;
        if (pressed & SCE_CTRL_RIGHT) sel = sel + rows < total ? sel + rows : (total > 0 ? total - 1 : 0);
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;

        if (pressed & SCE_CTRL_START) {
            free(entries);
            return false;
        }
        if (pressed & SCE_CTRL_CIRCLE) {
            if (has_up) {
                path_up(cwd);
                reload = true;
            }
        }
        if ((pressed & SCE_CTRL_CROSS) && total > 0) {
            if (has_up && sel == 0) {
                path_up(cwd);
                reload = true;
            } else {
                const Entry *e = &entries[sel - (has_up ? 1 : 0)];
                char full[300];
                path_join(full, sizeof(full), cwd, e->name);
                if (e->dir) {
                    snprintf(cwd, sizeof(cwd), "%s", full);
                    reload = true;
                } else {
                    snprintf(out_path, out_size, "%s", full);
                    chosen = true;
                    break;
                }
            }
        }
    }

    free(entries);
    return chosen;
}

static void load_error_text(int code, char *out, size_t size) {
    if (code == EMSGSIZE) snprintf(out, size, "ROM size must be a power of two\nbetween 16 bytes and 16 MB.");
    else if (code == -1) snprintf(out, size, "Could not open the file.");
    else snprintf(out, size, "Error code %d.", code);
}

static bool load_rom(const char *path) {
    save_sram();

    if (tVBOpt.GAME_SETTINGS) loadFileOptions();

    snprintf(tVBOpt.ROM_PATH, sizeof(tVBOpt.ROM_PATH), "%s", path);
    snprintf(tVBOpt.RAM_PATH, sizeof(tVBOpt.RAM_PATH), "%s", path);
    char *dot = strrchr(tVBOpt.RAM_PATH, '.');
    if (dot) strcpy(dot, ".ram");
    else strncat(tVBOpt.RAM_PATH, ".ram", sizeof(tVBOpt.RAM_PATH) - strlen(tVBOpt.RAM_PATH) - 1);
    saveFileOptions();

    int ret = v810_load_init();
    if (ret) {
        char msg[200];
        load_error_text(ret, msg, sizeof(msg));
        message_screen("Failed to load ROM", msg);
        return false;
    }

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    for (;;) {
        ret = v810_load_step();
        if (ret < 0 || ret == 100) break;
        char shown[128];
        fit_name(shown, sizeof(shown), name, 100);
        ui_begin(0.02f, 0.0f, 0.0f);
        ui_backdrop();
        ui_header("Loading", shown);
        ui_text_center(SCREEN_W / 2, 220, 2, COL_WHITE, "Loading...");
        ui_progress(160, 280, 640, 28, ret / 100.0f);
        ui_text_center(SCREEN_W / 2, 322, 1, COL_GREY, "%d%%", ret < 0 ? 0 : ret);
        ui_end();
    }

    if (ret != 100) {
        char msg[200];
        load_error_text(ret, msg, sizeof(msg));
        message_screen("Failed to load ROM", msg);
        return false;
    }

    loadGameOptions();
    return true;
}

static int current_tint_index(void) {
    for (int i = 0; i < TINT_COUNT; i++) {
        if (tintPresets[i].multicol) {
            if (tVBOpt.MULTICOL && tVBOpt.MULTIID == tintPresets[i].multiid) return i;
        } else if (!tVBOpt.MULTICOL && tVBOpt.TINT == tintPresets[i].tint) {
            return i;
        }
    }
    return 0;
}

static void apply_tint_index(int i) {
    tVBOpt.MULTICOL = tintPresets[i].multicol;
    tVBOpt.MULTIID = tintPresets[i].multiid;
    tVBOpt.TINT = tintPresets[i].tint;
    tVBOpt.MODIFIED = true;
}

enum {
    MI_RESUME,
    MI_RESET,
    MI_SAVESTATE,
    MI_LOADSTATE,
    MI_SLOT,
    MI_SCALE,
    MI_FILTER,
    MI_TINT,
    MI_ANTIFLICKER,
    MI_DYNAREC,
    MI_SAVECFG,
    MI_BROWSER,
    MI_QUIT,
    MI_COUNT
};

static const char *menuLabels[MI_COUNT] = {
    "Resume",
    "Reset game",
    "Save state",
    "Load state",
    "State slot",
    "Screen size",
    "Scaling filter",
    "Colors",
    "Anti-flicker",
    "Dynamic Recompiler ",
    "Save settings",
    "Close game",
    "Quit Red Viter",
};

static int pause_menu(bool *reset_requested) {
    SceCtrlData pad;
    int sel = 0;
    char status[96] = "";
    int action = ACT_RESUME;

    sound_pause();
    save_sram();
    wait_release();

    for (;;) {
        char values[MI_COUNT][64];
        for (int i = 0; i < MI_COUNT; i++) values[i][0] = 0;
        bool adjustable[MI_COUNT] = {false};
        bool muted[MI_COUNT] = {false};
        snprintf(values[MI_SLOT], sizeof(values[0]), "%d", stateSlot);
        snprintf(values[MI_SCALE], sizeof(values[0]), "%s", scaleNames[viewScale]);
        snprintf(values[MI_FILTER], sizeof(values[0]), "%s", viewSmooth ? "Smooth" : "Sharp");
        snprintf(values[MI_TINT], sizeof(values[0]), "%s", tintPresets[current_tint_index()].name);
        snprintf(values[MI_ANTIFLICKER], sizeof(values[0]), "%s", tVBOpt.ANTIFLICKER ? "On" : "Off");
        if (!drc_available()) {
            snprintf(values[MI_DYNAREC], sizeof(values[0]), "Unavailable");
            muted[MI_DYNAREC] = true;
        } else {
            snprintf(values[MI_DYNAREC], sizeof(values[0]), "%s", drc_get_enabled() ? "On" : "Off");
            adjustable[MI_DYNAREC] = true;
        }
        if (!emulation_hasstate(stateSlot)) {
            snprintf(values[MI_LOADSTATE], sizeof(values[0]), "empty");
            muted[MI_LOADSTATE] = true;
        }
        adjustable[MI_SLOT] = adjustable[MI_SCALE] = adjustable[MI_FILTER] = adjustable[MI_TINT] = adjustable[MI_ANTIFLICKER] = true;

        char game[128];
        fit_name(game, sizeof(game), base_name(tVBOpt.ROM_PATH), 100);

        ui_begin(0.02f, 0.0f, 0.0f);
        ui_backdrop();
        ui_header("Paused", game);
        if (status[0]) ui_text_right(SCREEN_W - 24, 24, 1, COL_VALUE, "%s", status);
        ui_panel(MENU_PX, 84, MENU_PW, 408);

        float y = 94;
        for (int i = 0; i < MI_COUNT; i++) {
            const char *group = i == MI_RESUME ? "GAME" : i == MI_SCALE ? "DISPLAY" : i == MI_DYNAREC ? "SYSTEM" : NULL;
            if (group) {
                if (i != MI_RESUME) y += 6;
                int gw = ui_text_width(1, group);
                ui_text(MENU_PX + 24, y, 1, COL_MUTED, "%s", group);
                ui_rect(MENU_PX + 24 + gw + 12, y + 8, MENU_PW - 48 - gw - 12, 1, COL_LINE);
                y += 22;
            }
            bool selected = i == sel;
            if (selected) {
                ui_rect_grad(MENU_PX + 10, y, MENU_PW - 20, MENU_ROW_H, COL_SEL_TOP, COL_SEL_BOT);
                ui_rect(MENU_PX + 10, y, 4, MENU_ROW_H, COL_ACCENT);
            }
            ui_text(MENU_PX + 30, y + 5, 1, COL_WHITE, "%s", menuLabels[i]);
            if (values[i][0]) {
                char shown[80];
                if (selected && adjustable[i]) snprintf(shown, sizeof(shown), "< %s >", values[i]);
                else snprintf(shown, sizeof(shown), "%s", values[i]);
                unsigned int vc = selected ? COL_WHITE : (muted[i] ? COL_MUTED : COL_VALUE);
                float right = MENU_PX + MENU_PW - 30;
                ui_text_right(right, y + 5, 1, vc, "%s", shown);
                if (i == MI_TINT && !tintPresets[current_tint_index()].multicol) {
                    int t = tintPresets[current_tint_index()].tint;
                    unsigned int sw = ((unsigned int)(t & 0xff) << 24) | ((unsigned int)((t >> 8) & 0xff) << 16) | ((unsigned int)((t >> 16) & 0xff) << 8) | 0xFFu;
                    float sx = right - ui_text_width(1, shown) - 26;
                    ui_round_rect(sx, y + 5, 16, 16, COL_WHITE);
                    ui_round_rect(sx + 1, y + 6, 14, 14, sw);
                }
            }
            y += MENU_ROW_H;
        }

        ui_footer();
        float hx = 16;
        hx = ui_hint(hx, SCREEN_H - 30, "^v", "Select");
        hx = ui_hint(hx, SCREEN_H - 30, "< >", "Change");
        hx = ui_hint(hx, SCREEN_H - 30, "X", "Confirm");
        ui_hint(hx, SCREEN_H - 30, "O", "Resume");
        ui_end();

        read_pad(&pad);
        unsigned int pressed = pad_pressed(&pad, true);

        if (pressed & SCE_CTRL_UP) sel = sel > 0 ? sel - 1 : MI_COUNT - 1;
        if (pressed & SCE_CTRL_DOWN) sel = sel + 1 < MI_COUNT ? sel + 1 : 0;
        if (pressed & SCE_CTRL_CIRCLE) break;

        int delta = 0;
        if (pressed & SCE_CTRL_LEFT) delta = -1;
        if (pressed & SCE_CTRL_RIGHT) delta = 1;
        bool confirm = (pressed & SCE_CTRL_CROSS) != 0;

        if (delta != 0 || confirm) {
            int step = delta != 0 ? delta : 1;
            switch (sel) {
                case MI_RESUME:
                    if (confirm) goto done;
                    break;
                case MI_RESET:
                    if (confirm) {
                        *reset_requested = true;
                        goto done;
                    }
                    break;
                case MI_SAVESTATE:
                    if (confirm) {
                        if (emulation_sstate(stateSlot) == 0) snprintf(status, sizeof(status), "State saved to slot %d", stateSlot);
                        else snprintf(status, sizeof(status), "Saving state failed");
                    }
                    break;
                case MI_LOADSTATE:
                    if (confirm) {
                        if (!emulation_hasstate(stateSlot)) {
                            snprintf(status, sizeof(status), "Slot %d is empty", stateSlot);
                        } else if (emulation_lstate(stateSlot) == 0) {
                            goto done;
                        } else {
                            snprintf(status, sizeof(status), "Loading state failed");
                        }
                    }
                    break;
                case MI_SLOT:
                    stateSlot = (stateSlot + step + 10) % 10;
                    break;
                case MI_SCALE:
                    viewScale = (viewScale + step + SCALE_COUNT) % SCALE_COUNT;
                    gpu_set_present(viewScale, viewSmooth);
                    save_view_config();
                    break;
                case MI_FILTER:
                    viewSmooth = !viewSmooth;
                    gpu_set_present(viewScale, viewSmooth);
                    save_view_config();
                    break;
                case MI_TINT:
                    apply_tint_index((current_tint_index() + step + TINT_COUNT) % TINT_COUNT);
                    break;
                case MI_ANTIFLICKER:
                    tVBOpt.ANTIFLICKER = !tVBOpt.ANTIFLICKER;
                    tVBOpt.MODIFIED = true;
                    break;
                case MI_DYNAREC:
                    if (drc_available()) {
                        drc_set_enabled(!drc_get_enabled());
                        save_view_config();
                    }
                    break;
                case MI_SAVECFG:
                    if (confirm) {
                        snprintf(status, sizeof(status), "%s", saveFileOptions() == 0 ? "Settings saved" : "Saving settings failed");
                    }
                    break;
                case MI_BROWSER:
                    if (confirm) {
                        action = ACT_BROWSER;
                        goto done;
                    }
                    break;
                case MI_QUIT:
                    if (confirm) {
                        action = ACT_QUIT;
                        goto done;
                    }
                    break;
            }
        }
    }

done:
    wait_release();
    clearCache();
    sound_refresh();
    return action;
}

static HWORD pad_to_vb(const SceCtrlData *pad) {
    unsigned int b = pad->buttons;
    HWORD v = 0x0002;

    if (b & SCE_CTRL_UP) v |= VB_LPAD_U;
    if (b & SCE_CTRL_DOWN) v |= VB_LPAD_D;
    if (b & SCE_CTRL_LEFT) v |= VB_LPAD_L;
    if (b & SCE_CTRL_RIGHT) v |= VB_LPAD_R;

    if (pad->ly < 128 - STICK_DEAD) v |= VB_LPAD_U;
    if (pad->ly > 128 + STICK_DEAD) v |= VB_LPAD_D;
    if (pad->lx < 128 - STICK_DEAD) v |= VB_LPAD_L;
    if (pad->lx > 128 + STICK_DEAD) v |= VB_LPAD_R;

    if (pad->ry < 128 - STICK_DEAD) v |= VB_RPAD_U;
    if (pad->ry > 128 + STICK_DEAD) v |= VB_RPAD_D;
    if (pad->rx < 128 - STICK_DEAD) v |= VB_RPAD_L;
    if (pad->rx > 128 + STICK_DEAD) v |= VB_RPAD_R;

    if (b & SCE_CTRL_CIRCLE) v |= VB_KEY_A;
    if (b & SCE_CTRL_CROSS) v |= VB_KEY_B;
    if (b & BTN_L) v |= VB_KEY_L;
    if (b & BTN_R) v |= VB_KEY_R;
    if (b & SCE_CTRL_START) v |= VB_KEY_START;
    if (b & SCE_CTRL_SELECT) v |= VB_KEY_SELECT;

    if ((v & VB_LPAD_L) && (v & VB_LPAD_R)) v &= ~(VB_LPAD_L | VB_LPAD_R);
    if ((v & VB_LPAD_U) && (v & VB_LPAD_D)) v &= ~(VB_LPAD_U | VB_LPAD_D);
    if ((v & VB_RPAD_L) && (v & VB_RPAD_R)) v &= ~(VB_RPAD_L | VB_RPAD_R);
    if ((v & VB_RPAD_U) && (v & VB_RPAD_D)) v &= ~(VB_RPAD_U | VB_RPAD_D);

    return v;
}

static void clear_hard_targets(void) {
    render_thread_sync();
    for (int i = 0; i < 2; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[i]);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

static int run_game(void) {
    bool on_time = false;
    bool just_lagged = false;
    bool display_blanked = false;
    int action = ACT_BROWSER;

    clearCache();
    sound_refresh();
    heldPrev = 0;
    uint64_t next_frame = now_us();

    for (;;) {
        if (resumePending) {
            resumePending = 0;
            drc_resume();
            heldPrev = 0;
            repeatFrames = 0;
            on_time = false;
            just_lagged = false;
            display_blanked = false;
            next_frame = now_us();
        }

        SceCtrlData pad;
        read_pad(&pad);

        if ((pad.buttons & BTN_L) && (pad.buttons & BTN_R) && (pad.buttons & SCE_CTRL_START)) {
            bool reset = false;
            int act = pause_menu(&reset);
            if (act == ACT_QUIT || act == ACT_BROWSER) {
                action = act;
                break;
            }
            if (reset) {
                v810_reset();
                clearCache();
            }
            on_time = false;
            just_lagged = false;
            display_blanked = false;
            next_frame = now_us();
            continue;
        }

        vb_state = &vb_players[0];

        bool is_golf = CHECK_GAMEID("01VVGE") || CHECK_GAMEID("E4VVGJ");
        bool is_test_chamber = CHECK_GAMEID("PRCHMB");
        if (is_golf || is_test_chamber) just_lagged = false;

        if (vb_state->tVIPREG.tFrame == 0 && !vb_state->tVIPREG.drawing) {
            int displayed_fb = vb_state->tVIPREG.tDisplayedFB;

            if (tDSPCACHE.DDSPDataState[displayed_fb] == CPU_WROTE) on_time = false;

            if (is_golf) {
                if (*(uint8_t *)(vb_state->V810_DISPLAY_RAM.off + 0x3dbc0) == 0x40 &&
                    *(uint16_t *)(vb_state->V810_DISPLAY_RAM.off + 0x3dbe6) == 0x48 &&
                    memcmp((uint8_t *)vb_state->V810_DISPLAY_RAM.off + 0x3dbec, "\0\0\x80\x01\x1f\0\0\x80\0\0", 10) == 0) {
                    if (tVBOpt.RENDERMODE != RM_CPUONLY) {
                        render_thread_sync();
                        tVBOpt.RENDERMODE = RM_CPUONLY;
                        clearCache();
                    }
                } else if (tVBOpt.RENDERMODE != RM_TOGPU) {
                    render_thread_sync();
                    tVBOpt.RENDERMODE = RM_TOGPU;
                    for (int i = 0; i < 3; i++) {
                        memset((uint8_t *)vb_state->V810_DISPLAY_RAM.off + (0x8000 * i), 0, 0x6000);
                    }
                }
            }

            if (is_test_chamber) {
                WORLD *worlds = (WORLD *)(vb_state->V810_DISPLAY_RAM.off + 0x3d800);
                bool vip_download = worlds[30].end;
                if (!vip_download) {
                    for (int i = 31; i >= 0; i--) {
                        if (worlds[i].end) break;
                        if (worlds[i].on == 0) continue;
                        if (worlds[i].bgm == 2) {
                            vip_download = true;
                            break;
                        }
                    }
                }
                if (vip_download) {
                    if (tVBOpt.RENDERMODE != RM_TOCPU) {
                        render_thread_sync();
                        tVBOpt.RENDERMODE = RM_TOCPU;
                    }
                } else if (tVBOpt.RENDERMODE != RM_TOGPU) {
                    render_thread_sync();
                    tVBOpt.RENDERMODE = RM_TOGPU;
                    clear_hard_targets();
                }
            }

            if ((vb_state->tVIPREG.DPCTRL & 0x0002) && (!on_time || !just_lagged)) {
                video_render(displayed_fb, on_time);
                on_time = true;
                display_blanked = false;
            } else if (on_time) {
                if (tVBOpt.ANTIFLICKER) {
                    video_flush(false);
                }
                on_time = false;
            } else if (!(vb_state->tVIPREG.DPCTRL & 0x0002) && !display_blanked) {
                ui_begin(0, 0, 0);
                ui_end();
                display_blanked = true;
            }
        }

        tVBOpt.FASTFORWARD = (pad.buttons & SCE_CTRL_TRIANGLE) != 0;

        HWORD inputs = pad_to_vb(&pad);
        vb_players[0].tHReg.SLB = inputs & 0xff;
        vb_players[0].tHReg.SHB = inputs >> 8;

        int err = v810_run();
        if (err) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Emulation error %d", err);
            message_screen("The emulator stopped", msg);
            action = ACT_BROWSER;
            break;
        }

        if (!tVBOpt.FASTFORWARD) {
            next_frame += FRAME_US;
            uint64_t now = now_us();
            if (now < next_frame) {
                just_lagged = false;
                while (next_frame - now > 1500) {
                    sceKernelDelayThread((SceUInt32)(next_frame - now - 1000));
                    now = now_us();
                }
                while (now_us() < next_frame) {
                }
            } else {
                just_lagged = true;
                if (now - next_frame > 5 * FRAME_US) next_frame = now;
            }
        } else {
            next_frame = now_us();
            just_lagged = false;
        }
    }

    save_sram();
    sound_pause();
    return action;
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    callback_thread_start();

    sceIoMkdir(CONFIG_DIR_PARENT, 0777);
    sceIoMkdir(CONFIG_DIR, 0777);
    sceIoMkdir(ROM_DIR, 0777);

    vglInitExtended(0, SCREEN_W, SCREEN_H, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);

    setDefaults();
    if (loadFileOptions() < 0) saveFileOptions();
    load_view_config();

    v810_init();
    video_init();
    ui_init();
    sound_init();

    char rom[300];
    int action = ACT_BROWSER;
    while (action != ACT_QUIT) {
        if (!browse_rom(rom, sizeof(rom))) break;
        if (!load_rom(rom)) continue;
        action = run_game();
    }

    save_sram();
    sound_close();
    video_quit();
    callback_thread_stop();
    sceKernelExitProcess(0);
    return 0;
}
