/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fmod_audio.h
 * @brief Native replacement for org.fmod.FMODAudioDevice (Java AudioTrack
 *        output of FMOD Ex), feeding libfmodex's mixer into sceAudioOut.
 */

#ifndef SOLOADER_FMOD_AUDIO_H
#define SOLOADER_FMOD_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

void fmod_audio_init();

void fmod_audio_start();

void fmod_audio_stop();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_FMOD_AUDIO_H
