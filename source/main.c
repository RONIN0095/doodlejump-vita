#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <math.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"

#include "dj_audio.h"
#include "dj_game.h"
#include "dj_java.h"
#include "dj_jni.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

/*
 * Screen layout: the game is portrait-only, so it gets a 2:3 area in the
 * middle of the 960x544 screen.
 */

#define SCREEN_W 960
#define SCREEN_H 544

// Rotated (default): the picture is turned by 90 degrees and fills the whole
// screen; the Vita is held upright with its left side (D-pad) at the bottom.
// Pillarboxed: a 2:3 strip in the middle of the landscape screen. Selected by
// creating DATA_PATH "landscape.txt".
int dj_rotated = 1;

int dj_game_w = 362;
int dj_game_h = SCREEN_H;
int dj_game_x = (SCREEN_W - 362) / 2;

static void setup_layout(void) {
    dj_rotated = !file_exists(DATA_PATH "landscape.txt");
    if (dj_rotated) {
        dj_game_w = SCREEN_H;
        dj_game_h = SCREEN_W;
        dj_game_x = 0;
    }
    l_info("Layout: %s, game screen %ix%i", dj_rotated ? "rotated" : "pillarboxed", dj_game_w, dj_game_h);
}

/*
 * Input
 */

#define MAX_TOUCH_EVENTS 16
#define MAX_KEY_EVENTS 16

#define TOUCH_DOWN 0
#define TOUCH_MOVE 1
#define TOUCH_UP 2

// Simulated finger for the "shoot" button, out of range of real touch ids
#define SHOOT_TOUCH_ID 9

// Accelerometer value (m/s^2) sent for a fully tilted stick
#define TILT_MAX 6.0f
#define STICK_DEADZONE 16

static jfloatArray touches_arr, accels_arr;
static jintArray keys_arr;
static jfloat *touches, *accels;
static jint *keys;
static int touch_count = 0;

typedef struct {
    int active;
    int id;
    float x, y;
} TouchSlot;

static TouchSlot prev_touch[SCE_TOUCH_MAX_REPORT];

static void push_touch(int id, int type, float x, float y) {
    if (touch_count >= MAX_TOUCH_EVENTS) return;
    float *t = &touches[touch_count * 4];
    t[0] = (float)id;
    t[1] = (float)type;
    t[2] = x;
    t[3] = y;
    touch_count++;
}

static void to_game_coords(int tx, int ty, float *gx, float *gy) {
    // Front panel reports 1920x1088
    float sx = (float)tx * SCREEN_W / 1920.0f;
    float sy = (float)ty * SCREEN_H / 1088.0f;
    if (dj_rotated) {
        // Game top is the screen's right edge, game left is the screen's top edge.
        *gx = sy;
        *gy = (float)(SCREEN_W - 1) - sx;
        return;
    }
    float x = sx - (float)dj_game_x;
    if (x < 0) x = 0;
    if (x > dj_game_w - 1) x = (float)(dj_game_w - 1);
    *gx = x;
    *gy = sy * dj_game_h / SCREEN_H;
}

static void poll_touch(void) {
    SceTouchData td;
    memset(&td, 0, sizeof(td));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);

    TouchSlot now[SCE_TOUCH_MAX_REPORT];
    memset(now, 0, sizeof(now));

    for (int i = 0; i < td.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
        now[i].active = 1;
        now[i].id = td.report[i].id;
        to_game_coords(td.report[i].x, td.report[i].y, &now[i].x, &now[i].y);
    }

    // Ended touches
    for (int p = 0; p < SCE_TOUCH_MAX_REPORT; p++) {
        if (!prev_touch[p].active) continue;
        int found = 0;
        for (int n = 0; n < SCE_TOUCH_MAX_REPORT; n++)
            if (now[n].active && now[n].id == prev_touch[p].id) found = 1;
        if (!found)
            push_touch(prev_touch[p].id & 7, TOUCH_UP, prev_touch[p].x, prev_touch[p].y);
    }

    // New and moved touches
    for (int n = 0; n < SCE_TOUCH_MAX_REPORT; n++) {
        if (!now[n].active) continue;
        int found = -1;
        for (int p = 0; p < SCE_TOUCH_MAX_REPORT; p++)
            if (prev_touch[p].active && prev_touch[p].id == now[n].id) found = p;
        if (found < 0)
            push_touch(now[n].id & 7, TOUCH_DOWN, now[n].x, now[n].y);
        else if (prev_touch[found].x != now[n].x || prev_touch[found].y != now[n].y)
            push_touch(now[n].id & 7, TOUCH_MOVE, now[n].x, now[n].y);
    }

    memcpy(prev_touch, now, sizeof(now));
}

static uint32_t prev_buttons = 0;

