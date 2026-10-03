#include <vitaGL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render_thread.h"
#include "video_hard.h"
#include "vb_dsp.h"
#include "v810_mem.h"
#include "vb_set.h"
#include "vita_ui.h"

uint8_t maxRepeat = 1;

GLuint transparentPixelTexture;
static GLuint tileTexture;
static u32 *tileTextureBuffer;
GLuint screenTexHard[2];
GLuint screenTargetHard[2];
GLuint screenTexSoft[2];
static u32 *screenTexSoftBuffer;

static GLuint sChar, sFinal, sAffine;

static GLint charPosLoc, charTexLoc, charSelLoc, charOffsetLoc, charSamplerLoc, charToCpuLoc;
static GLint affPosLoc, affTexLoc, affSamplerLoc, affRepeatLoc, affStartLoc, affSizeLoc, affVisLoc;
static GLint finPosLoc, finTexLoc, finVipLoc, finSoftLoc, finOverLoc, finAlphaLoc;
static GLint finCol0Loc, finCol1Loc, finCol2Loc, finCol3Loc;

static float palSel[8][3];
static bool antiflickerBlend;
static bool flushedSinceTake;
static int presentFilter = GL_NEAREST;
static int presentScale = 0;

typedef struct {
    float x, y, u, v, s0, s1, s2;
} tilevert;

typedef struct {
    float x, y, u, v;
} affvert;

#define TILE_VERTS_PER_QUAD 6
#define TILE_CHUNK 4096

static tilevert *tileScratch;
static affvert *affScratch;
static int affScratchLines;

static const u32 texColors[4] = {0x00000000u, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};

static const char *kCharVertex =
    "precision highp float;\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute vec3 aSel;\n"
    "uniform vec2 uOffset;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec3 vSel;\n"
    "void main() {\n"
    "    vec2 realPos = aPosition * (1.0 / 256.0) - 1.0 + uOffset;\n"
    "    gl_Position = vec4(realPos.y, realPos.x, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vSel = aSel;\n"
    "}\n";

static const char *kCharFragment =
    "precision highp float;\n"
    "uniform sampler2D sTex;\n"
    "uniform float uToCpu;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec3 vSel;\n"
    "void main() {\n"
    "    vec4 color = texture2D(sTex, vTexCoord);\n"
    "    float k = dot(vSel, color.rgb);\n"
    "    float m1 = step(0.5, k) - step(1.5, k);\n"
    "    float m2 = step(1.5, k) - step(2.5, k);\n"
    "    float m3 = step(2.5, k);\n"
    "    vec3 normalCol = vec3(m1, m2, m3);\n"
    "    vec3 toCpuCol = vec3(0.0, k / 16.0, 0.0);\n"
    "    gl_FragColor = vec4(mix(normalCol, toCpuCol, uToCpu), color.a);\n"
    "}\n";

static const char *kAffineVertex =
    "precision highp float;\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition.y / 256.0 - 1.0, aPosition.x / 256.0 - 1.0, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

static const char *kAffineFragment =
    "precision highp float;\n"
    "uniform sampler2D sTex;\n"
    "uniform float uRepeat;\n"
    "uniform float uStartMap;\n"
    "uniform vec3 uWorldSize;\n"
    "uniform float uVisMask;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    vec2 tc = vTexCoord;\n"
    "    vec2 fullSize = uWorldSize.xy;\n"
    "    float inside = step(0.0, tc.x) * step(0.0, tc.y) * (1.0 - step(fullSize.x, tc.x)) * (1.0 - step(fullSize.y, tc.y));\n"
    "    inside = mix(inside, 1.0, uRepeat);\n"
    "    vec2 modSize = uWorldSize.zy;\n"
    "    tc = tc - modSize * floor(tc / modSize);\n"
    "    float subMap = floor(tc.x) + floor(tc.y) * uWorldSize.x;\n"
    "    float mapIndex = floor(uStartMap + subMap);\n"
    "    float mapRow = floor(mapIndex / 4.0);\n"
    "    float mapCol = mapIndex - 4.0 * mapRow;\n"
    "    float visHalf = floor(uVisMask / exp2(subMap)) / 2.0;\n"
    "    float visible = (visHalf - floor(visHalf)) * 2.0;\n"
    "    vec2 frac = tc.yx - floor(tc.yx);\n"
    "    vec4 color = texture2D(sTex, (frac + vec2(mapCol, mapRow)) * vec2(0.25, 0.5));\n"
    "    gl_FragColor = color * (inside * visible);\n"
    "}\n";

