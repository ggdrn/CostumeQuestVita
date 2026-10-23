/* MIT license; see LICENSE. */
#ifndef CQ_CONTROLS_GFX_H
#define CQ_CONTROLS_GFX_H
#include <stddef.h>
enum { CQ_GFX_INTERACTIONS, CQ_GFX_INTERACTIONS_DLC, CQ_GFX_NOTEBOOK_DLC };
/* Patch the supported original GFX, recompress and pad to its stored size.
   Returns 0 without modifying out for unknown revisions or insufficient space. */
size_t controls_gfx_patch(int kind, const void *src, size_t size, void *out, size_t cap);
#endif
