/* MIT license; see LICENSE. */
#include "particle_progressive.h"
#include <math.h>
#include <string.h>

int particle_progressive_matches(const char *name, unsigned kind, float requested, unsigned capacity) {
    return name && kind == 1 && requested == 20.0f && capacity == 661 &&
        !strcmp(name, "particles/effects/weather/precipitation/pollenlight/leaves");
}

void particle_progressive_frame(ParticleProgressive *s, uint32_t frame) {
    if (s->frame != frame) {
        s->frame = frame;
        s->spent_us = 0;
    }
    // An in-flight native update still owns its reservation across frame swaps.
}
unsigned particle_progressive_pending(const ParticleProgressive *s) {
    unsigned n = 0;
    for (unsigned i = 0; i < PARTICLE_PROGRESSIVE_JOBS; ++i) n += s->jobs[i].instance != 0;
    return n;
}
int particle_progressive_cancel(ParticleProgressive *s, uintptr_t instance) {
    for (unsigned i = 0; i < PARTICLE_PROGRESSIVE_JOBS; ++i) {
        if (instance && s->jobs[i].instance == instance) {
            s->jobs[i] = (ParticleProgressiveJob){0};
            ++s->cancelled;
            return 1;
        }
    }
    return 0;
}
int particle_progressive_start(ParticleProgressive *s, uintptr_t instance,
                               uint32_t generation, float remaining) {
    if (!instance || !generation || !isfinite(remaining) || remaining <= 0 || remaining > 90)
        return 0;
    for (unsigned i = 0; i < PARTICLE_PROGRESSIVE_JOBS; ++i)
        if (s->jobs[i].instance == instance) return 0;
    for (unsigned i = 0; i < PARTICLE_PROGRESSIVE_JOBS; ++i) {
        if (!s->jobs[i].instance) {
            s->jobs[i] = (ParticleProgressiveJob){instance, generation, s->frame,
                                                 s->frame - 1, remaining};
            ++s->started;
            return 1;
        }
    }
    ++s->fallback;
    return 0;
}
int particle_progressive_reserve(ParticleProgressive *s, uintptr_t instance,
                                 uint32_t generation, float dt, ParticleProgressiveTicket *out) {
    unsigned slot = PARTICLE_PROGRESSIVE_JOBS;
    for (unsigned i = 0; i < PARTICLE_PROGRESSIVE_JOBS; ++i) {
        ParticleProgressiveJob *j = &s->jobs[i];
        if (j->instance == instance && instance) {
            if (!generation || j->generation != generation) {
                particle_progressive_cancel(s, instance);
                return 0;
            }
            j->seen_frame = s->frame;
            slot = i;
            break;
        }
    }
    if (slot == PARTICLE_PROGRESSIVE_JOBS || s->busy_token ||
        s->spent_us >= PARTICLE_PROGRESSIVE_BUDGET_US || !isfinite(dt) ||
        dt <= 0 || dt >= PARTICLE_PROGRESSIVE_MAX_DT) return 0;
    unsigned chosen = PARTICLE_PROGRESSIVE_JOBS;
    for (unsigned n = 0; n < PARTICLE_PROGRESSIVE_JOBS; ++n) {
        unsigned i = (s->cursor + n) % PARTICLE_PROGRESSIVE_JOBS;
        ParticleProgressiveJob *j = &s->jobs[i];
        if (j->instance && s->frame - j->seen_frame <= 1 && j->served_frame != s->frame) {
            chosen = i;
            break;
        }
    }
    if (chosen != slot) return 0;
    ParticleProgressiveJob *j = &s->jobs[slot];
    float extra = PARTICLE_PROGRESSIVE_MAX_DT - dt;
    if (extra > j->remaining) extra = j->remaining;
    if (++s->sequence == 0) ++s->sequence;
    s->busy_token = s->sequence;
    j->served_frame = s->frame;
    *out = (ParticleProgressiveTicket){slot, generation, s->sequence, extra};
    return 1;
}
int particle_progressive_finish(ParticleProgressive *s, ParticleProgressiveTicket t, uint32_t us) {
    if (!t.token || t.token != s->busy_token || t.slot >= PARTICLE_PROGRESSIVE_JOBS) return 0;
    s->busy_token = 0;
    // Charge the whole native call, conservatively, even when it spans a swap.
    if (UINT32_MAX - s->spent_us < us) s->spent_us = UINT32_MAX;
    else s->spent_us += us;
    if (us > s->max_step_us) s->max_step_us = us;
    if (s->spent_us > PARTICLE_PROGRESSIVE_BUDGET_US) ++s->overruns;
    ++s->steps;
    s->cursor = (t.slot + 1) % PARTICLE_PROGRESSIVE_JOBS;
    ParticleProgressiveJob *j = &s->jobs[t.slot];
    if (!j->instance || j->generation != t.generation) return 0;
    j->remaining -= t.extra;
    if (j->remaining > 0.00001f) return 0;
    *j = (ParticleProgressiveJob){0};
    ++s->completed;
    return 1;
}
