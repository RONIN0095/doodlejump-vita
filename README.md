# Doodle Jump — PS Vita port

A loader that runs the official Android release of **Doodle Jump 3.10.0**
(`com.lima.doodlejump`, versionCode 3910006) natively on the PS Vita, built on
[solobop](https://github.com/v-atamanenko/soloader-boilerplate) (so_util,
FalsoJNI, vitaGL).

No game files are included: you need your own copy of the Doodle Jump 3.10.0
APK.

## Features

- Full-screen portrait view (Vita held upright) or a landscape strip,
  switchable while playing
- Physical controls: steer with the stick, shoot with the D-pad
- Steering by touch and by the rear touchpad
- Sound and music, saves
- Store prices and score-marker names drawn in the game's own handwriting
- No ads; the "remove ads", "share" and "toys" buttons are hidden

Not supported: in-app purchases, online highscores, Google Play Games, cloud
sync and sharing.

## Installation

### Requirements

- A PS Vita with HENkaku/Ensō
- [kubridge](https://github.com/bythos14/kubridge/releases): add
  `ur0:tai/kubridge.skprx` (or `ux0:tai/kubridge.skprx`) under `*KERNEL` in
  your `config.txt` and reboot
- `libshacccg.suprx`, the shader compiler: the easiest way to get it is
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D)
- **Doodle Jump 3.10.0** for Android (32-bit `armeabi`/`armeabi-v7a` build).
  Newer versions are not supported.

### Steps

1. Install `doodlejump.vpk` from the
   [releases](https://github.com/RONIN0095/doodlejump-vita/releases) with
   VitaShell.
2. Open the APK as a zip file (rename it to `.zip` or use 7-Zip) and copy:
   - `lib/armeabi-v7a/libDoodleJump.so` → `ux0:data/doodlejump/libDoodleJump.so`
   - the whole `assets/` folder → `ux0:data/doodlejump/assets/`
3. Start the game. The first start takes a bit longer.

Saves are kept in `ux0:data/doodlejump/files/`.

## Controls

Directions are as you see the game. In the upright view, hold the Vita with
the D-pad at the bottom.

| Vita | Game |
|---|---|
| Left stick | Steer left/right |
| Rear touchpad, left/right half | Steer left/right |
| Front touch, lower part of the screen (while playing) | Steer left/right |
| Front touch, elsewhere | Tap (menus, shooting at the touched point) |
| D-pad up / left / right | Shoot up / up-left / up-right |
| Cross / Square / R | Shoot straight up |
| Circle / Start | Back / pause |
| SELECT, or right stick towards the Vita's right edge | Switch between upright full screen and landscape strip |

## Troubleshooting

- **Error `0x8010100C` while installing**: your home screen has no free icon
  slot. Delete an app or put some icons into a folder, then install again.
- **The game doesn't start or crashes**: check that both game files are in
  `ux0:data/doodlejump/` and that you use version 3.10.0. A log is written to
  `ux0:data/doodlejump/log.txt`; please attach it (and the crash dump from
  `ux0:data/`, if there is one) when reporting a problem.

## Optional: LiveArea from the game's own artwork

The VPK uses neutral LiveArea images. To build one with Doodle Jump's artwork
from your own copy of the game (keep that VPK for yourself, it contains Lima
Sky's artwork):

```sh
python3 extras/scripts/make_livearea.py /path/to/extracted-apk livearea-out
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIVEAREA_DIR=$PWD/livearea-out
cmake --build build
```

## How the port works

- `libDoodleJump.so` (armeabi-v7a, OpenGL ES 1.1, OpenSL ES) is loaded and its
  imports are resolved against Vita implementations (`source/dynlib.c`).
- The game drives everything through `NotificationCenter` messages to/from
  Java. `source/dj_jni.c` provides real per-object Java fields on top of
  FalsoJNI; `source/dj_java.c` reimplements the Java message handlers
  (text files/saves, asset checks, alert dialogs, sound).
- `source/dj_audio.c` replaces Android's SoundPool/AudioTrack: Ogg assets are
  decoded with stb_vorbis and mixed into a SceAudio port.
- The game is portrait-only and always renders at 544×960. `source/dj_game.c`
  either rotates the projection by 90° to fill the screen or scales the
  viewport into a strip in the middle of the landscape screen.
- The text textures Android drew with Canvas (store prices, score-marker
  names) are rendered with the game's own handwritten font (`source/dj_text.c`).
- The "remove ads", "share" and "toys" buttons get transparent images
  (`source/dj_hidden.c`), since none of them work offline.

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

Optionally, `-DGAME_DATA_DIR=/path/to/extracted-apk` packs the `.so` and
`assets/` into the VPK (read from `app0:`). Such a VPK contains Lima Sky's
game data: keep it for your own use and do not share it.

## Credits

- Port: 1theronin ([RONIN0095](https://github.com/RONIN0095))
- Lima Sky for Doodle Jump.
- TheFloW, Rinnegatamante and Volodymyr Atamanenko for so_util, vitaGL,
  FalsoJNI and the solobop boilerplate.
- Sean Barrett for stb_vorbis and stb_image.

## License

The loader code is MIT licensed, see [LICENSE](LICENSE). Doodle Jump is the
property of Lima Sky; no game data is distributed here.