static const char *kFinalVertex =
    "precision highp float;\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

static const char *kFinalFragment =
    "precision highp float;\n"
    "uniform sampler2D sVip;\n"
    "uniform sampler2D sSoft;\n"
    "uniform vec3 uCol0;\n"
    "uniform vec3 uCol1;\n"
    "uniform vec3 uCol2;\n"
    "uniform vec3 uCol3;\n"
    "uniform float uVipOverSoft;\n"
    "uniform float uAlpha;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    vec4 vip = texture2D(sVip, vTexCoord);\n"
    "    vec4 soft = texture2D(sSoft, vTexCoord);\n"
    "    float vipLit = step(0.001, vip.x + vip.y + vip.z);\n"
    "    float softEmpty = 1.0 - step(0.001, soft.a);\n"
    "    float useVip = mix(softEmpty, vipLit, uVipOverSoft);\n"
    "    vec4 color = mix(soft, vip, useVip);\n"
    "    vec3 rgb = mix(mix(mix(uCol0, uCol1, color.x), uCol2, color.y), uCol3, color.z);\n"
    "    gl_FragColor = vec4(rgb, uAlpha);\n"
    "}\n";

static void log_shader(const char *what, GLuint object, bool program) {
    GLint len = 0;
    if (program) glGetProgramiv(object, GL_INFO_LOG_LENGTH, &len);
    else glGetShaderiv(object, GL_INFO_LOG_LENGTH, &len);
    char *msg = malloc(len > 1 ? len : 2);
    if (!msg) return;
    msg[0] = 0;
    if (len > 1) {
        if (program) glGetProgramInfoLog(object, len, NULL, msg);
        else glGetShaderInfoLog(object, len, NULL, msg);
    }
    vita_log("%s failed: %s", what, msg);
    free(msg);
}

static GLuint compile_stage(GLenum type, const char *source, const char *what) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) log_shader(what, shader, false);
    return shader;
}

static GLuint build_shader(const char *vs, const char *fs, const char *name) {
    char what[64];
    snprintf(what, sizeof(what), "%s vertex shader", name);
    GLuint v = compile_stage(GL_VERTEX_SHADER, vs, what);
    snprintf(what, sizeof(what), "%s fragment shader", name);
    GLuint f = compile_stage(GL_FRAGMENT_SHADER, fs, what);
    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        snprintf(what, sizeof(what), "%s program link", name);
        log_shader(what, program, true);
    }
    return program;
}

static void make_texture(GLuint tex, int w, int h, int wrap) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

