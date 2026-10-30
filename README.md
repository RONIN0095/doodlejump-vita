# Doodle Jump — PS Vita port (work in progress)

A loader that runs the official Android release of **Doodle Jump 3.10.0**
(`com.lima.doodlejump`, versionCode 3910006) natively on the PS Vita, built on
[solobop](https://github.com/v-atamanenko/soloader-boilerplate) (so_util,
FalsoJNI, vitaGL).

**Status: untested.** The code is written against an analysis of the APK
(native library imports and the Java side's bytecode) but has not run on real
hardware yet. Expect the first runs to need fixes. Logs go to `sceClibPrintf`
(readable with PrincessLog or similar); for crashes, the crash dump from
`ux0:data/` helps the most.

No game files are included. You need your own copy of the APK.

## How the port works

- `libDoodleJump.so` (armeabi-v7a, OpenGL ES 1.1, OpenSL ES) is loaded and its
  imports are resolved against Vita implementations (`source/dynlib.c`).
- The game drives everything through `NotificationCenter` messages to/from
  Java. `source/dj_jni.c` provides real per-object Java fields on top of
  FalsoJNI; `source/dj_java.c` reimplements the Java message handlers
  (text files/saves, asset checks, alert dialogs, sound).
- `source/dj_audio.c` replaces Android's SoundPool/AudioTrack: Ogg assets are
  decoded with stb_vorbis and mixed into a SceAudio port.
- The game is portrait-only and always renders at 544×960. Two views can be
  switched while playing (`source/dj_game.c`): rotated by 90° to fill the whole
  screen (hold the Vita upright, D-pad at the bottom), or a strip in the middle
  of the landscape screen. The choice is remembered.
- The text textures Android drew with Canvas (store prices, score-marker
  names) are rendered with the game's own handwritten font (`source/dj_text.c`).
- The "remove ads", "share" and "toys" buttons get transparent images
  (`source/dj_hidden.c`), since none of them work offline.

Not supported: ads, in-app purchases, online highscores, Google Play Games,
cloud sync and share images.

## Installation

1. Install [kubridge](https://github.com/bythos14/kubridge/releases)
   (`kubridge.skprx` under `*KERNEL` in your taiHEN config) and reboot.
2. Install `doodlejump.vpk` with VitaShell.
3. Game files, either:
   - **bundled** (no copying on the Vita): extract the APK and build with
     `-DGAME_DATA_DIR=/path/to/extracted-apk`; the `.so` and `assets/` are then
     packed into the VPK and read from `app0:`; or
   - **separately**: open the APK as a zip and copy
     `lib/armeabi-v7a/libDoodleJump.so` → `ux0:data/doodlejump/libDoodleJump.so`
     and the whole `assets/` folder → `ux0:data/doodlejump/assets/`.

   A VPK with bundled game files contains Lima Sky's copyrighted data: keep it
   for your own use and do not share it.

Saves are written to `ux0:data/doodlejump/files/`.

## Controls

Directions are as you see the game (in the upright view, hold the Vita with the
D-pad at the bottom).

| Vita | Game |
|---|---|
| Left stick | Steer |
| Rear touchpad, left/right half | Steer |
| Front touch, lower part of the screen (while playing) | Steer |
| Front touch, elsewhere | Touch (menus, shooting) |
| D-pad up / left / right | Shoot up / up-left / up-right |
| Cross / Square / R | Shoot straight up |
| Circle / Start | Android back button (pause / back) |
| SELECT, or right stick towards the Vita's right edge | Switch between upright full screen and landscape strip |

## Building

Requires [VitaSDK-softfp](https://github.com/vitasdk-softfp) with its vdpm
packages installed and `VITASDK` set:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is `build/doodlejump.vpk`. A GitHub Actions workflow
(`.github/workflows/build.yml`) does the same on every push and uploads the VPK
as an artifact.

For verbose logs, build with `-DCMAKE_BUILD_TYPE=Debug`.

## Credits

- Lima Sky for Doodle Jump.
- TheFloW, Rinnegatamante and Volodymyr Atamanenko for so_util, vitaGL,
  FalsoJNI and the solobop boilerplate.
- Sean Barrett for stb_vorbis.

## License

The loader code is MIT licensed, see [LICENSE](LICENSE). Doodle Jump itself is
the property of Lima Sky and is not distributed here.
