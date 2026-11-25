// Concurrency and hook tests for runtime/memory.c (Windows lazy heap / mem_commit_later / mem_decommit_hook):
//   1. Deterministically proves cl_thread holds memory.c's `lock` shared (mem_query, mem_guest_readable, mem_info
//      succeed while cl_thread is parked inside plat_mem_commit; exclusive locking would deadlock).
//   2. Verifies mem_decommit_hook runs after plat_mem_decommit and after frz_lock is released, with exact (a, n).
//   3. Stresses concurrent cl_thread slices, reader threads calling mem_query/mem_guest_readable/mem_info, and
//      mem_set_heap_size shrink/growth, asserting no committed page or lazy_map entry leaks past heap_size.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

static unsigned checks, failures;
static SRWLOCK check_lock = SRWLOCK_INIT;
#define CHECK(c) do {                                                                                                  \
        AcquireSRWLockExclusive(&check_lock);                                                                          \
        checks++;                                                                                                      \
        if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); }                                 \
        ReleaseSRWLockExclusive(&check_lock);                                                                          \
    } while (0)

unsigned rt_trace;

void rt_log(const char *fmt, ...) {
    (void)fmt;
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

// ---- minimal host stubs
void cfg_init(void) {}
const char *cfg_get(const char *name) { (void)name; return NULL; }
int cfg_bool(const char *name) { return !strcmp(name, "DKB_HEAP_LAZY"); }
uint64_t res_heap_extra_bytes(void) { return 0; }
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
Cpu *kern_cpu(void) { return NULL; }
uint64_t perf_ph_begin(void) { return 0; }
void perf_ph_end(int part, uint64_t t0) { (void)part; (void)t0; }

const RtLayoutEntry rt_layout[] = {{"main", 0x7000000000ull, 0x1000000ull}};
const int rt_layout_count = 1;

int plat_op_on;
int plat_op_begin_(int kind, uint64_t addr, uint64_t size) { (void)kind; (void)addr; (void)size; return -1; }
void plat_op_end_(int token, uint64_t run) { (void)token; (void)run; }

// ---- real Win32 SRWLOCK / CONDITION_VARIABLE / CreateThread primitives
static volatile uint64_t sim_clock_ns = 10000000000ull; // 10 s

uint64_t plat_now_ns(void) {
    return __atomic_load_n(&sim_clock_ns, __ATOMIC_ACQUIRE);
}

static void sim_clock_advance_ms(uint64_t ms) {
    __atomic_add_fetch(&sim_clock_ns, ms * 1000000ull, __ATOMIC_SEQ_CST);
}

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
void plat_yield(void) { SwitchToThread(); }
uintptr_t plat_exe_base(void) { return 0; }
void plat_reexec(void) { rt_fatal("unexpected plat_reexec"); }
int plat_setenv(const char *name, const char *val) { (void)name; (void)val; return 1; }

void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->p); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)&m->p) != 0; }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->p); }
void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)&m->p); }
int plat_mutex_trylock_shared(PlatMutex *m) { return TryAcquireSRWLockShared((PSRWLOCK)&m->p) != 0; }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)&m->p); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->p, (PSRWLOCK)&m->p, INFINITE, 0); }
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    DWORD ms = (DWORD)(ns / 1000000ull);
    if (ms > 5) ms = 5; // keep wakeups snappy when virtual clock advances
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->p, (PSRWLOCK)&m->p, ms, 0) != 0;
}
void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)&c->p); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)&c->p); }

