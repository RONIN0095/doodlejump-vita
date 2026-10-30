/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_java.c
 * @brief Reimplementation of Doodle Jump's Java message handlers.
 *
 * Mirrors what UtilityMessageHandler, LSAudioManager, AudioTrackManager and
 * OpenGL10Renderer do on Android for the messages that matter offline.
 * Online features (ads, IAP, highscores, cloud sync, analytics) are dropped.
 */

#include "dj_java.h"
#include "dj_jni.h"
#include "dj_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>

#include "utils/logger.h"
#include "utils/utils.h"

#define SAVE_PATH DATA_PATH "files/"
#define PKG "com/limasky/doodlejumpandroid/"

static int (*sendMessageNative)(JNIEnv *env, jclass clazz, int msg, jobject data, int arg1, int arg2);
static jclass notification_center = NULL;
static jobject asset_manager = NULL;

void dj_java_init(so_module *mod) {
    sendMessageNative = (void *)so_symbol(mod, "Java_com_limasky_doodlejumpandroid_NotificationCenter_sendMessageNative");
    if (!sendMessageNative)
        l_fatal("[JAVA] sendMessageNative not found!");

    notification_center = (*dj_env())->FindClass(dj_env(), PKG "NotificationCenter");
    asset_manager = dj_new_object("android/content/res/AssetManager");

    file_mkpath(SAVE_PATH, 0777);
}

int dj_java_send_native(int msg, jobject data, int arg1, int arg2) {
    l_info("[JAVA] -> native msg %i (%s)", msg, data ? dj_class_name(data) : "null");
    int ret = sendMessageNative(dj_env(), notification_center, msg, data, arg1, arg2);
    l_info("[JAVA] -> native msg %i done", msg);
    return ret;
}

jobject dj_java_asset_manager(void) {
    return asset_manager;
}

/*
 * Files
 */

static size_t utf16_length(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue; // continuation byte
        n += (*p >= 0xF0) ? 2 : 1;          // 4-byte sequences need a surrogate pair
    }
    return n;
}

static void handle_save_text_file(jobject data) {
    char *name = dj_get_string(data, "filename");
    char *text = dj_get_string(data, "text");
    dj_set_int(data, "status", 0);

    if (name) {
        char path[512];
        snprintf(path, sizeof(path), SAVE_PATH "%s", name);
        if (file_save(path, (const uint8_t *)(text ? text : ""), text ? strlen(text) : 0))
            dj_set_int(data, "status", 1);
        else
            l_error("[JAVA] Failed to save text file: %s", path);
    }

    free(name);
    free(text);
}

// Like BufferedReader.readLine() concatenation on Android: line breaks are dropped.
static void strip_newlines(char *s) {
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != '\n' && *r != '\r') *w++ = *r;
    *w = '\0';
}

static void handle_load_text_file(jobject data) {
    char *name = dj_get_string(data, "filename");
    dj_set_int(data, "status", 0);
    dj_set_obj(data, "text", NULL);
    dj_set_int(data, "text_size", 0);

    if (name) {
        char path[512];
        uint8_t *buf = NULL;
        size_t size = 0;

        // Context.openFileInput() first, then the APK assets.
        snprintf(path, sizeof(path), SAVE_PATH "%s", name);
        if (!file_load(path, &buf, &size)) {
            snprintf(path, sizeof(path), GAME_PATH "assets/%s", name);
            if (!file_load(path, &buf, &size)) buf = NULL;
        }

        if (buf) {
            char *text = malloc(size + 1);
            memcpy(text, buf, size);
            text[size] = '\0';
            free(buf);

            strip_newlines(text);
            dj_set_string(data, "text", text);
            dj_set_int(data, "text_size", (jint)utf16_length(text));
            dj_set_int(data, "status", 1);
            free(text);
        } else {
            l_debug("[JAVA] Text file not found: %s", name);
        }
    }

    free(name);
}

static void handle_does_file_exist(jobject data) {
    char *name = dj_get_string(data, "filename");
    jboolean exists = JNI_FALSE;
    if (name) {
        char path[512];
        snprintf(path, sizeof(path), GAME_PATH "assets/%s", name);
        exists = file_exists(path) ? JNI_TRUE : JNI_FALSE;
    }
    dj_set_bool(data, "file_exists", exists);
    free(name);
}

/*
 * Alert dialogs
 */

#define MAX_PENDING_DIALOGS 4

typedef struct {
    int id;
    jlong user_data;
    char *text;
    char *positive;
    char *negative;
    char *neutral;
} PendingDialog;

static PendingDialog dialogs[MAX_PENDING_DIALOGS];
static int dialog_count = 0;
static int dialog_running = 0;
static SceMsgDialogButtonsParam dialog_buttons;
static SceMsgDialogUserMessageParam dialog_msg;

