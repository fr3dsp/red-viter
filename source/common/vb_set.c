#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "inih/ini.h"
#include "vb_set.h"

VB_OPT tVBOpt;

static bool parse_indexed(const char *name, const char *prefix, int min, int max, int *index) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) != 0 || name[n] < '0' || name[n] > '9' || name[n + 1] != 0) return false;
    *index = name[n] - '0';
    return *index >= min && *index <= max;
}

static void set_palette_value(VB_OPT *c, int pal, const char *name, const char *value) {
    int i;
    if (parse_indexed(name, "tint", 0, 3, &i)) c->MTINT[pal][i] = atoi(value);
    else if (parse_indexed(name, "scale", 1, 3, &i)) c->STINT[pal][i - 1] = atof(value);
}

static int handler(void *user, const char *section, const char *name, const char *value) {
    VB_OPT *c = user;
    int i;

    if (strcmp(section, "vbopt") == 0) {
        if (strcmp(name, "multicol") == 0) c->MULTICOL = atoi(value);
        else if (strcmp(name, "tint") == 0) c->TINT = atoi(value);
        else if (strcmp(name, "multicolid") == 0) c->MULTIID = atoi(value);
        else if (strcmp(name, "antiflicker") == 0) c->ANTIFLICKER = atoi(value);
        else if (strcmp(name, "vip_overclock") == 0) c->VIP_OVERCLOCK = atoi(value);
        else if (strcmp(name, "lastrom") == 0) {
            snprintf(c->ROM_PATH, sizeof(c->ROM_PATH), "%s", value);
            snprintf(c->LAST_ROM, sizeof(c->LAST_ROM), "%s", value);
        } else if (strcmp(name, "homepath") == 0) {
            snprintf(c->HOME_PATH, sizeof(c->HOME_PATH), "%s", value);
            size_t n = strlen(c->HOME_PATH);
            if (n > 0 && c->HOME_PATH[n - 1] == '/') c->HOME_PATH[n - 1] = 0;
        } else if (parse_indexed(name, "multitint", 0, 3, &i)) {
            c->MTINT[0][i] = atoi(value);
        } else if (parse_indexed(name, "tintscale", 1, 3, &i)) {
            c->STINT[0][i - 1] = atof(value);
        }
    } else if (strncmp(section, "palette", 7) == 0 && section[7] >= '0' && section[7] <= '3' && section[8] == 0) {
        set_palette_value(c, section[7] - '0', name, value);
    }
    return 1;
}

void setDefaults(void) {
    memset(&tVBOpt, 0, sizeof(tVBOpt));

    tVBOpt.SOUND = 1;
    tVBOpt.RENDERMODE = RM_TOGPU;
    tVBOpt.TINT = 0x0000ff;

    tVBOpt.MTINT[0][1] = tVBOpt.MTINT[0][2] = tVBOpt.MTINT[0][3] = 0x0000ff;
    tVBOpt.STINT[0][0] = tVBOpt.STINT[0][1] = tVBOpt.STINT[0][2] = 1.0;
    tVBOpt.MTINT[1][0] = 0x394129;
    tVBOpt.MTINT[1][1] = tVBOpt.MTINT[1][2] = tVBOpt.MTINT[1][3] = 0x10827b;
    tVBOpt.STINT[1][0] = tVBOpt.STINT[1][1] = tVBOpt.STINT[1][2] = 1.0;
    tVBOpt.MTINT[2][0] = 0x000208;
    tVBOpt.MTINT[2][1] = 0x031342;
    tVBOpt.MTINT[2][2] = 0x00bb1d;
    tVBOpt.MTINT[2][3] = 0x00d8ff;
    tVBOpt.STINT[2][0] = 2.5;
    tVBOpt.STINT[2][1] = tVBOpt.STINT[2][2] = 1.0;
    tVBOpt.MTINT[3][0] = 0xffffff;
    tVBOpt.STINT[3][0] = tVBOpt.STINT[3][1] = tVBOpt.STINT[3][2] = 1.0;

    snprintf(tVBOpt.HOME_PATH, sizeof(tVBOpt.HOME_PATH), "%s", DEFAULT_HOME_PATH);
}

static void make_config_dirs(void) {
    struct stat st;
    if (stat(CONFIG_DIR_PARENT, &st) == -1) mkdir(CONFIG_DIR_PARENT, 0777);
    if (stat(CONFIG_DIR, &st) == -1) mkdir(CONFIG_DIR, 0777);
}

static const char *getGameIniPath(void) {
    struct stat st;
    const char *last_slash = strrchr(tVBOpt.ROM_PATH, '/');
    if (last_slash == NULL) return NULL;

    static char inipath[300];
    snprintf(inipath, sizeof(inipath), "%s/configs", tVBOpt.HOME_PATH);
    if (stat(tVBOpt.HOME_PATH, &st) == -1 && mkdir(tVBOpt.HOME_PATH, 0777)) return NULL;
    if (stat(inipath, &st) == -1 && mkdir(inipath, 0777)) return NULL;

    size_t len = strlen(inipath);
    snprintf(inipath + len, sizeof(inipath) - len, "%s", last_slash);
    char *end = strrchr(inipath, '.');
    if (!end) end = inipath + strlen(inipath);
    if ((size_t)(end - inipath) + 5 > sizeof(inipath)) return NULL;
    strcpy(end, ".ini");
    return inipath;
}

int loadFileOptions(void) {
    int ret = ini_parse(CONFIG_FILENAME, handler, &tVBOpt);
    if (!ret) tVBOpt.GAME_SETTINGS = false;
    tVBOpt.MODIFIED = false;
    return ret;
}

int loadGameOptions(void) {
    const char *ini_path = getGameIniPath();
    int ret = ini_path ? ini_parse(ini_path, handler, &tVBOpt) : ENOENT;
    tVBOpt.GAME_SETTINGS = ret == 0;
    tVBOpt.MODIFIED = false;
    return ret;
}

static void writeOptionsFile(FILE *f) {
    fprintf(f, "[vbopt]\n");
    fprintf(f, "multicol=%d\n", tVBOpt.MULTICOL);
    fprintf(f, "tint=%d\n", tVBOpt.TINT);
    fprintf(f, "multicolid=%d\n", tVBOpt.MULTIID);
    fprintf(f, "antiflicker=%d\n", tVBOpt.ANTIFLICKER);
    fprintf(f, "vip_overclock=%d\n", tVBOpt.VIP_OVERCLOCK);
    fprintf(f, "lastrom=%s\n", tVBOpt.LAST_ROM);
    fprintf(f, "homepath=%s\n", tVBOpt.HOME_PATH);
    for (int i = 0; i < 4; i++) {
        fprintf(f, "\n[palette%d]\n", i);
        for (int j = 0; j < 4; j++) fprintf(f, "tint%d=%d\n", j, tVBOpt.MTINT[i][j]);
        for (int j = 0; j < 3; j++) fprintf(f, "scale%d=%f\n", j + 1, tVBOpt.STINT[i][j]);
    }
}

int saveFileOptions(void) {
    snprintf(tVBOpt.LAST_ROM, sizeof(tVBOpt.LAST_ROM), "%s", tVBOpt.ROM_PATH);

    make_config_dirs();
    FILE *f = fopen(CONFIG_FILENAME, "w");
    if (!f) return 1;

    writeOptionsFile(f);

    fclose(f);
    tVBOpt.GAME_SETTINGS = false;
    tVBOpt.MODIFIED = false;
    return 0;
}
