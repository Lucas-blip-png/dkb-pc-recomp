// The helper translation threads (vk_spec.h): each thread and its one-job handoff, the command buffers its parts are
// recorded into, its failure flag, and the counters. What a part is and when it is taken is vk_exec.c's (par_*).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_spec.h"

_Thread_local int vk_spec_here;

// One helper. A helper reads its failure flag at every op and the replay thread polls job_state while it waits, so
// each helper's fields keep to cache lines of their own.
#define SPEC_CBS 32
enum { J_IDLE, J_READY, J_DONE };
typedef struct {
    _Alignas(64) volatile int32_t fail_why;   // the first reason of the running part
    volatile int32_t job_state, sleeps, joiner_sleeps;
    VkSpecFn fn;
    void *arg;
    uint64_t fence;
    PlatEvent wake_ev, done_ev;
    PlatThread *thread;
    VkCommandPool pool;   // its command buffers (below)
    struct { VkCommandBuffer cb; uint64_t fence; } cbs[SPEC_CBS];
    int ncbs, cur_cb;
} Helper;
static Helper HS[VK_SPEC_HELPERS];
static int nhelpers;

static volatile uint64_t world = 1;
static volatile uint32_t events;

static void fail_one(Helper *h, int why) {
    int32_t none = VK_SPEC_NONE;
    __atomic_compare_exchange_n(&h->fail_why, &none, why, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

int vk_spec_fail(int why) {
    if (vk_spec_here) fail_one(&HS[vk_spec_here - 1], why);
    else
        for (int k = 0; k < nhelpers; k++) fail_one(&HS[k], why);   // an idle helper's flag is cleared at its next launch
    return 1;
}

int vk_spec_part_failed(int k) { return __atomic_load_n(&HS[k].fail_why, __ATOMIC_RELAXED); }
int vk_spec_failed(void) { return vk_spec_here ? vk_spec_part_failed(vk_spec_here - 1) : VK_SPEC_NONE; }
uint64_t vk_spec_fence(void) { return vk_spec_here ? HS[vk_spec_here - 1].fence : 0; }
void vk_spec_world_changed(void) { __atomic_add_fetch(&world, 1, __ATOMIC_RELEASE); }
uint64_t vk_spec_world(void) { return __atomic_load_n(&world, __ATOMIC_ACQUIRE); }
static int spec_draw_fast_on(void) {
    static int mode = -1;
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        m = rt_knob("DRAW_FAST", "1");
        __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    }
    return RT_KNOB_ON(m);
}

void vk_spec_event(void) {
    // When no helper threads exist (DKB_REPLAY_PAR=0, the default), nothing ever reads `events`; skip the cross-core
    // atomic RMW that every GPU-written range across all recording threads would otherwise execute.
    if (!__atomic_load_n(&nhelpers, __ATOMIC_RELAXED) && spec_draw_fast_on()) return;
    __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
}
uint32_t vk_spec_events(void) { return __atomic_load_n(&events, __ATOMIC_RELAXED); }

// ---- the threads
// A helper has one job at a time, handed over by the replay thread and joined by it. Both sides spin before they
// sleep: a part is a fraction of a millisecond to two milliseconds of work, the runs of one frame follow each other
// within a few milliseconds, and a sleeping thread takes tens of microseconds to come back. A helper spins once after
// a job; while no job comes (a menu, a scene without a long run) it only sleeps.
static int prio, checking;

#define SPIN_NS 200000u   // a helper's spin for its next job

static void spec_main(void *arg) {
    Helper *h = arg;
    plat_thread_priority(prio);
    vk_spec_here = (int)(h - HS) + 1;
    for (;;) {
        uint64_t t0 = plat_now_ns();
        for (unsigned i = 1; __atomic_load_n(&h->job_state, __ATOMIC_ACQUIRE) != J_READY; i++) {
            plat_pause();
            if (i % 64 || plat_now_ns() - t0 < SPIN_NS) continue;
            __atomic_store_n(&h->sleeps, 1, __ATOMIC_SEQ_CST);
            // the timeout: a missed wake costs at most 2 ms
            if (__atomic_load_n(&h->job_state, __ATOMIC_SEQ_CST) != J_READY) plat_event_wait_ns(&h->wake_ev, 2000000);
            __atomic_store_n(&h->sleeps, 0, __ATOMIC_RELAXED);
        }
        h->fn(h->arg);
        __atomic_store_n(&h->job_state, J_DONE, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&h->joiner_sleeps, __ATOMIC_SEQ_CST)) plat_event_set(&h->done_ev);
    }
}

