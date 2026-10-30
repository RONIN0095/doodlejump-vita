/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_audio.h
 * @brief Replacement for the game's Java SoundPool/AudioTrack managers.
 */

#ifndef DJ_AUDIO_H
#define DJ_AUDIO_H

#include <stdint.h>

void dj_audio_init(void);

// Queues an asset (path relative to assets/) for background decoding.
void dj_audio_load(int64_t id, const char *asset_path, int looping);
void dj_audio_unload(int64_t id);

void dj_audio_play(int64_t id, float volume, int loop);
void dj_audio_stop(int64_t id);
void dj_audio_pause(int64_t id);
void dj_audio_resume(int64_t id);
void dj_audio_set_volume(int64_t id, float volume);

void dj_audio_pause_all(void);
void dj_audio_resume_all(void);
void dj_audio_stop_all(void);

#endif // DJ_AUDIO_H
