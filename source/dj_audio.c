/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_audio.c
 * @brief Replacement for the game's Java SoundPool/AudioTrack managers.
 *
 * Sounds are Ogg Vorbis assets, decoded to PCM on a loader thread with
 * stb_vorbis and mixed in software into a single SceAudio BGM port.
 */

#include "dj_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb/stb_vorbis.c>

#include "utils/logger.h"
#include "utils/utils.h"

#define MIX_RATE     44100
#define MIX_GRAIN    512
#define MAX_SOUNDS   256
#define MAX_VOICES   24

typedef enum {
    SOUND_FREE = 0,
    SOUND_QUEUED,
    SOUND_READY,
    SOUND_FAILED,
} SoundState;

typedef struct {
    int64_t id;
    volatile SoundState state;
    char path[256];
    int16_t *pcm;
    int frames;
    int channels;
    int rate;
} Sound;

typedef struct {
    Sound *sound;
    uint64_t pos;  // in source frames, 16.16 fixed point
    uint32_t step; // source frames per output frame, 16.16 fixed point
    float volume;
    int loops_left; // -1: forever
    int paused;
    int active;
} Voice;

static Sound sounds[MAX_SOUNDS];
static Voice voices[MAX_VOICES];
static int all_paused = 0;

static SceKernelLwMutexWork audio_lock;
static SceUID loader_sema;

static void lock(void) { sceKernelLockLwMutex(&audio_lock, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&audio_lock, 1); }

static Sound *find_sound(int64_t id) {
    for (int i = 0; i < MAX_SOUNDS; i++)
        if (sounds[i].state != SOUND_FREE && sounds[i].id == id) return &sounds[i];
    return NULL;
}

static void decode_sound(Sound *s) {
    char full[512];
    snprintf(full, sizeof(full), GAME_PATH "assets/%s", s->path);

    uint8_t *data = NULL;
    size_t size = 0;
    if (!file_load(full, &data, &size)) {
        l_error("[AUDIO] Could not read %s", full);
        s->state = SOUND_FAILED;
        return;
    }

    int channels = 0, rate = 0;
    short *pcm = NULL;
    int frames = stb_vorbis_decode_memory(data, (int)size, &channels, &rate, &pcm);
    free(data);

    if (frames <= 0 || !pcm || channels < 1 || channels > 2) {
        l_error("[AUDIO] Could not decode %s", full);
        free(pcm);
        s->state = SOUND_FAILED;
        return;
    }

    lock();
    s->pcm = pcm;
    s->frames = frames;
    s->channels = channels;
    s->rate = rate;
    s->state = SOUND_READY;
    unlock();
    l_debug("[AUDIO] Loaded %s: %i frames, %i ch, %i Hz", s->path, frames, channels, rate);
}

static int loader_thread(SceSize args, void *argp) {
    while (1) {
        sceKernelWaitSema(loader_sema, 1, NULL);
        for (int i = 0; i < MAX_SOUNDS; i++) {
            if (sounds[i].state == SOUND_QUEUED)
                decode_sound(&sounds[i]);
        }
    }
    return 0;
}

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int mixer_thread(SceSize args, void *argp) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, MIX_GRAIN, MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        l_error("[AUDIO] sceAudioOutOpenPort failed: 0x%08x", port);
        return 0;
    }

    static int16_t out[2][MIX_GRAIN * 2];
    static int32_t acc[MIX_GRAIN * 2];
    int cur = 0;

    while (1) {
        memset(acc, 0, sizeof(acc));

        lock();
        if (!all_paused) {
            for (int v = 0; v < MAX_VOICES; v++) {
                Voice *vc = &voices[v];
                if (!vc->active || vc->paused) continue;
                Sound *s = vc->sound;
                if (!s || s->state != SOUND_READY) {
                    vc->active = 0;
                    continue;
                }

                int32_t gain = (int32_t)(vc->volume * 256.0f);
                for (int i = 0; i < MIX_GRAIN; i++) {
                    uint32_t frame = (uint32_t)(vc->pos >> 16);
                    if (frame >= (uint32_t)s->frames) {
                        if (vc->loops_left != 0) {
                            if (vc->loops_left > 0) vc->loops_left--;
                            vc->pos -= (uint64_t)s->frames << 16;
                            frame = (uint32_t)(vc->pos >> 16);
                            if (frame >= (uint32_t)s->frames) {
                                vc->pos = 0;
                                frame = 0;
                            }
                        } else {
                            vc->active = 0;
                            break;
                        }
                    }

                    int32_t l, r;
                    if (s->channels == 2) {
                        l = s->pcm[frame * 2];
                        r = s->pcm[frame * 2 + 1];
                    } else {
                        l = r = s->pcm[frame];
                    }
                    acc[i * 2] += (l * gain) >> 8;
                    acc[i * 2 + 1] += (r * gain) >> 8;
                    vc->pos += vc->step;
                }
            }
        }
        unlock();

        for (int i = 0; i < MIX_GRAIN * 2; i++)
            out[cur][i] = clamp16(acc[i]);

        sceAudioOutOutput(port, out[cur]);
        cur ^= 1;
    }
    return 0;
}

