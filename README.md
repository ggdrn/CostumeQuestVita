# Costume Quest — PS Vita 1.0.0

PS Vita port of Costume Quest, based on the 2015 Android/OUYA PowerVR build.
It loads the original ARM libraries from the APK using
[so_util](https://github.com/Rinnegatamante/so_util),
[FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) and
[vitaGL](https://github.com/Rinnegatamante/vitaGL), starting from the
[soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate).

**Status:** version 1.0.0, tested on a Vita, with performance accepted on the
user's tested routes. New configurations default to **30 FPS, MSAA off,
PlayStation controls and an English port menu**. Updating preserves existing
settings and mappings.

See the [installation guide](docs/INSTALL.en.md), [changelog](CHANGELOG.md)
and [third-party notices](docs/NOTICE.md).

## Requirements

- A compatible original Costume Quest PowerVR APK, renamed to `game.apk`.
- `kubridge.skprx` and `libshacccg.suprx` on the Vita.
- For building: a **softfp VitaSDK** and its dependencies. The original
  `armeabi-v7a` libraries use softfp; a hard-float SDK is not compatible.

The original game data pack, game libraries, saves and translations are not
included in the distribution.

## Installation

Install the VPK using VitaShell and place your APK at:

```text
ux0:data/costumequest/
└── game.apk
```

This release uses the complete APK. The game reads `assets/ant.mp3`, its
approximately 670 MB uncompressed data archive, directly from it.

On first launch, the loader extracts `libCq.so`, `libfmodex.so` and
`libfmodevent.so` from `lib/armeabi-v7a/` in the APK, with CRC32 validation.
Later launches reuse these files. The regular release does not use the
FMOD debug libraries or `libfaad.so`.

The loader also builds `apk_index.bin`, an approximately 1.2 MB index of
35,000 local archive headers. Subsequent launches load this index instead of
performing thousands of small card reads. The index is rebuilt if the APK changes.

When updating, install over the existing application and keep your
`ux0:data/costumequest/save/` directory, translations and shader cache.
The application ID remains `CSTMQST01` and the Vita application version is `01.00`.

## Port menu

Press **L + R + Start** during gameplay. The game pauses while the menu is open.
All port-menu labels are in English; the game's language is configured separately.

- **FPS limit:** 60, 45, 30 or Unlimited. Default: 30. Changes apply immediately.
- **Antialiasing (MSAA):** Off, 2x or 4x. Default: Off. Restart to apply changes.
- **Profiler (debug.log):** enable or disable stage timing diagnostics.
- **Controls:** map each game input to a Vita button or rear-touch quadrant.
- **Restore default controls:** reset button mappings only.
- **Save and resume:** save settings and return to the game.

Use Up/Down to select, Left/Right or Cross to change, and Circle or Start to
save and resume. The Start press used to open the port menu does not also
open the game's pause menu. The menu is drawn directly into the framebuffer
by the CPU, outside the game's OpenGL rendering.

## Settings

`ux0:data/costumequest/config.txt` is created on first launch. Each line uses
`key value`. Existing values are preserved when updating.

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `fps_cap` | 60, 45, 30, 0 | 30 | Frame limit. 60 and 30 use vsync intervals of one and two refreshes; 45 and 0 use no vsync. |
| `msaa` | 0, 2, 4 | 0 | Antialiasing. 0 reduces GPU load. Restart required. |
| `profiler` | 0, 1, 2 | 0 | 1 records engine stage timings; 2 also profiles every Lua call and is significantly slower. |
| `particle_leaves` | 0, 1 | 1 | Gradual preparation of ambient leaves. 0 restores continuous native preparation after restarting. |
| `language` | auto, folder name | auto | Use a translation in `translations/`, or the Vita language if none is available. A folder name selects that translation explicitly. Restart required. |
| `controls` | gamepad, touch | gamepad | PlayStation controller interface or the Android touch interface. Restart required. |
| `allocator` | fast, newlib | fast | Small-block allocator for Lua and particles, or standard malloc. Restart required. |
| `lua_gcpause`, `lua_gcstepmul` | 50–1000 | 200, 180 | Lua garbage collector settings. The port replaces the game's pause value of 0 with 200. Restart required. |

If updating from a diagnostic build, set Profiler to Off in the port menu
for normal play. For the release video settings, select FPS limit 30 and MSAA Off.

## Controls

The Vita appears to the game as an SDL GameController, including both analog
sticks. Default mappings use the following internal game inputs:

| Game input | Vita input |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| Back / Start | Select / Start |
| LB / RB | L / R |
| LT / RT | Rear touch: upper-left / upper-right quadrant |
| L3 / R3 | Rear touch: lower-left / lower-right quadrant |
| D-pad and analog sticks | D-pad and analog sticks |

The interface displays PlayStation prompts in gamepad mode. Front touch remains
available as an alternative, and rear touch provides the extra controller inputs.

The controller mode uses the engine's console interface through
`VirtualGamepadManager::m_bIsGamepad`. The loader supplies a replacement
`Mobile_InputIcons.gfx` library for `Shared/ControllerIcons.swf` imports,
referencing the PlayStation and touch textures already present in the APK.
No additional controller artwork is required.

Battle minigames use the controller state already processed by the game.
Directional prompts also accept left-stick movement. Continuous-motion actions
use actual left-stick displacement; simply entering the action or holding a
direction does not charge the bar. Repeated-press actions still require new
presses, including timing bars. See the [combat mapping and audit](docs/controles-console.md#comandos-de-combate)
for additional technical details in Portuguese.

PlayStation prompts also cover interactions, costume selection, notebook
symbols and Yes/No dialogs, including DLC interface variants. Interaction
icons use the corrected size. In the notebook, the left stick scrolls active
quests and the right stick scrolls completed quests; scrollbar and page-drag
touch paths are also supported.

For escape confirmation, Cross confirms and Circle cancels with the default
mapping. Front touch is an alternative. Combat input is blocked while the
dialog is open, and holding the escape command does not stack or immediately
reopen confirmation dialogs.

Mappings can also be changed in `config.txt` with `map_a`, `map_b`, `map_x`,
`map_y`, `map_back`, `map_start`, `map_lb`, `map_rb`, `map_lt`, `map_rt`,
`map_l3`, `map_r3` and `map_guide`. Available sources are:

```text
cross circle square triangle start select l r
up down left right rear_tl rear_tr rear_bl rear_br none
```

For example, `map_a circle` and `map_b cross` swap confirm and cancel.

## Translations

Place translation tables in `ux0:data/costumequest/translations/<name>/`.
With `language auto`, the port uses an available translation folder, otherwise
the Vita language. `language ptbr`, for example, explicitly selects `translations/ptbr/`.

Supported plain-text tables are `costumequest.STbl`, `costumequestdlc1.STbl`,
`cq1patch1.STbl`, `cqmobile.STbl` and `cqpc.STbl`. At boot, the loader compresses
them and serves them in place of the French tables while selecting the French
language internally. Each replacement must fit within the original table's
space in the APK. This internal language selection is separate from the
translation text and the English port menu.

The converter can extract tables from PC fan-translation xdelta patches and
fill missing strings from the APK's English tables:

```sh
brew install xdelta
python3 extras/scripts/translation_from_pc.py <translation-folder> CostumeQuestPowerVR.apk translations/ptbr
```

Copy the resulting `translations/ptbr` folder to the Vita. The game's fonts
already support Portuguese accents. Generated translation text remains with
your local game files and is not included in this repository.

## Building

The 1.0 release was built with a local softfp VitaSDK on macOS. Install the SDK
and dependencies including zlib, vitashark, SceShaccCgExt, libmathneon,
kubridge and taihen.

Use a source path without spaces, or a symlink to one, because of the SDK's
VPK packager:

```sh
export VITASDK=/path/to/vitasdk-softfp
export PATH="$VITASDK/bin:$PATH"
export CPATH="$(pwd)/lib/vitagl/source${CPATH:+:$CPATH}"
cmake -S . -B /tmp/cq-release-build -DCMAKE_BUILD_TYPE=Release \
  -DFMOD_DEBUG=OFF -DHEAPGUARD=OFF -DGL_TRACE=OFF -DGL_CENSUS=OFF
cmake --build /tmp/cq-release-build -j4
```

Recent CMake versions may require `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for
supporting libraries. vitaGL is built from the vendored `lib/vitagl` sources
with `SOFTFP_ABI=1`.

Alternatively, `build-vita.sh` builds through an OrbStack Linux amd64 machine
named `vitadev`, with the SDK installed in `~/vitasdk-softfp`:

```sh
./build-vita.sh Release
```

- `./build-vita.sh Release`: optimized VPK for normal play.
- `./build-vita.sh`: Debug VPK with additional, slower logging.
- `./build-vita.sh Release fmod`: Release using the APK's FMOD debug libraries
  for audio diagnostics. Their messages appear as `[FMOD]` in `debug.log`.

## Diagnostics

Each run writes `ux0:data/costumequest/debug.log`; the previous run is retained
as `debug.prev.log`. It includes startup information, game logging and
Scaleform messages. With profiling enabled, heartbeat records include FPS,
mean frame time, the worst frame and engine stage timings.

Normal log writes use a bounded asynchronous queue. On a crash, the kubridge
exception handler writes a direct report with registers, stack code addresses
and PC/LR translated into `libCq.so+offset (symbol)` form.

Heavy diagnostics are disabled by default:

- `-DHEAPGUARD=ON`: allocation guards, quarantine and heap scans.
- `-DGL_TRACE=ON`: calling threads and origins for OpenGL calls.
- `-DGL_CENSUS=ON`: distinct texture formats and vertex layouts.

The particle capture analyzer is available at `extras/analyze-particle-trace.py`.

## Shaders and performance

vitaGL caches compiled shaders in `ux0:data/shader_cache/CSTMQST01/`.
The first appearance of an uncached shader can be slow; keeping the cache
avoids recompilation on later runs.

The release includes a corrected particle-preparation clamp, gradual settling
for ambient leaves, independent file-cache waits and asynchronous logging.
Ambient leaves initially appear less dense while preparation is distributed
across frames. Initial loading and scene transitions can still pause for
several seconds.

## Validation scope and limitations

- Exploration, notebook scrolling and performance were checked on a Vita using
  user-provided screenshots and logs. This does not establish full-campaign
  or all-DLC coverage.
- Some costumes, abilities and interface paths still require broader hardware
  coverage. Local build and regression tests do not establish complete gameplay coverage.
- Memory remains limited compared with the original OUYA environment. Large
  textures and audio can exhaust available memory.
- Saves are redirected from `/sdcard/dfp/cq-droid/_write` to
  `ux0:data/costumequest/save/`. The remaining `/sdcard` paths stay unavailable,
  as expected by the game.

## Implementation overview

| Android component | Vita implementation |
| --- | --- |
| Native game and FMOD libraries | Extracted from the APK and loaded at 0x98C10000, 0x98010000 and 0x98810000 respectively; imports are resolved between modules. |
| Activity initialization | `dfNativeSetLanguage`, `dfNativeInit`, `setDeviceInfo`, `onNativeResize(960x544)` and `nativeInit()` on a thread with a 4 MB stack. |
| SDL EGL and window operations | `patch.c` bridges the Android backend to vitaGL. |
| Android file access | File hooks bridge to Vita stdio; the game reads its archive through the APK path. |
| AudioTrack | `fmod_audio.c` submits the FMOD audio stream to `sceAudioOut`. |
| OUYA controller and touch input | SDL GameController for buttons, sticks and rear touch; Android-style events for front touch. |

## License

See [LICENSE](LICENSE) for the port license and [third-party notices](docs/NOTICE.md)
for library licensing. The matching vendored sources are included in the source distribution.
