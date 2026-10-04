#ifndef VB_SET_H_
#define VB_SET_H_

#include <stdbool.h>
#include "vb_types.h"

#define CONFIG_DIR_PARENT "ux0:data"
#define CONFIG_DIR "ux0:data/redviter"
#define CONFIG_FILENAME CONFIG_DIR "/rv_config.ini"
#define DEFAULT_HOME_PATH CONFIG_DIR
#define ROM_DIR CONFIG_DIR "/roms"

typedef enum {
    RM_GPUONLY,
    RM_TOGPU,
    RM_TOCPU,
    RM_CPUONLY,
} RENDERMODE_t;

typedef struct VB_OPT {
    bool  MULTICOL;
    int   TINT;
    int   MULTIID;
    int   MTINT[4][4];
    float STINT[4][3];
    int   SOUND;
    int   FASTFORWARD;
    RENDERMODE_t RENDERMODE;
    bool  SOFT_FLUSH;
    char  ROM_PATH[300];
    char  RAM_PATH[300];
    char  LAST_ROM[300];
    char  HOME_PATH[240];
    unsigned long CRC32;
    int   GAME_ID;
    bool  GAME_SETTINGS;
    bool  MODIFIED;
    bool  ANTIFLICKER;
    bool  VIP_OVERCLOCK;
    bool  VIP_OVER_SOFT;
    bool  DOUBLE_BUFFER;
} VB_OPT;

#define MAKE_GAMEID(s) ((s)[0] ^ ((s)[1] << 5) ^ ((s)[2] << 10) ^ ((s)[3] << 15) ^ ((s)[4] << 20) ^ ((s)[5] << 25))
#define CHECK_GAMEID(s) (tVBOpt.GAME_ID == MAKE_GAMEID(s))

void setDefaults(void);
int loadFileOptions(void);
int saveFileOptions(void);
int loadGameOptions(void);

extern VB_OPT tVBOpt;

#endif
