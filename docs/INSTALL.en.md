# Costume Quest Vita 1.0.0

PS Vita port for the tested Costume Quest Android/OUYA PowerVR APK.
Application ID: CSTMQST01. Vita application version: 01.00.

## Install or update

1. Install kubridge.skprx and provide libshacccg.suprx on your Vita.
2. Place your compatible game APK at ux0:data/costumequest/game.apk.
3. Install CostumeQuestVita-v1.0.0.vpk using VitaShell, over the existing application if updating.
4. Start the game. Required native libraries are extracted from the APK on first launch.

Keep your existing ux0:data/costumequest/save/ folder, translations and shader
cache when updating. The original game data pack, game libraries and saves
are not included in this distribution. This release uses the complete game.apk;
it is not the loose-files variant.

## Defaults and port menu

New configurations use 30 FPS, MSAA off, profiling off, fast allocation,
PlayStation controls and progressive ambient leaves. Existing configuration
values and mappings are preserved when updating.

Press L + R + Start to open the English port menu. Use Up/Down to select,
Left/Right or Cross to change, and Circle or Start to save and resume.
MSAA changes take effect after restarting the game. Restore default controls
resets button mappings only.

If updating from a diagnostic build, choose FPS limit 30, MSAA Off and
Profiler Off in the port menu for the release configuration. The game's
language is separate from the port menu; existing translations still work.

Relevant config.txt values:

    fps_cap 30
    msaa 0
    profiler 0
    particle_leaves 1

particle_leaves 0 restores continuous native leaf preparation after a restart.
profiler 1 enables diagnostic timing in ux0:data/costumequest/debug.log.
The boot log identifies this release as Costume Quest Vita 1.0.0.

## Included changes

- English port settings menu and PlayStation button names.
- Corrected PS3 prompts, interaction icon size, notebook scrolling and escape confirmation.
- Reduced particle initialization stalls; ambient leaves fill in gradually.
- Independent file-cache waits and asynchronous log writes.

## Tested scope and known limitations

The user accepted performance on the tested Vita routes. The final capture
showed progressive leaf steps up to 2.663 ms with no 4 ms budget overruns;
remaining continuous particle preparations stayed below 50 ms. This does not
establish complete-campaign or all-DLC coverage. Initial loading and scene
transitions can still pause for several seconds, and ambient leaves initially
appear less dense. The final English-menu and default-setting edits were
checked locally after the hardware performance capture.

## Files and verification

SHA256SUMS covers the files in this package. The separately distributed
CostumeQuestVita-v1.0.0-source.zip contains the working source snapshot,
including the vendored libraries and tests, without the original game data.
BUILD-INFO.json records the build, source archive hash and validation results.
See CHANGELOG.md and the included license files for project and library notices.
