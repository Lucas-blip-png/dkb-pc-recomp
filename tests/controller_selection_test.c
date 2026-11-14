// Hot-swap policy: startup preferences, real arrivals, removal fallback and explicit choices.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"

const char *cfg_get(const char *key) { (void)key; abort(); }
void rt_log(const char *fmt, ...) { (void)fmt; abort(); }

static InputPadDevice device(uint32_t id, const char *key) {
    InputPadDevice d = {.id = id};
    snprintf(d.key, sizeof d.key, "%s", key);
    return d;
}

int main(void) {
    InputPadSelection s = {0};
    InputPadDevice d[4] = {device(1, "dualsense"), device(2, "switch")};
    assert(input_pad_choose(&s, d, 2, "switch", 0) == 2 && !s.automatic);
    for (int i = 0; i < 120; i++) assert(input_pad_choose(&s, d, 2, "switch", 2) == 2);
    assert(!s.automatic);   // repeated polls/list refreshes never reinterpret the baseline as arrivals

    d[2] = device(3, "xbox");
    assert(input_pad_choose(&s, d, 3, "switch", 2) == 3 && s.automatic);
    assert(input_pad_choose(&s, d, 3, "switch", 2) == 3);   // retry a pending open without losing the arrival
    assert(input_pad_choose(&s, d, 3, "switch", 3) == 3);
    assert(input_pad_choose(&s, d, 2, "switch", 0) == 1 && s.automatic);   // disconnect falls back

    // Same saved key after a takeover still works as an explicit menu request.
    s.force = 1;
    assert(input_pad_choose(&s, d, 2, "switch", 1) == 2 && !s.automatic);
    assert(input_pad_choose(&s, d, 2, "dualsense", 2) == 1 && !s.automatic);
    d[2] = device(4, "xbox");
    assert(input_pad_choose(&s, d, 3, "dualsense", 1) == 4 && s.automatic);
    assert(input_pad_choose(&s, d, 3, "dualsense", 4) == 4);

    // Enumeration reordering does not count as a new controller.
    InputPadDevice reordered[3] = {d[2], d[0], d[1]};
    assert(input_pad_choose(&s, reordered, 3, "dualsense", 4) == 4);
    assert(input_pad_choose(&s, reordered, 3, "dualsense", 4) == 4);

    // An explicit request wins over a simultaneous arrival, which is then part of the baseline.
    d[3] = device(5, "wired-pro"); s.force = 1;
    assert(input_pad_choose(&s, d, 4, "dualsense", 4) == 1 && !s.automatic);
    assert(input_pad_choose(&s, d, 4, "dualsense", 1) == 1);
    assert(input_pad_choose(&s, NULL, 0, "dualsense", 0) == 0 && s.automatic);
    d[0] = device(6, "dualsense");
    assert(input_pad_choose(&s, d, 1, "dualsense", 0) == 6 && s.automatic);
    assert(input_pad_choose(&s, d, 1, "dualsense", 6) == 6);
    free(s.ids);

    s = (InputPadSelection){0};
    assert(input_pad_choose(&s, d, 1, "missing-at-launch", 0) == 6 && s.automatic);
    d[1] = device(7, "switch"); d[2] = device(8, "xbox");
    assert(input_pad_choose(&s, d, 3, "missing-at-launch", 6) == 8);   // last simultaneous arrival wins
    assert(input_pad_choose(&s, d, 3, "missing-at-launch", 8) == 8);
    s.force = 1;
    assert(input_pad_choose(&s, d, 3, "auto", 8) == 6 && s.automatic);
    assert(input_pad_choose(&s, d, 3, "auto", 6) == 6);
    free(s.ids);
    puts("controller selection: startup, arrivals, fallback, explicit choices, pending opens and topology stability passed");
    return 0;
}
