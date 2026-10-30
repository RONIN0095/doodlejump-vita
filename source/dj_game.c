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
#include "dj_text.h"

#include <stdlib.h>
#include <string.h>

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

/*
 * Signature: a small "1theronin" in the game's handwriting in the top right
 * corner of the menus. Hidden while a round is played.
 */

#define SIGNATURE_TEXT "1theronin"
#define SIGNATURE_TEX_W 256
#define SIGNATURE_TEX_H 64
#define SIGNATURE_SIZE 40.0f   // text size inside the texture
#define SIGNATURE_SCALE 0.55f  // texture pixels -> game pixels
#define SIGNATURE_ALPHA 0.55f

static GLuint signature_tex = 0;
static int signature_failed = 0;

static int load_signature(void) {
    if (signature_tex || signature_failed) return signature_tex != 0;
    if (!dj_text_init()) {
        signature_failed = 1;
        return 0;
    }
    uint8_t *rgba = calloc(SIGNATURE_TEX_W * SIGNATURE_TEX_H, 4);
    dj_text_draw(rgba, SIGNATURE_TEX_W, SIGNATURE_TEX_H, SIGNATURE_TEXT, -1,
                 SIGNATURE_TEX_W - 4, SIGNATURE_TEX_H * 0.7f, SIGNATURE_SIZE, DJ_ALIGN_RIGHT, 40, 40, 40);
    signature_tex = dj_text_upload(rgba, SIGNATURE_TEX_W, SIGNATURE_TEX_H);
    free(rgba);
    return signature_tex != 0;
}

static void draw_signature(void) {
    if (dj_java_gameplay_active() || dj_java_dialog_active()) return;

    // Save the state the game may rely on
    GLint active_tex, bound_tex, blend_src, blend_dst;
    GLfloat color[4];
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex);
    glGetIntegerv(GL_BLEND_SRC, &blend_src);
    glGetIntegerv(GL_BLEND_DST, &blend_dst);
    glGetFloatv(GL_CURRENT_COLOR, color);
    GLboolean tex2d = glIsEnabled(GL_TEXTURE_2D);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean alpha_test = glIsEnabled(GL_ALPHA_TEST);
    GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);

    if (!load_signature()) {
        glBindTexture(GL_TEXTURE_2D, bound_tex);
        glActiveTexture(active_tex);
        return;
    }

    GLint saved_viewport[4] = { game_viewport[0], game_viewport[1], game_viewport[2], game_viewport[3] };
    game_viewport[0] = 0;
    game_viewport[1] = 0;
    game_viewport[2] = DJ_GAME_W;
    game_viewport[3] = DJ_GAME_H;
    apply_viewport();

    // Game pixels, origin top left
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    apply_projection_rotation();
    glOrthof(0, DJ_GAME_W, DJ_GAME_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, signature_tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glColor4f(1.0f, 1.0f, 1.0f, SIGNATURE_ALPHA);

    float qw = SIGNATURE_TEX_W * SIGNATURE_SCALE, qh = SIGNATURE_TEX_H * SIGNATURE_SCALE;
    float x1 = DJ_GAME_W - 8.0f, x0 = x1 - qw;
    float y0 = 4.0f, y1 = y0 + qh;
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(1, 0); glVertex2f(x1, y0);
    glTexCoord2f(0, 1); glVertex2f(x0, y1);
    glTexCoord2f(1, 1); glVertex2f(x1, y1);
    glEnd();

    // Restore
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(matrix_mode);

    memcpy(game_viewport, saved_viewport, sizeof(saved_viewport));
    apply_viewport();

    glColor4f(color[0], color[1], color[2], color[3]);
    glBlendFunc(blend_src, blend_dst);
    if (!blend) glDisable(GL_BLEND);
    if (alpha_test) glEnable(GL_ALPHA_TEST);
    if (depth_test) glEnable(GL_DEPTH_TEST);
    if (!tex2d) glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, bound_tex);
    glActiveTexture(active_tex);
}

void dj_gl_end_frame(void) {
    // On Android, GLSurfaceView swaps after onDrawFrame returns.
    if (!swapped_this_frame) {
        draw_signature();
        vglSwapBuffers(dj_java_dialog_active() ? GL_TRUE : GL_FALSE);
    }
}

unsigned int eglSwapBuffers_dj(void *display, void *surface) {
    if (swapped_this_frame) return 1;
    swapped_this_frame = 1;
    draw_signature();
    vglSwapBuffers(dj_java_dialog_active() ? GL_TRUE : GL_FALSE);
    return 1;
}

void *eglGetCurrentDisplay_dj(void) {
    return (void *)0x1;
}

void *eglGetCurrentSurface_dj(int readdraw) {
    return (void *)0x1;
}
