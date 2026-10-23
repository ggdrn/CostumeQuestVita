#include "particle_progressive.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    const char *leaves = "particles/effects/weather/precipitation/pollenlight/leaves";
    assert(particle_progressive_matches(leaves, 1, 20, 661));
    assert(!particle_progressive_matches(leaves, 0, 20, 661));
    assert(!particle_progressive_matches(leaves, 1, 20, 660));
    assert(!particle_progressive_matches(leaves, 1, NAN, 661));
    assert(!particle_progressive_matches(NULL, 1, 20, 661));
    assert(!particle_progressive_matches("particles/effects/combat/leaves", 1, 20, 661));
    assert(!particle_progressive_matches("particles/effects/weather/precipitation/pollenlight/leavesb", 1, 20, 661));
    ParticleProgressive s = {0};
    ParticleProgressiveTicket a, b;
    particle_progressive_frame(&s, 1);
    assert(!particle_progressive_start(&s, 1, 0, 20));
    assert(!particle_progressive_start(&s, 1, 1, NAN));
    assert(particle_progressive_start(&s, 1, 1, 20));
    assert(particle_progressive_start(&s, 2, 2, 20));
    assert(!particle_progressive_start(&s, 1, 1, 20));
    assert(!particle_progressive_reserve(&s, 1, 1, 0, &a));
    assert(!particle_progressive_reserve(&s, 1, 1, NAN, &a));
    assert(!particle_progressive_reserve(&s, 1, 1, 0.3f, &a));
    assert(particle_progressive_reserve(&s, 1, 1, 1.f/30, &a));
    assert(!particle_progressive_reserve(&s, 2, 2, 1.f/30, &b)); // no parallel extras
    assert(a.extra > 0 && a.extra + 1.f/30 <= PARTICLE_PROGRESSIVE_MAX_DT);
    assert(!particle_progressive_finish(&s, a, 2500));
    assert(!particle_progressive_reserve(&s, 1, 1, 1.f/30, &b)); // once per instance/frame
    assert(particle_progressive_reserve(&s, 2, 2, 1.f/30, &b));
    assert(!particle_progressive_finish(&s, b, 2500));
    assert(s.spent_us == 5000 && s.overruns == 1); // at most one quantum over target
    assert(!particle_progressive_reserve(&s, 1, 1, 1.f/30, &b));
    particle_progressive_frame(&s, 2);
    assert(s.spent_us == 0);
    // Cancellation and address reuse while a call is outstanding cannot debit new work.
    assert(particle_progressive_reserve(&s, 1, 1, 1.f/30, &a));
    assert(particle_progressive_cancel(&s, 1));
    assert(particle_progressive_start(&s, 1, 3, 20));
    particle_progressive_frame(&s, 3);
    assert(!particle_progressive_reserve(&s, 1, 3, 1.f/30, &b));
    assert(!particle_progressive_finish(&s, a, 1500));
    assert(s.jobs[0].remaining == 20 && s.spent_us == 1500);
    assert(!particle_progressive_finish(&s, a, 1500)); // duplicate ticket cannot charge twice
    assert(!particle_progressive_reserve(&s, 1, 1, 1.f/30, &b)); // stale generation cancels
    assert(!s.jobs[0].instance);
    // Simulate simultaneous weather instances with a shared budget for 20 extra seconds each.
    s = (ParticleProgressive){0};
    particle_progressive_frame(&s, 1);
    assert(particle_progressive_start(&s, 1, 10, 20));
    assert(particle_progressive_start(&s, 2, 20, 20));
    double extra[2] = {0};
    for (unsigned frame = 1; frame < 250 && particle_progressive_pending(&s); ++frame) {
        particle_progressive_frame(&s, frame);
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (unsigned j = 0; j < 2; ++j) {
                if (particle_progressive_reserve(&s, j+1, (j+1)*10, 1.f/30, &a)) {
                    extra[j] += a.extra;
                    particle_progressive_finish(&s, a, 2000);
                }
            }
        }
        assert(s.spent_us <= 4000);
    }
    assert(s.completed == 2 && !particle_progressive_pending(&s));
    assert(fabs(extra[0]-20) < 0.0001 && fabs(extra[1]-20) < 0.0001);
    assert(s.steps == 172);
    // Exhausted capacity has an explicit fallback and never evicts an active job.
    s = (ParticleProgressive){0};
    particle_progressive_frame(&s, 10);
    for (unsigned i = 1; i <= PARTICLE_PROGRESSIVE_JOBS; ++i)
        assert(particle_progressive_start(&s, i, i, 20));
    assert(!particle_progressive_start(&s, 99, 99, 20));
    assert(s.fallback == 1 && particle_progressive_pending(&s) == PARTICLE_PROGRESSIVE_JOBS);
    // An inactive job at the head must not starve an instance still being updated.
    particle_progressive_frame(&s, 15);
    assert(particle_progressive_reserve(&s, 8, 8, 1.f/30, &a));
    particle_progressive_finish(&s, a, 3000);
    puts("Progressive weather: shared budget, fairness, timing limits, completion, reuse and fallback passed");
}
