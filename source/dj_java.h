/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_java.h
 * @brief Reimplementation of Doodle Jump's Java message handlers.
 */

#ifndef DJ_JAVA_H
#define DJ_JAVA_H

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

// Message IDs from com.limasky.doodlejumpandroid.Messages
enum {
    Msg_System_Configuration = 0,
    Msg_Map_File_To_Memory = 1,
    Msg_Close_Mapped_File = 2,
    Msg_Load_Sound = 6,
    Msg_Unload_Sound = 7,
    Msg_Play_Sound = 8,
    Msg_Stop_Sound = 9,
    Msg_Pause_Sound = 10,
    Msg_Resume_Sound = 11,
    Msg_Pause_All_Sound = 12,
    Msg_Resume_All_Sound = 13,
    Msg_Stop_All_Sound = 14,
    Msg_Set_Sound_Volume = 15,
    Msg_Save_Text_File = 17,
    Msg_Load_Text_File = 18,
    Msg_Vibrate = 19,
    Msg_Flurry_Log_Event = 20,
    Msg_Generate_Scoremarker_Texture = 21,
    Msg_Get_Highscores = 22,
    Msg_Submit_Highscore = 24,
    Msg_Show_Alert_Dialog = 25,
    Msg_Alert_Dialog_Result = 26,
    Msg_Does_File_Exist = 27,
    Msg_Pause = 28,
    Msg_Resume = 29,
    Msg_Request_Ad_Status = 30,
    Msg_Show_Ad = 31,
    Msg_Terminate = 36,
    Msg_Swap_Buffer = 43,
    Msg_Generate_Currency_Texture = 63,
    Msg_On_Back_Pressed = -7,
    Msg_On_Destroy = -6,
    Msg_On_Stop = -5,
    Msg_On_Start = -4,
    Msg_Native_Environment_Ready = -3,
    Msg_Launch_Android_Permissions_Settings = -11,
    Msg_Android_Permissions_Settings_Ready = -12,
    Msg_Initialize_Native_Environment = -1,
};

// Resolves the game's JNI entry points. Call after the .so is initialized.
void dj_java_init(so_module *mod);

// Java -> native (NotificationCenter.sendMessageNative)
int dj_java_send_native(int msg, jobject data, int arg1, int arg2);

// Native -> Java (NotificationCenter static methods)
int dj_java_receive_message(int msg, jobject data, int arg1, int arg2);
int dj_java_receive_audio_message(int msg, jlong id, float volume, int loop);
jobject dj_java_asset_manager(void);

// Main-thread housekeeping between frames (dialogs, deferred replies).
void dj_java_update(void);
int dj_java_dialog_active(void);

#endif // DJ_JAVA_H
