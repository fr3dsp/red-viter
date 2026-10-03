#ifndef VB_GUI_H
#define VB_GUI_H

#include <stdbool.h>

#define SAVESTATE_VER   0x00000002

void save_sram(void);
bool emulation_hasstate(int state);
int emulation_sstate(int state);
int emulation_lstate(int state);

#endif
