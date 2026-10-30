/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_hidden.h
 * @brief Hides menu buttons that do nothing on the Vita.
 */

#ifndef DJ_HIDDEN_H
#define DJ_HIDDEN_H

#ifdef __cplusplus
extern "C" {
#endif

// For an asset path relative to assets/, returns the full path of a
// replacement file to open instead, or NULL to open the original.
const char *dj_asset_override(const char *filename);

#ifdef __cplusplus
}
#endif

#endif // DJ_HIDDEN_H