int vk_spec_helpers_for(uint32_t cpus) {
    return cpus >= 24 ? 5 : cpus >= 16 ? 4 : cpus >= 8 ? 3 : cpus >= 4 ? 2 : 1;
}

// How many helpers to start: DKB_REPLAY_HELPERS, or what the logical processors of the process give.
static int helpers_wanted(void) {
    const char *e = cfg_get("DKB_REPLAY_HELPERS");
    int n = e && *e ? atoi(e) : 0;
    if (n <= 0) n = vk_spec_helpers_for(plat_cpu_usable());
    return n > VK_SPEC_HELPERS ? VK_SPEC_HELPERS : n;
}

int vk_spec_on(void) {
    static int mode = -1;
    if (mode < 0) {
        static char name[VK_SPEC_HELPERS][16];
        mode = rt_knob("REPLAY_PAR", "0");
        const char *e = cfg_get("DKB_REPLAY_PAR");
        checking = e && !strcmp(e, "check");
        if (mode) {
            // Normal, not the caller's: the first call comes from inside submit_record, which raises the render thread
            // to above normal for the submission, and helpers that kept that would leave no core to normal threads.
            prio = PLAT_PRIO_NORMAL;
            int want = helpers_wanted(), n = 0;
            for (; n < want; n++) {
                Helper *h = &HS[n];
                h->cur_cb = -1;
                plat_event_init(&h->wake_ev, 0, 0);
                plat_event_init(&h->done_ev, 0, 0);
                snprintf(name[n], sizeof name[n], "vk helper %d", n + 1);
                if (!(h->thread = plat_thread_new(spec_main, h, name[n]))) break;
            }
            __atomic_store_n(&nhelpers, n, __ATOMIC_RELEASE);
            if (!n) mode = 0;
        }
        rt_log("vk_spec: long runs of draws are %s (DKB_REPLAY_PAR=%s, %d helper thread%s)",
               !mode ? "translated by the replay thread alone"
               : mode == 2 ? "shared with the helper threads in the B blocks (DKB_AB)"
               : checking ? "translated by the helper threads and by the replay thread and compared, the replay thread's submitted"
               : "shared with the helper threads",
               mode == 2 ? "ab" : checking ? "check" : mode ? "1" : "0", nhelpers, nhelpers == 1 ? "" : "s");
    }
    return RT_KNOB_ON(mode);
}

int vk_spec_count(void) { return nhelpers; }
int vk_spec_checking(void) { return checking; }

void vk_spec_launch(int k, VkSpecFn fn, void *arg, uint64_t fence) {
    Helper *h = &HS[k];
    __atomic_store_n(&h->fail_why, VK_SPEC_NONE, __ATOMIC_RELAXED);
    h->fn = fn, h->arg = arg, h->fence = fence;
    __atomic_store_n(&h->job_state, J_READY, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&h->sleeps, __ATOMIC_SEQ_CST)) plat_event_set(&h->wake_ev);
}

