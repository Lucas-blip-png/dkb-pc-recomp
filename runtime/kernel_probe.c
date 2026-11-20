// --probe-kernel: the kernel's waits, wake orders, virtual cores, fibers and
// ExitThread on guest threads whose entries are host functions, then real lifted code on four guest threads: main's
// PLT stub for nn::os::GetSystemTick, through the lifted sdk function, to rt_ticks.
//
// Every probe thread is created with SVC 0x08 and drives the kernel through rt_svc, the entry the lifted code uses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"

#define STACK 0x10000
#define JOIN_NS 10000000000ll
#define TICK_SYMBOL "_ZN2nn2os13GetSystemTickEv"
#define FIBER_SWITCHES 10000
#define CORE_WORKERS 8
#define CORE_RUNS 300
#define TICK_THREADS 4
#define TICK_CALLS 1000

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static uint64_t a_lock_waiter, a_cv_waiter, a_cv_order_waiter, a_arb_waiter, a_event_waiter, a_core_worker,
    a_exit_entry, a_exit_nested, a_fiber_fn, a_fiber_exit_thread, a_fiber_exit_fn, a_tick_worker;
static uint64_t a_fib_init, a_fib_fin, a_fib_switch, a_fib_cur, tick_stub;

// ---- helpers: SVCs through rt_svc, thread start and join
static uint32_t sv(Cpu *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    rt_svc(c, imm, 0);
    return (uint32_t)c->x[0];
}

static void sleep_ms(Cpu *c, int ms) { sv(c, 0x0B, (uint64_t)ms * 1000000, 0, 0, 0); }
static __attribute__((noreturn)) void exit_thread(Cpu *c) {
    sv(c, 0x0A, 0, 0, 0, 0);
    rt_fatal("probe-kernel: ExitThread returned");
}
static uint64_t ga(const volatile void *p) { return (uint64_t)(uintptr_t)p; }

static uint32_t create(Cpu *c, uint64_t entry, void *arg, int prio, int core) {
    uint8_t *stack = mem_kernel_alloc(STACK, 16);
    c->x[1] = entry;
    c->x[2] = ga(arg);
    c->x[3] = ga(stack + STACK);
    c->x[4] = (uint64_t)prio;
    c->x[5] = (uint64_t)(int64_t)core;
    rt_svc(c, 0x08, 0);
    if (c->x[0]) rt_fatal("probe-kernel: CreateThread failed %#llx", (unsigned long long)c->x[0]);
    return (uint32_t)c->x[1];
}

static void start(Cpu *c, uint32_t h) { CHECK(sv(c, 0x09, h, 0, 0, 0) == 0, "StartThread %#x", h); }

static uint32_t wait1(Cpu *c, uint32_t h, int64_t ns) {
    uint32_t hs[1] = {h};
    return sv(c, 0x18, 0, ga(hs), 1, (uint64_t)ns);
}

static void join(Cpu *c, uint32_t h) {
    uint32_t r = wait1(c, h, JOIN_NS);
    if (r) rt_fatal("probe-kernel: thread %#x did not end (wait result %#x)", h, r);
    CHECK(sv(c, 0x16, h, 0, 0, 0) == 0, "CloseHandle(thread %#x)", h);
}

// Poll (sleeping, so the virtual core is free) until the thread reaches a state; 5 s at most.
static int wait_state(Cpu *c, uint32_t h, int want) {
    for (int i = 0; i < 5000; i++) {
        if (kern_thread_state(h) == want) return 1;
        sleep_ms(c, 1);
    }
    CHECK(0, "thread %#x never reached state %d (now %d)", h, want, kern_thread_state(h));
    return 0;
}

static double ms_since(uint64_t t0) { return (double)(kern_now_ns() - t0) / 1e6; }

