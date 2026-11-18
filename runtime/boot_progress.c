// The start's progress (boot_progress.h). The counters are reset when a step begins; an item counted after its step
// ended lands in the next step, which only the screen reads, and it clamps done to the total.
#include "boot_progress.h"

#include "platform/platform.h"

static volatile int32_t g_step;
static volatile int64_t g_done, g_total, g_work, g_since;

void boot_progress_step(int step, uint64_t total) {
    plat_xchg64(&g_done, 0);
    plat_xchg64(&g_work, 0);
    plat_xchg64(&g_total, (int64_t)total);
    plat_xchg64(&g_since, (int64_t)plat_now_ns());
    plat_xchg32(&g_step, step);
}

void boot_progress_total(uint64_t total) { plat_xchg64(&g_total, (int64_t)total); }
void boot_progress_add(uint64_t n) { __atomic_add_fetch(&g_done, (int64_t)n, __ATOMIC_RELAXED); }
void boot_progress_work(uint64_t n) { __atomic_add_fetch(&g_work, (int64_t)n, __ATOMIC_RELAXED); }

void boot_progress_read(BootProgress *out) {
    out->step = __atomic_load_n(&g_step, __ATOMIC_ACQUIRE);
    out->total = (uint64_t)__atomic_load_n(&g_total, __ATOMIC_ACQUIRE);
    out->done = (uint64_t)__atomic_load_n(&g_done, __ATOMIC_RELAXED);
    out->work = (uint64_t)__atomic_load_n(&g_work, __ATOMIC_RELAXED);
    out->since_ns = (uint64_t)__atomic_load_n(&g_since, __ATOMIC_RELAXED);
    if (out->total && out->done > out->total) out->done = out->total;
}
