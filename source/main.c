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
 * Screen layout: see dj_game.c. The game always gets a 544x960 screen.
 */

#define SCREEN_W 960
#define SCREEN_H 544

/*
 * Input
 *
 * "Player directions" below are as the player sees the game: in the rotated
 * view the Vita is held upright with the D-pad at the bottom, so the player's
 * right is the Vita's down and the player's up is the Vita's right.
 */

#define MAX_TOUCH_EVENTS 16
#define MAX_KEY_EVENTS 16

#define TOUCH_DOWN 0
#define TOUCH_MOVE 1
#define TOUCH_UP 2

// Android pointer ids: real fingers use 0-3, the simulated shooting finger 4
#define MAX_FINGER_ID 3
#define SHOOT_TOUCH_ID 4

// Accelerometer value (m/s^2) sent for a full tilt
#define TILT_MAX 6.0f
#define STICK_DEADZONE 16

// While playing, front touches starting in this lower part of the game
// screen steer instead of shooting.
#define STEER_ZONE_TOP 0.6f

static jfloatArray touches_arr, accels_arr;
static jintArray keys_arr;
static jfloat *touches, *accels;
static jint *keys;
static int touch_count = 0;

typedef struct {
    int active;
    int id;
    int steer; // finger is used for steering, not passed to the game
    float x, y;
} TouchSlot;

static TouchSlot prev_touch[SCE_TOUCH_MAX_REPORT];
static float touch_tilt = 0.0f;

static SceTouchPanelInfo back_panel;

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
    float x = (sx - (float)dj_strip_x) / dj_strip_scale;
    if (x < 0) x = 0;
    if (x > DJ_GAME_W - 1) x = (float)(DJ_GAME_W - 1);
    *gx = x;
    *gy = sy / dj_strip_scale;
}

static float steer_from_game_x(float gx) {
    return gx < DJ_GAME_W * 0.5f ? -1.0f : 1.0f;
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
        if (!found && !prev_touch[p].steer)
            push_touch(prev_touch[p].id & MAX_FINGER_ID, TOUCH_UP, prev_touch[p].x, prev_touch[p].y);
    }

    // New and moved touches
    touch_tilt = 0.0f;
    for (int n = 0; n < SCE_TOUCH_MAX_REPORT; n++) {
        if (!now[n].active) continue;
        int found = -1;
        for (int p = 0; p < SCE_TOUCH_MAX_REPORT; p++)
            if (prev_touch[p].active && prev_touch[p].id == now[n].id) found = p;

        if (found < 0) {
            // A finger keeps its role (steering or game touch) until lifted.
            now[n].steer = dj_java_gameplay_active() && now[n].y > DJ_GAME_H * STEER_ZONE_TOP;
            if (!now[n].steer)
                push_touch(now[n].id & MAX_FINGER_ID, TOUCH_DOWN, now[n].x, now[n].y);
        } else {
            now[n].steer = prev_touch[found].steer;
            if (!now[n].steer && (prev_touch[found].x != now[n].x || prev_touch[found].y != now[n].y))
                push_touch(now[n].id & MAX_FINGER_ID, TOUCH_MOVE, now[n].x, now[n].y);
        }

        if (now[n].steer)
            touch_tilt = steer_from_game_x(now[n].x);
    }

    memcpy(prev_touch, now, sizeof(now));

    // Rear touchpad: hold the player's left or right half to steer.
    memset(&td, 0, sizeof(td));
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &td, 1);
    if (td.reportNum > 0) {
        int x = td.report[0].x, y = td.report[0].y;
        if (dj_rotated) {
            int mid = (back_panel.minAaY + back_panel.maxAaY) / 2;
            touch_tilt = y > mid ? 1.0f : -1.0f;
        } else {
            int mid = (back_panel.minAaX + back_panel.maxAaX) / 2;
            touch_tilt = x > mid ? 1.0f : -1.0f;
        }
    }
}

static uint32_t prev_buttons = 0;
static int view_stick_armed = 1;
static uint32_t shoot_button = 0;

// Lifts all fingers, so that no touch stays stuck across a view switch.
static void release_touches(void) {
    for (int p = 0; p < SCE_TOUCH_MAX_REPORT; p++)
        if (prev_touch[p].active && !prev_touch[p].steer)
            push_touch(prev_touch[p].id & MAX_FINGER_ID, TOUCH_UP, prev_touch[p].x, prev_touch[p].y);
    memset(prev_touch, 0, sizeof(prev_touch));
}

static float stick_axis(uint8_t v) {
    int d = (int)v - 128;
    if (d > -STICK_DEADZONE && d < STICK_DEADZONE) return 0.0f;
    float f = (float)d / 127.0f;
    return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

// Where to "tap" for a shot: the game shoots towards the touched point.
static void shoot_target(uint32_t button, float *x, float *y) {
    uint32_t up, left, right;
    if (dj_rotated) {
        up = SCE_CTRL_RIGHT;
        left = SCE_CTRL_UP;
        right = SCE_CTRL_DOWN;
    } else {
        up = SCE_CTRL_UP;
        left = SCE_CTRL_LEFT;
        right = SCE_CTRL_RIGHT;
    }

    *y = DJ_GAME_H * 0.15f;
    if (button & left)
        *x = DJ_GAME_W * 0.05f;
    else if (button & right)
        *x = DJ_GAME_W * 0.95f;
    else
        *x = DJ_GAME_W * 0.5f; // up, and the face buttons
    (void)up;
}

static void poll_buttons(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    uint32_t pressed = pad.buttons & ~prev_buttons;
    prev_buttons = pad.buttons;

    // Switch views: SELECT, or the right stick pushed towards the Vita's right
    // edge ("right" when held landscape, "up" when held upright).
    int toggle = (pressed & SCE_CTRL_SELECT) != 0;
    if (view_stick_armed && pad.rx > 224) {
        toggle = 1;
        view_stick_armed = 0;
    } else if (pad.rx < 176) {
        view_stick_armed = 1;
    }
    if (toggle) {
        release_touches();
        dj_set_rotated(!dj_rotated);
    }

    // Steering: left stick, or a steering touch. Android reports a negative X
    // acceleration when the phone's right edge goes down.
    float tilt = dj_rotated ? stick_axis(pad.ly) : stick_axis(pad.lx);
    if (touch_tilt != 0.0f) tilt = touch_tilt;
    accels[0] = -tilt * TILT_MAX;
    accels[1] = 0.0f;
    accels[2] = 9.81f;

    // Shooting: D-pad (up / up-left / up-right as the player sees it),
    // or Cross/Square/R straight up. One simulated finger at a time.
    const uint32_t shoot_buttons = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT |
                                   SCE_CTRL_CROSS | SCE_CTRL_SQUARE | SCE_CTRL_RTRIGGER;
    float sx, sy;
    if (shoot_button && !(pad.buttons & shoot_button)) {
        shoot_target(shoot_button, &sx, &sy);
        push_touch(SHOOT_TOUCH_ID, TOUCH_UP, sx, sy);
        shoot_button = 0;
    }
    if (!shoot_button && (pressed & shoot_buttons)) {
        // Pick one button out of the newly pressed ones
        uint32_t b = pressed & shoot_buttons;
        shoot_button = b & -b;
        shoot_target(shoot_button, &sx, &sy);
        push_touch(SHOOT_TOUCH_ID, TOUCH_DOWN, sx, sy);
    }

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
    dj_set_int(cfg, "screenWidth", DJ_GAME_W);
    dj_set_int(cfg, "screenHeight", DJ_GAME_H);
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
    dj_game_load_layout();
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
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &back_panel);
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