// ---- user-mode lock of the SDK: tag word = owner handle | waiters bit
static uint32_t ld(volatile uint32_t *a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }
static int cas(volatile uint32_t *a, uint32_t *e, uint32_t v) {
    return __atomic_compare_exchange_n(a, e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void user_lock(Cpu *c, volatile uint32_t *a, uint32_t tag) {
    for (;;) {
        uint32_t v = 0;
        if (cas(a, &v, tag)) return;
        if (!(v & KERN_WAITERS_BIT)) {
            if (!cas(a, &v, v | KERN_WAITERS_BIT)) continue;
            v |= KERN_WAITERS_BIT;
        }
        sv(c, 0x1A, v & ~KERN_WAITERS_BIT, ga(a), tag, 0);
        if ((ld(a) & ~KERN_WAITERS_BIT) == tag) return;
    }
}

static void user_unlock(Cpu *c, volatile uint32_t *a, uint32_t tag) {
    uint32_t v = tag;
    if (cas(a, &v, 0)) return;
    sv(c, 0x1B, ga(a), 0, 0, 0);
}

// ---- lock handoff order
typedef struct { volatile uint32_t *lock, *key; uint32_t tag, r; int idx, held; } LockJob;
static volatile uint32_t g_lockw __attribute__((aligned(8))), g_keyw __attribute__((aligned(8)));
static volatile int32_t g_order_n;
static int g_order[8];

static void record(int idx) { g_order[plat_inc32(&g_order_n) - 1] = idx; }

static void lock_waiter(Cpu *c) {
    LockJob *j = arg_p(c, 0);
    user_lock(c, j->lock, j->tag);
    record(j->idx);
    user_unlock(c, j->lock, j->tag);
    exit_thread(c);
}

static int order_is(const int *want, int n, char *buf, size_t size) {
    int ok = g_order_n == n;
    buf[0] = 0;
    for (int i = 0; i < g_order_n && i < 8; i++) {
        snprintf(buf + strlen(buf), size - strlen(buf), "%s%d", i ? " " : "", g_order[i]);
        ok &= i < n && g_order[i] == want[i];
    }
    return ok;
}

static void test_lock(Cpu *c, uint32_t me) {
    static const int prio[] = {40, 30, 40, 30, 35}, want[] = {1, 3, 4, 0, 2};
    LockJob j[5];
    uint32_t h[5];
    g_lockw = me;
    g_order_n = 0;
    for (int i = 0; i < 5; i++) {
        j[i] = (LockJob){.lock = &g_lockw, .idx = i};
        h[i] = create(c, a_lock_waiter, &j[i], prio[i], -2);
        j[i].tag = h[i];
        start(c, h[i]);
        wait_state(c, h[i], KERN_WAIT_LOCK);
    }
    CHECK(ld(&g_lockw) == (me | KERN_WAITERS_BIT), "lock word %#x with 5 waiters", ld(&g_lockw));
    user_unlock(c, &g_lockw, me);
    for (int i = 0; i < 5; i++) join(c, h[i]);
    char got[64];
    int ok = order_is(want, 5, got, sizeof got);
    printf("lock handoff: priorities 40 30 40 30 35 registered in order, woken %s (expected 1 3 4 0 2): %s\n", got,
           ok ? "ok" : "FAIL");
    CHECK(ok, "lock handoff order");
    CHECK(ld(&g_lockw) == 0, "lock word %#x after the last unlock", ld(&g_lockw));
}

// ---- condition variables
static void cv_waiter(Cpu *c) {   // waits forever; woken holding the lock
    LockJob *j = arg_p(c, 0);
    user_lock(c, j->lock, j->tag);
    j->r = sv(c, 0x1C, ga(j->lock), ga(j->key), j->tag, (uint64_t)-1);
    j->held = (ld(j->lock) & ~KERN_WAITERS_BIT) == j->tag;
    record(j->idx);
    user_unlock(c, j->lock, j->tag);
    exit_thread(c);
}

static void test_cv_wake_order(Cpu *c) {
    static const int prio[] = {45, 35, 40}, want[] = {1, 2, 0};
    LockJob j[3];
    uint32_t h[3];
    g_lockw = 0;
    g_keyw = 0;
    g_order_n = 0;
    for (int i = 0; i < 3; i++) {
        j[i] = (LockJob){.lock = &g_lockw, .key = &g_keyw, .idx = i};
        h[i] = create(c, a_cv_waiter, &j[i], prio[i], -2);
        j[i].tag = h[i];
        start(c, h[i]);
        wait_state(c, h[i], KERN_WAIT_CONDVAR);
    }
    CHECK(ld(&g_keyw) == 1, "condvar key %u with 3 waiters", ld(&g_keyw));
    for (int i = 0; i < 3; i++) CHECK(sv(c, 0x1D, ga(&g_keyw), 1, 0, 0) == 0, "SignalProcessWideKey");
    for (int i = 0; i < 3; i++) join(c, h[i]);
    char got[64];
    int ok = order_is(want, 3, got, sizeof got);
    for (int i = 0; i < 3; i++) ok &= j[i].r == 0 && j[i].held;
    printf("condvar wake order: priorities 45 35 40, signalled one at a time, woken %s holding the lock (expected "
           "1 2 0): %s\n", got, ok ? "ok" : "FAIL");
    CHECK(ok, "condvar wake order");
    CHECK(ld(&g_keyw) == 0 && ld(&g_lockw) == 0, "key %u lock %#x after the last waiter", ld(&g_keyw), ld(&g_lockw));
}

// Another thread's user-mode critical section and Signal, run inside the waiter's svc 0x1C right after the lock
// release: take the lock, read the key (the SDK's Signal skips svc 0x1D on 0), release.
static volatile int g_seen_key;
static void cv_signaller(void) {
    uint32_t e = 0;
    if (cas(&g_lockw, &e, 0x7777)) {
        g_seen_key = (int)ld(&g_keyw);
        __atomic_store_n(&g_lockw, 0, __ATOMIC_SEQ_CST);
    }
    kern_test_cv_hook = NULL;
}

static void cv_order_waiter(Cpu *c) {
    LockJob *j = arg_p(c, 0);
    user_lock(c, j->lock, j->tag);
    j->r = sv(c, 0x1C, ga(j->lock), ga(j->key), j->tag, 200000000);
    if (!j->r) user_unlock(c, j->lock, j->tag);
    exit_thread(c);
}

static uint32_t cv_order_case(Cpu *c, int old) {
    LockJob j = {.lock = &g_lockw, .key = &g_keyw};
    g_lockw = 0;
    g_keyw = 0;
    g_seen_key = -1;
    kern_test_cv_old_order = old;
    kern_test_cv_hook = cv_signaller;
    uint32_t h = create(c, a_cv_order_waiter, &j, 44, -2);
    j.tag = h;
    start(c, h);
    while (kern_thread_state(h) != KERN_WAIT_CONDVAR && kern_thread_state(h) != KERN_THREAD_EXITED) sleep_ms(c, 1);
    if (g_seen_key != 0) sv(c, 0x1D, ga(&g_keyw), 1, 0, 0);   // what the SDK's Signal does
    join(c, h);
    kern_test_cv_old_order = 0;
    kern_test_cv_hook = NULL;
    return j.r;
}

static void test_cv_key_order(Cpu *c) {
    uint32_t r = cv_order_case(c, 0);
    int seen = g_seen_key;
    printf("condvar key order: key set before the lock release; a signaller between release and wait saw key %d, "
           "waiter result %#x: %s\n", seen, r, seen == 1 && r == 0 ? "ok" : "FAIL");
    CHECK(seen == 1 && r == 0, "condvar key order");
    r = cv_order_case(c, 1);
    seen = g_seen_key;
    int lost = seen == 0 && r == KERN_R_TIMEOUT;
    printf("condvar key order, negative control (MNG's old order, release then set the key): signaller saw key %d, "
           "waiter result %#x: %s\n", seen, r, lost ? "lost wakeup, fails as expected" : "FAIL (the control did not "
           "fail)");
    CHECK(lost, "the old-order negative control did not lose the wakeup");
}

// ---- address arbiter
typedef struct { int type, value, idx; int64_t timeout; uint32_t r; } ArbJob;
static volatile uint32_t g_arb __attribute__((aligned(8)));

static void arb_waiter(Cpu *c) {
    ArbJob *j = arg_p(c, 0);
    j->r = sv(c, 0x34, ga(&g_arb), (uint64_t)j->type, (uint64_t)(int64_t)j->value, (uint64_t)j->timeout);
    exit_thread(c);
}

static uint32_t arb_spawn(Cpu *c, ArbJob *j) {
    uint32_t h = create(c, a_arb_waiter, j, 44, -2);
    start(c, h);
    wait_state(c, h, KERN_WAIT_ADDRESS);
    return h;
}

static uint32_t sig(Cpu *c, int type, int value, int count) {
    return sv(c, 0x35, ga(&g_arb), (uint64_t)type, (uint64_t)(int64_t)value, (uint64_t)(int64_t)count);
}

static void test_arbiter(Cpu *c) {
    int before = failures;
    uint64_t arb = ga(&g_arb);
    // wait type 0, if less than
    g_arb = 0;
    CHECK(sv(c, 0x34, arb, 0, 0, (uint64_t)-1) == KERN_R_BAD_STATE, "wait type 0 with value not less");
    CHECK(sv(c, 0x34, arb, 0, 1, 0) == KERN_R_TIMEOUT, "wait type 0 with timeout 0");
    ArbJob j0 = {.type = 0, .value = 1, .timeout = -1};
    uint32_t h = arb_spawn(c, &j0);
    CHECK(sig(c, 0, 0, 1) == 0, "signal type 0");
    join(c, h);
    CHECK(j0.r == 0, "wait type 0 woken by signal type 0: %#x", j0.r);
    // wait type 1, decrement if less than; signal type 1, increment if equal
    g_arb = 0;
    ArbJob j1 = {.type = 1, .value = 1, .timeout = -1};
    h = arb_spawn(c, &j1);
    CHECK((int32_t)g_arb == -1, "wait type 1 did not decrement (%d)", (int32_t)g_arb);
    CHECK(sig(c, 1, 0, 1) == KERN_R_BAD_STATE && (int32_t)g_arb == -1 && kern_thread_state(h) == KERN_WAIT_ADDRESS,
          "signal type 1 with a different value");
    CHECK(sig(c, 1, -1, 1) == 0 && g_arb == 0, "signal type 1 did not increment (%d)", (int32_t)g_arb);
    join(c, h);
    CHECK(j1.r == 0, "wait type 1 woken by signal type 1: %#x", j1.r);
    // wait type 2, if equal; signal type 2, modify by the number of waiters if equal
    g_arb = 5;
    CHECK(sv(c, 0x34, arb, 2, 6, (uint64_t)-1) == KERN_R_BAD_STATE, "wait type 2 with a different value");
    ArbJob ja = {.type = 2, .value = 5, .timeout = -1}, jb = ja;
    uint32_t ha = arb_spawn(c, &ja), hb = arb_spawn(c, &jb);
    CHECK(sig(c, 2, 5, 1) == 0 && g_arb == 5, "signal type 2, 2 waiters, count 1: value %d (want 5)", (int32_t)g_arb);
    wait_state(c, ha, KERN_THREAD_EXITED);
    CHECK(kern_thread_state(hb) == KERN_WAIT_ADDRESS, "signal type 2 woke more than one, or not the first");
    CHECK(sig(c, 2, 5, 1) == 0 && g_arb == 4, "signal type 2, 1 waiter, count 1: value %d (want 4)", (int32_t)g_arb);
    join(c, ha);
    join(c, hb);
    CHECK(sig(c, 2, 4, 1) == 0 && g_arb == 5, "signal type 2, no waiter: value %d (want 5)", (int32_t)g_arb);
    CHECK(sig(c, 2, 5, 0) == 0 && g_arb == 6, "signal type 2, no waiter, count 0: value %d (want 6)", (int32_t)g_arb);
    ArbJob jc = {.type = 2, .value = 6, .timeout = -1};
    h = arb_spawn(c, &jc);
    CHECK(sig(c, 2, 6, -1) == 0 && g_arb == 5, "signal type 2, 1 waiter, count -1: value %d (want 5)", (int32_t)g_arb);
    join(c, h);
    CHECK(ja.r == 0 && jb.r == 0 && jc.r == 0, "wait type 2 results %#x %#x %#x", ja.r, jb.r, jc.r);
    CHECK(sig(c, 2, 7, 1) == KERN_R_BAD_STATE, "signal type 2 with a different value");
    // timeout, invalid type
    uint64_t t0 = kern_now_ns();
    uint32_t r = sv(c, 0x34, arb, 2, 5, 5000000);
    double ms = ms_since(t0);
    CHECK(r == KERN_R_TIMEOUT && ms >= 5.0, "wait type 2 with a 5 ms timeout: %#x after %.2f ms", r, ms);
    CHECK(sv(c, 0x34, arb, 3, 0, 0) == KERN_R_INVALID_ENUM && sig(c, 3, 0, 0) == KERN_R_INVALID_ENUM, "type 3");
    printf("address arbiter: wait types 0-2 (less, decrement-less, equal), signal types 0-2 (wake, increment, modify by "
           "waiters), 5 ms timeout after %.2f ms: %s\n", ms, failures == before ? "ok" : "FAIL");
}

// ---- events
typedef struct { uint32_t h, r, idx; int64_t timeout; volatile int delay, go; } EvJob;

static void event_waiter(Cpu *c) {
    EvJob *j = arg_p(c, 0);
    while (j->delay && !j->go) sleep_ms(c, 1);
    uint32_t hs[1] = {j->h};
    j->r = sv(c, 0x18, 0, ga(hs), 1, (uint64_t)j->timeout);
    j->idx = (uint32_t)c->x[1];
    exit_thread(c);
}

static void test_events(Cpu *c) {
    int before = failures;
    CHECK(sv(c, 0x45, 0, 0, 0, 0) == 0, "CreateEvent");
    uint32_t hw = (uint32_t)c->x[1], hr = (uint32_t)c->x[2];
    uint64_t t0 = kern_now_ns();
    uint32_t r = wait1(c, hr, 10000000);
    double ms = ms_since(t0);
    CHECK(r == KERN_R_TIMEOUT && ms >= 10.0, "wait with a 10 ms timeout: %#x after %.2f ms", r, ms);
    CHECK(sv(c, 0x17, hr, 0, 0, 0) == KERN_R_BAD_STATE, "ResetSignal of an unsignalled event");

    EvJob e = {.h = hr, .timeout = -1};
    uint32_t h = create(c, a_event_waiter, &e, 44, -2);
    start(c, h);
    wait_state(c, h, KERN_WAIT_SYNC);
    CHECK(sv(c, 0x11, hw, 0, 0, 0) == 0, "SignalEvent");
    join(c, h);
    CHECK(e.r == 0 && e.idx == 0, "waiter woken by SignalEvent: %#x index %u", e.r, e.idx);

    CHECK(sv(c, 0x45, 0, 0, 0, 0) == 0, "CreateEvent");
    uint32_t hw2 = (uint32_t)c->x[1], hr2 = (uint32_t)c->x[2];
    uint32_t two[2] = {hr2, hr};
    CHECK(sv(c, 0x18, 0, ga(two), 2, 0) == 0 && c->x[1] == 1, "wait on two handles, the second signalled");
    CHECK(wait1(c, hw, 0) == KERN_R_BAD_HANDLE, "wait on a writable event handle");
    CHECK(sv(c, 0x12, hr, 0, 0, 0) == 0 && wait1(c, hr, 0) == KERN_R_TIMEOUT, "ClearEvent");
    CHECK(sv(c, 0x11, hr, 0, 0, 0) == KERN_R_BAD_HANDLE, "SignalEvent on the readable end");
    CHECK(sv(c, 0x11, hw, 0, 0, 0) == 0 && sv(c, 0x17, hr, 0, 0, 0) == 0 && sv(c, 0x17, hr, 0, 0, 0) == KERN_R_BAD_STATE,
          "ResetSignal");

    EvJob f = {.h = hr, .timeout = -1};
    h = create(c, a_event_waiter, &f, 44, -2);
    start(c, h);
    wait_state(c, h, KERN_WAIT_SYNC);
    CHECK(sv(c, 0x19, h, 0, 0, 0) == 0, "CancelSynchronization");
    join(c, h);
    CHECK(f.r == KERN_R_CANCELLED, "cancelled wait: %#x", f.r);

    EvJob g = {.h = hr, .timeout = -1, .delay = 1};
    h = create(c, a_event_waiter, &g, 44, -2);
    start(c, h);
    CHECK(sv(c, 0x19, h, 0, 0, 0) == 0, "CancelSynchronization before the wait");
    g.go = 1;
    join(c, h);
    CHECK(g.r == KERN_R_CANCELLED, "wait after a cancel: %#x", g.r);

    uint32_t sw = kern_event_new(), sr = kern_event_readable(sw);
    CHECK(wait1(c, sr, 0) == KERN_R_TIMEOUT, "service event before the signal");
    kern_event_signal(sw);
    CHECK(wait1(c, sr, 0) == 0, "service event after kern_event_signal");
    uint32_t handles[] = {sr, sw, hr2, hw2, hr, hw};
    for (unsigned i = 0; i < 6; i++) CHECK(sv(c, 0x16, handles[i], 0, 0, 0) == 0, "CloseHandle %#x", handles[i]);
    CHECK(sv(c, 0x16, hr, 0, 0, 0) == KERN_R_BAD_HANDLE, "CloseHandle twice");
    printf("events: signal, clear, reset, two handles, cancel while waiting and before, service events, 10 ms "
           "timeout after %.2f ms: %s\n", ms, failures == before ? "ok" : "FAIL");
}

// ---- virtual cores: count the threads running guest code at once, and that no two of them share a core number
static volatile int32_t g_running, g_run_max, g_core_use[3], g_core_clash;

static void core_worker(Cpu *c) {
    for (int i = 0; i < CORE_RUNS; i++) {
        uint32_t core = sv(c, 0x10, 0, 0, 0, 0);
        if (core > 2 || plat_inc32(&g_core_use[core > 2 ? 0 : core]) != 1) plat_inc32(&g_core_clash);
        long n = plat_inc32(&g_running), m;
        while (n > (m = g_run_max) && plat_cas32(&g_run_max, n, m) != m) {}
        uint64_t until = kern_now_ns() + 20000;
        while (kern_now_ns() < until) {}
        plat_dec32(&g_running);
        plat_dec32(&g_core_use[core > 2 ? 0 : core]);
        sv(c, 0x0B, i % 8 == 7 ? 50000 : 0, 0, 0, 0);
    }
    exit_thread(c);
}

static void test_cores(Cpu *c) {
    uint32_t h[CORE_WORKERS];
    g_running = g_run_max = g_core_clash = 0;
    kern_core_stats_reset();
    for (int i = 0; i < CORE_WORKERS; i++) {
        // Created with the process's ideal core, a thread may run on core 0 alone, as on Horizon; the game widens a
        // thread's mask with SetThreadCoreMask, and so does this test, to cores 0-2 (ideal core kept: -3).
        h[i] = create(c, a_core_worker, NULL, 44, -2);
        CHECK(sv(c, 0x0E, 0, 0, h[i], 0) == 0 && c->x[1] == 0 && c->x[2] == 1, "a new thread's core mask is %#llx, "
              "core %lld, not core 0 alone", (unsigned long long)c->x[2], (long long)(int32_t)c->x[1]);
        CHECK(sv(c, 0x0F, h[i], (uint64_t)(int64_t)-3, 7, 0) == 0, "SetThreadCoreMask %#x", h[i]);
        start(c, h[i]);
    }
    for (int i = 0; i < CORE_WORKERS; i++) join(c, h[i]);
    int now, held_max;
    kern_core_stats(&now, &held_max);
    uint32_t cpus = plat_cpu_usable();
    int gate = !cfg_bool("TOTK_FREE_CORES");   // the kernel's own switch
    int ok = g_run_max <= 3 && held_max <= 3 && !g_core_clash && (cpus < 4 || g_run_max == 3);
    printf("virtual cores: %d threads x %d runs on %u host CPUs: at most %d running guest code at once, at most %d "
           "core holders (kernel count), %d core-number clashes: %s\n", CORE_WORKERS, CORE_RUNS, cpus, g_run_max,
           held_max, g_core_clash, !gate ? "not checked (TOTK_FREE_CORES is set)" : ok ? "ok" : "FAIL");
    CHECK(ok || !gate, "virtual cores");
}

// ---- directed core hand-off (kernel.c, TOTK_SCHED_HANDOFF): threads on core 3 alone. A holder L (priority 50) hands
// a mutex to a waiter H (40). Case 1: L releases it with ArbitrateUnlock, then runs host code without an SVC for
// 40 ms, where nothing stops it (the asynchronous preemption only interrupts lifted code); with the hand-off H gets the
// core as the SVC returns, without it at L's next SVC, 40 ms later. Case 2: L releases it inside
// WaitProcessWideKeyAtomic, which blocks L, while X (55) waits for the core at the end of a SleepThread; with the
// hand-off the core goes to H, without it to X, which returns from the SVC and runs host code 40 ms before it exits.
typedef struct { volatile uint32_t *lock, *key; uint32_t tag; int mode; } HoJob;
static volatile uint32_t g_hol __attribute__((aligned(8))), g_hok __attribute__((aligned(8)));
static volatile uint32_t g_ho_h;   // H's handle, for L to watch
static volatile uint64_t g_ho_t0, g_ho_at, g_ho_xs;   // L's release, H's acquisition, X's sleep start
static uint64_t a_ho_holder, a_ho_waiter, a_ho_busy;

#define HO_XSLEEP_NS 100000000ll

static void busy_ns(uint64_t ns) {
    uint64_t until = kern_now_ns() + ns;
    while (kern_now_ns() < until) plat_pause();
}

static void ho_holder(Cpu *c) {
    HoJob *j = arg_p(c, 0);
    user_lock(c, j->lock, j->tag);
    while (!g_ho_h || kern_thread_state(g_ho_h) != KERN_WAIT_LOCK) sleep_ms(c, 1);   // the core is free meanwhile
    if (j->mode == 2)   // holding the core until 10 ms after X's sleep has ended: X waits for it in SleepThread
        while (kern_now_ns() < g_ho_xs + HO_XSLEEP_NS + 10000000) plat_pause();
    g_ho_t0 = kern_now_ns();
    if (j->mode == 1) user_unlock(c, j->lock, j->tag);
    else sv(c, 0x1C, ga(j->lock), ga(j->key), j->tag, 50000000);   // times out: no one signals the key
    busy_ns(40000000);
    exit_thread(c);
}

static void ho_waiter(Cpu *c) {
    HoJob *j = arg_p(c, 0);
    user_lock(c, j->lock, j->tag);
    g_ho_at = kern_now_ns();
    user_unlock(c, j->lock, j->tag);
    exit_thread(c);
}

static void ho_busy(Cpu *c) {
    g_ho_xs = kern_now_ns();
    sv(c, 0x0B, HO_XSLEEP_NS, 0, 0, 0);
    busy_ns(40000000);
    exit_thread(c);
}

static uint32_t on_core3(Cpu *c, uint64_t entry, void *arg, int prio) {
    uint32_t h = create(c, entry, arg, prio, -2);
    CHECK(sv(c, 0x0F, h, 3, 1u << 3, 0) == 0, "SetThreadCoreMask %#x to core 3", h);
    return h;
}

static double ho_case(Cpu *c, int mode) {
    g_hol = g_hok = 0;
    g_ho_h = 0;
    g_ho_t0 = g_ho_at = g_ho_xs = 0;
    uint32_t hx = 0;
    if (mode == 2) {   // X first, so that it is asleep before L takes the core
        hx = on_core3(c, a_ho_busy, NULL, 55);
        start(c, hx);
        while (!g_ho_xs) sleep_ms(c, 1);
    }
    HoJob jl = {.lock = &g_hol, .key = &g_hok, .mode = mode}, jh = jl;
    uint32_t hl = on_core3(c, a_ho_holder, &jl, 50), hh = on_core3(c, a_ho_waiter, &jh, 40);
    jl.tag = hl;
    jh.tag = hh;
    start(c, hl);
    while (ld(&g_hol) != hl) sleep_ms(c, 1);
    g_ho_h = hh;
    start(c, hh);
    join(c, hh);
    join(c, hl);
    if (hx) join(c, hx);
    return g_ho_at > g_ho_t0 ? (double)(g_ho_at - g_ho_t0) / 1e6 : -1;
}

static void test_handoff(Cpu *c) {
    int on = __atomic_load_n(&kern_sched_handoff, __ATOMIC_RELAXED);
    double d1 = ho_case(c, 1), d2 = ho_case(c, 2);
    int ok = d1 >= 0 && d2 >= 0 && (!on || (d1 < 15 && d2 < 15));
    printf("core hand-off (%s): a better waiter got a released mutex's core after %.3f ms (ArbitrateUnlock, holder "
           "busy 40 ms after it) and %.3f ms (WaitProcessWideKeyAtomic, a worse thread queued for the core): %s\n",
           on ? "TOTK_SCHED_HANDOFF on, want under 15 ms" : "off, about 40 ms expected", d1, d2,
           !ok ? "FAIL" : on ? "ok" : "ok (not checked)");
    CHECK(ok, "core hand-off");
}

// ---- fibers: two fibers switch to each other; each checks its callee-saved registers, sp and tpidr survive
typedef struct { uint64_t self, other; int id; } FiberJob;
static volatile int32_t g_switches, g_runs[3];
static volatile int g_last_fiber;
static long g_fiber_bad, g_fiber_turns;

static void set_regs(Cpu *c, uint64_t seed, uint64_t *save, __uint128_t *vsave) {
    for (int r = 19; r <= 28; r++) save[r - 19] = c->x[r] = seed * 0x9E3779B97F4A7C15ull + (uint64_t)r;
    for (int r = 8; r <= 15; r++)
        vsave[r - 8] = c->v[r] = ((__uint128_t)(seed ^ 0xABCDull) << 64) | (seed * 31u + (uint64_t)r);
}

static int same_regs(const Cpu *c, const uint64_t *save, const __uint128_t *vsave) {
    for (int r = 19; r <= 28; r++)
        if (c->x[r] != save[r - 19]) return 0;
    for (int r = 8; r <= 15; r++)
        if (c->v[r] != vsave[r - 8]) return 0;
    return 1;
}

static void fiber_fn(Cpu *c) {
    FiberJob *j = arg_p(c, 0);
    uint64_t save[10];
    __uint128_t vsave[8];
    for (;;) {
        long n = g_switches;
        if (n >= FIBER_SWITCHES) {
            ret_x(c, 0);   // back to the thread
            return;
        }
        g_switches = n + 1;
        g_runs[j->id]++;
        set_regs(c, (uint64_t)n * 2 + (uint64_t)j->id, save, vsave);
        uint64_t sp = c->sp, tp = c->tpidr, tlr = c->tpidrro;
        g_last_fiber = j->id;
        rt_call(c, a_fib_switch, 1, j->other);
        if (g_last_fiber != 3 - j->id) g_fiber_turns++;   // the other fiber did not run in between
        if (!same_regs(c, save, vsave) || c->sp != sp || c->tpidr != tp || c->tpidrro != tlr) g_fiber_bad++;
        if (rt_call(c, a_fib_cur, 0) != j->self) g_fiber_bad++;
    }
}

static void test_fibers(Cpu *c) {
    uint64_t fa = ga(mem_kernel_alloc(0x100, 16)), fb = ga(mem_kernel_alloc(0x100, 16));
    uint64_t sa = ga(mem_kernel_alloc(STACK, 16)), sb = ga(mem_kernel_alloc(STACK, 16));
    FiberJob ja = {fa, fb, 1}, jb = {fb, fa, 2};
    rt_call(c, a_fib_init, 6, fa, a_fiber_fn, ga(&ja), sa, (uint64_t)STACK, (uint64_t)0);
    rt_call(c, a_fib_init, 6, fb, a_fiber_fn, ga(&jb), sb, (uint64_t)STACK, (uint64_t)0);
    g_switches = g_runs[1] = g_runs[2] = 0;
    g_fiber_bad = g_fiber_turns = 0;
    uint64_t save[10];
    __uint128_t vsave[8];
    set_regs(c, 0x5555, save, vsave);
    uint64_t sp = c->sp, tp = c->tpidr;
    uint64_t t0 = kern_now_ns();
    rt_call(c, a_fib_switch, 1, fa);
    double ms = ms_since(t0);
    int base_ok = same_regs(c, save, vsave) && c->sp == sp && c->tpidr == tp && rt_call(c, a_fib_cur, 0) == 0;
    rt_call(c, a_fib_fin, 1, fa);
    rt_call(c, a_fib_fin, 1, fb);
    int ok = g_switches == FIBER_SWITCHES && g_runs[1] == FIBER_SWITCHES / 2 && g_runs[2] == FIBER_SWITCHES / 2 &&
             !g_fiber_turns && !g_fiber_bad && base_ok;
    printf("fibers: %d switches (fiber 1 %d, fiber 2 %d, %ld out of turn) in %.1f ms (%.3f us each), %ld register "
           "mismatches, thread context %s: %s\n", g_switches, g_runs[1], g_runs[2], g_fiber_turns, ms,
           ms * 1000.0 / (double)(g_switches ? g_switches : 1), g_fiber_bad, base_ok ? "preserved" : "CHANGED",
           ok ? "ok" : "FAIL");
    CHECK(ok, "fiber ping-pong");
}

// ---- ExitThread unwinding: from three nested host calls deep, and from a fiber
typedef struct { int stage, depth, bad; uint64_t fiber, stack; } ExitJob;

static void exit_nested(Cpu *c) {
    ExitJob *j = arg_p(c, 0);
    if (++j->depth < 3) {
        rt_call(c, a_exit_nested, 1, ga(j));
        j->bad = 1;
        return;
    }
    j->stage = 2;
    sv(c, 0x0A, 0, 0, 0, 0);
    j->bad = 2;
}

static void exit_entry(Cpu *c) {
    ExitJob *j = arg_p(c, 0);
    j->stage = 1;
    rt_call(c, a_exit_nested, 1, ga(j));
    j->bad = 3;
    exit_thread(c);
}

static void fiber_exit_fn(Cpu *c) {
    ExitJob *j = arg_p(c, 0);
    j->stage = 3;
    sv(c, 0x0A, 0, 0, 0, 0);
    j->bad = 4;
}

static void fiber_exit_thread(Cpu *c) {
    ExitJob *j = arg_p(c, 0);
    rt_call(c, a_fib_init, 6, j->fiber, a_fiber_exit_fn, ga(j), j->stack, (uint64_t)STACK, (uint64_t)0);
    rt_call(c, a_fib_switch, 1, j->fiber);
    j->bad = 5;
    exit_thread(c);
}

static void test_exit(Cpu *c) {
    ExitJob a = {0};
    uint32_t h = create(c, a_exit_entry, &a, 44, -2);
    start(c, h);
    join(c, h);
    CHECK(wait1(c, h, 0) == KERN_R_BAD_HANDLE, "wait on a closed thread handle");
    ExitJob b = {.fiber = ga(mem_kernel_alloc(0x100, 16)), .stack = ga(mem_kernel_alloc(STACK, 16))};
    h = create(c, a_fiber_exit_thread, &b, 44, -2);
    start(c, h);
    join(c, h);
    rt_call(c, a_fib_fin, 1, b.fiber);
    int ok = a.stage == 2 && a.depth == 3 && !a.bad && b.stage == 3 && !b.bad;
    printf("ExitThread: unwound from %d nested host calls (stage %d) and from a fiber (stage %d), nothing ran after "
           "it: %s\n", a.depth, a.stage, b.stage, ok ? "ok" : "FAIL");
    CHECK(ok, "ExitThread unwinding (bad %d %d)", a.bad, b.bad);
}

// ---- lifted code: main's PLT stub for nn::os::GetSystemTick -> lifted sdk -> rt_ticks
typedef struct { uint64_t first, last; long bad; } TickJob;

static void tick_worker(Cpu *c) {
    TickJob *j = arg_p(c, 0);
    uint64_t prev = 0;
    for (int i = 0; i < TICK_CALLS; i++) {
        uint64_t t = rt_call(c, tick_stub, 0);
        if (!t || t < prev) j->bad++;
        if (!i) j->first = t;
        prev = t;
    }
    j->last = prev;
    exit_thread(c);
}

static void test_lifted_ticks(Cpu *c) {
    TickJob j[TICK_THREADS] = {0};
    uint32_t h[TICK_THREADS];
    uint64_t calls0 = hostfn_calls(), before = rt_ticks();
    for (int i = 0; i < TICK_THREADS; i++) h[i] = create(c, a_tick_worker, &j[i], 44, -2);
    for (int i = 0; i < TICK_THREADS; i++) start(c, h[i]);
    for (int i = 0; i < TICK_THREADS; i++) join(c, h[i]);
    uint64_t after = rt_ticks(), calls = hostfn_calls() - calls0;
    char stub[128], def[128];
    printf("lifted GetSystemTick: %d guest threads x %d calls of %s -> %s:\n", TICK_THREADS, TICK_CALLS,
           ldr_describe(tick_stub, stub, sizeof stub), ldr_describe(ldr_symbol(TICK_SYMBOL), def, sizeof def));
    int ok = 1;
    for (int i = 0; i < TICK_THREADS; i++) {
        int in = j[i].first >= before && j[i].last <= after && j[i].last > j[i].first;
        printf("  thread %d: ticks %llu .. %llu, %ld out of order, inside the host's rt_ticks window: %s\n", i,
               (unsigned long long)j[i].first, (unsigned long long)j[i].last, j[i].bad, in ? "yes" : "NO");
        ok &= !j[i].bad && in;
    }
    // Host functions entered by dispatch meanwhile: only the four thread entries, so no call reached a host override.
    int lifted = calls == TICK_THREADS;
    printf("  monotonic per thread: %s; host-function calls meanwhile %llu (the %d thread entries): %s\n",
           ok ? "ok" : "FAIL", (unsigned long long)calls, TICK_THREADS, lifted ? "all ticks came from lifted code"
                                                                                : "FAIL");
    CHECK(ok && lifted, "lifted GetSystemTick");
}

// svc 0x07 ExitProcess: the path up to the final ExitProcess (logged, pipeline cache saved, logs flushed) runs with the
// test hook in its place, which must see exit code 0; the SVC then returns to the probe.
static volatile int32_t exit_calls;
static volatile unsigned exit_code = 99;
static void exit_hook(unsigned code) { exit_code = code, exit_calls++; }
static void test_exit_process(Cpu *c) {
    svc_test_exit_hook = exit_hook;
    sv(c, 0x07, 0, 0, 0, 0);
    svc_test_exit_hook = NULL;
    CHECK(exit_calls == 1 && exit_code == 0, "ExitProcess reached the exit %d times with code %u", exit_calls, exit_code);
    printf("ExitProcess: exit path reached once with code %u\n", exit_code);
}

static void probe_main(Cpu *c) {
    Cpu *again;
    uint32_t me = kern_main_thread(&again);
    uint64_t mb, ms;
    kern_main_stack(&mb, &ms);
    printf("main thread %#x: prio 44 core 0, guest stack %#llx+%#llx, sp %#llx, TLR %#llx (+0x1F8 -> %#llx)\n", me,
           (unsigned long long)mb, (unsigned long long)ms, (unsigned long long)c->sp, (unsigned long long)c->tpidrro,
           (unsigned long long)LD64(c->tpidrro + 0x1F8));
    CHECK(again == c && c->sp == mb + ms && LD64(c->tpidrro + 0x1F8) == c->tpidrro + 0x200, "main thread set-up");
    CHECK(sv(c, 0x25, 0, KERN_H_CURRENT_THREAD, 0, 0) == 0 && c->x[1] == 0x51, "GetThreadId(current) %#llx",
          (unsigned long long)c->x[1]);
    test_lock(c, me);
    test_cv_wake_order(c);
    test_cv_key_order(c);
    test_arbiter(c);
    test_events(c);
    test_cores(c);
    test_handoff(c);
    test_fibers(c);
    test_exit(c);
    test_lifted_ticks(c);
    test_exit_process(c);
}

// ---- platform layer (platform/win32.c): a thread handing values over a condition variable, and plat_sleep_ns(1 ms)
// overshoot (median of 21 under 0.5 ms; frame pacing depends on it).
static PlatMutex g_pm = PLAT_MUTEX_INIT;
static PlatCond g_pc = PLAT_COND_INIT;
static int g_pv;

static void plat_worker(void *arg) {
    plat_mutex_lock(&g_pm);
    g_pv = (int)(intptr_t)arg;
    plat_cond_broadcast(&g_pc);
    while (g_pv != 2) plat_cond_wait(&g_pc, &g_pm);
    g_pv = 3;
    plat_mutex_unlock(&g_pm);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// ---- costs the kernel's scheduling pays, printed only (no pass or fail; the numbers depend on the machine): a clock
// read (kernel.c's core wait counters take a few per wait), a stop and resume of a running thread as the asynchronous
// preemption does it (kernel.c ap_try: plat_intr_stop, plat_intr_resume, without and with a context write), and a
// wake-up through a condition variable to a thread blocked on it (kernel.c wake to the waiter's return from block).
static volatile int g_spin_stop, g_pp_turn;
static PlatIntr *volatile g_spin_intr;
static PlatMutex g_ppm = PLAT_MUTEX_INIT;
static PlatCond g_ppc[2] = {PLAT_COND_INIT, PLAT_COND_INIT};
static uint64_t g_pp_sent;
static uint64_t g_pp_lat[400];

static void bench_spinner(void *arg) {
    (void)arg;
    g_spin_intr = plat_intr_self();
    while (!__atomic_load_n(&g_spin_stop, __ATOMIC_RELAXED)) plat_pause();
}

static void bench_ponger(void *arg) {   // side 1: waits for its turn, notes the wake latency, gives the turn back
    (void)arg;
    plat_mutex_lock(&g_ppm);
    for (int i = 0; i < 400; i++) {
        while (g_pp_turn != 1) plat_cond_wait(&g_ppc[1], &g_ppm);
        g_pp_lat[i] = plat_now_ns() - g_pp_sent;
        g_pp_turn = 0;
        plat_cond_signal(&g_ppc[0]);
    }
    plat_mutex_unlock(&g_ppm);
}

static void bench_sched(void) {
    enum { CLK = 1000000, STOPS = 400 };
    uint64_t t0 = plat_now_ns(), sink = 0;
    for (int i = 0; i < CLK; i++) sink += plat_now_ns();
    double clk = (double)(plat_now_ns() - t0) / CLK;
    g_spin_stop = 0;
    PlatThread *s = plat_thread_new(bench_spinner, NULL, "probe-kernel spinner");
    while (s && !g_spin_intr) plat_yield();
    static uint64_t a[STOPS], b[STOPS];
    int ok = s && g_spin_intr;
    for (int i = 0; ok && i < STOPS; i++) {
        uint64_t rip, rsp, x = plat_now_ns();
        ok = plat_intr_stop(g_spin_intr, &rip, &rsp);
        if (ok) plat_intr_resume(g_spin_intr, 0);
        uint64_t y = plat_now_ns();
        if (ok) ok = plat_intr_stop(g_spin_intr, &rip, &rsp);
        if (ok) plat_intr_resume(g_spin_intr, rip);   // a context write sending it back where it was
        a[i] = y - x, b[i] = plat_now_ns() - y;
        plat_sleep_ns(20000);
    }
    g_spin_stop = 1;
    if (s) plat_thread_join(s);
    if (g_spin_intr) plat_intr_free(g_spin_intr);
    if (ok) {
        qsort(a, STOPS, sizeof *a, cmp_u64);
        qsort(b, STOPS, sizeof *b, cmp_u64);
    }
    PlatThread *p = plat_thread_new(bench_ponger, NULL, "probe-kernel ponger");
    plat_mutex_lock(&g_ppm);
    for (int i = 0; p && i < 400; i++) {
        plat_mutex_unlock(&g_ppm);
        plat_sleep_ns(i & 1 ? 0 : 50000);   // half the wake-ups find the waiter asleep a while, half just after
        plat_mutex_lock(&g_ppm);
        g_pp_sent = plat_now_ns();
        g_pp_turn = 1;
        plat_cond_signal(&g_ppc[1]);
        while (g_pp_turn != 0) plat_cond_wait(&g_ppc[0], &g_ppm);
    }
    plat_mutex_unlock(&g_ppm);
    if (p) {
        plat_thread_join(p);
        qsort(g_pp_lat, 400, sizeof *g_pp_lat, cmp_u64);
    }
    printf("  scheduling costs: clock read %.1f ns; stop and resume of a running thread %s median %.1f us p90 %.1f max "
           "%.1f, with a context write median %.1f us p90 %.1f max %.1f; condition variable wake-up median %.1f us p90 "
           "%.1f max %.1f (%llu)\n", clk, ok ? "" : "(failed)", a[STOPS / 2] / 1e3, a[STOPS * 9 / 10] / 1e3,
           a[STOPS - 1] / 1e3, b[STOPS / 2] / 1e3, b[STOPS * 9 / 10] / 1e3, b[STOPS - 1] / 1e3, g_pp_lat[200] / 1e3,
           g_pp_lat[360] / 1e3, g_pp_lat[399] / 1e3, (unsigned long long)(sink & 1));
}

static void test_platform(void) {
    PlatThread *t = plat_thread_new(plat_worker, (void *)(intptr_t)1, "probe-kernel plat");
    CHECK(t, "plat_thread_new");
    if (t) {
        plat_mutex_lock(&g_pm);
        int timed_out = 0;
        while (g_pv != 1 && !timed_out) timed_out = !plat_cond_wait_ns(&g_pc, &g_pm, 5000000000ull);
        CHECK(g_pv == 1, "plat thread did not start (%d)", g_pv);
        g_pv = 2;
        plat_cond_broadcast(&g_pc);
        plat_mutex_unlock(&g_pm);
        plat_thread_join(t);
        CHECK(g_pv == 3, "plat thread did not finish (%d)", g_pv);
    }
    PlatMutex m = PLAT_MUTEX_INIT;
    PlatCond c = PLAT_COND_INIT;
    plat_mutex_lock(&m);
    uint64_t t0 = plat_now_ns();
    CHECK(plat_cond_wait_ns(&c, &m, 2000000) == 0 && plat_now_ns() - t0 >= 1000000, "plat_cond_wait_ns timeout");
    plat_mutex_unlock(&m);
    CHECK(plat_mutex_trylock(&m), "plat_mutex_trylock");
    plat_mutex_unlock(&m);

    enum { N = 21 };
    uint64_t over[N];
    for (int i = 0; i < N; i++) {
        uint64_t a = plat_now_ns();
        plat_sleep_ns(1000000);
        uint64_t d = plat_now_ns() - a;
        CHECK(d >= 1000000, "plat_sleep_ns(1 ms) returned after %llu ns", (unsigned long long)d);
        over[i] = d > 1000000 ? d - 1000000 : 0;
    }
    qsort(over, N, sizeof *over, cmp_u64);
    printf("  plat_sleep_ns(1 ms) overshoot: median %.3f ms, max %.3f ms\n", over[N / 2] / 1e6, over[N - 1] / 1e6);
    CHECK(over[N / 2] < 500000, "plat_sleep_ns(1 ms) median overshoot %.3f ms (want < 0.5)", over[N / 2] / 1e6);
    bench_sched();
}

// ---- a redirect of a thread that faults in a loop (kernel.c ap_try against the lazy heap's first touches), plus the
// synchronous fault-handler redirect hook (platform/win32.c plat_mem_fault_ret_hook). A host thread writes every 64th
// byte of a 64 MiB guest heap that SetHeapSize leaves reserved under TOTK_HEAP_LAZY, so each chunk faults into
// fault_veh, then shrinks the heap to nothing and starts again. Another thread stops it as ap_try does, and when its
// rip is in the loop sends it to a stub that counts its arrival, clears the pending flag and jumps back. A redirect still
// pending after RF_LOST_NS is lost (the stub never ran). Two async rounds (redirecting every stop vs refusing stops
// above 1, TOTK_AP_SAFE), followed by a synchronous pass with plat_mem_fault_ret_hook registered. x86-64 Windows only.
#if defined(_WIN32) && defined(__x86_64__)
#define RF_HEAP (64ull << 20)
#define RF_STOPS 6000
#define RF_LOST_NS 50000000ull
void probe_rf_touch(uint8_t *p, uint64_t n);
void probe_rf_touch_end(void);
void probe_rf_stub(void);
volatile uint64_t probe_rf_ret, probe_rf_taken;
volatile int probe_rf_pending;
__asm__(".text\n"
        ".globl probe_rf_touch\n"
        ".globl probe_rf_touch_end\n"
        ".globl probe_rf_stub\n"
        ".p2align 4\n"
        "probe_rf_touch:\n"   // rcx: p, rdx: n
        "    xorl %eax, %eax\n"
        "1:  movq %rax, (%rcx,%rax)\n"
        "    addq $64, %rax\n"
        "    cmpq %rdx, %rax\n"
        "    jb 1b\n"
        "    ret\n"
        "probe_rf_touch_end:\n"
        "    nop\n"
        ".p2align 4\n"
        "probe_rf_stub:\n"   // as kernel.c kern_ap_stub: the flags are the loop's, the return slot is reserved first
        "    leaq -8(%rsp), %rsp\n"
        "    pushfq\n"
        "    pushq %rax\n"
        "    movq probe_rf_ret(%rip), %rax\n"
        "    movq %rax, 16(%rsp)\n"
        "    lock incq probe_rf_taken(%rip)\n"
        "    movl $0, probe_rf_pending(%rip)\n"
        "    popq %rax\n"
        "    popfq\n"
        "    ret\n");

static PlatIntr *volatile g_rf_intr;
static volatile int g_rf_stop;
static volatile uint64_t g_rf_passes, g_rf_hook_sent;

static void rf_worker(void *arg) {
    (void)arg;
    g_rf_intr = plat_intr_self();
    while (!__atomic_load_n(&g_rf_stop, __ATOMIC_RELAXED)) {
        uint64_t base = 0;
        if (mem_set_heap_size(RF_HEAP, &base) != 0) break;
        probe_rf_touch((uint8_t *)(uintptr_t)base, RF_HEAP);
        mem_set_heap_size(0, NULL);
        g_rf_passes++;
    }
}

static void rf_fault_ret_hook(uint64_t *rip) {
    const uintptr_t lo = (uintptr_t)probe_rf_touch, hi = (uintptr_t)probe_rf_touch_end;
    if (*rip < lo || *rip >= hi) return;
    probe_rf_ret = *rip;
    __atomic_add_fetch(&g_rf_hook_sent, 1, __ATOMIC_RELAXED);
    *rip = (uint64_t)(uintptr_t)probe_rf_stub;
}

static void test_redirect_fault(void) {
    uint64_t faults0 = plat_mem_lazy_faults();
    g_rf_stop = 0;
    g_rf_passes = 0;
    PlatThread *w = plat_thread_new(rf_worker, NULL, "probe-kernel faulter");
    while (w && !g_rf_intr) plat_yield();
    if (!w || !g_rf_intr) {
        CHECK(0, "redirect-fault: worker did not start");
        return;
    }
    const uintptr_t lo = (uintptr_t)probe_rf_touch, hi = (uintptr_t)probe_rf_touch_end;
    for (int safe = 0; safe < 2; safe++) {
        uint64_t seen[5] = {0}, sent[5] = {0}, lost[5] = {0}, refused = 0, outside = 0, taken0 = probe_rf_taken;
        for (int i = 0; i < RF_STOPS; i++) {
            uint64_t rip, rsp;
            int st = plat_intr_stop(g_rf_intr, &rip, &rsp);
            if (!st) continue;
            seen[st]++;
            if (rip < lo || rip >= hi) {
                outside++;
                plat_intr_resume(g_rf_intr, 0);
            } else if (safe && st > 1) {
                refused++;
                plat_intr_resume(g_rf_intr, 0);
            } else {
                probe_rf_ret = rip;
                __atomic_store_n(&probe_rf_pending, 1, __ATOMIC_SEQ_CST);
                if (!plat_intr_resume(g_rf_intr, (uint64_t)(uintptr_t)probe_rf_stub)) {
                    probe_rf_pending = 0;
                    continue;
                }
                sent[st]++;
                uint64_t t0 = plat_now_ns();
                while (__atomic_load_n(&probe_rf_pending, __ATOMIC_SEQ_CST) && plat_now_ns() - t0 < RF_LOST_NS)
                    plat_pause();
                if (__atomic_load_n(&probe_rf_pending, __ATOMIC_SEQ_CST)) {
                    lost[st]++;
                    probe_rf_pending = 0;
                }
            }
            for (volatile int s = 0; s < (i * 7919) % 20000; s++) {}   // stop at a varying point of the loop
        }
        uint64_t ns = 0, nl = 0;
        for (int k = 0; k < 5; k++) ns += sent[k], nl += lost[k];
        printf("  redirect-fault (%s): stops: plain %llu, in an exception's dispatch %llu, in a system call %llu, "
               "unreported %llu; outside the loop %llu, refused %llu; sent %llu (plain %llu, exception %llu, system "
               "call %llu, unreported %llu), reached the stub %llu, lost %llu (plain %llu, exception %llu, system call "
               "%llu, unreported %llu)\n",
               safe ? "refusing stops above 1" : "redirecting every stop in the loop", (unsigned long long)seen[1],
               (unsigned long long)seen[2], (unsigned long long)seen[3], (unsigned long long)seen[4],
               (unsigned long long)outside, (unsigned long long)refused, (unsigned long long)ns,
               (unsigned long long)sent[1], (unsigned long long)sent[2], (unsigned long long)sent[3],
               (unsigned long long)sent[4], (unsigned long long)(probe_rf_taken - taken0), (unsigned long long)nl,
               (unsigned long long)lost[1], (unsigned long long)lost[2], (unsigned long long)lost[3],
               (unsigned long long)lost[4]);
        CHECK(probe_rf_taken - taken0 + nl == ns, "redirect-fault: %llu reached the stub and %llu lost of %llu sent",
              (unsigned long long)(probe_rf_taken - taken0), (unsigned long long)nl, (unsigned long long)ns);
        if (safe) CHECK(nl == 0 && ns > 0, "redirect-fault: %llu of %llu redirects lost while refusing",
                        (unsigned long long)nl, (unsigned long long)ns);
    }
    g_rf_stop = 1;
    plat_thread_join(w);
    plat_intr_free(g_rf_intr);
    g_rf_intr = NULL;
    printf("  redirect-fault: %llu heap passes, %llu lazy-heap faults\n", (unsigned long long)g_rf_passes,
           (unsigned long long)(plat_mem_lazy_faults() - faults0));

    uint64_t base = 0, hfaults0 = plat_mem_lazy_faults(), htaken0 = probe_rf_taken;
    g_rf_hook_sent = 0;
    CHECK(mem_set_heap_size(RF_HEAP, &base) == 0 && base, "fault-ret-hook: SetHeapSize(%llu) failed",
          (unsigned long long)RF_HEAP);
    if (base) {
        plat_mem_fault_ret_hook(rf_fault_ret_hook);
        probe_rf_touch((uint8_t *)(uintptr_t)base, RF_HEAP);
        plat_mem_fault_ret_hook(NULL);
        uint64_t hfaults = plat_mem_lazy_faults() - hfaults0, htaken = probe_rf_taken - htaken0, mismatches = 0;
        for (uint64_t off = 0; off < RF_HEAP; off += 64)
            if (*(const volatile uint64_t *)(uintptr_t)(base + off) != off) mismatches++;
        mem_set_heap_size(0, NULL);
        printf("  fault-ret-hook: %llu lazy-heap faults, %llu redirected, %llu reached the stub, %llu store "
               "mismatches\n",
               (unsigned long long)hfaults, (unsigned long long)g_rf_hook_sent, (unsigned long long)htaken,
               (unsigned long long)mismatches);
        CHECK(g_rf_hook_sent > 0 && htaken == g_rf_hook_sent,
              "fault-ret-hook: %llu redirected vs %llu reached the stub", (unsigned long long)g_rf_hook_sent,
              (unsigned long long)htaken);
        CHECK(mismatches == 0, "fault-ret-hook: %llu store mismatches after fault redirect",
              (unsigned long long)mismatches);
    }
}
#else
static void test_redirect_fault(void) { printf("  redirect-fault: skipped (x86-64 Windows only)\n"); }
#endif

int probe_kernel(void) {
    a_lock_waiter = hostfn_register("probe-kernel lock waiter", lock_waiter);
    a_cv_waiter = hostfn_register("probe-kernel condvar waiter", cv_waiter);
    a_cv_order_waiter = hostfn_register("probe-kernel condvar order waiter", cv_order_waiter);
    a_arb_waiter = hostfn_register("probe-kernel arbiter waiter", arb_waiter);
    a_event_waiter = hostfn_register("probe-kernel event waiter", event_waiter);
    a_core_worker = hostfn_register("probe-kernel core worker", core_worker);
    a_exit_entry = hostfn_register("probe-kernel exit entry", exit_entry);
    a_exit_nested = hostfn_register("probe-kernel exit nested", exit_nested);
    a_fiber_fn = hostfn_register("probe-kernel fiber", fiber_fn);
    a_fiber_exit_thread = hostfn_register("probe-kernel fiber exit thread", fiber_exit_thread);
    a_fiber_exit_fn = hostfn_register("probe-kernel fiber exit", fiber_exit_fn);
    a_tick_worker = hostfn_register("probe-kernel tick worker", tick_worker);
    a_ho_holder = hostfn_register("probe-kernel hand-off holder", ho_holder);
    a_ho_waiter = hostfn_register("probe-kernel hand-off waiter", ho_waiter);
    a_ho_busy = hostfn_register("probe-kernel hand-off busy", ho_busy);
    a_fib_init = hostfn_register(KERN_SYM_INITIALIZE_FIBER, kern_fiber_initialize);   // the loader's addresses
    a_fib_fin = hostfn_register(KERN_SYM_FINALIZE_FIBER, kern_fiber_finalize);
    a_fib_switch = hostfn_register(KERN_SYM_SWITCH_TO_FIBER, kern_fiber_switch);
    a_fib_cur = hostfn_register(KERN_SYM_GET_CURRENT_FIBER, kern_fiber_current);
    ldr_load(rt_config()->exefs);
    tick_stub = ldr_plt_stub("main", TICK_SYMBOL);
    if (!tick_stub || !ldr_symbol(TICK_SYMBOL)) {
        printf("probe-kernel: FAILED (no PLT stub for %s in main)\n", TICK_SYMBOL);
        return 1;
    }
    test_platform();
    test_redirect_fault();
    kern_run_on_guest_main(probe_main);
    RtStats s = rt_stats();
    printf("stats: %llu SVCs, %llu threads, %llu host calls\n", (unsigned long long)s.svcs,
           (unsigned long long)s.threads, (unsigned long long)s.host_calls);
    if (failures) {
        printf("probe-kernel: FAILED (%d failures)\n", failures);
        return 1;
    }
    printf("probe-kernel: passed\n");
    return 0;
}
