/* MIT license; see LICENSE. */
#ifndef CQ_CONTROLS_COMPAT_H
#define CQ_CONTROLS_COMPAT_H
#include <stddef.h>
#include <stdint.h>

const char *controls_ps3_texture(const char *path, char *out, size_t size);
/* VirtualGamepad press edges (170=A, 171=B), not physical Vita buttons. */
int controls_modal_result(int type, const uint8_t *pad);
int controls_battle_block(int modal_active, const uint8_t *pad, int *wait_release);
#endif