typedef struct { void (*fn)(void *); void *arg; } ThrStart;
static DWORD WINAPI thr_tramp(LPVOID p) {
    ThrStart s = *(ThrStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
PlatThread *plat_thread_new(void (*fn)(void *), void *arg, const char *name) {
    (void)name;
    ThrStart *s = malloc(sizeof *s);
    if (!s) rt_fatal("oom");
    *s = (ThrStart){fn, arg};
    return (PlatThread *)CreateThread(NULL, 0, thr_tramp, s, 0, NULL);
}

// ---- simulated virtual memory backing the guest heap (64 MiB window, 4 KiB pages, 64 KiB lazy_map chunks)
#define SIM_HEAP_MAX (64ull << 20)
#define SIM_PAGES (SIM_HEAP_MAX >> 12)
#define SIM_CHUNKS (SIM_HEAP_MAX >> 16)

static SRWLOCK sim_mem_lock = SRWLOCK_INIT;
static uint64_t sim_heap_base;
static uint64_t sim_lazy_base, sim_lazy_size;
static uint8_t sim_page_committed[SIM_PAGES]; // 0 reserved, 1 committed RW
static uint8_t sim_lazy_map[SIM_CHUNKS];      // 0 unknown, 1 known committed

// Synchronization gate for Test A (pauses cl_thread inside plat_mem_commit while holding memory.c's lock).
static volatile int gate_arm;
static volatile int gate_entered;
static volatile int gate_release;

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    (void)addr; (void)n; (void)watch;
    return 1;
}

void plat_mem_fault_hook(int (*fn)(uint64_t)) { (void)fn; }
void plat_mem_lazy_commit_hook(void (*fn)(uint64_t, uint64_t, int)) { (void)fn; }
void plat_mem_probe_hooks(int (*f)(uint64_t, int, uint64_t), int (*m)(uint64_t, uint64_t),
                          void (*r)(uint64_t, uint64_t, uintptr_t)) {
    (void)f; (void)m; (void)r;
}
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    (void)addr; (void)n; (void)pages; (void)max; (void)reset;
    return 0;
}
void plat_mem_walk(void (*cb)(uint64_t, uint64_t, int, void *), void *arg) { (void)cb; (void)arg; }
uint64_t plat_mem_resident(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
void plat_commit_info(double *p, double *u, double *l) { *p = *u = *l = 0; }
uint64_t plat_mem_lazy_faults(void) { return 0; }
void plat_mem_lazy_chunk(uint64_t bytes) { (void)bytes; }

void plat_mem_lazy(uint64_t base, uint64_t size) {
    AcquireSRWLockExclusive(&sim_mem_lock);
    uint64_t old_size = sim_lazy_size;
    sim_lazy_base = base;
    sim_lazy_size = size;
    if (size < old_size) {
        uint64_t c0 = size >> 16, c1 = old_size >> 16;
        if (c1 > SIM_CHUNKS) c1 = SIM_CHUNKS;
        if (c0 < c1) memset(sim_lazy_map + c0, 0, (size_t)(c1 - c0));
    }
    ReleaseSRWLockExclusive(&sim_mem_lock);
}

uint64_t plat_mem_lazy_run(uint64_t a, uint64_t e, int *known) {
    AcquireSRWLockShared(&sim_mem_lock);
    if (!sim_lazy_base || a < sim_lazy_base || a >= sim_lazy_base + SIM_HEAP_MAX) {
        ReleaseSRWLockShared(&sim_mem_lock);
        *known = 0;
        return e;
    }
    uint64_t c0 = (a - sim_lazy_base) >> 16;
    uint64_t c1 = (e - sim_lazy_base) >> 16;
    if (c1 > SIM_CHUNKS) c1 = SIM_CHUNKS;
    int v = sim_lazy_map[c0];
    uint64_t c = c0 + 1;
    while (c < c1 && sim_lazy_map[c] == v) c++;
    ReleaseSRWLockShared(&sim_mem_lock);
    *known = v;
    uint64_t out = sim_lazy_base + (c << 16);
    return out < e ? out : e;
}

void plat_mem_lazy_mark(uint64_t a, uint64_t e) {
    AcquireSRWLockExclusive(&sim_mem_lock);
    if (sim_lazy_base && a >= sim_lazy_base && e <= sim_lazy_base + SIM_HEAP_MAX && a < e) {
        uint64_t c0 = (a - sim_lazy_base) >> 16, c1 = (e - sim_lazy_base) >> 16;
        memset(sim_lazy_map + c0, 1, (size_t)(c1 - c0));
    }
    ReleaseSRWLockExclusive(&sim_mem_lock);
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    AcquireSRWLockShared(&sim_mem_lock);
    if (!sim_heap_base || addr < sim_heap_base || addr >= sim_heap_base + SIM_HEAP_MAX) {
        ReleaseSRWLockShared(&sim_mem_lock);
        *end = limit;
        return PLAT_MEM_RESERVED;
    }
    uint64_t p0 = (addr - sim_heap_base) >> 12;
    uint64_t p1 = (limit - sim_heap_base) >> 12;
    if (p1 > SIM_PAGES) p1 = SIM_PAGES;
    uint8_t v = sim_page_committed[p0];
    uint64_t p = p0 + 1;
    while (p < p1 && sim_page_committed[p] == v) p++;
    ReleaseSRWLockShared(&sim_mem_lock);
    uint64_t out = sim_heap_base + (p << 12);
    *end = out < limit ? out : limit;
    return v ? PLAT_PROT_RW : PLAT_MEM_RESERVED;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    if (__atomic_load_n(&gate_arm, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&gate_arm, 0, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&gate_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&gate_release, __ATOMIC_ACQUIRE)) SwitchToThread();
    }
    AcquireSRWLockExclusive(&sim_mem_lock);
    if (sim_heap_base && addr >= sim_heap_base && addr + n <= sim_heap_base + SIM_HEAP_MAX) {
        uint64_t p0 = (addr - sim_heap_base) >> 12, cnt = n >> 12;
        memset(sim_page_committed + p0, 1, (size_t)cnt);
    }
    ReleaseSRWLockExclusive(&sim_mem_lock);
    return 1;
}