int vk_spec_join(int k) {
    Helper *h = &HS[k];
    for (unsigned i = 1; __atomic_load_n(&h->job_state, __ATOMIC_ACQUIRE) != J_DONE; i++) {
        plat_pause();
        if (i % 20000) continue;   // well over a part's length: the helper lost its core
        __atomic_store_n(&h->joiner_sleeps, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&h->job_state, __ATOMIC_SEQ_CST) != J_DONE) plat_event_wait_ns(&h->done_ev, 1000000);
        __atomic_store_n(&h->joiner_sleeps, 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&h->job_state, J_IDLE, __ATOMIC_RELEASE);
    return vk_spec_part_failed(k);
}

// ---- command buffers
// A pool of each helper's own: a command pool is used by one thread at a time, and the replay thread's is in use by it
// or by the recorder (vk_rec.c) while the helpers record. A buffer stays with its part until the submission's value is
// done; a dropped part's is free at once.
VkCommandBuffer vk_spec_cb(int k, uint64_t fence) {
    VkCtx *c = vk_ctx();
    Helper *h = &HS[k];
    if (!h->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &h->pool) != VK_SUCCESS) return h->pool = VK_NULL_HANDLE, VK_NULL_HANDLE;
    }
    uint64_t done = vk_sync_fence_done();
    h->cur_cb = -1;
    for (int i = 0; i < h->ncbs && h->cur_cb < 0; i++)
        if (h->cbs[i].fence <= done) h->cur_cb = i;
    if (h->cur_cb < 0) {
        if (h->ncbs == SPEC_CBS) return VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = h->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &ai, &h->cbs[h->ncbs].cb) != VK_SUCCESS) return VK_NULL_HANDLE;
        h->cur_cb = h->ncbs++;
    }
    h->cbs[h->cur_cb].fence = fence;
    return h->cbs[h->cur_cb].cb;
}

void vk_spec_cb_drop(int k) {
    Helper *h = &HS[k];
    if (h->cur_cb >= 0) h->cbs[h->cur_cb].fence = 0;
    h->cur_cb = -1;
}

void vk_spec_shutdown(void) {
    VkCtx *c = vk_ctx();
    for (int k = 0; k < VK_SPEC_HELPERS; k++) {
        Helper *h = &HS[k];
        if (c && h->pool) vkDestroyCommandPool(c->device, h->pool, NULL);
        h->pool = VK_NULL_HANDLE, h->ncbs = 0, h->cur_cb = -1;
        memset(h->cbs, 0, sizeof h->cbs);
    }
}

// ---- counters (replay thread)
static struct { uint64_t parts, taken, draws, wait_ns, busy_ns, why[VK_SPEC_N]; } S;

void vk_spec_note(int why, uint32_t draws, uint64_t wait_ns, uint64_t busy_ns) {
    S.parts++, S.wait_ns += wait_ns, S.busy_ns += busy_ns;
    if (why == VK_SPEC_NONE) S.taken++, S.draws += draws;
    else S.why[why < VK_SPEC_N ? why : VK_SPEC_NONE]++;
}

void vk_spec_log(void) {
    static const char *const name[VK_SPEC_N] = {"?", "write", "stand-in", "feedback", "pending", "texture", "upload",
                                                "mirror", "target", "op", "world", "usnap", "state", "cancel"};
    if (!S.parts) return;
    char why[256] = "";
    size_t o = 0;
    for (int i = 0; i < VK_SPEC_N && o < sizeof why - 32; i++)
        if (S.why[i]) o += (size_t)snprintf(why + o, sizeof why - o, " %s %llu", name[i], (unsigned long long)S.why[i]);
    rt_log("perf: helper parts %llu, taken %llu with %llu draws (%.0f each), helper %.1f us and join wait %.1f us a part; "
           "dropped:%s",
           (unsigned long long)S.parts, (unsigned long long)S.taken, (unsigned long long)S.draws,
           S.taken ? (double)S.draws / (double)S.taken : 0.0, (double)S.busy_ns / 1e3 / (double)S.parts,
           (double)S.wait_ns / 1e3 / (double)S.parts, o ? why : " none");
    memset(&S, 0, sizeof S);
}
