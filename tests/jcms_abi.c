#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "host.h"

static RtHostEntry init, fini, start, stop, states;
#undef HOST_OVERRIDE
#define HOST_OVERRIDE(sym, fn) \
    static void __attribute__((constructor)) RT_CAT(register_jcms_, __COUNTER__)(void) { \
        if (!strcmp(sym, "_ZN2nn3hid14InitializeJcmsEPvm")) init = (RtHostEntry){sym, fn}; \
        if (!strcmp(sym, "_ZN2nn3hid12FinalizeJcmsEv")) fini = (RtHostEntry){sym, fn}; \
        if (!strcmp(sym, "_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE")) start = (RtHostEntry){sym, fn}; \
        if (!strcmp(sym, "_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE")) stop = (RtHostEntry){sym, fn}; \
        if (!strcmp(sym, "_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE")) states = (RtHostEntry){sym, fn}; \
    }
#include "../runtime/ov_input.c"

void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

int main(void) {
    if (!init.fn || !fini.fn || !start.fn || !stop.fn || !states.fn) {
        fputs("missing disconnected Jcms binding\n", stderr);
        return 1;
    }
    unsigned char storage[0x4000];
    memset(storage, 0xa5, sizeof storage);
    Cpu c = {0};
    c.x[0] = (uintptr_t)storage;
    c.x[1] = sizeof storage;
    for (int i = 19; i < 31; i++) c.x[i] = 0x12300 + i;
    init.fn(&c);
    start.fn(&c);
    c.x[0] = (uintptr_t)storage;
    c.x[1] = 8;
    states.fn(&c);
    assert(c.x[0] == 0);
    stop.fn(&c);
    fini.fn(&c);
    for (int i = 19; i < 31; i++) assert(c.x[i] == (uint64_t)(0x12300 + i));
    for (size_t i = 0; i < sizeof storage; i++) assert(storage[i] == 0xa5);
    puts("disconnected Jcms ABI passed");
    return 0;
}
