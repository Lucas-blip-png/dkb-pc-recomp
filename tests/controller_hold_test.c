// Selected-controller fullscreen hold: deterministic time, no SDL, game data or display changes.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "input.h"
#include "input_map.h"

// Other input_map.c entry points use these services; the pure hold gate must not.
const char *cfg_get(const char *key) { (void)key; abort(); }
void rt_log(const char *fmt, ...) { (void)fmt; abort(); }

static InputHoldGate gate;
static const uint64_t start = 1ull << B_PLUS, back = 1ull << B_MINUS;

static void sample(uint32_t device, uint64_t now, uint64_t buttons, int fires, uint64_t expected) {
    assert(input_hold_gate(&gate, device, now, &buttons) == fires);
    assert(buttons == expected);
}

int main(void) {
    const uint64_t both = start | back, a = 1ull << B_A;
    sample(1, 0, 0, 0, 0);
    sample(1, 1, start | a, 0, start | a);
    sample(1, 2, back, 0, back);
    sample(1, 10, both | a, 0, a);
    sample(1, 500000009, both, 0, 0);
    sample(1, 500000010, both | a, 1, a);
    sample(1, 9000000000ull, both, 0, 0);
    sample(1, 9100000000ull, start, 0, 0);
    sample(1, 9200000000ull, both, 0, 0);
    sample(1, 9300000000ull, back, 0, 0);
    sample(1, 9400000000ull, 0, 0, 0);
    sample(1, 9500000000ull, start, 0, start);

    // A partial release before activation restarts the continuous hold timer.
    sample(1, 10000000000ull, both, 0, 0);
    sample(1, 10200000000ull, back, 0, 0);
    sample(1, 10400000000ull, both, 0, 0);
    sample(1, 10899999999ull, both, 0, 0);
    sample(1, 10900000000ull, both, 1, 0);

    // Switching to a held controller cannot inherit or immediately start a hold.
    sample(2, 11000000000ull, both, 0, 0);
    sample(2, 12000000000ull, both, 0, 0);
    sample(2, 12100000000ull, back, 0, 0);
    sample(2, 12200000000ull, 0, 0, 0);
    sample(2, 12300000000ull, both, 0, 0);
    sample(2, 12800000000ull, both, 1, 0);

    // Disconnect and reconnect are a new SDL instance, requiring a fresh release.
    sample(0, 13000000000ull, 0, 0, 0);
    sample(3, 14000000000ull, both, 0, 0);
    sample(3, 15000000000ull, both, 0, 0);
    sample(3, 16000000000ull, 0, 0, 0);
    sample(3, 17000000000ull, both, 0, 0);
    sample(3, 17500000000ull, both, 1, 0);
    puts("controller hold: timing, release, isolation and reconnect passed");
    return 0;
}