static void handle_show_alert_dialog(jobject data) {
    if (dialog_count >= MAX_PENDING_DIALOGS) {
        l_warn("[JAVA] Too many pending dialogs, dropping one.");
        return;
    }

    PendingDialog *d = &dialogs[dialog_count++];
    memset(d, 0, sizeof(*d));
    d->id = dj_get_int(data, "alert_dialog_id");
    d->user_data = dj_get_long(data, "user_data");
    d->positive = dj_get_string(data, "positive_button_text");
    d->negative = dj_get_string(data, "negative_button_text");
    d->neutral = dj_get_string(data, "neutral_button_text");

    char *title = dj_get_string(data, "title");
    char *message = dj_get_string(data, "message");
    size_t len = (title ? strlen(title) : 0) + (message ? strlen(message) : 0) + 4;
    d->text = malloc(len);
    snprintf(d->text, len, "%s%s%s", title ? title : "", (title && message) ? "\n\n" : "", message ? message : "");
    free(title);
    free(message);

    l_info("[JAVA] Alert dialog %i: %s", d->id, d->text);
}

static void free_dialog(PendingDialog *d) {
    free(d->text);
    free(d->positive);
    free(d->negative);
    free(d->neutral);
}

static void start_dialog(PendingDialog *d) {
    memset(&dialog_buttons, 0, sizeof(dialog_buttons));
    memset(&dialog_msg, 0, sizeof(dialog_msg));

    // Button order on screen: positive, neutral, negative
    const char *labels[3];
    int n = 0;
    if (d->positive) labels[n++] = d->positive;
    if (d->neutral) labels[n++] = d->neutral;
    if (d->negative) labels[n++] = d->negative;
    if (n == 0) labels[n++] = "Ok";

    dialog_msg.msg = (const SceChar8 *)d->text;
    if (n == 1) {
        dialog_msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    } else {
        dialog_buttons.msg1 = labels[0];
        dialog_buttons.msg2 = labels[1];
        dialog_buttons.msg3 = n > 2 ? labels[2] : NULL;
        dialog_buttons.fontSize1 = dialog_buttons.fontSize2 = dialog_buttons.fontSize3 = SCE_MSG_DIALOG_FONT_SIZE_DEFAULT;
        dialog_msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_3BUTTONS;
        dialog_msg.buttonParam = &dialog_buttons;
    }

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &dialog_msg;

    int ret = sceMsgDialogInit(&param);
    if (ret < 0) {
        l_error("[JAVA] sceMsgDialogInit failed: 0x%08x", ret);
        return;
    }
    dialog_running = 1;
}

static void finish_dialog(PendingDialog *d, int button_id) {
    // Map the on-screen button index back to positive/neutral/negative.
    const char *order[3] = { NULL, NULL, NULL };
    int n = 0;
    if (d->positive) order[n++] = "positive";
    if (d->neutral) order[n++] = "neutral";
    if (d->negative) order[n++] = "negative";

    const char *chosen;
    if (button_id >= 1 && button_id <= n)
        chosen = order[button_id - 1];
    else if (d->negative)
        chosen = "negative"; // dialog cancelled
    else
        chosen = n ? order[0] : "positive";

    jobject res = dj_new_object(PKG "UtilityMessageHandler$AlertDialogResultData");
    dj_set_bool(res, "positive_button_selected", strcmp(chosen, "positive") == 0);
    dj_set_bool(res, "negative_button_selected", strcmp(chosen, "negative") == 0);
    dj_set_bool(res, "neutral_button_selected", strcmp(chosen, "neutral") == 0);
    dj_set_int(res, "alert_dialog_id", d->id);
    dj_set_long(res, "user_data", d->user_data);

    l_info("[JAVA] Alert dialog %i answered: %s", d->id, chosen);
    dj_java_send_native(Msg_Alert_Dialog_Result, res, 0, 0);
}

static int gameplay_active = 0;

int dj_java_gameplay_active(void) {
    return gameplay_active;
}

int dj_java_dialog_active(void) {
    return dialog_running;
}

/*
 * Replies that Java sends asynchronously (sendMessageThreadSafe): delivered
 * from the main loop between frames, never from inside a native call.
 */

#define MAX_DEFERRED 16

static int deferred_msgs[MAX_DEFERRED];
static int deferred_count = 0;

static void defer_reply(int msg) {
    if (deferred_count < MAX_DEFERRED)
        deferred_msgs[deferred_count++] = msg;
}

static void deliver_deferred(void) {
    int n = deferred_count;
    int msgs[MAX_DEFERRED];
    memcpy(msgs, deferred_msgs, sizeof(int) * n);
    deferred_count = 0;
    for (int i = 0; i < n; i++)
        dj_java_send_native(msgs[i], NULL, 0, 0);
}

