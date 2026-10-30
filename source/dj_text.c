/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_text.c
 * @brief Text rendering with the game's own handwritten bitmap font.
 *
 * The font is an AngelCode BMFont stored as a plist (doodlejumpfont.plist)
 * with a single atlas page (fonts/doodlejumpfont_X.png). Glyphs are scaled
 * and composited on the CPU, then uploaded as a GL texture.
 */

#include "dj_text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb/stb_image.h>

#include "utils/logger.h"
#include "utils/utils.h"

#define FONT_PLIST GAME_PATH "assets/doodlejumpfont.plist"
#define FONT_ATLAS GAME_PATH "assets/fonts/doodlejumpfont_X.png"

typedef struct {
    int id;
    int x, y, w, h;
    int xoffset, yoffset, xadvance;
} Glyph;

static Glyph *glyphs = NULL;
static int glyph_count = 0;

static uint8_t *atlas = NULL; // RGBA8
static int atlas_w = 0, atlas_h = 0;

// Distance from the top of a line to the baseline, and capital height, in atlas pixels
static int font_base = 0;
static int font_cap = 1;

static int glyph_cmp(const void *a, const void *b) {
    return ((const Glyph *)a)->id - ((const Glyph *)b)->id;
}

static const Glyph *find_glyph(int id) {
    Glyph key;
    key.id = id;
    return bsearch(&key, glyphs, glyph_count, sizeof(Glyph), glyph_cmp);
}

// Reads the value of the <string> that follows *p, advancing *p past it.
static int next_int(const char **p) {
    const char *s = strstr(*p, "<string>");
    if (!s) return 0;
    s += 8;
    *p = s;
    return atoi(s);
}

static int parse_plist(const char *xml) {
    int capacity = 512;
    glyphs = malloc(sizeof(Glyph) * capacity);

    const char *p = strstr(xml, "<key>char</key>");
    if (!p) return 0;

    Glyph g;
    memset(&g, 0, sizeof(g));
    int fields = 0;

    while ((p = strstr(p, "<key>-")) != NULL) {
        p += 6;
        const char *key = p;
        if (strncmp(key, "id<", 3) == 0) { g.id = next_int(&p); fields++; }
        else if (strncmp(key, "x<", 2) == 0) { g.x = next_int(&p); fields++; }
        else if (strncmp(key, "y<", 2) == 0) { g.y = next_int(&p); fields++; }
        else if (strncmp(key, "width<", 6) == 0) { g.w = next_int(&p); fields++; }
        else if (strncmp(key, "height<", 7) == 0) { g.h = next_int(&p); fields++; }
        else if (strncmp(key, "xoffset<", 8) == 0) { g.xoffset = next_int(&p); fields++; }
        else if (strncmp(key, "yoffset<", 8) == 0) { g.yoffset = next_int(&p); fields++; }
        else if (strncmp(key, "xadvance<", 9) == 0) { g.xadvance = next_int(&p); fields++; }
        else continue;

        if (fields == 8) {
            if (glyph_count == capacity) {
                capacity *= 2;
                glyphs = realloc(glyphs, sizeof(Glyph) * capacity);
            }
            glyphs[glyph_count++] = g;
            memset(&g, 0, sizeof(g));
            fields = 0;
        }
    }

    qsort(glyphs, glyph_count, sizeof(Glyph), glyph_cmp);
    return glyph_count > 0;
}

int dj_text_init(void) {
    if (atlas) return 1;

    uint8_t *data = NULL;
    size_t size = 0;
    if (!file_load(FONT_PLIST, &data, &size)) {
        l_error("[TEXT] Could not load %s", FONT_PLIST);
        return 0;
    }
    char *xml = malloc(size + 1);
    memcpy(xml, data, size);
    xml[size] = '\0';
    free(data);

    int ok = parse_plist(xml);
    free(xml);
    if (!ok) {
        l_error("[TEXT] Could not parse %s", FONT_PLIST);
        return 0;
    }

    if (!file_load(FONT_ATLAS, &data, &size)) {
        l_error("[TEXT] Could not load %s", FONT_ATLAS);
        return 0;
    }
    int n;
    atlas = stbi_load_from_memory(data, (int)size, &atlas_w, &atlas_h, &n, 4);
    free(data);
    if (!atlas) {
        l_error("[TEXT] Could not decode %s", FONT_ATLAS);
        return 0;
    }

    const Glyph *H = find_glyph('H');
    if (H) {
        font_base = H->yoffset + H->h;
        font_cap = H->h > 0 ? H->h : 1;
    }

    l_info("[TEXT] Font loaded: %i glyphs, atlas %ix%i", glyph_count, atlas_w, atlas_h);
    return 1;
}

