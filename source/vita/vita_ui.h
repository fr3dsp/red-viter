#ifndef VITA_UI_H
#define VITA_UI_H

#include <stdbool.h>

#define SCREEN_W 960
#define SCREEN_H 544

#define COL_WHITE 0xFFFFFFFFu
#define COL_GREY 0xA0A0A0FFu
#define COL_RED 0xFF3030FFu
#define COL_DIM 0x000000B0u
#define COL_BAR 0x601010FFu
#define COL_SEL 0x802020FFu

void vita_log(const char *fmt, ...);

void ui_init(void);
void ui_begin(float r, float g, float b);
void ui_rect(float x, float y, float w, float h, unsigned int rgba);
void ui_text(float x, float y, int scale, unsigned int rgba, const char *fmt, ...);
int ui_text_width(int scale, const char *s);
void ui_end(void);

void gpu_set_present(int scale_mode, bool smooth);
bool gpu_take_flushed(void);

#endif
