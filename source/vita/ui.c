#include <vitaGL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "render_thread.h"
#include "vita_ui.h"

#define ATLAS_COLS 16
#define ATLAS_W (ATLAS_COLS * FONT_W)
#define ATLAS_H (6 * FONT_H)
#define MAX_TEXT 200

typedef struct {
    float x, y;
} vec2f;

static GLuint fontTexture;
static vec2f textPos[MAX_TEXT * 6];
static vec2f textUv[MAX_TEXT * 6];

void vita_log(const char *fmt, ...) {
    FILE *f = fopen("ux0:data/red-viter/log.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void ui_init(void) {
    unsigned int *pixels = calloc(ATLAS_W * ATLAS_H, sizeof(unsigned int));
    for (int g = 0; g < FONT_COUNT; g++) {
        int ox = (g % ATLAS_COLS) * FONT_W;
        int oy = (g / ATLAS_COLS) * FONT_H;
        for (int y = 0; y < FONT_H; y++) {
            for (int x = 0; x < FONT_W; x++) {
                if (font_bits[g][y] & (0x80 >> x)) pixels[(oy + y) * ATLAS_W + ox + x] = 0xFFFFFFFFu;
            }
        }
    }
    glGenTextures(1, &fontTexture);
    glBindTexture(GL_TEXTURE_2D, fontTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
}

void ui_begin(float r, float g, float b) {
    render_thread_sync();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void set_color(unsigned int rgba) {
    glColor4f(((rgba >> 24) & 0xff) / 255.0f, ((rgba >> 16) & 0xff) / 255.0f, ((rgba >> 8) & 0xff) / 255.0f, (rgba & 0xff) / 255.0f);
}

void ui_rect(float x, float y, float w, float h, unsigned int rgba) {
    vec2f quad[4] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
}

int ui_text_width(int scale, const char *s) {
    return (int)strlen(s) * FONT_W * scale;
}

void ui_text(float x, float y, int scale, unsigned int rgba, const char *fmt, ...) {
    char buf[MAX_TEXT + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int count = 0;
    float cx = x;
    for (const char *p = buf; *p && count < MAX_TEXT; p++) {
        int code = (unsigned char)*p;
        if (code == '\n') {
            cx = x;
            y += FONT_H * scale;
            continue;
        }
        if (code < FONT_FIRST || code >= FONT_FIRST + FONT_COUNT) code = '?';
        int g = code - FONT_FIRST;
        float u0 = (float)((g % ATLAS_COLS) * FONT_W) / ATLAS_W;
        float v0 = (float)((g / ATLAS_COLS) * FONT_H) / ATLAS_H;
        float u1 = u0 + (float)FONT_W / ATLAS_W;
        float v1 = v0 + (float)FONT_H / ATLAS_H;
        float x1 = cx + FONT_W * scale;
        float y1 = y + FONT_H * scale;
        vec2f *pp = textPos + count * 6;
        vec2f *pu = textUv + count * 6;
        pp[0] = (vec2f){cx, y};
        pp[1] = (vec2f){x1, y};
        pp[2] = (vec2f){cx, y1};
        pp[3] = (vec2f){x1, y};
        pp[4] = (vec2f){x1, y1};
        pp[5] = (vec2f){cx, y1};
        pu[0] = (vec2f){u0, v0};
        pu[1] = (vec2f){u1, v0};
        pu[2] = (vec2f){u0, v1};
        pu[3] = (vec2f){u1, v0};
        pu[4] = (vec2f){u1, v1};
        pu[5] = (vec2f){u0, v1};
        count++;
        cx += FONT_W * scale;
    }
    if (count == 0) return;

    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, fontTexture);
    set_color(rgba);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, textPos);
    glTexCoordPointer(2, GL_FLOAT, 0, textUv);
    glDrawArrays(GL_TRIANGLES, 0, count * 6);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

void ui_end(void) {
    vglSwapBuffers(GL_FALSE);
}
