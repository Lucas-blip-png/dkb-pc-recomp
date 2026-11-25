// Tests for runtime/hostfn.c + runtime/dispatch.c fast dispatch (DKB_HOSTFN_FAST):
// direct range-check bypass, read-only hot_slots[], thread-local exact hostfn_calls(),
// DKB_TRACE=host first-call logging and per-slot counting, inline kern_host_track depth + preempt,
// blocking nn::fs core-away wrapper, rt_original observer dispatch, setjmp/longjmp unwind,
// non-hostfn rt_register_host fallback, and concurrent A/B toggling across worker threads.
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

// ---- minimal platform + runtime stubs for linking hostfn.c + dispatch.c standalone
void rt_setjmp_arm(Cpu *c, void *jb);
uint64_t hostfn_register_observer(const char *name, GuestFn fn, uint64_t original);

void plat_mutex_lock(PlatMutex *m) {
    while (__atomic_exchange_n((int *)m, 1, __ATOMIC_ACQUIRE)) {}
}
void plat_mutex_unlock(PlatMutex *m) {
    __atomic_store_n((int *)m, 0, __ATOMIC_RELEASE);
}
void *plat_aligned_alloc(size_t size, size_t align) {
    (void)align;
    return calloc(1, size);
}
void *plat_mem_alloc(uint64_t bytes) { return calloc(1, (size_t)bytes); }
int plat_mem_protect(uint64_t addr, uint64_t bytes, int prot) { (void)addr, (void)bytes, (void)prot; return 1; }
int plat_mem_readable(uint64_t addr, uint64_t bytes) { (void)addr, (void)bytes; return 0; }
void plat_commit_info(double *proc, double *used, double *limit) { *proc = *used = *limit = 0; }

struct PlatThread {
    void (*fn)(void *);
    void *arg;
#ifdef _WIN32
    HANDLE h;
#else
    pthread_t h;
#endif
};
#ifdef _WIN32
static DWORD WINAPI thread_tramp(LPVOID p) { PlatThread *t = p; t->fn(t->arg); return 0; }
#else
static void *thread_tramp(void *p) { PlatThread *t = p; t->fn(t->arg); return NULL; }
#endif
PlatThread *plat_thread_create(void (*fn)(void *), void *arg, size_t stack) {
    (void)stack;
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn, t->arg = arg;
#ifdef _WIN32
    t->h = CreateThread(NULL, 0, thread_tramp, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
#else
    if (pthread_create(&t->h, NULL, thread_tramp, t) != 0) { free(t); return NULL; }
#endif
    return t;
}
void plat_thread_join(PlatThread *t) {
    if (!t) return;
#ifdef _WIN32
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
#else
    pthread_join(t->h, NULL);
#endif
    free(t);
}

unsigned rt_trace;
int kern_host_track;

static int stub_knob_mode = 1;
static int stub_log_count;
static int stub_core_away_count;
static int stub_core_back_count;
static int stub_host_left_count;
static int stub_checkpoint_count;

int rt_knob(const char *name, const char *def) {
    (void)name, (void)def;
    return stub_knob_mode;
}

void rt_log(const char *fmt, ...) {
    (void)fmt;
    __atomic_fetch_add(&stub_log_count, 1, __ATOMIC_RELAXED);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void rt_vlogf(const char *fmt, va_list ap) { vfprintf(stderr, fmt, ap); }
void rt_log_raw(const char *p, size_t n) { fwrite(p, 1, n, stderr); }

int ldr_count(void) { return 0; }
const char *ldr_describe(uint64_t a, char *buf, int n) {
    snprintf(buf, (size_t)n, "%#llx", (unsigned long long)a);
    return buf;
}

void kern_checkpoint(void) { __atomic_fetch_add(&stub_checkpoint_count, 1, __ATOMIC_RELAXED); }
int kern_core_away(void) { __atomic_fetch_add(&stub_core_away_count, 1, __ATOMIC_RELAXED); return 1; }
void kern_core_back(int *away) { if (*away) { __atomic_fetch_add(&stub_core_back_count, 1, __ATOMIC_RELAXED); *away = 0; } }
void kern_host_left(void *thread) { (void)thread; __atomic_fetch_add(&stub_host_left_count, 1, __ATOMIC_RELAXED); }

// Dummy guest module so rt_dispatch_init has at least one lifted function.
static uint64_t guest_orig_hits;
static void guest_orig_fn(Cpu *c) {
    __atomic_fetch_add(&guest_orig_hits, 1, __ATOMIC_RELAXED);
    c->x[0] = c->x[0] + 100;
}
static const uint64_t mod_addrs[1] = {0x7000001000ull};
static const GuestFn mod_fns[1] = {guest_orig_fn};

int rt_linked_modules(RtModule *out, int max) {
    if (max < 1) return 0;
    out[0] = (RtModule){"test", 0x7000000000ull, 0x10000ull, mod_addrs, mod_fns, 1};
    return 1;
}

// ---- test helpers
static int failures;
#define CHECK(cond, msg, ...) do {                                               \
    if (!(cond)) {                                                               \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__);                       \
        failures++;                                                              \
    }                                                                            \
} while (0)