void gpu_init(void) {
    sChar = build_shader(kCharVertex, kCharFragment, "tile");
    sAffine = build_shader(kAffineVertex, kAffineFragment, "affine");
    sFinal = build_shader(kFinalVertex, kFinalFragment, "present");

    charPosLoc = glGetAttribLocation(sChar, "aPosition");
    charTexLoc = glGetAttribLocation(sChar, "aTexCoord");
    charSelLoc = glGetAttribLocation(sChar, "aSel");
    charOffsetLoc = glGetUniformLocation(sChar, "uOffset");
    charSamplerLoc = glGetUniformLocation(sChar, "sTex");
    charToCpuLoc = glGetUniformLocation(sChar, "uToCpu");

    affPosLoc = glGetAttribLocation(sAffine, "aPosition");
    affTexLoc = glGetAttribLocation(sAffine, "aTexCoord");
    affSamplerLoc = glGetUniformLocation(sAffine, "sTex");
    affRepeatLoc = glGetUniformLocation(sAffine, "uRepeat");
    affStartLoc = glGetUniformLocation(sAffine, "uStartMap");
    affSizeLoc = glGetUniformLocation(sAffine, "uWorldSize");
    affVisLoc = glGetUniformLocation(sAffine, "uVisMask");

    finPosLoc = glGetAttribLocation(sFinal, "aPosition");
    finTexLoc = glGetAttribLocation(sFinal, "aTexCoord");
    finVipLoc = glGetUniformLocation(sFinal, "sVip");
    finSoftLoc = glGetUniformLocation(sFinal, "sSoft");
    finOverLoc = glGetUniformLocation(sFinal, "uVipOverSoft");
    finAlphaLoc = glGetUniformLocation(sFinal, "uAlpha");
    finCol0Loc = glGetUniformLocation(sFinal, "uCol0");
    finCol1Loc = glGetUniformLocation(sFinal, "uCol1");
    finCol2Loc = glGetUniformLocation(sFinal, "uCol2");
    finCol3Loc = glGetUniformLocation(sFinal, "uCol3");

    glGenTextures(1, &transparentPixelTexture);
    glBindTexture(GL_TEXTURE_2D, transparentPixelTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    u32 pixel = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);

    glGenTextures(1, &tileTexture);
    make_texture(tileTexture, 256, 512, GL_CLAMP_TO_EDGE);
    tileTextureBuffer = calloc(256 * 512, sizeof(u32));

    glGenTextures(2, screenTexHard);
    glGenFramebuffers(2, screenTargetHard);
    for (int i = 0; i < 2; i++) {
        make_texture(screenTexHard[i], 512, 512, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screenTexHard[i], 0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glGenTextures(2, screenTexSoft);
    for (int i = 0; i < 2; i++) {
        make_texture(screenTexSoft[i], 512, 512, GL_CLAMP_TO_EDGE);
    }
    screenTexSoftBuffer = calloc(512 * 512, sizeof(u32));

    glGenTextures(1, &tileMapCache[0].tex);
    glGenFramebuffers(1, &tileMapCache[0].target);
    for (int i = 1; i < AFFINE_CACHE_SIZE; i++) {
        tileMapCache[i].tex = tileMapCache[0].tex;
        tileMapCache[i].target = tileMapCache[0].target;
    }
    make_texture(tileMapCache[0].tex, 512 * 4, 512 * 2, GL_REPEAT);
    glBindFramebuffer(GL_FRAMEBUFFER, tileMapCache[0].target);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tileMapCache[0].tex, 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    tileScratch = malloc(sizeof(tilevert) * TILE_VERTS_PER_QUAD * TILE_CHUNK);
    affScratchLines = 1024;
    affScratch = malloc(sizeof(affvert) * 6 * affScratchLines);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
}

void gpu_clear_screen(int start_eye, int end_eye) {
    glDisable(GL_SCISSOR_TEST);
    glClearColor(rvb->tVIPREG.BKCOL == 1, rvb->tVIPREG.BKCOL == 2, rvb->tVIPREG.BKCOL == 3, rvb->tVIPREG.BKCOL != 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gpu_setup_drawing(void) {
    for (int i = 0; i < 4; i++) {
        HWORD pal = rvb->tVIPREG.GPLT[i];
        for (int j = 0; j < 3; j++) {
            pal = pal >> 2;
            palSel[i][j] = (float)(pal & 3);
        }
        pal = rvb->tVIPREG.JPLT[i];
        for (int j = 0; j < 3; j++) {
            pal = pal >> 2;
            palSel[i + 4][j] = (float)(pal & 3);
        }
    }
}

void gpu_setup_tile_drawing(void) {
    glUseProgram(sChar);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tileTexture);
    glUniform1i(charSamplerLoc, 0);
    glUniform1f(charToCpuLoc, tVBOpt.RENDERMODE == RM_TOCPU ? 1.0f : 0.0f);
}

void gpu_set_tile_offset(float xoffset, float yoffset) {
    glUniform2f(charOffsetLoc, xoffset, yoffset);
}

void gpu_target_screen(int drawn_fb) {
    glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[drawn_fb]);
    glViewport(0, 0, 512, 512);
}

void gpu_target_affine(int cache_id) {
    glBindFramebuffer(GL_FRAMEBUFFER, tileMapCache[cache_id].target);
    int xoff = cache_id % 4;
    int yoff = cache_id / 4;
    glViewport(512 * xoff, 512 * yoff, 512, 512);
    gpu_set_scissor(true, 512 * xoff, 512 * yoff, 512 * (xoff + 1), 512 * (yoff + 1));
}

void gpu_set_scissor(bool enabled, u32 left, u32 top, u32 right, u32 bottom) {
    if (enabled) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    glScissor(left, top, right - left, bottom - top);
}

void gpu_set_opaque(bool opaque) {
    if (opaque) glDisable(GL_BLEND);
    else glEnable(GL_BLEND);
}

static void emit_tile_quad(tilevert *out, const vertex *t) {
    float x0 = (float)t->x;
    float y0 = (float)t->y;
    float x1 = x0 + 8.0f;
    float y1 = y0 + 8.0f;
    float u = (float)t->u;
    float v = (float)t->v;
    bool hflip = t->orient >= 2;
    bool vflip = t->orient == 1 || t->orient == 3;
    float ua = (u + (hflip ? 1.0f : 0.0f)) / 32.0f;
    float ub = (u + (hflip ? 0.0f : 1.0f)) / 32.0f;
    float va = (v + (vflip ? 1.0f : 0.0f)) / 64.0f;
    float vb = (v + (vflip ? 0.0f : 1.0f)) / 64.0f;
    const float *sel = palSel[t->palette & 7];

    tilevert a = {x0, y0, ua, va, sel[0], sel[1], sel[2]};
    tilevert b = {x1, y0, ub, va, sel[0], sel[1], sel[2]};
    tilevert c = {x0, y1, ua, vb, sel[0], sel[1], sel[2]};
    tilevert d = {x1, y1, ub, vb, sel[0], sel[1], sel[2]};

    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = b;
    out[4] = d;
    out[5] = c;
}

void gpu_draw_tiles(int first, int count) {
    glEnableVertexAttribArray(charPosLoc);
    glEnableVertexAttribArray(charTexLoc);
    glEnableVertexAttribArray(charSelLoc);
    int done = 0;
    while (done < count) {
        int n = count - done;
        if (n > TILE_CHUNK) n = TILE_CHUNK;
        for (int i = 0; i < n; i++) {
            emit_tile_quad(tileScratch + i * TILE_VERTS_PER_QUAD, vbuf + first + done + i);
        }
        glVertexAttribPointer(charPosLoc, 2, GL_FLOAT, GL_FALSE, sizeof(tilevert), &tileScratch[0].x);
        glVertexAttribPointer(charTexLoc, 2, GL_FLOAT, GL_FALSE, sizeof(tilevert), &tileScratch[0].u);
        glVertexAttribPointer(charSelLoc, 3, GL_FLOAT, GL_FALSE, sizeof(tilevert), &tileScratch[0].s0);
        glDrawArrays(GL_TRIANGLES, 0, n * TILE_VERTS_PER_QUAD);
        done += n;
    }
    glDisableVertexAttribArray(charPosLoc);
    glDisableVertexAttribArray(charTexLoc);
    glDisableVertexAttribArray(charSelLoc);
}

static void emit_affine_quad(affvert *out, const avertex *l) {
    float x0 = (float)l->x1;
    float x1 = (float)l->x2;
    float y0 = (float)l->y1;
    float y1 = y0 + 1.0f;
    float ua = (float)l->u1 / 4096.0f;
    float va = (float)l->v1 / 4096.0f;
    float ub = (float)(l->u2 + l->uoff2) / 4096.0f;
    float vb = (float)(l->v2 + l->voff2) / 4096.0f;

    affvert a = {x0, y0, ua, va};
    affvert b = {x0, y1, ua, va};
    affvert c = {x1, y0, ub, vb};
    affvert d = {x1, y1, ub, vb};

    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = b;
    out[4] = d;
    out[5] = c;
}

void gpu_draw_affine(WORLD *world, int umin, int vmin, int umax, int vmax, int drawn_fb, avertex *vbufs[], bool visible[]) {
    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t map_count_pow = scx_pow + scy_pow;
    bool huge_bg = map_count_pow > 3;
    if (huge_bg) map_count_pow = 3;
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    uint8_t map_count = 1 << map_count_pow;
    mapid &= ~(map_count - 1);
    bool over = world->head & 0x80;
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;

    glUseProgram(sAffine);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tileMapCache[0].tex);
    glUniform1i(affSamplerLoc, 0);
    glUniform1f(affRepeatLoc, over ? 0.0f : 1.0f);
    glUniform1f(affStartLoc, (float)(mapid % AFFINE_CACHE_SIZE));
    glUniform3f(affSizeLoc, scx, scy, huge_bg ? 8 / scy : scx);

    int mask = 0;
    for (int sub_bg = 0; sub_bg < map_count; sub_bg++) {
        int cache_id = (mapid + sub_bg) % AFFINE_CACHE_SIZE;
        bool vis = visible[sub_bg] && tileMapCache[cache_id].bg == mapid + sub_bg && tileMapCache[cache_id].visible;
        if (vis) mask |= 1 << sub_bg;
    }
    glUniform1f(affVisLoc, (float)mask);

    glEnableVertexAttribArray(affPosLoc);
    glEnableVertexAttribArray(affTexLoc);

    for (int eye = 0; eye < 2; eye++) {
        if (vbufs[eye] == NULL) continue;
        int gx = base_gx + (eye == 0 ? -gp : gp);

        gpu_set_scissor(true, 256 * eye + (gy >= 0 ? gy : 0), gx >= 0 ? gx : 0, (gy + h < 256 ? gy + h : 256) + 256 * eye, gx + w);

        const avertex *lines = vbufs[eye];
        int done = 0;
        while (done < h) {
            int n = h - done;
            if (n > affScratchLines) n = affScratchLines;
            for (int i = 0; i < n; i++) {
                emit_affine_quad(affScratch + i * 6, lines + done + i);
            }
            glVertexAttribPointer(affPosLoc, 2, GL_FLOAT, GL_FALSE, sizeof(affvert), &affScratch[0].x);
            glVertexAttribPointer(affTexLoc, 2, GL_FLOAT, GL_FALSE, sizeof(affvert), &affScratch[0].u);
            glDrawArrays(GL_TRIANGLES, 0, n * 6);
            done += n;
        }
    }

    glDisableVertexAttribArray(affPosLoc);
    glDisableVertexAttribArray(affTexLoc);
}

void update_texture_cache_hard(void) {
    int min_updated = 2048, max_updated = -1;
    for (int t = 0; t < 2048; t++) {
        if (!rdsp->CharacterCache[t]) {
            if (blankTile < 0 && !tileVisible[t])
                blankTile = t;
            continue;
        }

        if (min_updated > t) min_updated = t;
        if (max_updated < t) max_updated = t;

        u16 *tile = (u16 *)(rvb->V810_DISPLAY_RAM.off + ((t & 0x600) << 6) + 0x6000 + (t & 0x1ff) * 16);

        int ty = t / 32;
        int tx = t % 32;
        u32 *dstbuf = tileTextureBuffer + ty * 256 * 8 + tx * 8;

        {
            bool tv = ((uint64_t *)tile)[0] | ((uint64_t *)tile)[1];
            tileVisible[t] = tv;
            if (!tv) {
                if (blankTile < 0) blankTile = t;
                for (int y = 0; y < 8; y++) {
                    memset(dstbuf + 256 * y, 0, 8 * sizeof(u32));
                }
                continue;
            }
        }

        for (int y = 0; y < 8; y++) {
            u16 row = *tile++;
            for (int x = 0; x < 8; x++) {
                dstbuf[x] = texColors[row & 3];
                row >>= 2;
            }
            dstbuf += 256;
        }
    }
    if (min_updated <= max_updated) {
        int first_row = (min_updated / 32) * 8;
        int rows = (max_updated / 32 + 1 - min_updated / 32) * 8;
        glBindTexture(GL_TEXTURE_2D, tileTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first_row, 256, rows, GL_RGBA, GL_UNSIGNED_BYTE, tileTextureBuffer + first_row * 256);
    }
}

void gpu_soft_to_texture(int displayed_fb) {
    if (rdsp->DDSPDataState[displayed_fb] == CPU_WROTE) {
        rdsp->DDSPDataState[displayed_fb] = GPU_WROTE;
    } else {
        return;
    }
    int start_eye = eye_count == 2 ? 0 : tVBOpt.DEFAULT_EYE;
    for (int eye = start_eye; eye < start_eye + eye_count; eye++) {
        for (int x = 0; x < 384; x++) {
            uint32_t *in_fb_ptr = (uint32_t *)(rvb->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * displayed_fb + x * (256 / 4));
            u32 *out_fb = screenTexSoftBuffer + x * 512 + eye * 256;

            for (int y = 0; y < 224 / 16; y++) {
                uint32_t blob = *in_fb_ptr++;
                for (int i = 0; i < 16; i++) {
                    *out_fb++ = texColors[blob & 3];
                    blob >>= 2;
                }
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, screenTexSoft[displayed_fb]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_RGBA, GL_UNSIGNED_BYTE, screenTexSoftBuffer);
}

static bool downloaded;

void gpu_reset_vip_download(void) {}

void gpu_init_vip_download(int start_eye, int end_eye, int drawn_fb) {
    downloaded = false;
    glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[tVBOpt.DOUBLE_BUFFER ? drawn_fb : 0]);
    for (int eye = start_eye; eye < end_eye; eye++) {
        glReadPixels(eye * 256, 0, DOWNLOADED_FRAMEBUFFER_WIDTH, 384, GL_RGBA, GL_UNSIGNED_BYTE, (u32 *)rgba4_framebuffers + (384 * DOWNLOADED_FRAMEBUFFER_WIDTH * eye));
    }
}

void video_download_vip(int drawn_fb) {
    if (tVBOpt.RENDERMODE != RM_TOCPU) return;
    if (downloaded) return;
    downloaded = true;
    for (int eye = 0; eye < 2; eye++) {
        const u32 *in_fb = (const u32 *)rgba4_framebuffers + (384 * DOWNLOADED_FRAMEBUFFER_WIDTH) * eye;
        uint32_t *out_fb = (uint32_t *)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);
        for (int x = 0; x < 384; x++) {
            for (int y = 0; y < 224; y += 16) {
                uint32_t buf = 0;
                for (int i = 0; i < 16; i++) {
                    uint32_t px = *in_fb++;
                    uint32_t g = (px >> 8) & 0xff;
                    uint32_t idx = ((g + 8) >> 4) & 3;
                    buf |= idx << (i * 2);
                }
                *out_fb++ = buf;
            }
            in_fb += DOWNLOADED_FRAMEBUFFER_WIDTH - 224;
            out_fb += (256 - 224) / 16;
        }
    }

    tDSPCACHE.DDSPDataState[drawn_fb] = CPU_WROTE;
    for (int i = 0; i < 64; i++) {
        tDSPCACHE.SoftBufWrote[drawn_fb][i].min = 0;
        tDSPCACHE.SoftBufWrote[drawn_fb][i].max = 31;
    }
}

void gpu_blend_antiflicker(void) {
    antiflickerBlend = true;
}

void gpu_blend_default(void) {
    antiflickerBlend = false;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

bool gpu_antiflicker_allowed(void) {
    return true;
}

void gpu_set_present(int scale_mode, bool smooth) {
    presentScale = scale_mode;
    presentFilter = smooth ? GL_LINEAR : GL_NEAREST;
}

static void set_color_uniform(GLint loc, int id, int brt) {
    int c = video_get_colour(id, brt);
    glUniform3f(loc, (c & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, ((c >> 16) & 0xff) / 255.0f);
}

static void present_rect(int *x, int *y, int *w, int *h) {
    int sw = SCREEN_W, sh = SCREEN_H;
    int dw, dh;
    if (presentScale == 0) {
        dw = 384 * 2;
        dh = 224 * 2;
    } else if (presentScale == 1) {
        dh = sh;
        dw = 384 * sh / 224;
    } else {
        dw = sw;
        dh = sh;
    }
    *w = dw;
    *h = dh;
    *x = (sw - dw) / 2;
    *y = (sh - dh) / 2;
}

bool gpu_take_flushed(void) {
    bool r = flushedSinceTake;
    flushedSinceTake = false;
    return r;
}

void gpu_flush(bool default_for_both, int displayed_fb, int vip_displayed_fb) {
    flushedSinceTake = true;
    int px, py, pw, ph;
    present_rect(&px, &py, &pw, &ph);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, SCREEN_W, SCREEN_H);

    if (!antiflickerBlend) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glViewport(px, py, pw, ph);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(sFinal);

    GLuint vipTex = tVBOpt.RENDERMODE != RM_TOCPU ? screenTexHard[vip_displayed_fb] : transparentPixelTexture;
    GLuint softTex = rdsp->DDSPDataState[displayed_fb] != GPU_CLEAR ? screenTexSoft[displayed_fb] : transparentPixelTexture;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vipTex);
    if (vipTex != transparentPixelTexture) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, presentFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, presentFilter);
    }
    glUniform1i(finVipLoc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, softTex);
    if (softTex != transparentPixelTexture) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, presentFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, presentFilter);
    }
    glUniform1i(finSoftLoc, 1);

    glUniform1f(finOverLoc, tVBOpt.VIP_OVER_SOFT ? 1.0f : 0.0f);
    glUniform1f(finAlphaLoc, antiflickerBlend ? 0.5f : 1.0f);

    int a = rvb->tVIPREG.BRTA * maxRepeat;
    int b = rvb->tVIPREG.BRTB * maxRepeat;
    int c = (rvb->tVIPREG.BRTA + rvb->tVIPREG.BRTB + rvb->tVIPREG.BRTC) * maxRepeat;
    set_color_uniform(finCol0Loc, 0, 0);
    set_color_uniform(finCol1Loc, 1, a);
    set_color_uniform(finCol2Loc, 2, b);
    set_color_uniform(finCol3Loc, 3, c);

    static const GLfloat positions[] = {
        -1, -1,
        1, -1,
        -1, 1,
        1, 1};
    GLfloat texcoords[8] = {
        224.0f / 512.0f, 0,
        224.0f / 512.0f, 384.0f / 512.0f,
        0, 0,
        0, 384.0f / 512.0f};

    int eye_shift = eye_count == 2 ? 0 : tVBOpt.DEFAULT_EYE;
    for (int i = 0; i < 4; i++) {
        texcoords[i * 2] += 0.5f * eye_shift;
    }

    glEnableVertexAttribArray(finPosLoc);
    glEnableVertexAttribArray(finTexLoc);
    glVertexAttribPointer(finPosLoc, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(finTexLoc, 2, GL_FLOAT, GL_FALSE, 0, texcoords);

    if (tVBOpt.ANAGLYPH) {
        glColorMask(tVBOpt.ANAGLYPH_LEFT & 1, tVBOpt.ANAGLYPH_LEFT & 2, tVBOpt.ANAGLYPH_LEFT & 4, GL_TRUE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        for (int i = 0; i < 4; i++) {
            texcoords[i * 2] += 0.5f;
        }
        glColorMask(tVBOpt.ANAGLYPH_RIGHT & 1, tVBOpt.ANAGLYPH_RIGHT & 2, tVBOpt.ANAGLYPH_RIGHT & 4, GL_TRUE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glDisableVertexAttribArray(finPosLoc);
    glDisableVertexAttribArray(finTexLoc);
}

void gpu_quit(void) {
    free(tileTextureBuffer);
    free(screenTexSoftBuffer);
    free(tileScratch);
    free(affScratch);
    tileTextureBuffer = NULL;
    screenTexSoftBuffer = NULL;
    tileScratch = NULL;
    affScratch = NULL;
}