// Helvetica's capitals are about 0.72 of the text size.
float dj_text_cap_height(float size) {
    return size * 0.72f;
}

static float glyph_scale(float size) {
    return dj_text_cap_height(size) / (float)font_cap;
}

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = p[0], n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else { c &= 0x07; n = 3; }
    p++;
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++)
        c = (c << 6) | (*p & 0x3F);
    *s = (const char *)p;
    return c;
}

static float text_width(const char *text, int max_chars, float s) {
    float w = 0;
    for (int i = 0; *text && (max_chars < 0 || i < max_chars); i++) {
        const Glyph *g = find_glyph(utf8_next(&text));
        if (g) w += g->xadvance * s;
    }
    return w;
}

static void blend_pixel(uint8_t *px, uint8_t r, uint8_t g, uint8_t b, float a) {
    if (a <= 0.0f) return;
    float da = px[3] / 255.0f;
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) return;
    px[0] = (uint8_t)((r * a + px[0] * da * (1.0f - a)) / oa);
    px[1] = (uint8_t)((g * a + px[1] * da * (1.0f - a)) / oa);
    px[2] = (uint8_t)((b * a + px[2] * da * (1.0f - a)) / oa);
    px[3] = (uint8_t)(oa * 255.0f);
}

// Bilinear alpha lookup in the atlas, limited to one glyph's cell.
static float atlas_alpha(const Glyph *g, float u, float v) {
    u -= 0.5f;
    v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    float sum = 0.0f;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int x = x0 + i, y = y0 + j;
            if (x < 0 || y < 0 || x >= g->w || y >= g->h) continue;
            float wgt = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
            sum += wgt * atlas[((g->y + y) * atlas_w + (g->x + x)) * 4 + 3];
        }
    }
    return sum / 255.0f;
}

void dj_text_draw(uint8_t *rgba, int w, int h, const char *text, int max_chars,
                  float x, float y, float size, int align,
                  uint8_t r, uint8_t g, uint8_t b) {
    if (!atlas || !text) return;

    float s = glyph_scale(size);
    float pen = x;
    if (align == DJ_ALIGN_CENTER) pen -= text_width(text, max_chars, s) / 2.0f;
    else if (align == DJ_ALIGN_RIGHT) pen -= text_width(text, max_chars, s);

    for (int n = 0; *text && (max_chars < 0 || n < max_chars); n++) {
        const Glyph *gl = find_glyph(utf8_next(&text));
        if (!gl) continue;

        float gx = pen + gl->xoffset * s;
        float gy = y - (font_base - gl->yoffset) * s;
        int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
        int x1 = (int)ceilf(gx + gl->w * s), y1 = (int)ceilf(gy + gl->h * s);

        for (int py = y0; py < y1; py++) {
            if (py < 0 || py >= h) continue;
            for (int px = x0; px < x1; px++) {
                if (px < 0 || px >= w) continue;
                float u = (px + 0.5f - gx) / s;
                float v = (py + 0.5f - gy) / s;
                blend_pixel(&rgba[(py * w + px) * 4], r, g, b, atlas_alpha(gl, u, v));
            }
        }
        pen += gl->xadvance * s;
    }
}

void dj_text_fill_rect(uint8_t *rgba, int w, int h, int x0, int y0, int x1, int y1,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            blend_pixel(&rgba[(y * w + x) * 4], r, g, b, a / 255.0f);
}

GLuint dj_text_upload(const uint8_t *rgba, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}
