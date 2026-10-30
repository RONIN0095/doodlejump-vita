/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_game.c
 * @brief Rendering glue: portrait game area inside the landscape screen.
 *
 * The game believes its screen is dj_game_w x dj_game_h.
 *
 * Rotated mode: the game screen is 544x960 and the projection matrix gets an
 * extra 90 degree rotation, so the picture fills the 960x544 framebuffer.
 *
 * Pillarboxed mode: viewports are shifted into the middle of the framebuffer
 * and a permanent scissor keeps the game's clears off the black side bars.
 */

#include "dj_game.h"
#include "dj_java.h"

static GLfloat clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static int swapped_this_frame = 0;
static GLenum matrix_mode = GL_MODELVIEW;

void dj_gl_setup(void) {
    if (dj_rotated) {
        glViewport(0, 0, dj_game_h, dj_game_w);
        return;
    }
    glViewport(dj_game_x, 0, dj_game_w, dj_game_h);
    glScissor(dj_game_x, 0, dj_game_w, dj_game_h);
    glEnable(GL_SCISSOR_TEST);
}

void glViewport_dj(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (dj_rotated) {
        // Game x runs down the screen, game y runs to the right.
        glViewport(y, dj_game_w - (x + w), h, w);
        return;
    }
    glViewport(x + dj_game_x, y, w, h);
}

void glDisable_dj(GLenum cap) {
    if (cap == GL_SCISSOR_TEST && !dj_rotated) return; // keep the game inside its area
    glDisable(cap);
}

void glMatrixMode_dj(GLenum mode) {
    matrix_mode = mode;
    glMatrixMode(mode);
}

void glLoadIdentity_dj(void) {
    glLoadIdentity();
    // Everything the game projects afterwards is turned clockwise by 90 degrees.
    if (dj_rotated && matrix_mode == GL_PROJECTION)
        glRotatef(-90.0f, 0.0f, 0.0f, 1.0f);
}

void glClearColor_dj(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    clear_color[0] = r;
    clear_color[1] = g;
    clear_color[2] = b;
    clear_color[3] = a;
    glClearColor(r, g, b, a);
}

void dj_gl_begin_frame(void) {
    swapped_this_frame = 0;
    if (dj_rotated) return;

    // Clear the whole screen (side bars included) to black.
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glEnable(GL_SCISSOR_TEST);
}

void dj_gl_end_frame(void) {
    // On Android, GLSurfaceView swaps after onDrawFrame returns.
    if (!swapped_this_frame)
        vglSwapBuffers(dj_java_dialog_active() ? GL_TRUE : GL_FALSE);
}

unsigned int eglSwapBuffers_dj(void *display, void *surface) {
    if (swapped_this_frame) return 1;
    swapped_this_frame = 1;
    vglSwapBuffers(dj_java_dialog_active() ? GL_TRUE : GL_FALSE);
    return 1;
}

void *eglGetCurrentDisplay_dj(void) {
    return (void *)0x1;
}

void *eglGetCurrentSurface_dj(int readdraw) {
    return (void *)0x1;
}