static float stick_axis(uint8_t v) {
    int d = (int)v - 128;
    if (d > -STICK_DEADZONE && d < STICK_DEADZONE) return 0.0f;
    float f = (float)d / 127.0f;
    return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

static void poll_buttons(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    uint32_t pressed = pad.buttons & ~prev_buttons;
    uint32_t released = ~pad.buttons & prev_buttons;
    prev_buttons = pad.buttons;

    // Tilt: left stick or D-pad. Android reports a negative X acceleration
    // when the phone's right edge goes down.
    float tilt;
    if (dj_rotated) {
        // Held upright, the player's right is the Vita's down.
        tilt = stick_axis(pad.ly);
        if (pad.buttons & SCE_CTRL_UP) tilt = -1.0f;
        if (pad.buttons & SCE_CTRL_DOWN) tilt = 1.0f;
    } else {
        tilt = stick_axis(pad.lx);
        if (pad.buttons & SCE_CTRL_LEFT) tilt = -1.0f;
        if (pad.buttons & SCE_CTRL_RIGHT) tilt = 1.0f;
    }
    accels[0] = -tilt * TILT_MAX;
    accels[1] = 0.0f;
    accels[2] = 9.81f;

    // Cross/Square/R: shoot straight up (a tap above the doodler)
    const uint32_t shoot = SCE_CTRL_CROSS | SCE_CTRL_SQUARE | SCE_CTRL_RTRIGGER;
    float sx = dj_game_w * 0.5f, sy = dj_game_h * 0.25f;
    if (pressed & shoot)
        push_touch(SHOOT_TOUCH_ID, TOUCH_DOWN, sx, sy);
    else if (released & shoot && !(pad.buttons & shoot))
        push_touch(SHOOT_TOUCH_ID, TOUCH_UP, sx, sy);

    // Circle/Start: Android back button
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
        dj_java_send_native(Msg_On_Back_Pressed, NULL, 0, 0);
}

/*
 * Main
 */

static int (*runGameLoopNative)(JNIEnv *env, jclass clazz, int num_touches, jfloatArray touches,
                                int has_accel, jfloatArray accels, int num_keys, jintArray keys);

static jobject make_system_configuration(void) {
    jobject cfg = dj_new_object("com/limasky/doodlejumpandroid/Messages$MsgSystemConfigurationData");
    dj_set_int(cfg, "screenWidth", dj_game_w);
    dj_set_int(cfg, "screenHeight", dj_game_h);
    dj_set_string(cfg, "locale", "en_US");
    dj_set_string(cfg, "country", "us");
    dj_set_string(cfg, "languageCode", "en");
    dj_set_bool(cfg, "ifga", JNI_FALSE);
    dj_set_bool(cfg, "areAllThemesAvailable", JNI_TRUE);
    dj_set_bool(cfg, "enableSlowAppAlert", JNI_FALSE);
    dj_set_int(cfg, "numCpuCores", 3);
    dj_set_int(cfg, "totalMemSize", 524288); // kB, from /proc/meminfo on Android
    dj_set_int(cfg, "version", 3910006);     // versionCode of the supported APK (3.10.0)
    return cfg;
}

int main() {
    // Needed before anything logs: the log file lives here.
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/doodlejump", 0777);
    l_info("Doodle Jump loader starting.");

    soloader_init_all();
    l_info("Loader initialized.");

    dj_jni_install();
    dj_java_init(&so_mod);
    l_info("Java bridge initialized.");
    setup_layout();
    dj_audio_init();

    runGameLoopNative = (void *)so_symbol(&so_mod, "Java_com_limasky_doodlejumpandroid_NotificationCenter_runGameLoopNative");
    if (!runGameLoopNative) {
        l_fatal("runGameLoopNative not found");
        return 0;
    }

    JNIEnv *env = dj_env();
    jclass nc = (*env)->FindClass(env, "com/limasky/doodlejumpandroid/NotificationCenter");

    touches_arr = (*env)->NewFloatArray(env, MAX_TOUCH_EVENTS * 4);
    accels_arr = (*env)->NewFloatArray(env, 12);
    keys_arr = (*env)->NewIntArray(env, MAX_KEY_EVENTS * 2);
    touches = (*env)->GetFloatArrayElements(env, touches_arr, NULL);
    accels = (*env)->GetFloatArrayElements(env, accels_arr, NULL);
    keys = (*env)->GetIntArrayElements(env, keys_arr, NULL);

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    gl_init();
    dj_gl_setup();
    l_info("vitaGL initialized.");

    // Show a black frame right away, so the system's loading indicator goes away.
    dj_gl_begin_frame();
    vglSwapBuffers(GL_FALSE);

    // Same order as MainApplication.onCreate / BaseActivity / OpenGL10Renderer
    dj_java_send_native(Msg_Initialize_Native_Environment, NULL, 0, 0);
    dj_java_send_native(Msg_On_Start, NULL, 0, 0);
    dj_java_send_native(Msg_Resume, NULL, 0, 0);
    dj_java_send_native(Msg_System_Configuration, make_system_configuration(), 0, 0);
    l_success("Native environment initialized, entering game loop.");

    int frame = 0;
    while (1) {
        if (frame < 5 || frame % 600 == 0)
            l_info("Frame %i", frame);
        frame++;

        dj_java_update();

        touch_count = 0;
        if (!dj_java_dialog_active()) {
            poll_touch();
            poll_buttons();
        }

        dj_gl_begin_frame();
        runGameLoopNative(env, nc, touch_count, touches_arr, 1, accels_arr, 0, keys_arr);
        dj_gl_end_frame();
    }

    sceKernelExitDeleteThread(0);
}

// Input is polled on the main thread above; the boilerplate controls thread is unused.
void controls_handler_key(int32_t keycode, ControlsAction action) {}
void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {}
void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {}
