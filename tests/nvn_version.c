#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn_api.h"

#undef HOST_OVERRIDE
#define HOST_OVERRIDE(name, fn)
#undef HOST_OBSERVER
#define HOST_OBSERVER(name, fn)
#include "../runtime/nvn_api_core.c"

const char rt_game_target[] = TEST_TARGET;
GfxStats gfx_counters;
unsigned rt_trace;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
const GfxBackend *gfx_backend(void) { abort(); }
uint64_t nvn_submit_count(void) { abort(); }
int kern_core_away(void) { abort(); }
void kern_core_back(int *away) { (void)away; abort(); }
void plat_mutex_lock(PlatMutex *m) { (void)m; abort(); }
void plat_mutex_unlock(PlatMutex *m) { (void)m; abort(); }
void plat_mutex_lock_shared(PlatMutex *m) { (void)m; abort(); }
void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; abort(); }
void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) {
    (void)p; (void)off; (void)size; abort();
}
uint64_t rt_ticks(void) { abort(); }

int main(void) {
    Cpu c = {0};
    struct { int before, value, after; } out = {123, -1, 456};
    for (int query = 0; query < 3; query++) {
        c.x[1] = query;
        c.x[2] = (uintptr_t)&out.value;
        nvnp_nvnDeviceGetInteger(&c);
        int expected = query == 0 ? 231 : query == 1 ? TEST_MINOR : 14;
        if (out.value != expected) {
            fprintf(stderr, "%s query %d: expected %d, got %d\n", TEST_TARGET, query, expected, out.value);
            return 1;
        }
        assert(out.before == 123 && out.after == 456);
    }
    puts("NVN target version passed");
    return 0;
}
