/* MIT license; see LICENSE. */
#ifndef CQ_PARTICLE_TRACE_H
#define CQ_PARTICLE_TRACE_H
#include <stdint.h>
#include <stddef.h>
#define PARTICLE_TRACE_SLOTS 2048
#define PARTICLE_TRACE_RECORDS 256
typedef struct {
    uintptr_t instance;
    uint64_t reset_us;
    uint32_t generation;
} ParticleTraceEntry;
typedef struct {
    ParticleTraceEntry entries[PARTICLE_TRACE_SLOTS];
    uint32_t sequence, active, untracked, records;
    uint64_t calls, slow, total_us, max_us;
    uint32_t frame_calls;
    uint64_t frame_sum_us;
} ParticleTrace;
/* Caller serializes all access. No game pointers are dereferenced or retained
 * for work; this table is diagnostic identity only. Zero means untracked. */
uint32_t particle_trace_reset(ParticleTrace *, uintptr_t, uint64_t);
void particle_trace_retire(ParticleTrace *, uintptr_t);
ParticleTraceEntry particle_trace_find(const ParticleTrace *, uintptr_t);
int particle_trace_finish(ParticleTrace *, uint64_t);
#endif
