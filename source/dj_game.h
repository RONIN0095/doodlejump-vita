/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_game.h
 * @brief Rendering glue: fitting the portrait game onto the landscape screen.
 */

#ifndef DJ_GAME_H
#define DJ_GAME_H

#include <vitaGL.h>

// The screen size the game is told about
#define DJ_GAME_W 544
#define DJ_GAME_H 960

// 1: rotated full screen (Vita held upright), 0: pillarboxed strip
extern int dj_rotated;

// Pillarboxed strip on the 960x544 screen
extern const float dj_strip_scale;
extern const int dj_strip_w, dj_strip_x;

void dj_game_load_layout(void);
void dj_set_rotated(int rotated);

void dj_gl_setup(void);
void dj_gl_begin_frame(void);
void dj_gl_end_frame(void);

// Imports of the .so redirected through the current view
void glViewport_dj(GLint x, GLint y, GLsizei w, GLsizei h);
void glDisable_dj(GLenum cap);
void glClearColor_dj(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glMatrixMode_dj(GLenum mode);
void glLoadIdentity_dj(void);
void glOrthof_dj(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
unsigned int eglSwapBuffers_dj(void *display, void *surface);
void *eglGetCurrentDisplay_dj(void);
void *eglGetCurrentSurface_dj(int readdraw);

#endif // DJ_GAME_H