typedef struct {
    int preempt;
    int host_depth;
} FakeThread;

static uint64_t fn_add_hits;
static int seen_depth_inside;

static void fn_add(Cpu *c) {
    __atomic_fetch_add(&fn_add_hits, 1, __ATOMIC_RELAXED);
    if (c->thread) {
        int d = *kern_host_depth(c->thread);
        if (d != 1) seen_depth_inside = d;
        else if (!seen_depth_inside) seen_depth_inside = 1;
    }
    c->x[0] = c->x[0] + c->x[1];
}

static uint64_t fs_hits;
static void fn_fs_blocking(Cpu *c) {
    __atomic_fetch_add(&fs_hits, 1, __ATOMIC_RELAXED);
    c->x[0] = 77;
}

static void fn_observer(Cpu *c) {
    c->x[0] = rt_original(c) + 7;
}

static uint64_t raw_host_hits;
static void fn_raw_host(Cpu *c) {
    __atomic_fetch_add(&raw_host_hits, 1, __ATOMIC_RELAXED);
    c->x[0] = 999;
}

static void fn_set_preempt(Cpu *c) {
    ((FakeThread *)c->thread)->preempt = 1;
    c->x[0] = 55;
}

// ---- setjmp / longjmp test
static uint64_t a_longjmp;
static void test_setjmp_longjmp(void) {
    Cpu c;
    memset(&c, 0, sizeof c);
    FakeThread th = {0, 0};
    c.thread = &th;
    c.sp = 0x10000;
    c.x[30] = 0x7000001004ull;
    kern_host_track = 1;

    // Guest setjmp env buffer on host stack (x19-x30 at 0, sp at 0x68, d8-d15 at 0x70).
    uint8_t env[0x100];
    memset(env, 0, sizeof env);
    *(uint64_t *)(env + 0x58) = c.x[30];
    *(uint64_t *)(env + 0x68) = c.sp;

    jmp_buf jb;
    rt_setjmp_arm(&c, &jb);
    if (!setjmp(jb)) {
        c.x[0] = (uint64_t)(uintptr_t)env;
        c.x[1] = 42;
        c.pc = a_longjmp;
        rt_dispatch(&c);
        CHECK(0, "longjmp returned");
    }
    CHECK(c.x[0] == 42, "longjmp return value %llu != 42", (unsigned long long)c.x[0]);
    CHECK(th.host_depth == 0, "longjmp host_depth %d != 0", th.host_depth);
}

// ---- multi-thread stress with live A/B toggling
#define NTHREADS 4
#define CALLS_PER_THREAD 25000
static uint64_t a_add;

typedef struct {
    int id;
    uint64_t sum;
    int bad_depth;
} WorkerArg;

static void worker_main(void *arg) {
    WorkerArg *w = arg;
    Cpu c;
    memset(&c, 0, sizeof c);
    FakeThread th = {0, 0};
    c.thread = &th;
    for (int i = 0; i < CALLS_PER_THREAD; i++) {
        c.x[0] = (uint64_t)i;
        c.x[1] = (uint64_t)w->id;
        c.pc = a_add;
        rt_dispatch(&c);
        w->sum += c.x[0];
        if (th.host_depth != 0) w->bad_depth++;
        if ((i & 511) == 0 && w->id == 0) hostfn_ab_sync((i >> 9) & 1);
    }
}

