/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_game.c
 * @brief Rendering glue: fitting the portrait game onto the landscape screen.
 *
 * The game always believes its screen is DJ_GAME_W x DJ_GAME_H (544x960).
 * Two views can be switched at runtime:
 *
 * Rotated: the projection matrix gets an extra 90 degree rotation and the
 * picture fills the whole 960x544 framebuffer (Vita held upright).
 *
 * Pillarboxed: viewports are scaled down into a strip in the middle of the
 * landscape screen, and a scissor keeps the game's clears off the side bars.
 *
 * To switch while the game runs, the last viewport and orthographic
 * projection the game set are replayed with the new transformation.
 */

#include "dj_game.h"
#include "dj_java.h"

#include <psp2/io/fcntl.h>

#include "utils/logger.h"
#include "utils/utils.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define LANDSCAPE_FLAG DATA_PATH "landscape.txt"

int dj_rotated = 1;

const float dj_strip_scale = (float)SCREEN_H / DJ_GAME_H;
const int dj_strip_w = (int)(DJ_GAME_W * (float)SCREEN_H / DJ_GAME_H);
const int dj_strip_x = (SCREEN_W - (int)(DJ_GAME_W * (float)SCREEN_H / DJ_GAME_H)) / 2;

static GLfloat clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static int swapped_this_frame = 0;
static GLenum matrix_mode = GL_MODELVIEW;

// What the game last asked for, in its own coordinates
static GLint game_viewport[4] = { 0, 0, DJ_GAME_W, DJ_GAME_H };
static int have_ortho = 0;
static GLfloat game_ortho[6];

static void apply_viewport(void) {
    GLint x = game_viewport[0], y = game_viewport[1];
    GLsizei w = game_viewport[2], h = game_viewport[3];
    if (dj_rotated) {
        // Game x runs down the screen, game y runs to the right.
        glViewport(y, DJ_GAME_W - (x + w), h, w);
    } else {
        glViewport(dj_strip_x + (GLint)(x * dj_strip_scale), (GLint)(y * dj_strip_scale),
                   (GLsizei)(w * dj_strip_scale), (GLsizei)(h * dj_strip_scale));
    }
}

static void apply_scissor(void) {
    if (dj_rotated) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        glScissor(dj_strip_x, 0, dj_strip_w, SCREEN_H);
        glEnable(GL_SCISSOR_TEST);
    }
}

static void apply_projection_rotation(void) {
    // Everything the game projects is turned clockwise by 90 degrees.
    if (dj_rotated)
        glRotatef(-90.0f, 0.0f, 0.0f, 1.0f);
}

void dj_game_load_layout(void) {
    dj_rotated = !file_exists(LANDSCAPE_FLAG);
    l_info("View: %s", dj_rotated ? "rotated" : "pillarboxed");
}

void dj_gl_setup(void) {
    apply_viewport();
    apply_scissor();
}

void dj_set_rotated(int rotated) {
    if (rotated == dj_rotated) return;
    dj_rotated = rotated;

    // Remember the choice for the next start.
    if (rotated) {
        sceIoRemove(LANDSCAPE_FLAG);
    } else {
        SceUID fd = sceIoOpen(LANDSCAPE_FLAG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }

    apply_viewport();
    apply_scissor();

    if (have_ortho) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        apply_projection_rotation();
        glOrthof(game_ortho[0], game_ortho[1], game_ortho[2], game_ortho[3], game_ortho[4], game_ortho[5]);
        glMatrixMode(matrix_mode);
    }

    l_info("View switched to %s", rotated ? "rotated" : "pillarboxed");
}

void glViewport_dj(GLint x, GLint y, GLsizei w, GLsizei h) {
    game_viewport[0] = x;
    game_viewport[1] = y;
    game_viewport[2] = w;
    game_viewport[3] = h;
    apply_viewport();
}

void glDisable_dj(GLenum cap) {
    if (cap == GL_SCISSOR_TEST && !dj_rotated) return; // keep the game inside its strip
    glDisable(cap);
}

void glMatrixMode_dj(GLenum mode) {
    matrix_mode = mode;
    glMatrixMode(mode);
}

void glLoadIdentity_dj(void) {
    glLoadIdentity();
    if (matrix_mode == GL_PROJECTION)
        apply_projection_rotation();
}

void glOrthof_dj(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    if (matrix_mode == GL_PROJECTION) {
        game_ortho[0] = l;
        game_ortho[1] = r;
        game_ortho[2] = b;
        game_ortho[3] = t;
        game_ortho[4] = n;
        game_ortho[5] = f;
        have_ortho = 1;
    }
    glOrthof(l, r, b, t, n, f);
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
