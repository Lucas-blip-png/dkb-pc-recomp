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
    assert(input_pad_choose(&s, d, 2, "switch", "auto", 0) == 2 && !s.automatic);
    for (int i = 0; i < 120; i++) assert(input_pad_choose(&s, d, 2, "switch", "auto", 2) == 2);
    assert(!s.automatic);   // repeated polls/list refreshes never reinterpret the baseline as arrivals

    d[2] = device(3, "xbox");
    assert(input_pad_choose(&s, d, 3, "switch", "auto", 2) == 3 && s.automatic);
    assert(input_pad_choose(&s, d, 3, "switch", "auto", 2) == 3);   // retain the requested arrival until selected
    assert(input_pad_choose(&s, d, 3, "switch", "auto", 3) == 3);
    assert(input_pad_choose(&s, d, 2, "switch", "auto", 0) == 1 && s.automatic);   // disconnect falls back

    // Same saved key after a takeover still works as an explicit menu request.
    s.force = 1;
    assert(input_pad_choose(&s, d, 2, "switch", "auto", 1) == 2 && !s.automatic);
    assert(input_pad_choose(&s, d, 2, "dualsense", "auto", 2) == 1 && !s.automatic);
    d[2] = device(4, "xbox");
    assert(input_pad_choose(&s, d, 3, "dualsense", "auto", 1) == 4 && s.automatic);
    assert(input_pad_choose(&s, d, 3, "dualsense", "auto", 4) == 4);

    // Enumeration reordering does not count as a new controller.
    InputPadDevice reordered[3] = {d[2], d[0], d[1]};
    assert(input_pad_choose(&s, reordered, 3, "dualsense", "auto", 4) == 4);
    assert(input_pad_choose(&s, reordered, 3, "dualsense", "auto", 4) == 4);

    // An explicit request wins over a simultaneous arrival, which is then part of the baseline.
    d[3] = device(5, "wired-pro"); s.force = 1;
    assert(input_pad_choose(&s, d, 4, "dualsense", "auto", 4) == 1 && !s.automatic);
    assert(input_pad_choose(&s, d, 4, "dualsense", "auto", 1) == 1);
    assert(input_pad_choose(&s, NULL, 0, "dualsense", "auto", 0) == 0 && s.automatic);
    d[0] = device(6, "dualsense");
    assert(input_pad_choose(&s, d, 1, "dualsense", "auto", 0) == 6 && s.automatic);
    assert(input_pad_choose(&s, d, 1, "dualsense", "auto", 6) == 6);
    free(s.ids);

    s = (InputPadSelection){0};
    assert(input_pad_choose(&s, d, 1, "missing-at-launch", "auto", 0) == 6 && s.automatic);
    d[1] = device(7, "switch"); d[2] = device(8, "xbox");
    assert(input_pad_choose(&s, d, 3, "missing-at-launch", "auto", 6) == 8);   // last simultaneous arrival wins
    assert(input_pad_choose(&s, d, 3, "missing-at-launch", "auto", 8) == 8);
    s.force = 1;
    assert(input_pad_choose(&s, d, 3, "auto", "auto", 8) == 6 && s.automatic);
    assert(input_pad_choose(&s, d, 3, "auto", "auto", 6) == 6);
    free(s.ids);

    // Co-op keeps player 1: an arrival joins as player 2 while the current pad stays, and a disconnect still falls back.
    s = (InputPadSelection){.keep = 1};
    assert(input_pad_choose(&s, d, 1, "auto", "auto", 0) == 6);
    assert(input_pad_choose(&s, d, 2, "auto", "auto", 6) == 6 && !s.pending);
    assert(input_pad_choose(&s, &d[1], 1, "auto", "auto", 0) == 7);
    free(s.ids);
    // A configured pad that connects after a fallback pad still becomes player 1 in co-op.
    s = (InputPadSelection){.keep = 1};
    assert(input_pad_choose(&s, d, 1, "xbox", "auto", 0) == 6);
    assert(input_pad_choose(&s, d, 3, "xbox", "auto", 6) == 8);
    assert(input_pad_choose(&s, d, 3, "xbox", "auto", 6) == 8);   // keep the request until the open succeeds
    free(s.ids);
    // Two pads with the same name and GUID differ by instance id only: player 2 is the other one, and keeps it.
    InputPadDevice twin[2] = {device(10, "pro:n1"), device(11, "pro:n2")};
    assert(input_pad2_choose(twin, 2, 10, 0, "auto") == 11 && input_pad2_choose(twin, 2, 11, 0, "auto") == 10);
    assert(input_pad2_choose(twin, 2, 10, 11, "auto") == 11 && input_pad2_choose(twin, 2, 11, 10, "auto") == 10);
    assert(input_pad2_choose(twin, 2, 10, 10, "auto") == 11);   // never player 1's own pad
    assert(input_pad2_choose(twin, 1, 10, 0, "auto") == 0 && input_pad2_choose(twin, 2, 0, 0, "auto") == 0);

    InputPadDevice coop[3] = {device(20, "saved-p2"), device(21, "spare"), device(22, "arrival")};
    // The saved Player 2 pad can play alone, then moves to Player 2 when a spare connects.
    s = (InputPadSelection){.keep = 1};
    assert(input_pad_choose(&s, coop, 1, "auto", "saved-p2", 0) == 20 && !s.reserved);
    assert(input_pad_choose(&s, coop, 2, "auto", "saved-p2", 20) == 21 && s.reserved == 20);
    assert(input_pad2_choose(coop, 2, 21, 0, "saved-p2") == 20);
    // A further arrival must not take either slot while both assigned pads are connected.
    assert(input_pad_choose(&s, coop, 3, "auto", "saved-p2", 21) == 21 && !s.pending);
    assert(input_pad2_choose(coop, 3, 21, 20, "saved-p2") == 20);
    assert(input_pad2_choose(coop + 1, 2, 21, 0, "saved-p2") == 0);
    free(s.ids);

    // If both players prefer the same device, an explicit Player 1 choice takes priority.
    s = (InputPadSelection){.keep = 1};
    assert(input_pad_choose(&s, coop, 2, "saved-p2", "saved-p2", 0) == 20 && !s.reserved);
    assert(input_pad2_choose(coop, 2, 20, 0, "saved-p2") == 0);
    free(s.ids);
    puts("controller selection: startup, arrivals, fallback, explicit choices, pending opens, topology stability and co-op passed");
    return 0;
}