int plat_mem_decommit(uint64_t addr, uint64_t n) {
    AcquireSRWLockExclusive(&sim_mem_lock);
    if (sim_heap_base && addr >= sim_heap_base && addr + n <= sim_heap_base + SIM_HEAP_MAX) {
        uint64_t p0 = (addr - sim_heap_base) >> 12, cnt = n >> 12;
        memset(sim_page_committed + p0, 0, (size_t)cnt);
    }
    ReleaseSRWLockExclusive(&sim_mem_lock);
    return 1;
}

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    (void)addr; (void)n; (void)prot;
    return 1;
}

// Include the real production runtime/memory.c directly so tests exercise cl_thread, decommit, and lock.
#include "../runtime/memory.c"

// ---- decommit_hook observer
static volatile uint64_t hook_calls, hook_last_a, hook_last_n;
static volatile int hook_frz_unlocked_ok = 1;
static volatile int hook_already_decommitted_ok = 1;

static void on_decommit(uint64_t a, uint64_t n) {
    // Verify frz_lock is already released (lock order requirement: lock -> gen_lock, never frz_lock -> gen_lock).
    if (plat_mutex_trylock(&frz_lock)) {
        plat_mutex_unlock(&frz_lock);
    } else {
        __atomic_store_n(&hook_frz_unlocked_ok, 0, __ATOMIC_RELEASE);
    }
    // Verify plat_mem_decommit already ran on [a, a+n).
    uint64_t y = 0;
    if (plat_mem_state(a, a + n, &y) != PLAT_MEM_RESERVED || y != a + n)
        __atomic_store_n(&hook_already_decommitted_ok, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hook_last_a, a, __ATOMIC_RELEASE);
    __atomic_store_n(&hook_last_n, n, __ATOMIC_RELEASE);
    __atomic_add_fetch(&hook_calls, 1, __ATOMIC_SEQ_CST);
}

static int wait_cl_ranges(uint64_t want_total) {
    uint64_t got = 0, s[4];
    for (int i = 0; i < 2000 && got < want_total; i++) {
        mem_commit_later_stats(s);
        got += s[0];
        if (got >= want_total) return 1;
        sim_clock_advance_ms(500);
        Sleep(1);
    }
    return got >= want_total;
}

