/* MIT license; see LICENSE. */
#ifndef CQ_PARTICLE_PROGRESSIVE_H
#define CQ_PARTICLE_PROGRESSIVE_H
#include <stdint.h>
#include <stddef.h>
#define PARTICLE_PROGRESSIVE_JOBS 8
#define PARTICLE_PROGRESSIVE_BUDGET_US 4000
#define PARTICLE_PROGRESSIVE_MAX_DT (0.26666668f)
int particle_progressive_matches(const char *, unsigned kind, float requested, unsigned capacity);
typedef struct {
    uintptr_t instance;
    uint32_t generation, seen_frame, served_frame;
    float remaining;
} ParticleProgressiveJob;
typedef struct {
    unsigned slot;
    uint32_t generation, token;
    float extra;
} ParticleProgressiveTicket;
typedef struct {
    ParticleProgressiveJob jobs[PARTICLE_PROGRESSIVE_JOBS];
    uint32_t frame, spent_us, cursor, sequence, busy_token;
    uint32_t started, completed, cancelled, fallback, steps, max_step_us, overruns;
} ParticleProgressive;
/* All access is serialized by the caller; jobs contain identity and scalar
 * progress only. No native task, RNG, resource or particle buffer is retained. */
void particle_progressive_frame(ParticleProgressive *, uint32_t);
int particle_progressive_start(ParticleProgressive *, uintptr_t, uint32_t, float);
int particle_progressive_cancel(ParticleProgressive *, uintptr_t);
int particle_progressive_reserve(ParticleProgressive *, uintptr_t, uint32_t,
                                 float, ParticleProgressiveTicket *);
int particle_progressive_finish(ParticleProgressive *, ParticleProgressiveTicket, uint32_t);
unsigned particle_progressive_pending(const ParticleProgressive *);
#endif
