/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_game.h
 * @brief Rendering glue: portrait game area inside the landscape screen.
 */

#ifndef DJ_GAME_H
#define DJ_GAME_H

#include <vitaGL.h>

extern int dj_game_w, dj_game_h, dj_game_x;

void dj_gl_setup(void);
void dj_gl_begin_frame(void);
void dj_gl_end_frame(void);

// Imports of the .so redirected through the game area
void glViewport_dj(GLint x, GLint y, GLsizei w, GLsizei h);
void glDisable_dj(GLenum cap);
void glClearColor_dj(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
unsigned int eglSwapBuffers_dj(void *display, void *surface);
void *eglGetCurrentDisplay_dj(void);
void *eglGetCurrentSurface_dj(int readdraw);

#endif // DJ_GAME_H
