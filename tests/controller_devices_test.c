// Identity caching must avoid USB opens on routine polls, preserve keys, and retry failed opens.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t SDL_JoystickID;
typedef unsigned SDL_GUID;
typedef struct {
    SDL_JoystickID id;
    unsigned guid;
    const char *name, *serial;
    int opens, closes, failed, references;
} SDL_Gamepad;

static SDL_Gamepad pads[4];
static int connected[4], count;

static SDL_Gamepad *find(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) if (pads[i].id == id) return &pads[i];
    abort();
}
static SDL_JoystickID *SDL_GetGamepads(int *n) {
    *n = count;
    SDL_JoystickID *ids = count ? malloc((size_t)count * sizeof *ids) : NULL;
    for (int i = 0; i < count; i++) ids[i] = pads[connected[i]].id;
    return ids;
}
static const char *SDL_GetGamepadNameForID(SDL_JoystickID id) { return find(id)->name; }
static SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID id) { return find(id)->guid; }
static void SDL_GUIDToString(SDL_GUID guid, char *s, int size) { snprintf(s, (size_t)size, "%032x", guid); }
static SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    SDL_Gamepad *p = find(id);
    p->opens++;
    if (p->failed) return NULL;
    p->references++;
    return p;
}
static const char *SDL_GetGamepadSerial(SDL_Gamepad *p) { return p->serial; }
static void SDL_CloseGamepad(SDL_Gamepad *p) { p->closes++; assert(p->references-- > 0); }
#define SDL_free free

#include "input.h"
// The runner copies the production scanner block here, without the unrelated window/game runtime.
#include "controller_devices_under_test.inc"

static InputPadDevice *scan(int expected) {
    InputPadDevice *list = NULL;
    assert(devices_locked(&list) == expected);
    return list;
}

int main(void) {
    pads[0] = (SDL_Gamepad){.id = 101, .guid = 1, .name = "DualSense", .serial = "serial-one"};
    pads[1] = (SDL_Gamepad){.id = 202, .guid = 2, .name = "Switch controller", .serial = ""};
    connected[0] = 0; connected[1] = 1; count = 2;
    InputPadDevice *list = scan(2);
    char serial_key[128], first_key[128], second_key[128];
    snprintf(serial_key, sizeof serial_key, "%s", list[0].key);
    snprintf(first_key, sizeof first_key, "%s", list[1].key);
    free(list);
    assert(pads[0].opens == 1 && pads[1].opens == 1);
    for (int i = 0; i < 120; i++) {
        list = scan(2);
        assert(!strcmp(list[0].key, serial_key) && !strcmp(list[1].key, first_key));
        free(list);
    }
    assert(pads[0].opens == 1 && pads[1].opens == 1);
    assert(pads[0].closes == 1 && pads[1].closes == 1);

    // New duplicates are identified once; removing their peer must not renumber the survivor.
    pads[2] = (SDL_Gamepad){.id = 203, .guid = 2, .name = "Switch controller", .serial = ""};
    connected[2] = 2; count = 3;
    list = scan(3);
    snprintf(second_key, sizeof second_key, "%s", list[2].key);
    assert(strcmp(first_key, second_key)); free(list);
    assert(pads[2].opens == 1 && pads[0].opens == 1 && pads[1].opens == 1);
    connected[1] = 2; count = 2;
    list = scan(2); assert(!strcmp(list[1].key, second_key)); free(list);

    // Reconnect with a new SDL ID: open once and recover the vacant fallback ordinal.
    pads[1].id = 204;
    connected[1] = 1; connected[2] = 2; count = 3;
    list = scan(3); assert(!strcmp(list[1].key, first_key)); free(list);
    assert(pads[1].opens == 2 && pads[2].opens == 1);

    // Missing controllers stay absent, then reconnect by serial independently of SDL instance ID.
    connected[0] = 1; connected[1] = 2; count = 2;
    list = scan(2); free(list);
    pads[0].id = 301; pads[0].failed = 1;
    connected[0] = 0; connected[1] = 1; connected[2] = 2; count = 3;
    list = scan(3); free(list);
    int attempts = pads[0].opens;
    list = scan(3); free(list);
    assert(pads[0].opens == attempts + 1);   // a failed open cannot poison the identity cache
    pads[0].failed = 0;
    list = scan(3); assert(!strcmp(list[0].key, serial_key)); free(list);
    attempts = pads[0].opens;
    list = scan(3); free(list);
    assert(pads[0].opens == attempts);
    assert(pads[0].references == 0 && pads[1].references == 0 && pads[2].references == 0);
    count = 0;
    list = scan(0); assert(!list);
    free(device_cache);
    puts("controller devices: cached polls never reopen; duplicate keys, reconnect and failed-open retry passed");
    return 0;
}
