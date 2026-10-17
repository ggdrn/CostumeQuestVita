/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  markers.h
 * @brief The game's own profiler markers, timed.
 */

#ifndef SOLOADER_MARKERS_H
#define SOLOADER_MARKERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hooks the empty ThreadMarker constructors / destructor of libCq.so
void markers_patch();

// Prints and resets the stage times (frames rendered in window_us)
void markers_log(uint32_t frames, uint32_t window_us);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_MARKERS_H
