/* MIT license; see LICENSE. */
#ifndef CQ_PARTICLE_COMPAT_H
#define CQ_PARTICLE_COMPAT_H
#include <stddef.h>
#include <stdint.h>
/* Validate a native warmup routine and change max(requested,90) to min. */
int particle_warmup_prepare(uint32_t *code, size_t bytes);
#endif