int main(void) {
    const char *env_mode = getenv("HOSTFN_TEST_MODE");
    stub_knob_mode = env_mode ? atoi(env_mode) : 1;

    a_add = hostfn_register("test_add", fn_add);
    uint64_t a_traced = hostfn_register("test_traced", fn_add);
    uint64_t a_preempt = hostfn_register("test_preempt", fn_set_preempt);
    uint64_t a_fs = hostfn_register("_ZN2nn2fs9CloseFileEv", fn_fs_blocking);
    uint64_t a_obs = hostfn_register_observer("test_obs", fn_observer, mod_addrs[0]);
    a_longjmp = hostfn_register("longjmp_test", hostfn_override("longjmp"));
    uint64_t a_raw = 0x7020080000ull;   // outside [RT_HOSTFN_BASE, RT_HOSTFN_BASE + 16*MAX_HOSTFN)
    rt_register_host(a_raw, fn_raw_host);

    hostfn_seal();

    // 1. Basic dispatch + kern_host_track depth + preempt left notification.
    Cpu c;
    memset(&c, 0, sizeof c);
    FakeThread th = {0, 0};
    c.thread = &th;
    kern_host_track = 1;
    uint64_t calls0 = hostfn_calls();
    uint64_t r = rt_call(&c, a_add, 2, 13ull, 29ull);
    CHECK(r == 42, "rt_call test_add = %llu (want 42)", (unsigned long long)r);
    CHECK(seen_depth_inside == 1, "host_depth inside call = %d (want 1)", seen_depth_inside);
    CHECK(th.host_depth == 0, "host_depth after call = %d (want 0)", th.host_depth);
    CHECK(hostfn_calls() == calls0 + 1, "hostfn_calls %llu != %llu", (unsigned long long)hostfn_calls(),
          (unsigned long long)(calls0 + 1));

    // Preempt becoming pending during a host call fires kern_host_left on return to depth 0.
    int left0 = stub_host_left_count;
    r = rt_call(&c, a_preempt, 0);
    CHECK(r == 55, "rt_call test_preempt = %llu (want 55)", (unsigned long long)r);
    CHECK(stub_host_left_count == left0 + 1, "kern_host_left count %d != %d", stub_host_left_count, left0 + 1);
    th.preempt = 0;

    int away0 = stub_core_away_count, back0 = stub_core_back_count;
    r = rt_call(&c, a_fs, 0);
    CHECK(r == 77, "rt_call nn::fs = %llu (want 77)", (unsigned long long)r);
    CHECK(stub_core_away_count == away0 + 1 && stub_core_back_count == back0 + 1,
          "blocking nn::fs core away/back = %d/%d", stub_core_away_count - away0, stub_core_back_count - back0);

    // 2. Observer + rt_original inside fast dispatch.
    r = rt_call(&c, a_obs, 1, 5ull);
    CHECK(r == 112, "observer rt_original = %llu (want 112)", (unsigned long long)r);
    CHECK(guest_orig_hits == 1, "guest_orig_hits = %llu", (unsigned long long)guest_orig_hits);

    // 3. Raw rt_register_host fallback outside hostfn window.
    r = rt_call(&c, a_raw, 0);
    CHECK(r == 999 && raw_host_hits == 1, "raw host fallback = %llu (hits %llu)", (unsigned long long)r,
          (unsigned long long)raw_host_hits);

    // 4. DKB_TRACE=host: logs first call once and keeps hostfn_calls exact with no double counting.
    uint64_t calls_before_trace = hostfn_calls();
    int logs0 = stub_log_count;
    rt_trace = RT_TRACE_HOSTCALL;
    for (int i = 0; i < 5; i++) rt_call(&c, a_traced, 2, 1ull, 2ull);
    rt_trace = 0;
    CHECK(hostfn_calls() == calls_before_trace + 5, "trace hostfn_calls %llu != %llu",
          (unsigned long long)hostfn_calls(), (unsigned long long)(calls_before_trace + 5));
    CHECK(stub_log_count == logs0 + 1, "trace first-call log count %d (mode %d)", stub_log_count - logs0,
          stub_knob_mode);

    // 5. setjmp/longjmp unwind through hostfn_entry_fast restores host_depth and fires kern_host_left.
    test_setjmp_longjmp();

    // 6. Multi-thread stress (with live A/B toggling in mode 2).
    uint64_t calls_pre_mt = hostfn_calls();
    uint64_t hits_pre_mt = fn_add_hits;
    WorkerArg args[NTHREADS];
    PlatThread *threads[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        args[i] = (WorkerArg){.id = i, .sum = 0, .bad_depth = 0};
        threads[i] = plat_thread_create(worker_main, &args[i], 0);
        CHECK(threads[i] != NULL, "plat_thread_create %d failed", i);
    }
    for (int i = 0; i < NTHREADS; i++) plat_thread_join(threads[i]);
    uint64_t expected_mt = (uint64_t)NTHREADS * CALLS_PER_THREAD;
    CHECK(fn_add_hits - hits_pre_mt == expected_mt, "mt hits %llu != %llu",
          (unsigned long long)(fn_add_hits - hits_pre_mt), (unsigned long long)expected_mt);
    CHECK(hostfn_calls() - calls_pre_mt == expected_mt, "mt hostfn_calls %llu != %llu",
          (unsigned long long)(hostfn_calls() - calls_pre_mt), (unsigned long long)expected_mt);
    for (int i = 0; i < NTHREADS; i++) {
        uint64_t want = (uint64_t)CALLS_PER_THREAD * (CALLS_PER_THREAD - 1) / 2 + (uint64_t)i * CALLS_PER_THREAD;
        CHECK(args[i].sum == want, "thread %d sum %llu != %llu", i, (unsigned long long)args[i].sum,
              (unsigned long long)want);
        CHECK(args[i].bad_depth == 0, "thread %d bad_depth %d", i, args[i].bad_depth);
    }

    if (failures) return 1;
    printf("hostfn_fast mode=%d ok (calls=%llu)\n", stub_knob_mode, (unsigned long long)hostfn_calls());
    return 0;
}
