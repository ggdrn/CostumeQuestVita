# Changelog

## 1.0.0

- Consolidated the PS Vita port for the tested PowerVR APK installation.
- Set new-install defaults to 30 FPS, MSAA off and profiling off.
- Translated the port settings menu into English and used PlayStation button labels.
- Corrected remaining PlayStation prompts and interaction icon sizing.
- Fixed notebook mission scrolling and escape confirmation handling.
- Corrected excessive particle preparation and added gradual settling for ambient leaves.
- Removed cross-descriptor blocking from the read cache and batched log writes asynchronously.
- Kept existing saves, mappings, translations and user configuration when updating.

Known limitations: loading and scene transitions may still pause. Ambient leaves
build up gradually. Hardware validation covers the user's tested routes rather
than a complete campaign and every DLC scenario.