static int sim_all_state(uint64_t a, uint64_t e, int committed) {
    for (uint64_t x = a, y; x < e; x = y) {
        int st = plat_mem_state(x, e, &y);
        if (committed ? st != PLAT_PROT_RW : st != PLAT_MEM_RESERVED) return 0;
    }
    return 1;
}

static int sim_lazy_all(uint64_t a, uint64_t e, int want) {
    for (uint64_t x = a; x < e;) {
        int known = -1;
        uint64_t y = plat_mem_lazy_run(x, e, &known);
        if (known != want || y <= x) return 0;
        x = y;
    }
    return 1;
}

// ---- reader threads for concurrent stress
static volatile int stress_stop;
static volatile uint64_t reader_queries;

static DWORD WINAPI reader_worker(LPVOID arg) {
    uint64_t heap = (uint64_t)(uintptr_t)arg;
    uint64_t local = 0;
    while (!__atomic_load_n(&stress_stop, __ATOMIC_ACQUIRE)) {
        RtMemoryInfo mi;
        uint64_t off = (local & 15) * (4ull << 20);
        if (mem_query(heap + off, &mi) != R_OK) {
            CHECK(0);
            break;
        }
        CHECK(mi.state == ST_NORMAL || mi.state == ST_FREE);
        int rd = mem_guest_readable(heap + off, 0x1000);
        CHECK(rd == 0 || rd == 1);
        uint64_t used = 0;
        CHECK(mem_info(7, &used) == 1);
        local++;
    }
    __atomic_add_fetch(&reader_queries, local, __ATOMIC_SEQ_CST);
    return 0;
}

