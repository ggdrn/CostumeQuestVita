#include "particle_trace.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    ParticleTrace t = {0};
    uint32_t first = particle_trace_reset(&t, 0x1000, 100);
    assert(first && t.active == 1);
    ParticleTraceEntry snapshot = particle_trace_find(&t, 0x1000);
    assert(snapshot.generation == first && snapshot.reset_us == 100);
    uint32_t next = particle_trace_reset(&t, 0x1000, 500);
    assert(next != first && t.active == 1);
    assert(snapshot.generation == first); // snapshot survives reset independently
    particle_trace_retire(&t, 0x1000);
    assert(!particle_trace_find(&t, 0x1000).generation && t.active == 0);
    uint32_t reused = particle_trace_reset(&t, 0x1000, 900);
    assert(reused != first && reused != next);
    particle_trace_retire(&t, 0x1000);
    particle_trace_retire(&t, 0x1000); // repeated cancellation cannot underflow
    assert(!t.active);
    for (unsigned i = 1; i <= PARTICLE_TRACE_SLOTS; ++i)
        assert(particle_trace_reset(&t, i, i * 10));
    assert(!particle_trace_reset(&t, 0xffff, 0));
    assert(t.active == PARTICLE_TRACE_SLOTS && t.untracked == 1);
    assert(!particle_trace_find(&t, 0xffff).generation);
    // Full tables preserve existing identities and can still reset live entries.
    first = particle_trace_find(&t, 1).generation;
    assert(particle_trace_reset(&t, 1, 3000) != first);
    assert(t.active == PARTICLE_TRACE_SLOTS);
    particle_trace_retire(&t, 1);
    assert(particle_trace_reset(&t, 0xffff, 4000));
    assert(!particle_trace_finish(&t, 999));
    assert(particle_trace_finish(&t, 50000));
    for (unsigned i = 1; i < PARTICLE_TRACE_RECORDS; ++i)
        assert(particle_trace_finish(&t, 1000));
    // Exhausting detail logs must not stop aggregate measurements.
    assert(!particle_trace_finish(&t, 110000));
    assert(t.calls == PARTICLE_TRACE_RECORDS + 2);
    assert(t.slow == 2 && t.max_us == 110000);
    assert(t.total_us == 999 + 50000 + 255 * 1000 + 110000);
    assert(t.frame_calls == t.calls && t.frame_sum_us == t.total_us);
    t.sequence = UINT32_MAX;
    assert(particle_trace_reset(&t, 2, 5000) != 0);
    assert(!particle_trace_find(&t, 0).instance);
    puts("Particle trace: reset/reuse identities, bounded capacity/logs and aggregates passed");
}
