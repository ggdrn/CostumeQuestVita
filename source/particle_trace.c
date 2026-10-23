/* MIT license; see LICENSE. */
#include "particle_trace.h"

ParticleTraceEntry particle_trace_find(const ParticleTrace *t, uintptr_t instance) {
    for (size_t i = 0; i < PARTICLE_TRACE_SLOTS; ++i)
        if (instance && t->entries[i].instance == instance)
            return t->entries[i];
    return (ParticleTraceEntry){0};
}

uint32_t particle_trace_reset(ParticleTrace *t, uintptr_t instance, uint64_t now) {
    if (!instance) return 0;
    ParticleTraceEntry *empty = NULL;
    for (size_t i = 0; i < PARTICLE_TRACE_SLOTS; ++i) {
        ParticleTraceEntry *e = &t->entries[i];
        if (e->instance == instance) { empty = e; break; }
        if (!e->instance && !empty) empty = e;
    }
    if (!empty) { ++t->untracked; return 0; }
    if (!empty->instance) ++t->active;
    if (++t->sequence == 0) ++t->sequence;
    *empty = (ParticleTraceEntry){instance, now, t->sequence};
    return t->sequence;
}

void particle_trace_retire(ParticleTrace *t, uintptr_t instance) {
    for (size_t i = 0; i < PARTICLE_TRACE_SLOTS; ++i)
        if (instance && t->entries[i].instance == instance) {
            t->entries[i] = (ParticleTraceEntry){0};
            --t->active;
            return;
        }
}

int particle_trace_finish(ParticleTrace *t, uint64_t elapsed) {
    ++t->calls;
    ++t->frame_calls;
    t->frame_sum_us += elapsed;
    t->total_us += elapsed;
    if (elapsed > t->max_us) t->max_us = elapsed;
    if (elapsed >= 50000) ++t->slow;
    if (elapsed < 1000 || t->records >= PARTICLE_TRACE_RECORDS) return 0;
    ++t->records;
    return 1;
}