void dj_java_update(void) {
    deliver_deferred();

    if (dialog_running) {
        if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
            return;

        SceMsgDialogResult result;
        memset(&result, 0, sizeof(result));
        sceMsgDialogGetResult(&result);
        sceMsgDialogTerm();
        dialog_running = 0;

        PendingDialog d = dialogs[0];
        memmove(&dialogs[0], &dialogs[1], sizeof(PendingDialog) * (MAX_PENDING_DIALOGS - 1));
        dialog_count--;

        finish_dialog(&d, result.buttonId);
        free_dialog(&d);
    }

    if (!dialog_running && dialog_count > 0)
        start_dialog(&dialogs[0]);
}

/*
 * Dispatch
 */

static void log_unhandled(int msg) {
    static uint8_t seen[256];
    uint8_t idx = (uint8_t)msg;
    if (!seen[idx]) {
        seen[idx] = 1;
        l_info("[JAVA] Ignoring message %i", msg);
    }
}

int dj_java_receive_message(int msg, jobject data, int arg1, int arg2) {
    static uint8_t logged[256];
    if (logged[(uint8_t)msg] < 3) {
        logged[(uint8_t)msg]++;
        l_info("[JAVA] <- native msg %i (%s)", msg, data ? dj_class_name(data) : "null");
    }
    switch (msg) {
        case Msg_Load_Sound: {
            char *name = dj_get_string(data, "filename");
            dj_audio_load(dj_get_long(data, "identifier"), name, dj_get_bool(data, "isLooping"));
            free(name);
            break;
        }
        case Msg_Unload_Sound:
        case Msg_Stop_Sound:
        case Msg_Pause_Sound:
        case Msg_Resume_Sound:
            return dj_java_receive_audio_message(msg, dj_get_long(data, "identifier"), 0.0f, 0);
        case Msg_Play_Sound:
            return dj_java_receive_audio_message(msg, dj_get_long(data, "identifier"),
                                                 dj_get_float(data, "volume"), dj_get_int(data, "loop"));
        case Msg_Set_Sound_Volume:
            return dj_java_receive_audio_message(msg, dj_get_long(data, "identifier"),
                                                 dj_get_float(data, "volume"), 0);
        case Msg_Pause_All_Sound:
        case Msg_Resume_All_Sound:
        case Msg_Stop_All_Sound:
            return dj_java_receive_audio_message(msg, 0, 0.0f, 0);

        case Msg_Save_Text_File:
            handle_save_text_file(data);
            break;
        case Msg_Load_Text_File:
            handle_load_text_file(data);
            break;
        case Msg_Does_File_Exist:
            handle_does_file_exist(data);
            break;

        case Msg_Generate_Scoremarker_Texture:
        case Msg_Generate_Currency_Texture:
            // Android renders these with Canvas + system fonts. Not available
            // yet: report failure so the game skips the texture.
            dj_set_int(data, "status", 0);
            dj_set_int(data, "texture_id", 0);
            log_unhandled(msg);
            break;

        case Msg_Request_Ad_Status:
            dj_set_int(data, "status", 0);
            dj_set_int(data, "adTypesAvailable", 0);
            dj_set_int(data, "adTypeSelected", 0);
            break;
        case Msg_Show_Ad:
            dj_set_int(data, "status", 0);
            break;

        case Msg_Show_Alert_Dialog:
            handle_show_alert_dialog(data);
            break;

        case Msg_Launch_Android_Permissions_Settings:
        case 245:
            // MainActivity.checkForPermissions(): storage access is always granted here.
            defer_reply(Msg_Android_Permissions_Settings_Ready);
            break;

        case Msg_Terminate:
            l_info("[JAVA] Game requested termination.");
            sceKernelExitProcess(0);
            break;

        case Msg_Gameplay_Active:
            gameplay_active = 1;
            break;
        case Msg_Gameplay_Inactive:
            gameplay_active = 0;
            break;

        case Msg_Swap_Buffer:
        case Msg_Vibrate:
        case Msg_Flurry_Log_Event:
            break;

        default:
            log_unhandled(msg);
            break;
    }
    return 0;
}

int dj_java_receive_audio_message(int msg, jlong id, float volume, int loop) {
    switch (msg) {
        case Msg_Unload_Sound:     dj_audio_unload(id); break;
        case Msg_Play_Sound:       dj_audio_play(id, volume, loop); break;
        case Msg_Stop_Sound:       dj_audio_stop(id); break;
        case Msg_Pause_Sound:      dj_audio_pause(id); break;
        case Msg_Resume_Sound:     dj_audio_resume(id); break;
        case Msg_Pause_All_Sound:  dj_audio_pause_all(); break;
        case Msg_Resume_All_Sound: dj_audio_resume_all(); break;
        case Msg_Stop_All_Sound:   dj_audio_stop_all(); break;
        case Msg_Set_Sound_Volume: dj_audio_set_volume(id, volume); break;
        default:                   log_unhandled(msg); break;
    }
    return 0;
}
