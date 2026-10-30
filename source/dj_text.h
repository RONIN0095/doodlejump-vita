/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_text.h
 * @brief Text rendering with the game's own handwritten bitmap font.
 *
 * Replaces Android's Canvas.drawText() for the textures the Java side used to
 * generate (store prices, score markers), and draws the menu signature.
 */

#ifndef DJ_TEXT_H
#define DJ_TEXT_H

#include <stdint.h>
#include <vitaGL.h>

#define DJ_ALIGN_LEFT 0
#define DJ_ALIGN_CENTER 1
#define DJ_ALIGN_RIGHT 2

// Loads assets/doodlejumpfont.plist and its atlas. Returns 0 on failure.
int dj_text_init(void);

// Height of capital letters for a given Android text size (in pixels).
float dj_text_cap_height(float size);

// Draws UTF-8 text into a w x h RGBA8 image (rows top to bottom).
// y is the baseline. At most max_chars characters are drawn (-1: all).
void dj_text_draw(uint8_t *rgba, int w, int h, const char *text, int max_chars,
                  float x, float y, float size, int align,
                  uint8_t r, uint8_t g, uint8_t b);

// Fills a rectangle in an RGBA8 image, clipped.
void dj_text_fill_rect(uint8_t *rgba, int w, int h, int x0, int y0, int x1, int y1,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// Creates a linear-filtered GL texture from an RGBA8 image.
GLuint dj_text_upload(const uint8_t *rgba, int w, int h);

#endif // DJ_TEXT_H