void dj_audio_init(void) {
    sceKernelCreateLwMutex(&audio_lock, "dj_audio", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    loader_sema = sceKernelCreateSema("dj_audio_loader", 0, 0, 0x7FFFFFFF, NULL);

    // Keep decoding off the game's core and below its priority, so it does
    // not slow down the loading screen.
    SceUID t = sceKernelCreateThread("dj_audio_loader", loader_thread, 0x10000100 + 10, 0x10000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    sceKernelStartThread(t, 0, NULL);

    t = sceKernelCreateThread("dj_audio_mixer", mixer_thread, 0x10000100 - 10, 0x10000, 0,
                              SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(t, 0, NULL);

    l_success("[AUDIO] Initialized.");
}

void dj_audio_load(int64_t id, const char *asset_path, int looping) {
    if (!asset_path) return;

    lock();
    Sound *s = find_sound(id);
    if (s) {
        unlock();
        return;
    }
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (sounds[i].state == SOUND_FREE) {
            s = &sounds[i];
            memset(s, 0, sizeof(Sound));
            s->id = id;
            strncpy(s->path, asset_path, sizeof(s->path) - 1);
            s->state = SOUND_QUEUED;
            break;
        }
    }
    unlock();

    if (!s) {
        l_error("[AUDIO] No free sound slots for %s", asset_path);
        return;
    }
    sceKernelSignalSema(loader_sema, 1);
}

void dj_audio_unload(int64_t id) {
    lock();
    Sound *s = find_sound(id);
    if (s && s->state != SOUND_QUEUED) {
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].sound == s) voices[v].active = 0;
        free(s->pcm);
        memset(s, 0, sizeof(Sound));
    }
    unlock();
}

void dj_audio_play(int64_t id, float volume, int loop) {
    lock();
    Sound *s = find_sound(id);
    if (!s || s->state != SOUND_READY) {
        unlock();
        return;
    }

    int slot = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].active) {
            slot = v;
            break;
        }
    }
    if (slot < 0) slot = 0; // steal the oldest slot

    Voice *vc = &voices[slot];
    vc->sound = s;
    vc->pos = 0;
    vc->step = (uint32_t)(((uint64_t)s->rate << 16) / MIX_RATE);
    vc->volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    vc->loops_left = loop < 0 ? -1 : loop;
    vc->paused = 0;
    vc->active = 1;
    unlock();
}

#define FOR_VOICES_OF(id, stmt) \
    do { \
        lock(); \
        Sound *s_ = find_sound(id); \
        for (int v = 0; s_ && v < MAX_VOICES; v++) \
            if (voices[v].active && voices[v].sound == s_) { Voice *vc = &voices[v]; stmt; } \
        unlock(); \
    } while (0)

void dj_audio_stop(int64_t id) { FOR_VOICES_OF(id, vc->active = 0); }
void dj_audio_pause(int64_t id) { FOR_VOICES_OF(id, vc->paused = 1); }
void dj_audio_resume(int64_t id) { FOR_VOICES_OF(id, vc->paused = 0); }

void dj_audio_set_volume(int64_t id, float volume) {
    FOR_VOICES_OF(id, vc->volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume));
}

void dj_audio_pause_all(void) {
    lock();
    all_paused = 1;
    unlock();
}

void dj_audio_resume_all(void) {
    lock();
    all_paused = 0;
    unlock();
}

void dj_audio_stop_all(void) {
    lock();
    for (int v = 0; v < MAX_VOICES; v++) voices[v].active = 0;
    unlock();
}
