/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_hidden.c
 * @brief Hides menu buttons that do nothing on the Vita.
 *
 * "Remove ads", "share" and "toys" are hardcoded in the game. When it loads
 * their images, it gets a fully transparent PNG of the same size instead.
 * The replacements are generated once into DATA_PATH "cache/hidden/".
 */

#include "dj_hidden.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "utils/logger.h"
#include "utils/utils.h"

#define HIDDEN_DIR DATA_PATH "cache/hidden/"

static const char *hidden_buttons[] = {
    "removeads", "removeadson", "share", "shareon", "toys", "toyson",
};

// Matches "<button>_L.png" / "<button>_X.png", in any folder (localized ones too).
static int is_hidden(const char *base) {
    for (size_t i = 0; i < sizeof(hidden_buttons) / sizeof(hidden_buttons[0]); i++) {
        size_t n = strlen(hidden_buttons[i]);
        if (strncmp(base, hidden_buttons[i], n) == 0 &&
            (strcmp(base + n, "_L.png") == 0 || strcmp(base + n, "_X.png") == 0))
            return 1;
    }
    return 0;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static uint8_t *put_chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t len) {
    put_u32(p, len);
    memcpy(p + 4, type, 4);
    if (len) memcpy(p + 8, data, len);
    put_u32(p + 8 + len, (uint32_t)crc32(0, p + 4, len + 4));
    return p + 12 + len;
}

static int write_transparent_png(const char *path, uint32_t w, uint32_t h) {
    size_t raw_size = (size_t)(w * 4 + 1) * h; // filter byte + RGBA per row, all zero
    uint8_t *raw = calloc(raw_size, 1);
    uLongf z_size = compressBound(raw_size);
    uint8_t *z = malloc(z_size);
    if (!raw || !z || compress2(z, &z_size, raw, raw_size, 9) != Z_OK) {
        free(raw);
        free(z);
        return 0;
    }
    free(raw);

    uint8_t ihdr[13];
    put_u32(ihdr, w);
    put_u32(ihdr + 4, h);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // adaptive filtering
    ihdr[12] = 0; // no interlace

    uint8_t *png = malloc(8 + 25 + 12 + z_size + 12);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    memcpy(png, sig, 8);
    uint8_t *p = put_chunk(png + 8, "IHDR", ihdr, 13);
    p = put_chunk(p, "IDAT", z, (uint32_t)z_size);
    p = put_chunk(p, "IEND", NULL, 0);
    free(z);

    int ok = file_save(path, png, p - png);
    free(png);
    return ok;
}

const char *dj_asset_override(const char *filename) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (!is_hidden(base)) return NULL;

    // Keep localized variants apart: "localized/de/share_X.png" -> "localized_de_share_X.png"
    static char path[512];
    char flat[256];
    snprintf(flat, sizeof(flat), "%s", filename);
    for (char *c = flat; *c; c++)
        if (*c == '/') *c = '_';
    snprintf(path, sizeof(path), HIDDEN_DIR "%s", flat);

    if (file_exists(path)) return path;

    // The original's size, from its IHDR chunk
    char orig[512];
    snprintf(orig, sizeof(orig), GAME_PATH "assets/%s", filename);
    uint8_t *data = NULL;
    size_t size = 0;
    if (!file_load(orig, &data, &size)) return NULL;
    uint32_t w = 0, h = 0;
    if (size >= 24 && memcmp(data + 12, "IHDR", 4) == 0) {
        w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
        h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    }
    free(data);
    if (!w || !h || w > 4096 || h > 4096) return NULL;

    file_mkpath(HIDDEN_DIR, 0777);
    if (!write_transparent_png(path, w, h)) {
        l_error("[HIDE] Could not write %s", path);
        return NULL;
    }
    l_info("[HIDE] Hiding %s (%ux%u)", filename, w, h);
    return path;
}
