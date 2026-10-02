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
#define COL_BG_TOP 0x220609FFu
#define COL_BG_BOT 0x050102FFu
#define COL_HEAD_TOP 0x5A1218FFu
#define COL_HEAD_BOT 0x2C080CFFu
#define COL_LINE 0x70202AFFu
#define COL_PANEL 0x100304FFu
#define COL_SEL_TOP 0xB02A32FFu
#define COL_SEL_BOT 0x701820FFu
#define COL_ACCENT 0xFF5A5AFFu
#define COL_MUTED 0x8A6A6AFFu
#define COL_VALUE 0xFF9090FFu
#define COL_FOLDER 0xE0A040FFu
#define COL_CHIP 0x7A2028FFu

void vita_log(const char *fmt, ...);

void ui_init(void);
void ui_begin(float r, float g, float b);
void ui_rect(float x, float y, float w, float h, unsigned int rgba);
void ui_text(float x, float y, int scale, unsigned int rgba, const char *fmt, ...);
int ui_text_width(int scale, const char *s);
void ui_text_right(float xr, float y, int scale, unsigned int rgba, const char *fmt, ...);
void ui_text_center(float cx, float y, int scale, unsigned int rgba, const char *fmt, ...);
void ui_rect_grad(float x, float y, float w, float h, unsigned int top, unsigned int bottom);
void ui_round_rect(float x, float y, float w, float h, unsigned int rgba);
void ui_panel(float x, float y, float w, float h);
void ui_backdrop(void);
void ui_header(const char *title, const char *sub);
void ui_footer(void);
float ui_hint(float x, float y, const char *key, const char *label);
void ui_scrollbar(float x, float y, float h, int top, int visible, int total);
void ui_progress(float x, float y, float w, float h, float frac);
void ui_end(void);

void gpu_set_present(int scale_mode, bool smooth);
bool gpu_take_flushed(void);

#endif
