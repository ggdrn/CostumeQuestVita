/* MIT license; see LICENSE. */
#include "particle_compat.h"
#include <string.h>

int particle_warmup_prepare(uint32_t *code, size_t bytes) {
    static const uint32_t expected[] = {
        0xe92d4ff0, 0xed2d8b10, 0xe24dd014, 0xe1a07000,
        0xed9f0a7b, 0xed971a4d, 0xe1a08003, 0xe1a09002,
        0xe1a06001, 0xeeb41ac0, 0xeef1fa10, 0xceb00a41,
        0xeeb50ac0, 0xed870a4d, 0xeef1fa10, 0xda00006d
    };
    if (bytes < 0x208 || memcmp(code, expected, sizeof(expected)) ||
        code[0x204 / 4] != 0x42b40000)
        return 0;
    /* VCMP requested,90: MI selects requested only when it is smaller.
     * Keep the engine's simulation step, particles and lifetime unchanged. */
    code[0x2c / 4] = 0x4eb00a41;
    return 1;
}