int main(void) {
    const uint64_t M = 1ull << 20;
    mem_init();
    sim_heap_base = mem_layout()->heap.base;
    CHECK(sim_heap_base != 0);

    // ---- Test A: cl_thread holds `lock` shared, so mem_query / mem_guest_readable / mem_info do NOT block.
    uint64_t heap = 0, stats[4];
    mem_commit_later_stats(stats);
    CHECK(mem_set_heap_size(16 * M, &heap) == R_OK && heap == sim_heap_base);
    CHECK(sim_all_state(heap, heap + 16 * M, 0));

    __atomic_store_n(&gate_entered, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gate_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gate_arm, 1, __ATOMIC_RELEASE);
    mem_commit_later(heap, 8 * M);

    // Advance virtual clock so cl_thread runs immediately and hits gate inside plat_mem_commit.
    for (int i = 0; i < 2000 && !__atomic_load_n(&gate_entered, __ATOMIC_ACQUIRE); i++) {
        sim_clock_advance_ms(500);
        Sleep(1);
    }
    CHECK(__atomic_load_n(&gate_entered, __ATOMIC_ACQUIRE) == 1);

    // While cl_thread is blocked inside plat_mem_commit holding `lock`:
    //   - Shared readers MUST proceed without deadlocking!
    //   - Exclusive trylock on `lock` MUST fail (proving cl_thread actually holds `lock` right now!).
    CHECK(plat_mutex_trylock(&lock) == 0);
    RtMemoryInfo mi = {0};
    CHECK(mem_query(heap + 0x1000, &mi) == R_OK && mi.state == ST_NORMAL && mi.base == heap && mi.size == 16 * M);
    CHECK(mem_guest_readable(heap + 0x1000, 0x1000) == 1);
    uint64_t used = 0;
    CHECK(mem_info(7, &used) == 1 && used == 16 * M);

    // Release cl_thread and wait for the 8 MiB range to finish.
    __atomic_store_n(&gate_release, 1, __ATOMIC_RELEASE);
    CHECK(wait_cl_ranges(1));
    CHECK(sim_all_state(heap, heap + 8 * M, 1));
    CHECK(sim_lazy_all(heap, heap + 8 * M, 1));
    CHECK(sim_all_state(heap + 8 * M, heap + 16 * M, 0));

    // ---- Test B: mem_decommit_hook fires after plat_mem_decommit with frz_lock released.
    mem_decommit_hook(on_decommit);
    CHECK(mem_set_heap_size(8 * M, &heap) == R_OK);
    CHECK(__atomic_load_n(&hook_calls, __ATOMIC_ACQUIRE) == 1);
    CHECK(__atomic_load_n(&hook_last_a, __ATOMIC_ACQUIRE) == heap + 8 * M);
    CHECK(__atomic_load_n(&hook_last_n, __ATOMIC_ACQUIRE) == 8 * M);
    CHECK(__atomic_load_n(&hook_frz_unlocked_ok, __ATOMIC_ACQUIRE) == 1);
    CHECK(__atomic_load_n(&hook_already_decommitted_ok, __ATOMIC_ACQUIRE) == 1);

    // Clearing the hook stops notifications.
    mem_decommit_hook(NULL);
    CHECK(mem_set_heap_size(4 * M, &heap) == R_OK);
    CHECK(__atomic_load_n(&hook_calls, __ATOMIC_ACQUIRE) == 1);

    // ---- Test C: concurrent cl_thread + reader threads + mem_set_heap_size shrink/growth stress.
    mem_decommit_hook(on_decommit);
    uint64_t hooks_before = __atomic_load_n(&hook_calls, __ATOMIC_ACQUIRE);
    __atomic_store_n(&stress_stop, 0, __ATOMIC_RELEASE);
    HANDLE readers[2];
    for (int i = 0; i < 2; i++) readers[i] = CreateThread(NULL, 0, reader_worker, (LPVOID)(uintptr_t)heap, 0, NULL);

    for (int iter = 0; iter < 120; iter++) {
        uint64_t big = (uint64_t)(24 + (iter % 5) * 8) * M; // 24..56 MiB
        uint64_t small = (uint64_t)(4 + (iter % 4) * 4) * M; // 4..16 MiB
        CHECK(mem_set_heap_size(big, &heap) == R_OK);
        mem_commit_later(heap + small, big - small);
        sim_clock_advance_ms(3000);
        if (iter & 1) SwitchToThread();
        CHECK(mem_set_heap_size(small, &heap) == R_OK);
        // Drain background range so we can check no page beyond `small` was committed or marked in lazy_map.
        CHECK(wait_cl_ranges(1));
        CHECK(sim_all_state(heap + small, heap + SIM_HEAP_MAX, 0));
        CHECK(sim_lazy_all(heap + small, heap + SIM_HEAP_MAX, 0));
    }

    __atomic_store_n(&stress_stop, 1, __ATOMIC_RELEASE);
    WaitForMultipleObjects(2, readers, TRUE, 10000);
    for (int i = 0; i < 2; i++) CloseHandle(readers[i]);

    CHECK(__atomic_load_n(&reader_queries, __ATOMIC_ACQUIRE) > 0);
    CHECK(__atomic_load_n(&hook_calls, __ATOMIC_ACQUIRE) == hooks_before + 120);
    CHECK(__atomic_load_n(&hook_frz_unlocked_ok, __ATOMIC_ACQUIRE) == 1);
    CHECK(__atomic_load_n(&hook_already_decommitted_ok, __ATOMIC_ACQUIRE) == 1);

    // Final grow + mem_touch must commit every page cleanly.
    CHECK(mem_set_heap_size(32 * M, &heap) == R_OK);
    mem_touch(heap, 32 * M);
    CHECK(sim_all_state(heap, heap + 32 * M, 1));
    CHECK(mem_set_heap_size(0, &heap) == R_OK);
    CHECK(sim_all_state(heap, heap + SIM_HEAP_MAX, 0));

    printf("commit-later-lock: %u checks, %u failures (%llu concurrent reader queries)\n",
           checks, failures, (unsigned long long)__atomic_load_n(&reader_queries, __ATOMIC_ACQUIRE));
    return failures != 0;
}
