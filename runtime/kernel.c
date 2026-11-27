// Adapted from MacNativeGames games/totk/runtime/threads.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors; itself from d2r-mobile dm/host/hle_thread.c), d2r-mobile dm/host/thread.c @ 53e7c5b (MIT, Copyright
// (c) 2026 jhaizhou) and LAS Recomp runtime/kernel.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai); the preemption of
// virtual cores from MacNativeGames threads.c @ 33e8fa8 (MIT, as above).
// Threads, synchronization, virtual cores and fibers of the Horizon kernel.
//
// One guest thread = one Windows thread with a 256 MiB reserved host stack (every guest BL is a host call) and its own
// Cpu. The SDK's nn::os runs as lifted code; only the kernel under it is the host. Every wait hangs on one global lock
// G with one condition variable per thread; wake order is the kernel's: lower priority value first, then first come.
// Semantics follow the public Horizon kernel (KConditionVariable, KAddressArbiter, KSynchronization).
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"

#define R_OK 0u
#define R_TIMEOUT KERN_R_TIMEOUT
#define R_CANCELLED KERN_R_CANCELLED
#define R_BAD_HANDLE KERN_R_BAD_HANDLE
#define R_BAD_STATE KERN_R_BAD_STATE
#define R_INVALID_ENUM KERN_R_INVALID_ENUM
#define WAITERS_BIT KERN_WAITERS_BIT
#define H_CUR_THREAD KERN_H_CURRENT_THREAD

#define MAX_H 4096
#define NCORE 6                          // Switch 2 applications run on cores 0-5 (Bananza main.npdm ACI0 ThreadInfo)
#define HOST_STACK (256ull << 20)        // reserved, committed on demand
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400            // "current thread object" at TLR+0x1F8 (d2r-mobile thread.c)
#define TLS_SIZE 0x2000
#define MAIN_STACK (1ull << 20)          // main.npdm META+0x1C
#define MAIN_PRIO 44                     // META+0xE
#define MAIN_CORE 0                      // META+0xF
#define FIBER_MAX 1024

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION = KERN_SESSION, O_TMEM = KERN_TMEM };
_Static_assert(O_EVENT_W < O_SESSION, "handle kinds");
enum { W_NONE = KERN_WAIT_NONE, W_LOCK = KERN_WAIT_LOCK, W_CONDVAR = KERN_WAIT_CONDVAR, W_ADDRESS = KERN_WAIT_ADDRESS,
       W_SYNC = KERN_WAIT_SYNC };

// Scheduler accounting states (DKB_TRACE=sched).
enum { S_OFF, S_RUN, S_CORE, S_HOST, S_SLEEP, S_SYNC, S_CV, S_LOCK, S_ADDR, S_N };
static const char *const s_name[S_N] = {"off", "run", "core", "host", "sleep", "sync", "cv", "lock", "addr"};

typedef struct Event { int signaled, refs; } Event;
typedef struct Fiber Fiber;

// Core wait counters (see "core wait counters" below): what a ready thread waited for. A core wait is classed by the
// holders of the cores the thread may run on, as core_kick left them when the wait began: a worse holder asked to give
// its core up while it ran lifted code (CW_WORSE_GAME) or while it was in a host call or SVC (CW_WORSE_HOST), holders of
// equal priority and none worse (CW_EQ), only better holders (CW_BETTER), or worse holders all claimed by better
// waiters (CW_NOCORE). CW_WAKE is the time from a wake-up (wake) until the woken host thread runs again holding G;
// CW_LOCK the time a thread blocked on a guest mutex until the mutex was handed to it. CW_WORSE_WAKER is a worse
// holder that is the thread whose signal just woke the waiter (it went on after its SVC: Horizon switches there).
enum { CW_WORSE_GAME, CW_WORSE_HOST, CW_EQ, CW_BETTER, CW_NOCORE, CW_WAKE, CW_LOCK, CW_WORSE_WAKER, CW_N };

struct RtThread {
    int preempt;               // first member: rt_dispatch reads it (host.h kern_preempt_pending)
    int host_depth;            // second member: host calls and SVCs in progress, while kern_host_track (host.h)
    uint32_t handle;
    uint64_t id;
    Cpu *cpu;                  // the thread's own context
    Cpu *cur;                  // the context running now: cpu, or the current fiber's
    uint64_t entry, arg, stack_top;
    int prio, core;
    uint64_t mask;
    int started, exited, closed, cancel, is_main;
    PlatCond cv;
    // current wait
    int wkind, woke;
    uint64_t waddr, wkey;
    uint32_t wtag, wres;
    uint64_t wseq;             // registration order: equal priority wakes first come first
    RtThread *next;            // wait list
    RtThread *all;             // every thread
    // virtual core
    int cur_core;              // core held while running guest code, -1 while blocked
    uint64_t cseq;
    RtThread *cnext;
    uint8_t *tlr, *tls;
    jmp_buf exit_jmp;          // the thread start; ExitThread longjmps to it
    // fibers
    PlatFiber *base_fiber;     // the host thread as a host fiber, after the first switch
    Fiber *fiber;              // current guest fiber, NULL on the thread's own context
    Fiber *leaving;            // fiber just switched away from, released by whoever resumes next
    int exit_pending;          // ExitThread on a fiber: unwind once back on the thread's own context
    // DKB_TRACE=sched accounting (holding G)
    uint32_t tid;              // host thread id
    int st;                    // S_* state since st_since
    uint64_t st_since, st_ns[S_N], nblk, npre, pre_since, cpu_last;
    uint64_t wobj;             // what the current wait is on: owner handle, key, address or first handle
    uint32_t waker;            // the thread whose signal ended the last wait (0: a host thread or a timeout)
    uint64_t wake_at, grant_at;   // when a wait was signalled and a core granted, until the thread runs again
    uint64_t lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;   // those latencies; SleepThread asked/took
    uint64_t sp_n, sp_ho, sp_site;   // spin wait passes and core handoffs, the last loop head (the thread writes them)
    RtThread *target;          // as a core waiter: the owner core_kick asked to give its core up for it (holding G)
    // DKB_ASYNC_PREEMPT (holding G)
    PlatIntr *hthr;            // the host thread, for plat_intr_stop / resume; open while it may own a core
    uint64_t ap_rip;           // the interrupted host rip kern_ap_stub returns to
    uint64_t ap_last;          // the last redirect attempt on this owner
    uint64_t ap_sent_at;       // when the pending redirect was sent
    int ap_pending;            // redirected to kern_ap_stub, which has not reached kern_ap_yield yet
    // DKB_CORE_DIAG (holding G, except dg_host_end, which only the thread itself writes)
    uint64_t dg_since;         // as a waiter: core wait start
    int dg_cls;                // as a waiter: DG_* class at the start of the wait
    uint64_t dg_kick;          // as an owner: the preempt request
    uint64_t dg_host_end;      // as an owner: the return from the host call or SVC it was in at the request
    uint64_t dg_pc, dg_lr;     // as an owner: guest pc and lr at the checkpoint that gave the core up
    uint64_t dg_hrip;          // as an owner: the host rip kern_ap_stub interrupted, when that gave the core up
    int dg_host_kick;          // as an owner: in a host call or SVC at the request
    uint32_t dg_svc;           // the thread's latest SVC (DKB_CORE_DIAG: what an owner in a host call is in)
    // as a waiter: the owner that released
    struct DgRel { uint64_t kick, host_end, pc, lr, hrip; int host, pre, prio; uint32_t h; } dg_rel;
    // core wait counters (holding G)
    uint64_t cw_woke_at;       // when wake() ended the current wait
    uint64_t cw_core0;         // set by block: the core wait that follows starts here
    int cw_blk;                // set by block for core_take: 1 after a wait that timed out, 2 after a wake-up
    uint64_t cw_ap_at;         // as a core waiter: when its first asynchronous preemption was sent
    int cw_prio;               // as a core waiter: its priority in the latest core_kick
    int cw_rel;                // as a core waiter: how the core it got was given up (CW_REL_*)
    uint64_t cs_epoch;         // the main thread's wait the cs_ counts belong to (g_cw_epoch)
    uint64_t cs_ns[CW_N], cs_n[CW_N], cs_max[CW_N];   // this thread's waits during that wait of the main thread
    // DKB_SCHED_HANDOFF (holding G; see "directed core hand-off")
    int ho_transit;            // woken by wake() while it handed off, not yet back from block_inner (has not run)
    int ho_got;                // given a core in transit by its waker; core_take finishes the take
    RtThread *ho_cand;         // as a waker: the best thread woken since its last SVC return, never dereferenced
    uint64_t ho_cand_id;       // before ho_cand_h shows that thread still exists
    uint32_t ho_cand_h;
    int ho_cand_prio;
};

static PlatMutex G = PLAT_MUTEX_INIT;
static struct { int type; void *obj; } g_h[MAX_H];
static _Thread_local RtThread *tcur;
_Static_assert(offsetof(RtThread, preempt) == 0, "host.h kern_preempt_pending reads the first int");
_Static_assert(offsetof(RtThread, host_depth) == sizeof(int), "host.h kern_host_depth reads the second int");
static RtThread *g_all, *g_wait, *g_main;
static uint64_t g_next_id = 0x51, g_wseq, g_created;
static uint64_t g_main_stack;

static void lock(void) { plat_mutex_lock(&G); }
static void unlock(void) { plat_mutex_unlock(&G); }

uint64_t kern_now_ns(void) { return plat_now_ns(); }

// ---- DKB_CRIT=<first>-<last frame> with DKB_TRACE=sched: every state change and wake of the guest threads and
// every present over those frames, written to DKB_CRIT_OUT (default crit.bin) at the last one, for
// tools/crit_path.py, which walks the chain of waits back from each present (holding G). DKB_CRIT=w<lo>-<hi> takes
// world-clock seconds instead (frame_step_world_us): from the first present at or after lo to the first after hi, so a
// session covers the same game time at any frame rate.
typedef struct { uint64_t ts, obj; uint32_t a, b; uint16_t kind, st; uint32_t pad; } CritEv;
enum { CRIT_STATE, CRIT_WAKE, CRIT_FRAME };   // a: thread; b: its waker (STATE), the waker (WAKE); st: new S_* state
#define CRIT_N (1u << 22)                // 128 MiB, about 2000 fight frames
static CritEv *g_crit;
static uint32_t g_crit_n;
static int g_crit_on;
static uint64_t g_crit_lo, g_crit_hi;   // frames; UINT64_MAX in world mode until the world clock fixes them
static int64_t g_crit_wlo, g_crit_whi;  // world mode: the window in microseconds of world clock, else 0

static void crit_add(int kind, uint32_t a, uint32_t b, int st, uint64_t obj, uint64_t ts) {
    if (g_crit_n < CRIT_N) g_crit[g_crit_n++] = (CritEv){ts, obj, a, b, (uint16_t)kind, (uint16_t)st, 0};
}

// ---- DKB_TRACE=sched accounting (holding G)
#define RW_N 32
static RtThread *g_render;                               // the thread that presents (sched trace, core diag)
typedef struct { int st; uint32_t waker; uint64_t obj, ns; } WaitRec;
static WaitRec g_rw[RW_N], g_mw[RW_N];                   // the render and main thread's waits by object and waker
static uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;  // preempt requests; releases after one and their latency
static uint64_t g_sync_calls, g_sync_blocks;              // WaitSynchronization calls and the blocks inside them
static int g_cw_on;       // core wait counters on (DKB_CORE_WAITS, updated holding G: cw_init, kern_core_waits_frame)
int kern_sched_handoff;   // DKB_SCHED_HANDOFF on (written holding G: cores_enabled, then each present for "ab")
static int g_ho_mode;
static void ho_note(RtThread *t);

static void wait_add(WaitRec *w, int st, uint64_t obj, uint32_t waker, uint64_t ns) {
    for (int i = 0; i < RW_N; i++)
        if (!w[i].ns || (w[i].st == st && w[i].obj == obj && w[i].waker == waker)) {
            w[i].st = st;
            w[i].obj = obj;
            w[i].waker = waker;
            w[i].ns += ns;
            return;
        }
}

// Close the current state's interval and enter st.
static void acct(RtThread *t, int st) {
    if (!(rt_trace & RT_TRACE_SCHED)) return;
    uint64_t now = kern_now_ns();
    if (t->st_since) {
        t->st_ns[t->st] += now - t->st_since;
        if (t->st >= S_HOST && (t == g_render || t->is_main)) {
            uint32_t waker = t->st == S_HOST || t->st == S_SLEEP ? 0 : t->waker;
            wait_add(t == g_render ? g_rw : g_mw, t->st, t->wobj, waker, now - t->st_since);
        }
    }
    if (g_crit_on) crit_add(CRIT_STATE, t->handle, t->waker, st, t->wobj, now);
    t->st = st;
    t->st_since = now;
}

static int st_of(int kind) {
    return kind == W_LOCK ? S_LOCK : kind == W_CONDVAR ? S_CV : kind == W_ADDRESS ? S_ADDR : S_SYNC;
}

// ---- handles (caller holds G)
static uint32_t h_new(int type, void *obj) {
    for (int i = 1; i < MAX_H; i++)
        if (!g_h[i].type) {
            g_h[i].type = type;
            g_h[i].obj = obj;
            return 0x1000 + i;
        }
    rt_fatal("kernel: handle table full (%d handles)", MAX_H);
}

static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return tcur;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}

static void *h_get(uint32_t h, int type) {
    void *o = h_peek(h, type);
    if (!o) rt_log("kernel: invalid handle %#x (want kind %d)", h, type);
    return o;
}

static RtThread *cur(uint32_t svc) {
    if (!tcur) rt_fatal("kernel: svc %#x on a host thread (no guest thread)", svc);
    return tcur;
}

// ---- wait primitives (holding G)
static void wait_unlink(RtThread *t) {
    for (RtThread **p = &g_wait; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    t->next = NULL;
}

// The waiter to wake in kernel order. Lock waits match by lock address, condvar waits by key, arbiter waits by
// address.
static RtThread *pick(int kind, uint64_t a) {
    RtThread *b = NULL;
    for (RtThread *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == W_CONDVAR ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}

static void wake(RtThread *t, uint32_t res) {
    wait_unlink(t);
    t->wkind = W_NONE;
    t->woke = 1;
    t->wres = res;
    t->waker = tcur ? tcur->handle : 0;
    if (g_cw_on) t->cw_woke_at = kern_now_ns();
    if (rt_trace & RT_TRACE_SCHED) t->wake_at = kern_now_ns();
    if (g_crit_on) crit_add(CRIT_WAKE, t->handle, t->waker, 0, 0, t->wake_at);
    if (kern_sched_handoff) ho_note(t);
    plat_cond_signal(&t->cv);
}

static void wake_sync_waiters(void) {
    for (RtThread *w = g_wait; w;) {
        RtThread *n = w->next;
        if (w->wkind == W_SYNC) wake(w, (uint32_t)-1);
        w = n;
    }
}

// DKB_MUTEX_SPIN=<microseconds> (off unless set): a thread about to sleep in a guest mutex or condition variable wait,
// or in a wait for a virtual core, first polls for that long without the kernel lock, pausing the host core between
// reads. A hand-off that comes within it (a mutex convoy passes the mutex every few microseconds) then costs no host
// sleep and wake-up: a condition variable wake-up costs 3-4 us on Windows (--probe-kernel), twice per pass of a convoy
// (the mutex to its waiter, the core back to the thread that gave it up). Then the thread sleeps as before.
static uint64_t g_mspin_ns;
static int32_t g_mspin_cap;
static volatile int32_t g_mspin_now;
static void mspin_count(int woke, int got, uint64_t ns);

// Holding G on entry and return, not meanwhile: polls *p until it is nonzero (woke) or >= 0 (!woke), or until end.
// Capped at half the usable logical processors so holders always keep cores on 4-6 LP hosts, and yields every 256
// iterations so a descheduled holder on the same core can run.
static void mspin(const int *p, int woke, uint64_t end) {
    int32_t cap = g_mspin_cap;
    if (cap <= 0 || __atomic_add_fetch(&g_mspin_now, 1, __ATOMIC_RELAXED) > cap) {
        if (cap > 0) __atomic_sub_fetch(&g_mspin_now, 1, __ATOMIC_RELAXED);
        return;
    }
    int count = g_cw_on;
    unlock();
    uint64_t t0 = count ? kern_now_ns() : 0, now = t0;
    int got = 0;
    for (unsigned i = 0;; i++) {
        int v = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if ((got = woke ? v != 0 : v >= 0)) break;
        if ((i & 15) == 0 && (now = kern_now_ns()) >= end) break;
        if ((i & 255) == 255) plat_yield();
        else plat_pause();
    }
    __atomic_sub_fetch(&g_mspin_now, 1, __ATOMIC_RELAXED);
    if (got && count) now = kern_now_ns();
    lock();
    if (count) mspin_count(woke, got, now - t0);
}

// SleepConditionVariableSRW counts milliseconds: round up (late is legal, early is not) and re-check the deadline
// against the performance counter, because the wait may also end early.
static uint32_t block_inner(RtThread *t, int kind, int64_t ns) {
    t->wkind = kind;
    t->woke = 0;
    t->wseq = ++g_wseq;
    t->next = g_wait;
    g_wait = t;
    if (ns == 0) {
        wait_unlink(t);
        t->wkind = W_NONE;
        return R_TIMEOUT;
    }
    uint64_t now = kern_now_ns();
    int forever = ns < 0 || (uint64_t)ns > UINT64_MAX - now;
    uint64_t deadline = forever ? 0 : now + (uint64_t)ns;
    if (g_mspin_ns && (kind == W_LOCK || kind == W_CONDVAR))
        mspin(&t->woke, 1, now + (forever || deadline - now > g_mspin_ns ? g_mspin_ns : deadline - now));
    while (!t->woke) {
        if (forever) {
            plat_cond_wait(&t->cv, &G);
            continue;
        }
        now = kern_now_ns();
        if (now >= deadline) {
            wait_unlink(t);
            t->wkind = W_NONE;
            return R_TIMEOUT;
        }
        uint64_t ms = (deadline - now + 999999) / 1000000;
        plat_cond_wait_ms(&t->cv, &G, ms > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)ms);
    }
    if (t->wake_at) {
        uint64_t d = kern_now_ns() - t->wake_at;
        t->lw_ns += d;
        t->lw_n++;
        if (d > t->lw_max) t->lw_max = d;
        t->wake_at = 0;
    }
    return t->wres;
}

// ---- virtual cores
// Horizon runs at most one thread per core, and TotK keys per-core data (allocators, job queues) by the current core
// number without locks. A guest thread therefore holds one of the three application cores while it runs guest code
// and gives it up only where Horizon would switch threads (waits, sleep, yield, exit, blocking host calls); a free
// core goes to the best waiting thread. A waiter better than a holder asks one holder to give its core up (core_kick),
// which it does at its next SVC or indirect call (kern_checkpoint, rt_dispatch), as Horizon runs the better thread at
// once; guest code without either keeps the core until it reaches one. DKB_FREE_CORES=1 turns the gate off.
static RtThread *g_core_owner[NCORE], *g_core_wait;
static int g_cores_on = -1, g_held_max;

static void diag_init(void);
static void ap_init(void);
static void cw_init(void);

static int cores_enabled(void) {
    if (g_cores_on < 0) {
        g_cores_on = !cfg_bool("DKB_FREE_CORES");   // "0" keeps the gate, like unset
        if (!g_cores_on)
            rt_log("kernel: virtual core gate off (DKB_FREE_CORES): guest threads run without the limit of one per "
                   "core that the game's per-core data needs; for experiments only, the game can fail");
        diag_init();
        ap_init();
        cw_init();
        g_ho_mode = g_cores_on ? rt_knob("SCHED_HANDOFF", "0") : 0;
        kern_sched_handoff = RT_KNOB_ON(g_ho_mode);
        if (g_ho_mode)
            rt_log("kernel: directed core hand-off at SVC return %s (DKB_SCHED_HANDOFF)",
                   g_ho_mode == 2 ? "in the B blocks of DKB_AB" : "on");
        const char *e = cfg_get("DKB_MUTEX_SPIN");
        long us = e && *e ? atol(e) : 0;
        uint32_t cpus = plat_cpu_usable();
        g_mspin_cap = cpus >= 2 ? (int32_t)(cpus / 2) : 0;
        g_mspin_ns = us > 0 && g_mspin_cap > 0 ? (uint64_t)(us > 1000 ? 1000 : us) * 1000 : 0;
        if (g_mspin_ns)
            rt_log("kernel: mutex, condition variable and core waits poll %llu us before they sleep "
                   "(DKB_MUTEX_SPIN, at most %d threads at once on %u logical processors)",
                   (unsigned long long)(g_mspin_ns / 1000), g_mspin_cap, cpus);
    }
    return g_cores_on;
}

static int core_allowed(const RtThread *t, int c) { return (int)((t->mask >> c) & 1); }

// ---- core wait counters (off by default; DKB_CORE_WAITS=1 turns them on, =ab on in the B blocks of DKB_AB only)
// How long ready threads wait for a virtual core, by what holds the cores they may use (CW_*), and how much of it lies
// on the frame's critical path, taken as the main thread and the threads whose signal ends its waits: a waker's own
// core waits, wake-ups and mutex waits count when they fall inside the main thread's wait it ended (one level: the
// waker's own wakers are not followed). Horizon gives a core to a strictly better ready thread at once (KScheduler
// UpdateHighestPriorityThreads and the reschedule interrupt), so CW_WORSE_GAME on that path is time the console does
// not spend; it leaves an equal-priority thread running until it blocks or yields, as core_kick does (CW_EQ). Two clock
// reads per blocking wait (block start, return from the wake-up), one in wake(), two per core wait that does not find
// a free core; everything else is additions under G. DKB_TRACE=perf logs a line per slow frame
// (DKB_PERF_SLOW_MS), the totals every 60 presents and at exit.
enum { CW_REL_YIELD, CW_REL_CHECKPOINT, CW_REL_ASYNC };   // how an owner gave its core up: blocked or yielded, at a
                                                          // checkpoint (SVC or indirect call), interrupted (kern_ap_yield)
static const char *const cw_name[CW_N] = {"worse-game", "worse-host", "equal", "better", "nocore", "wake", "lock",
                                          "worse-waker"};
typedef struct { uint64_t n, ns, max; } CwStat;
typedef struct {
    CwStat all[CW_N], crit[CW_N];   // every thread; the main thread and its wakers
    uint64_t frames, main_ns, handoffs, worse_wake, rel[3];
    uint64_t stops, stop_ns, stop_max, sent, sent_n, sent_ns, sent_max;   // asynchronous preemption: stop and resume
                                                                          // pairs and their system calls, interruptions
                                                                          // sent, sent until the waiter got a core
    struct { uint32_t h; int prio; uint64_t ns; } wk[4];   // the main thread's wakers by the wait they ended
    uint64_t ho_svc, ho_blk, ho_kept, ho_req;   // DKB_SCHED_HANDOFF: cores given at SVC return, as the caller
                                                // blocked; kept by a spin lock or deferred preemption; preempt
                                                // requests pending at SVC return
    uint64_t sp_n[2], sp_ok[2], sp_ns[2];       // DKB_MUTEX_SPIN polls: [0] core waits, [1] mutex and condvar waits;
                                                // those that ended with the core or the wake-up; their time
} CwAcc;
static CwAcc g_cwf, g_cww, g_cwr;   // this frame, this 60-present window, the run
static uint64_t g_cw_epoch, g_cw_main_t0;   // the main thread's current wait: its number and start (0: not waiting)
static int g_kick_op[NCORE], g_put_why;   // core_kick: the holders' priorities; core_put: how the core is given up (CW_REL_*)
static int g_cw_mode = -1;

static void cw_init(void) {   // holding G
    if (g_cw_mode >= 0) return;
    g_cw_mode = rt_knob("CORE_WAITS", "0");
    __atomic_store_n(&g_cw_on, RT_KNOB_ON(g_cw_mode), __ATOMIC_RELAXED);
    if (g_cw_mode)
        rt_log("kernel: core wait counters %s (DKB_CORE_WAITS)", g_cw_mode == 2 ? "in the B blocks of DKB_AB" : "on");
}

static void mspin_count(int woke, int got, uint64_t ns) {   // holding G
    g_cwf.sp_n[woke]++;
    g_cwf.sp_ok[woke] += got;
    g_cwf.sp_ns[woke] += ns;
}

static void cw_stat(CwStat *s, uint64_t d) {
    s->n++;
    s->ns += d;
    if (d > s->max) s->max = d;
}

// One wait of class k on thread t over [a, b] (holding G).
static void cw_add(RtThread *t, int k, uint64_t a, uint64_t b) {
    uint64_t d = b > a ? b - a : 0;
    cw_stat(&g_cwf.all[k], d);
    if (t->is_main) {   // its own mutex waits are what the path is made of, not a part of it
        if (k != CW_LOCK) cw_stat(&g_cwf.crit[k], d);
        return;
    }
    if (!g_cw_main_t0) return;
    if (t->cs_epoch != g_cw_epoch) {
        memset(t->cs_ns, 0, sizeof t->cs_ns);
        memset(t->cs_n, 0, sizeof t->cs_n);
        memset(t->cs_max, 0, sizeof t->cs_max);
        t->cs_epoch = g_cw_epoch;
    }
    uint64_t s = a > g_cw_main_t0 ? a : g_cw_main_t0;   // only the part inside the main thread's wait
    if (b <= s) return;
    t->cs_ns[k] += b - s;
    t->cs_n[k]++;
    if (b - s > t->cs_max[k]) t->cs_max[k] = b - s;
}

// The class of t's core wait, right after core_kick claimed (or did not claim) a holder for it; woke: it follows a
// wake-up, whose waker is t->waker.
static int cw_class(const RtThread *t, int woke) {
    if (t->target && woke && t->target->handle == t->waker) return CW_WORSE_WAKER;
    if (t->target) return __atomic_load_n(&t->target->host_depth, __ATOMIC_RELAXED) > 0 ? CW_WORSE_HOST : CW_WORSE_GAME;
    int worse = 0, eq = 0;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i]) {
            worse |= g_kick_op[i] > t->cw_prio;
            eq |= g_kick_op[i] == t->cw_prio;
        }
    return worse ? CW_NOCORE : eq ? CW_EQ : CW_BETTER;
}

// The main thread blocks (t0) and comes back with its core (now), woken by t->waker or not (holding G): the waker's
// waits inside this one go to the critical path.
static void cw_main_end(RtThread *t, uint64_t t0, uint64_t now) {
    g_cw_main_t0 = 0;
    g_cwf.main_ns += now - t0;
    RtThread *x = t->waker ? h_peek(t->waker, O_THREAD) : NULL;
    if (!x || x == t) return;
    if (x->cs_epoch == g_cw_epoch)
        for (int k = 0; k < CW_N; k++) {
            CwStat *s = &g_cwf.crit[k];
            s->n += x->cs_n[k];
            s->ns += x->cs_ns[k];
            if (x->cs_max[k] > s->max) s->max = x->cs_max[k];
        }
    int i = 0, lo = 0;
    for (; i < 4 && g_cwf.wk[i].h && g_cwf.wk[i].h != x->handle; i++)
        if (g_cwf.wk[i].ns < g_cwf.wk[lo].ns) lo = i;
    if (i == 4) {   // full: the smallest goes if this waker already has more
        if (g_cwf.wk[lo].ns >= now - t0) return;
        i = lo;
        g_cwf.wk[i].ns = 0;
    }
    g_cwf.wk[i].h = x->handle;
    g_cwf.wk[i].prio = x->prio;
    g_cwf.wk[i].ns += now - t0;
}

static void cw_merge(CwAcc *d, const CwAcc *s) {
    for (int k = 0; k < CW_N; k++)
        for (int c = 0; c < 2; c++) {
            CwStat *a = c ? &d->crit[k] : &d->all[k];
            const CwStat *b = c ? &s->crit[k] : &s->all[k];
            a->n += b->n;
            a->ns += b->ns;
            if (b->max > a->max) a->max = b->max;
        }
    d->frames += s->frames;
    d->main_ns += s->main_ns;
    d->handoffs += s->handoffs;
    d->worse_wake += s->worse_wake;
    for (int i = 0; i < 3; i++) d->rel[i] += s->rel[i];
    d->stops += s->stops;
    d->stop_ns += s->stop_ns;
    if (s->stop_max > d->stop_max) d->stop_max = s->stop_max;
    d->sent += s->sent;
    d->sent_n += s->sent_n;
    d->sent_ns += s->sent_ns;
    if (s->sent_max > d->sent_max) d->sent_max = s->sent_max;
    d->ho_svc += s->ho_svc;
    d->ho_blk += s->ho_blk;
    d->ho_kept += s->ho_kept;
    d->ho_req += s->ho_req;
    for (int i = 0; i < 2; i++) {
        d->sp_n[i] += s->sp_n[i];
        d->sp_ok[i] += s->sp_ok[i];
        d->sp_ns[i] += s->sp_ns[i];
    }
}

static int cw_classes(char *s, size_t size, const CwStat *st) {
    int o = 0;
    for (int k = 0; k < CW_N && o < (int)size; k++)
        o += snprintf(s + o, size - o, "%s%s %.2f ms %llu max %.3f", k ? ", " : "", cw_name[k], st[k].ns / 1e6,
                      (unsigned long long)st[k].n, st[k].max / 1e6);
    return o;
}

static void cw_log(const char *what, const CwAcc *a) {
    char all[640], crit[640], ho[400] = "";
    cw_classes(all, sizeof all, a->all);
    cw_classes(crit, sizeof crit, a->crit);
    if (g_ho_mode || g_mspin_ns)
        snprintf(ho, sizeof ho, "; directed hand-offs at SVC return %llu, as the caller blocked %llu, kept by a spin "
                 "lock or a deferred preemption %llu; preempt requests pending at SVC return %llu; polls before sleeping: "
                 "mutex and condvar waits %llu (%llu woken, %.2f ms), core waits %llu (%llu got the core, %.2f ms)",
                 (unsigned long long)a->ho_svc, (unsigned long long)a->ho_blk, (unsigned long long)a->ho_kept,
                 (unsigned long long)a->ho_req, (unsigned long long)a->sp_n[1], (unsigned long long)a->sp_ok[1],
                 a->sp_ns[1] / 1e6, (unsigned long long)a->sp_n[0], (unsigned long long)a->sp_ok[0], a->sp_ns[0] / 1e6);
    rt_log("kernel: core waits %s: %llu frames, main thread waited %.2f ms; critical path: %s; all threads: %s; "
           "lock hand-offs %llu; worse-game and worse-waker waits after a wake-up %llu, ended by the holder blocking or yielding %llu, at "
           "a checkpoint %llu, interrupted %llu; asynchronous preemption: %llu stops, system calls avg %.1f us max %.1f "
           "us, %llu sent, sent to core avg %.1f us max %.1f us (%llu)%s",
           what, (unsigned long long)a->frames, a->main_ns / 1e6, crit, all, (unsigned long long)a->handoffs,
           (unsigned long long)a->worse_wake, (unsigned long long)a->rel[0], (unsigned long long)a->rel[1],
           (unsigned long long)a->rel[2], (unsigned long long)a->stops, a->stops ? a->stop_ns / 1e3 / a->stops : 0.0,
           a->stop_max / 1e3, (unsigned long long)a->sent, a->sent_n ? a->sent_ns / 1e3 / a->sent_n : 0.0,
           a->sent_max / 1e3, (unsigned long long)a->sent_n, ho);
}

void kern_core_waits_frame(uint64_t frame, uint64_t interval_ns, int log) {
    if (g_cw_mode <= 0) {
        if (g_cw_mode < 0) {
            lock();
            cw_init();
            unlock();
        }
        if (!g_cw_mode) {
            if (g_ho_mode == 2) __atomic_store_n(&kern_sched_handoff, rt_ab(), __ATOMIC_RELAXED);
            return;
        }
    }
    CwAcc f, w = {0};
    lock();
    __atomic_store_n(&g_cw_on, RT_KNOB_ON(g_cw_mode), __ATOMIC_RELAXED);
    if (g_ho_mode == 2) __atomic_store_n(&kern_sched_handoff, rt_ab(), __ATOMIC_RELAXED);
    g_cwf.frames = 1;
    f = g_cwf;
    memset(&g_cwf, 0, sizeof g_cwf);
    cw_merge(&g_cww, &f);
    cw_merge(&g_cwr, &f);
    int win = (log & 2) != 0;
    if (win) {
        w = g_cww;
        memset(&g_cww, 0, sizeof g_cww);
    }
    unlock();
    if (log & 1) {
        char what[160];
        int o = snprintf(what, sizeof what, "frame %llu (%.1f ms) main's wakers", (unsigned long long)frame,
                         interval_ns / 1e6);
        for (int i = 0; i < 4 && f.wk[i].h && o < (int)sizeof what; i++)
            o += snprintf(what + o, sizeof what - o, " %#x p%d %.1f", f.wk[i].h, f.wk[i].prio, f.wk[i].ns / 1e6);
        cw_log(what, &f);
    }
    if (win) {
        char what[64];
        snprintf(what, sizeof what, "frames to %llu", (unsigned long long)frame);
        cw_log(what, &w);
        static uint64_t cpu0, up0;   // the process's CPU time over the window (DKB_MUTEX_SPIN spends some)
        uint64_t cpu = plat_process_cpu_ns(), up = plat_process_uptime_ns();
        if (up0 && up > up0)
            rt_log("kernel: process cpu frames to %llu: %.3f s in %.3f s (%.2f host cores)", (unsigned long long)frame,
                   (cpu - cpu0) / 1e9, (up - up0) / 1e9, (double)(cpu - cpu0) / (double)(up - up0));
        cpu0 = cpu;
        up0 = up;
    }
}

void kern_core_waits_exit(void) {   // on an exit path: a lock that stays held does not stop the exit
    if (g_cw_mode <= 0) return;
    int got = 0;
    for (int i = 0; i < 200 && !(got = plat_mutex_trylock(&G)); i++) plat_sleep_ms(1);
    if (!got) return;
    CwAcc r = g_cwr;
    cw_merge(&r, &g_cwf);
    unlock();
    cw_log("run total (exit)", &r);
    rt_log("kernel: process cpu at exit: %.2f s in %.2f s", plat_process_cpu_ns() / 1e9, plat_process_uptime_ns() / 1e9);
}

// ---- asynchronous preemption (DKB_ASYNC_PREEMPT, on unless 0)
// Adapted from MacNativeGames games/totk/runtime/threads.c @ 33e8fa8 (8bf0893, PR #488; MIT, Copyright (c) 2026
// MacNativeGames contributors): Horizon gives a core to a strictly better thread the moment that thread is ready.
// Here a better waiter only marks the owner, which gives the core up at its next SVC or indirect call; an owner in a
// stretch of game code with neither keeps it (DKB_CORE_DIAG: 0.5-1.1 ms of thread-time per frame, waits to 16 ms and
// more). With it on, three parts, all of which MacNativeGames found necessary:
// - The waiter interrupts the owner (plat_intr_stop) when it has waited AP_FIRST_NS, then every
//   millisecond: if the owner's host rip is in the lifted modules' code (g_gen_lo..g_gen_hi) and its host depth is 0
//   (not in a host call or SVC, nor in game code a host call made), it holds no host lock, so its rip is moved to
//   kern_ap_stub, which saves every volatile register, gives the core up like a checkpoint (kern_ap_yield) and jumps
//   back. Otherwise it is only marked, as before.
// - Priority inheritance (eff_prio): a thread owning a guest mutex counts at the priority of the best thread blocked on
//   it, for granting, kicking, preempting and yielding. Without it MacNativeGames' game stopped at its first
//   "Continue" (a better thread took the core of the low-priority owner of a mutex it then waited for).
// - Horizon's preemption disable: the SDK's spin locks (1.2.1 sdk f_70051242a8, f_7005124360 and siblings) count
//   TLR+0x100 (disable_count) while they try or hold the lock, and after clearing it call SVC 0x36 when TLR+0x102
//   (interrupt_flag) is set. Such an owner is not preempted, at a checkpoint either: interrupt_flag is set and the
//   switch happens at that SVC. Preempting it anyway hung MacNativeGames' game at "Continue" (the lock holder lost its
//   core to a thread that then spun on that lock).
// DKB_ASYNC_PREEMPT=0 keeps the old scheduling exactly: raw priorities, no interruption, no disable_count; =2 keeps
// inheritance and the preemption disable without interrupting owners.
#define AP_FIRST_NS 250000ull   // the waiter spins this long before the first interruption (most waits end sooner)
#define AP_GAP_NS 1000000ull    // and interrupts the same owner at most this often
static int g_ap = -1, g_ap_int;   // g_ap_int: interrupt owners (DKB_ASYNC_PREEMPT=2: inheritance and disable only)
static int g_ap_log;              // redirects logged so far (AP_LOG_N)
#define AP_LOG_N 40
static uintptr_t g_gen_lo, g_gen_hi;   // host code of the lifted modules: first and last function start
static uint64_t g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok;
// Redirects that reached kern_ap_yield (taken or not: the waiter may have got a core meanwhile), fault-handler
// redirects, pending ones found lost, and refusals because the owner was stopped inside an exception's dispatch,
// inside a system call, or without the OS saying (platform.h plat_intr_stop).
static uint64_t g_ap_reached, g_ap_lost, g_ap_exc, g_ap_svc, g_ap_unrep, g_ap_fault;
// DKB_AP_SAFE (on unless 0; ab for an in-run A/B): refuse a redirect the OS could lose and expire a pending one that
// was lost. 0 redirects as before. DKB_AP_FAULT (on unless 0; ab for an in-run A/B): when a guest thread has a
// pending preemption while returning from a page fault, redirect it via the fault handler's saved context instead of
// waiting for ap_try between faults.
static int g_ap_safe_mode = 1, g_ap_fault_mode = 1;
#define AP_LOST_NS 2000000ull   // a pending redirect this old is checked for loss
static int g_ap_lost_log, g_ap_unrep_log;
#define AP_LOST_LOG_N 20
void kern_ap_stub(void);
uint64_t kern_ap_rip(void);
void kern_ap_yield(void);
#if defined(_WIN32) && defined(__x86_64__)
static void kern_ap_fault(uint64_t *rip);
#endif

static void ap_init(void) {
    if (g_ap >= 0) return;
    const char *e = cfg_value("DKB_ASYNC_PREEMPT");
    g_ap = strcmp(e, "0") != 0;
    g_ap_int = g_ap && strcmp(e, "2") != 0;
    if (g_ap) {
        RtModule m[16];
        int n = rt_linked_modules(m, 16);
        uintptr_t lo = UINTPTR_MAX, hi = 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < m[i].count; k++) {
                uintptr_t f = (uintptr_t)m[i].fns[k];
                if (f < lo) lo = f;
                if (f > hi) hi = f;
            }
        // The store exclusives must lie outside too: a thread stopped in one holds its granule (cpu.h, excl.c). So
        // must the stub and what it calls before kern_ap_yield clears ap_pending: ap_try takes a rip inside the range
        // as proof that a pending redirect is not on its way (it expires it and may redirect again).
        const uintptr_t host[] = {(uintptr_t)kern_ap_stub, (uintptr_t)kern_ap_rip, (uintptr_t)kern_ap_yield,
                                  (uintptr_t)kern_checkpoint, (uintptr_t)rt_svc, (uintptr_t)rt_dispatch,
                                  (uintptr_t)rt_call, (uintptr_t)excl_store8, (uintptr_t)excl_store16,
                                  (uintptr_t)excl_store32, (uintptr_t)excl_store64, (uintptr_t)excl_store128};
        int bad = !n || lo >= hi;
        for (unsigned i = 0; i < sizeof host / sizeof *host; i++) bad |= host[i] >= lo && host[i] <= hi;
        if (bad) {
            rt_log("kernel: asynchronous preemption off: the lifted code range %#llx-%#llx holds host code",
                   (unsigned long long)lo, (unsigned long long)hi);
            g_ap = 0;
        } else {
            g_gen_lo = lo;
            g_gen_hi = hi;
            kern_host_track = 1;
        }
        g_ap_safe_mode = rt_knob("AP_SAFE", "1");
        if (g_ap_safe_mode != 1)
            rt_log("kernel: redirects the OS could lose are %s (DKB_AP_SAFE)",
                   g_ap_safe_mode ? "refused in the B blocks of DKB_AB only" : "sent as before, and never expired");
        g_ap_fault_mode = rt_knob("AP_FAULT", "1");
        if (g_ap_fault_mode != 1)
            rt_log("kernel: fault-handler preemption redirect %s (DKB_AP_FAULT)",
                   g_ap_fault_mode ? "in the B blocks of DKB_AB only" : "off");
#if defined(_WIN32) && defined(__x86_64__)
        if (g_ap_int && g_ap_fault_mode != 0) plat_mem_fault_ret_hook(kern_ap_fault);
#endif
    }
    rt_log("kernel: asynchronous preemption of virtual cores %s, priority inheritance and preemption disable %s; lifted "
           "code exe+%#llx-exe+%#llx", g_ap_int ? "on" : "off", g_ap ? "on (DKB_ASYNC_PREEMPT=0 turns all off)" : "off",
           (unsigned long long)(g_gen_lo ? g_gen_lo - plat_exe_base() : 0),
           (unsigned long long)(g_gen_hi ? g_gen_hi - plat_exe_base() : 0));
}

// Horizon's priority inheritance: o counts at the priority of the best thread blocked on a guest mutex it owns (the
// owner is read from the mutex word, so it follows hand-offs). Holding G.
static int eff_prio(const RtThread *o) {
    int p = o->prio;
    for (const RtThread *w = g_wait; w; w = w->next)
        if (w->wkind == W_LOCK && w->prio < p && w->waddr &&
            (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle)
            p = w->prio;
    return p;
}

// The priority the virtual cores go by: raw with DKB_ASYNC_PREEMPT=0, as before.
static int prio_of(const RtThread *t) { return g_ap > 0 ? eff_prio(t) : t->prio; }

// TLR+0x100 disable_count, +0x102 interrupt_flag (any thread may read them; the flag is set from outside as the
// kernel does).
static volatile uint16_t *ap_tlr(const RtThread *t) { return (volatile uint16_t *)(t->tlr + 0x100); }
static int ap_disabled(const RtThread *t) { return g_ap > 0 && ap_tlr(t)[0]; }

// kern_ap_stub: entered in place of the interrupted instruction of top-level game code. It keeps every register the
// game code may have live (volatile GPRs, flags, YMM0-15, MXCSR; the C calls preserve the rest), gives the core up
// in kern_ap_yield and jumps back to kern_ap_rip(), whose value it first stores in the return slot it reserved at
// entry, so a stack walk sees the interrupted function as its caller. No ret: the jump keeps call/return pairs
// balanced. The waiter never writes the owner's stack.
#ifdef _WIN32
__asm__(".text\n"
        ".globl kern_ap_stub\n"
        ".def kern_ap_stub; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "kern_ap_stub:\n"
        ".seh_proc kern_ap_stub\n"
        "    leaq -8(%rsp), %rsp\n"   // not sub: the flags are still the interrupted code's until pushfq
        ".seh_stackalloc 8\n"
        "    pushfq\n"
        ".seh_stackalloc 8\n"
        "    pushq %rax\n"
        ".seh_pushreg %rax\n"
        "    pushq %rcx\n"
        ".seh_pushreg %rcx\n"
        "    pushq %rdx\n"
        ".seh_pushreg %rdx\n"
        "    pushq %r8\n"
        ".seh_pushreg %r8\n"
        "    pushq %r9\n"
        ".seh_pushreg %r9\n"
        "    pushq %r10\n"
        ".seh_pushreg %r10\n"
        "    pushq %r11\n"
        ".seh_pushreg %r11\n"
        "    pushq %rbp\n"
        ".seh_pushreg %rbp\n"
        "    movq %rsp, %rbp\n"
        ".seh_setframe %rbp, 0\n"
        ".seh_endprologue\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 0: shadow space, 32: MXCSR saved, 36: MXCSR default, 64: YMM0-15
        "    vmovdqa %ymm0, 64(%rsp)\n"
        "    vmovdqa %ymm1, 96(%rsp)\n"
        "    vmovdqa %ymm2, 128(%rsp)\n"
        "    vmovdqa %ymm3, 160(%rsp)\n"
        "    vmovdqa %ymm4, 192(%rsp)\n"
        "    vmovdqa %ymm5, 224(%rsp)\n"
        "    vmovdqa %ymm6, 256(%rsp)\n"
        "    vmovdqa %ymm7, 288(%rsp)\n"
        "    vmovdqa %ymm8, 320(%rsp)\n"
        "    vmovdqa %ymm9, 352(%rsp)\n"
        "    vmovdqa %ymm10, 384(%rsp)\n"
        "    vmovdqa %ymm11, 416(%rsp)\n"
        "    vmovdqa %ymm12, 448(%rsp)\n"
        "    vmovdqa %ymm13, 480(%rsp)\n"
        "    vmovdqa %ymm14, 512(%rsp)\n"
        "    vmovdqa %ymm15, 544(%rsp)\n"
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        "    vzeroupper\n"
        "    call kern_ap_rip\n"
        "    movq %rax, 72(%rbp)\n"   // the return slot: rbp + 8 pushes + pushfq
        "    call kern_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        "    vmovdqa 64(%rsp), %ymm0\n"
        "    vmovdqa 96(%rsp), %ymm1\n"
        "    vmovdqa 128(%rsp), %ymm2\n"
        "    vmovdqa 160(%rsp), %ymm3\n"
        "    vmovdqa 192(%rsp), %ymm4\n"
        "    vmovdqa 224(%rsp), %ymm5\n"
        "    vmovdqa 256(%rsp), %ymm6\n"
        "    vmovdqa 288(%rsp), %ymm7\n"
        "    vmovdqa 320(%rsp), %ymm8\n"
        "    vmovdqa 352(%rsp), %ymm9\n"
        "    vmovdqa 384(%rsp), %ymm10\n"
        "    vmovdqa 416(%rsp), %ymm11\n"
        "    vmovdqa 448(%rsp), %ymm12\n"
        "    vmovdqa 480(%rsp), %ymm13\n"
        "    vmovdqa 512(%rsp), %ymm14\n"
        "    vmovdqa 544(%rsp), %ymm15\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 8(%rsp), %rsp\n"
        "    jmpq *-8(%rsp)\n"
        ".seh_endproc\n");
#elif defined(__aarch64__) && !defined(__APPLE__)
#error "kern_ap_stub: the AArch64 version is written for Mach-O (macOS symbol names, plat_intr_return)"
#elif defined(__aarch64__)
// The AArch64 version, macOS. Apple's arm64 ABI keeps a 128-byte red zone below sp (Clang puts the locals of leaf functions
// there, and lifted functions are leaves), so the frame goes below it. The frame is a PlatIntrCtx: every general
// register, sp, the interrupted pc, NZCV, FPCR, FPSR and all 32 SIMD registers at their full 128 bits. AArch64 has no
// jump through memory and the interrupted code may have any register live, x30 included (a leaf keeps its return
// address there), so the stub does not jump back itself: plat_intr_return has the OS load the whole frame.
_Static_assert(offsetof(PlatIntrCtx, sp) == 248 && offsetof(PlatIntrCtx, pc) == 256 &&
                   offsetof(PlatIntrCtx, nzcv) == 264 && offsetof(PlatIntrCtx, fpcr) == 272 &&
                   offsetof(PlatIntrCtx, q) == 288 && sizeof(PlatIntrCtx) == 800,
               "kern_ap_stub frame layout");
__asm__(".text\n"
        ".globl _kern_ap_stub\n"
        ".p2align 4\n"
        "_kern_ap_stub:\n"
        "    sub sp, sp, #928\n"       // 128 red zone + 800 PlatIntrCtx
        "    stp x0, x1, [sp, #0]\n"
        "    stp x2, x3, [sp, #16]\n"
        "    stp x4, x5, [sp, #32]\n"
        "    stp x6, x7, [sp, #48]\n"
        "    stp x8, x9, [sp, #64]\n"
        "    stp x10, x11, [sp, #80]\n"
        "    stp x12, x13, [sp, #96]\n"
        "    stp x14, x15, [sp, #112]\n"
        "    stp x16, x17, [sp, #128]\n"
        "    stp x18, x19, [sp, #144]\n"
        "    stp x20, x21, [sp, #160]\n"
        "    stp x22, x23, [sp, #176]\n"
        "    stp x24, x25, [sp, #192]\n"
        "    stp x26, x27, [sp, #208]\n"
        "    stp x28, x29, [sp, #224]\n"
        "    str x30, [sp, #240]\n"
        "    add x9, sp, #928\n"
        "    str x9, [sp, #248]\n"     // the interrupted sp
        "    mrs x9, nzcv\n"
        "    str x9, [sp, #264]\n"
        "    mrs x9, fpcr\n"
        "    mrs x10, fpsr\n"
        "    stp x9, x10, [sp, #272]\n"
        "    stp q0, q1, [sp, #288]\n"
        "    stp q2, q3, [sp, #320]\n"
        "    stp q4, q5, [sp, #352]\n"
        "    stp q6, q7, [sp, #384]\n"
        "    stp q8, q9, [sp, #416]\n"
        "    stp q10, q11, [sp, #448]\n"
        "    stp q12, q13, [sp, #480]\n"
        "    stp q14, q15, [sp, #512]\n"
        "    stp q16, q17, [sp, #544]\n"
        "    stp q18, q19, [sp, #576]\n"
        "    stp q20, q21, [sp, #608]\n"
        "    stp q22, q23, [sp, #640]\n"
        "    stp q24, q25, [sp, #672]\n"
        "    stp q26, q27, [sp, #704]\n"
        "    stp q28, q29, [sp, #736]\n"
        "    stp q30, q31, [sp, #768]\n"
        "    bl _kern_ap_rip\n"
        "    str x0, [sp, #256]\n"     // the interrupted pc
        "    bl _kern_ap_yield\n"
        "    mov x0, sp\n"
        "    bl _plat_intr_return\n"
        "    brk #0x1\n");
#else
// The System V version: rsi and rdi are volatile there too, and the interrupted code may keep data in the 128 bytes
// below its stack pointer (the red zone), so the return slot and the saves go below that. Without a red zone in the
// way the slot is at entry - 136; the exit restores rsp to the entry value and jumps through the slot.
__asm__(".text\n"
        ".globl kern_ap_stub\n"
        ".type kern_ap_stub, @function\n"
        ".p2align 4\n"
        "kern_ap_stub:\n"
        "    .cfi_startproc\n"
        "    leaq -136(%rsp), %rsp\n"   // not sub: the flags are still the interrupted code's until pushfq
        "    pushfq\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %rsi\n"
        "    pushq %rdi\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 32: MXCSR saved, 36: MXCSR default, 64: YMM0-15
        "    vmovdqa %ymm0, 64(%rsp)\n"
        "    vmovdqa %ymm1, 96(%rsp)\n"
        "    vmovdqa %ymm2, 128(%rsp)\n"
        "    vmovdqa %ymm3, 160(%rsp)\n"
        "    vmovdqa %ymm4, 192(%rsp)\n"
        "    vmovdqa %ymm5, 224(%rsp)\n"
        "    vmovdqa %ymm6, 256(%rsp)\n"
        "    vmovdqa %ymm7, 288(%rsp)\n"
        "    vmovdqa %ymm8, 320(%rsp)\n"
        "    vmovdqa %ymm9, 352(%rsp)\n"
        "    vmovdqa %ymm10, 384(%rsp)\n"
        "    vmovdqa %ymm11, 416(%rsp)\n"
        "    vmovdqa %ymm12, 448(%rsp)\n"
        "    vmovdqa %ymm13, 480(%rsp)\n"
        "    vmovdqa %ymm14, 512(%rsp)\n"
        "    vmovdqa %ymm15, 544(%rsp)\n"
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        "    vzeroupper\n"
        "    call kern_ap_rip\n"
        "    movq %rax, 88(%rbp)\n"   // the return slot: rbp + 10 pushes + pushfq
        "    call kern_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        "    vmovdqa 64(%rsp), %ymm0\n"
        "    vmovdqa 96(%rsp), %ymm1\n"
        "    vmovdqa 128(%rsp), %ymm2\n"
        "    vmovdqa 160(%rsp), %ymm3\n"
        "    vmovdqa 192(%rsp), %ymm4\n"
        "    vmovdqa 224(%rsp), %ymm5\n"
        "    vmovdqa 256(%rsp), %ymm6\n"
        "    vmovdqa 288(%rsp), %ymm7\n"
        "    vmovdqa 320(%rsp), %ymm8\n"
        "    vmovdqa 352(%rsp), %ymm9\n"
        "    vmovdqa 384(%rsp), %ymm10\n"
        "    vmovdqa 416(%rsp), %ymm11\n"
        "    vmovdqa 448(%rsp), %ymm12\n"
        "    vmovdqa 480(%rsp), %ymm13\n"
        "    vmovdqa 512(%rsp), %ymm14\n"
        "    vmovdqa 544(%rsp), %ymm15\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdi\n"
        "    popq %rsi\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 136(%rsp), %rsp\n"
        "    jmpq *-136(%rsp)\n"
        "    .cfi_endproc\n"
        ".size kern_ap_stub, .-kern_ap_stub\n");
#endif

enum { AP_NONE, AP_RETRY, AP_SENT };

// The waiter w interrupts its target owner if that runs top-level game code (holding G; the owner is stopped only
// between plat_intr_stop and plat_intr_resume, during which nothing here takes a lock).
// A redirect the OS can lose (DKB_AP_SAFE): Windows reports an owner stopped inside an exception's dispatch, where a
// context write is overwritten when the handler continues (platform/win32.c plat_intr_stop), and ap_pending is cleared
// only in kern_ap_yield, so one lost redirect used to leave its owner uninterruptible for good: every later ap_try
// returned at ap_pending, and its waiters woke each millisecond and gave up without a trace. Such stops are refused now
// (the waiter tries again a millisecond later), and a redirect still pending after AP_LOST_NS is expired as lost at
// the next stop that finds the owner's rip in lifted code, whatever lost it. That is safe: kern_ap_stub, kern_ap_rip
// and kern_ap_yield, which clears ap_pending, lie outside the lifted range (ap_init checks it), so an owner running
// lifted code is not on its way to the stub and cannot take the old redirect after the new one; ap_rip is only ever
// written there, so the stub never jumps back through a value meant for another redirect.
static int ap_try(RtThread *w) {
    RtThread *o = w->target;
    if (!o || o->cur_core < 0 || !o->preempt) return AP_NONE;
    if (!o->hthr) return AP_RETRY;
    int safe = RT_KNOB_ON(g_ap_safe_mode);
    uint64_t now = kern_now_ns();
    int stale = 0;   // a pending redirect old enough to be checked for loss
    if (o->ap_pending) {
        if (!safe || now - o->ap_sent_at < AP_LOST_NS) return AP_RETRY;
        stale = 1;
    }
    volatile uint16_t *tl = ap_tlr(o);
    if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0) {
        g_ap_host++;
        return AP_RETRY;
    }
    if (tl[0]) {   // preemption disabled: SVC 0x36 switches
        tl[1] = 1;
        g_ap_deferred++;
        return AP_RETRY;
    }
    if (o->ap_last && now - o->ap_last < AP_GAP_NS) return AP_RETRY;
    o->ap_last = now;
    uint64_t rip, rsp;
    int st = plat_intr_stop(o->hthr, &rip, &rsp);
    if (!st) {
        g_ap_fail++;
        return AP_RETRY;
    }
    int r = AP_RETRY, in_gen = rip >= g_gen_lo && rip <= g_gen_hi;
    int lost_log = 0, unrep_log = 0;
    uint32_t lost_h = 0;
    int lost_prio = 0;
    double lost_age_ms = 0;
    uint64_t lost_old_rip = 0, lost_cur_rip = 0;
    uint64_t to = 0;
    if (stale && in_gen) {
        g_ap_lost++;
        if (g_ap_lost_log < AP_LOST_LOG_N) {
            g_ap_lost_log++;
            lost_log = 1;
            lost_h = o->handle;
            lost_prio = o->prio;
            lost_age_ms = (now - o->ap_sent_at) / 1e6;
            lost_old_rip = o->ap_rip;
            lost_cur_rip = rip;
        }
        o->ap_pending = 0;
    }
    if (!in_gen)
        g_ap_pc++;
    else if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0)
        g_ap_host++;
    else if (tl[0]) {
        tl[1] = 1;
        g_ap_deferred++;
    } else if (safe && st == PLAT_INTR_IN_EXCEPTION)
        g_ap_exc++;
    else if (safe && st == PLAT_INTR_IN_SERVICE)
        g_ap_svc++;
    else if (safe && st == PLAT_INTR_UNREPORTED) {
        g_ap_unrep++;
        if (!g_ap_unrep_log) {
            g_ap_unrep_log = 1;
            unrep_log = 1;
        }
    } else {
        o->ap_rip = rip;
        o->ap_pending = 1;
        o->ap_sent_at = now;
        to = (uint64_t)(uintptr_t)kern_ap_stub;
    }
    if (!plat_intr_resume(o->hthr, to)) {
        o->ap_pending = 0;
        g_ap_fail++;
    } else if (to) {
        g_ap_sent++;
        r = AP_SENT;
    }
    if (lost_log)
        rt_log("kernel: the redirect of %#x p%d sent %.2f ms ago at exe+%#llx was lost (it runs at exe+%#llx)",
               lost_h, lost_prio, lost_age_ms, (unsigned long long)(lost_old_rip - plat_exe_base()),
               (unsigned long long)(lost_cur_rip - plat_exe_base()));
    if (unrep_log)
        rt_log("kernel: asynchronous preemption redirect refused because the OS did not set "
               "CONTEXT_EXCEPTION_REPORTING (e.g. Wine); set DKB_AP_SAFE=0 to redirect anyway");
    if (g_cw_on) {   // core wait counters: the cost of a stop and resume, and when the first interruption went out
        uint64_t d = kern_now_ns() - now;
        g_cwf.stops++;
        g_cwf.stop_ns += d;
        if (d > g_cwf.stop_max) g_cwf.stop_max = d;
        if (r == AP_SENT) {
            g_cwf.sent++;
            if (!w->cw_ap_at) w->cw_ap_at = now;
        }
    }
    if (r == AP_SENT && g_ap_log < AP_LOG_N) {
        g_ap_log++;
        rt_log("kernel: interrupt %#x p%d at exe+%#llx rsp %#llx for %#x p%d", o->handle, o->prio,
               (unsigned long long)(o->ap_rip - plat_exe_base()), (unsigned long long)rsp, w->handle, w->prio);
    }
    return r;
}

// ---- DKB_CORE_DIAG=1: why ready threads wait for a virtual core, and for how long
// Each core wait is split into classes: the owner asked to give the core up ran game code (DG_GAME, the part an
// asynchronous preemption would remove), was in a host call or SVC that kept the core (DG_HOST), every owner the waiter
// may displace was of better or equal priority (DG_EQ: Horizon waits too), or the worse owners were claimed by better
// waiters (DG_NOCORE). Per 60 presents: time per frame and a wait-length histogram per class, the owners that cost
// most; a line per present over 25 ms; a line per wait with over 2 ms of DG_GAME (where the owner reached its
// checkpoint), and one host RIP sample of the owner per such wait (exe RVA, for dkb.map).
enum { DG_GAME, DG_HOST, DG_EQ, DG_NOCORE, DG_N };
static const char *const dg_name[DG_N] = {"game", "host", "eq", "nocore"};
#define DG_NB 8   // wait lengths: < 0.05, 0.25, 1, 2, 4, 8, 16 ms, longer
static const uint64_t dg_edge[DG_NB - 1] = {50000, 250000, 1000000, 2000000, 4000000, 8000000, 16000000};
#define DG_LONG_LINES 400
#define DG_SAMPLES 600
#define DG_OWN 32
int kern_host_track;
static int g_diag = -1;
typedef struct {
    uint64_t ns[DG_N], n[DG_N], hist[DG_N][DG_NB];                    // this window
    struct { uint32_t h; int prio, cls; uint64_t ns; } own[DG_OWN];   // this window: owners by class
    uint64_t long_frames, long_game_frames;
} DgWindow;
static DgWindow g_dgw;
static uint64_t g_dgf[DG_N], g_dgfr[DG_N];   // this frame: all threads, the render thread
static uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;

static void diag_init(void) {
    if (g_diag >= 0) return;
    const char *e = cfg_get("DKB_CORE_DIAG");
    g_diag = e && *e && strcmp(e, "0");
    if (g_diag) {
        kern_host_track = 1;
        rt_log("core-diag: on (DKB_CORE_DIAG)");
    }
}

int kern_core_diag_on(void) { return g_diag > 0; }

// A tracked host call or SVC returned to depth 0 with a preempt request pending (host.h); on the thread itself.
void kern_host_left(void *thread) {
    RtThread *t = thread;
    if (g_diag > 0 && t->dg_kick && t->dg_host_kick && !t->dg_host_end) t->dg_host_end = kern_now_ns();
}

static int dg_in_host(const RtThread *o) { return __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0; }

static void dg_add(const RtThread *w, int cls, uint64_t ns, uint32_t oh, int oprio) {
    if (!ns) return;
    g_dgw.ns[cls] += ns;
    g_dgf[cls] += ns;
    if (w == g_render) g_dgfr[cls] += ns;
    if (!oh) return;
    int i = 0;
    for (; i < DG_OWN && g_dgw.own[i].ns; i++)
        if (g_dgw.own[i].h == oh && g_dgw.own[i].cls == cls) break;
    if (i == DG_OWN) return;
    g_dgw.own[i].h = oh;
    g_dgw.own[i].prio = oprio;
    g_dgw.own[i].cls = cls;
    g_dgw.own[i].ns += ns;
}

// The waiter's class at the start of its wait, after core_kick claimed (or did not claim) an owner for it.
static int dg_class0(const RtThread *t) {
    if (t->target) return dg_in_host(t->target) ? DG_HOST : DG_GAME;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) return DG_NOCORE;
    return DG_EQ;
}

// The waiter got a core: split [dg_since, now] by what the owner that released it did after its preempt request.
static void dg_done(RtThread *t) {
    uint64_t now = kern_now_ns(), t0 = t->dg_since, w = now - t0;
    t->dg_since = 0;
    if (!t0) return;
    const struct DgRel *r = &t->dg_rel;
    uint64_t part[DG_N] = {0};
    uint32_t oh = t->target ? t->target->handle : r->h;
    int oprio = t->target ? t->target->prio : r->prio;
    if (r->pre && r->kick) {
        uint64_t k = r->kick > t0 ? (r->kick < now ? r->kick : now) : t0;
        part[t->dg_cls] += k - t0;
        if (r->host) {
            uint64_t he = r->host_end > k && r->host_end < now ? r->host_end : now;
            part[DG_HOST] += he - k;
            part[DG_GAME] += now - he;
        } else
            part[DG_GAME] += now - k;
        oh = r->h;
        oprio = r->prio;
    } else
        part[t->dg_cls] += w;
    int top = 0;
    for (int c = 0; c < DG_N; c++) {
        dg_add(t, c, part[c], oh, oprio);
        if (part[c] > part[top]) top = c;
    }
    int b = 0;
    while (b < DG_NB - 1 && w >= dg_edge[b]) b++;
    g_dgw.n[top]++;
    g_dgw.hist[top][b]++;
    if ((part[DG_GAME] >= 2000000 || part[DG_HOST] >= 2000000) && g_dg_lines < DG_LONG_LINES) {
        g_dg_lines++;
        char a[128], l[128];
        // An owner interrupted by kern_ap_stub gave its core up at a host rip in lifted code (an exe RVA); its guest pc
        // there is the one of its last dispatch, so it is not shown.
        if (r->hrip)
            snprintf(a, sizeof a, "exe+%#llx (interrupted)", (unsigned long long)(r->hrip - plat_exe_base()));
        rt_log("core-diag: wait %.2f ms (game %.2f host %.2f): %#x p%d mask %llx; released by %#x p%d%s at %s lr %s",
               w / 1e6, part[DG_GAME] / 1e6, part[DG_HOST] / 1e6, t->handle, t->prio, (unsigned long long)t->mask, r->h,
               r->prio, r->pre ? " (preempted)" : "", r->hrip ? a : r->pc ? ldr_describe(r->pc, a, sizeof a) : "-",
               r->lr ? ldr_describe(r->lr, l, sizeof l) : "-");
    }
    t->target = NULL;
    memset(&t->dg_rel, 0, sizeof t->dg_rel);
}

// Once per long DG_GAME or DG_HOST wait: where the owner's host thread runs (module and offset), its latest SVC and
// its guest lr. Holding G; the owner is resumed before anything that could take a lock it holds.
static void dg_sample(RtThread *t) {
    RtThread *o = t->target;
    if (!o || o->cur_core < 0 || !o->hthr || g_dg_samples >= DG_SAMPLES) return;
    uint64_t rip, rsp;
    if (!plat_intr_stop(o->hthr, &rip, &rsp)) return;
    int depth = __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED);
    plat_intr_resume(o->hthr, 0);
    g_dg_samples++;
    char mn[256] = "?";
    uintptr_t m = 0;
    plat_module_of((uintptr_t)rip, mn, sizeof mn, &m);
    char lr[128];
    rt_log("core-diag: sample owner %#x p%d depth %d rip %s+%#llx, last svc %#x, guest lr %s (waiter %#x p%d, %s, "
           "%.2f ms)", o->handle, o->prio, depth, mn, (unsigned long long)(rip - m), o->dg_svc,
           o->cur ? ldr_describe(o->cur->x[30], lr, sizeof lr) : "-", t->handle, t->prio,
           t->dg_cls == DG_HOST ? "host" : "game", (kern_now_ns() - t->dg_since) / 1e6);
}


// perf_present, every present: a line for a present over 25 ms, the 60-present summary.
// Stall dump (DKB_STALL_DUMP=<seconds>, default 5; 0 turns it off): a watchdog thread logs the scheduler state when no
// frame has been presented for that long, then every 10 s while the stall lasts: who holds each virtual core and where
// its host thread is, who waits for a core, and every other guest thread's wait, with priorities, core masks and
// pending preempt requests. A hang then leaves in the log what each thread was doing, which a hung process killed from
// outside does not. The dump only reads under the scheduler lock, and reports a lock it cannot get within 200 ms.
static volatile uint64_t g_stall_last_ns, g_stall_last_frame;

static void stall_line(const char *what, const RtThread *t) {
    const Cpu *c = t->cur;
    rt_log("stall: %s %#x p%d (now %d) mask %#llx core %d preempt %d host_depth %d wkind %d waddr %#llx wobj %#llx "
           "target %#x pc %#llx lr %#llx loop %#llx",
           what, t->handle, t->prio, prio_of(t), (unsigned long long)t->mask, t->cur_core, t->preempt, t->host_depth,
           t->wkind, (unsigned long long)t->waddr, (unsigned long long)t->wobj, t->target ? t->target->handle : 0,
           (unsigned long long)(c ? c->pc : 0), (unsigned long long)(c ? c->x[30] : 0), (unsigned long long)t->sp_site);
}

static void stall_dump(uint64_t idle_ns) {
    int got = 0;
    for (int i = 0; i < 200 && !(got = plat_mutex_trylock(&G)); i++) plat_sleep_ms(1);
    if (!got) {
        rt_log("stall: no frame for %.1f s; the scheduler lock stayed held for 200 ms: no dump", idle_ns / 1e9);
        return;
    }
    rt_log("stall: no frame for %.1f s after frame %llu", idle_ns / 1e9, (unsigned long long)g_stall_last_frame);
    char what[32];
    for (int i = 0; i < NCORE; i++) {
        RtThread *o = g_core_owner[i];
        if (!o) {
            rt_log("stall: core %d free", i);
            continue;
        }
        snprintf(what, sizeof what, "core %d owner", i);
        stall_line(what, o);
        // Where its host thread is now (sampled twice, 1 ms apart), and what asynchronous preemption would see there:
        // the preemption disable count, a redirect still pending, whether the rip is in lifted code.
        uint64_t rip[2] = {0, 0}, rsp;
        for (int k = 0; k < 2 && o->hthr; k++) {
            if (k) plat_sleep_ms(1);
            if (plat_intr_stop(o->hthr, &rip[k], &rsp)) plat_intr_resume(o->hthr, 0);
        }
        uintptr_t b = plat_exe_base();
        rt_log("stall:   host rip exe+%#llx, exe+%#llx (lifted code %d, %d); disable count %u, redirect pending %d",
               (unsigned long long)(rip[0] - b), (unsigned long long)(rip[1] - b),
               rip[0] >= g_gen_lo && rip[0] <= g_gen_hi, rip[1] >= g_gen_lo && rip[1] <= g_gen_hi,
               o->tlr ? ap_tlr(o)[0] : 0, o->ap_pending);
    }
    rt_log("stall: asynchronous preemption: %llu interrupted, %llu given up there; refused %llu in a host call, %llu "
           "outside game code, %llu disabled, %llu failed, %llu in an exception's dispatch, %llu in a system call, %llu "
           "unreported; %llu reached the stub, %llu redirected in fault handler, %llu lost",
           (unsigned long long)g_ap_sent, (unsigned long long)g_ap_taken, (unsigned long long)g_ap_host,
           (unsigned long long)g_ap_pc, (unsigned long long)g_ap_deferred, (unsigned long long)g_ap_fail,
           (unsigned long long)g_ap_exc, (unsigned long long)g_ap_svc, (unsigned long long)g_ap_unrep,
           (unsigned long long)g_ap_reached, (unsigned long long)g_ap_fault, (unsigned long long)g_ap_lost);
    int k = 0;
    for (const RtThread *w = g_core_wait; w; w = w->cnext) {
        snprintf(what, sizeof what, "core waiter %d", k++);
        stall_line(what, w);
    }
    for (const RtThread *t = g_all; t; t = t->all) {
        if (t->exited || !t->started || t->cur_core >= 0) continue;
        int queued = 0;
        for (const RtThread *w = g_core_wait; w && !queued; w = w->cnext) queued = w == t;
        if (!queued) stall_line("blocked", t);
    }
    unlock();
}

// A one-off report from a guest thread (runtime/patches.c edge_report): who holds each virtual core, with priority,
// core mask and where its host thread runs, to name a thread that ran beside the caller. Each owner is stopped once
// under G for its rip and resumed at once; the lines are logged after G is released, so a sampled thread that holds the
// log's lock cannot block the report. The calling thread is listed without a sample.
void kern_cores_report(const char *tag) {
    struct { uint32_t h; int prio, now, self, sampled; uint64_t mask, rip, lr; } o[NCORE];
    int got = 0;
    for (int i = 0; i < 200 && !(got = plat_mutex_trylock(&G)); i++) plat_sleep_ms(1);
    if (!got) {
        rt_log("%s: the scheduler lock stayed held for 200 ms: no core report", tag);
        return;
    }
    for (int i = 0; i < NCORE; i++) {
        RtThread *t = g_core_owner[i];
        memset(&o[i], 0, sizeof o[i]);
        if (!t) continue;
        o[i].h = t->handle, o[i].prio = t->prio, o[i].now = prio_of(t), o[i].mask = t->mask, o[i].self = t == tcur;
        o[i].lr = t->cur ? t->cur->x[30] : 0;
        uint64_t rsp;
        if (!o[i].self && t->hthr && plat_intr_stop(t->hthr, &o[i].rip, &rsp)) {
            plat_intr_resume(t->hthr, 0);
            o[i].sampled = 1;
        }
    }
    unlock();
    uintptr_t b = plat_exe_base();
    char lr[128];
    for (int i = 0; i < NCORE; i++) {
        if (!o[i].h) {
            rt_log("%s: core %d free", tag, i);
            continue;
        }
        rt_log("%s: core %d owner %#x p%d (now %d) mask %#llx%s host rip %s%#llx%s, guest lr %s", tag, i, o[i].h,
               o[i].prio, o[i].now, (unsigned long long)o[i].mask, o[i].self ? " (this thread)" : "",
               o[i].sampled ? "exe+" : "", (unsigned long long)(o[i].sampled ? o[i].rip - b : 0),
               o[i].sampled ? (o[i].rip >= g_gen_lo && o[i].rip <= g_gen_hi ? " (lifted code)" : "") : " (not sampled)",
               o[i].lr ? ldr_describe(o[i].lr, lr, sizeof lr) : "-");
    }
}

// The DKB_ASYNC_PREEMPT counters since the start: copied holding G (ap_snap), logged without it (ap_log), at every
// 600th present and at exit.
#define AP_SNAP_N 14
static void ap_snap(uint64_t v[AP_SNAP_N]) {
    const uint64_t s[AP_SNAP_N] = {g_ap_sent, g_ap_taken,   g_ap_host, g_ap_pc,  g_ap_deferred, g_ap_fail,  g_ap_spin_ok,
                                   g_sync36,  g_ap_reached, g_ap_exc,  g_ap_svc, g_ap_unrep,    g_ap_fault, g_ap_lost};
    memcpy(v, s, sizeof s);
}

static void ap_log(const char *when, const uint64_t v[AP_SNAP_N]) {
    rt_log("kernel: %s: asynchronous preemption: %llu interrupted, %llu cores given up there; refused: %llu in a host "
           "call, %llu outside game code, %llu preemption disabled, %llu failed; %llu waits ended in the first spin; "
           "SVC 0x36 %llu; %llu reached the stub; %llu redirected in fault handler; refused %llu in an exception's "
           "dispatch, %llu in a system call, %llu unreported; %llu lost",
           when, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
           (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
           (unsigned long long)v[7], (unsigned long long)v[8], (unsigned long long)v[12], (unsigned long long)v[9],
           (unsigned long long)v[10], (unsigned long long)v[11], (unsigned long long)v[13]);
}

void kern_ap_exit(void) {   // on an exit path: a lock that stays held does not stop the exit
    if (g_ap <= 0) return;
    int got = 0;
    for (int i = 0; i < 200 && !(got = plat_mutex_trylock(&G)); i++) plat_sleep_ms(1);
    if (!got) return;
    uint64_t v[AP_SNAP_N];
    ap_snap(v);
    unlock();
    ap_log("exit", v);
}

static void stall_main(void *arg) {
    uint64_t lim = (uint64_t)(uintptr_t)arg * 1000000000ull, next = 0;
    for (;;) {
        plat_sleep_ms(500);
        uint64_t last = __atomic_load_n(&g_stall_last_ns, __ATOMIC_RELAXED), now = kern_now_ns();
        if (now - last < lim) {
            next = 0;
            continue;
        }
        if (next && now < next) continue;
        stall_dump(now - last);
        next = now + 10000000000ull;
    }
}

void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns) {
    __atomic_store_n(&g_stall_last_ns, kern_now_ns(), __ATOMIC_RELAXED);
    g_stall_last_frame = frame;
    static int stall_started;
    if (!stall_started && frame >= 60) {   // after the first frames: the boot's long loads are not stalls
        stall_started = 1;
        int sec = cfg_int("DKB_STALL_DUMP");
        if (sec > 0 && !plat_thread_spawn(stall_main, (void *)(uintptr_t)sec, "stall watch", 0))
            rt_log("kernel: DKB_STALL_DUMP: the watchdog thread could not start");
    }
    if (g_ap > 0 && frame && frame % 600 == 0) {   // DKB_ASYNC_PREEMPT counters since the start
        uint64_t v[AP_SNAP_N];
        char when[32];
        lock();
        ap_snap(v);
        unlock();
        snprintf(when, sizeof when, "frame %llu", (unsigned long long)frame);
        ap_log(when, v);
    }
    if (g_diag <= 0) return;
    DgWindow w;
    uint64_t f[DG_N], fr[DG_N], s36, s36f;
    lock();
    g_render = tcur;
    memcpy(f, g_dgf, sizeof f);
    memcpy(fr, g_dgfr, sizeof fr);
    memset(g_dgf, 0, sizeof g_dgf);
    memset(g_dgfr, 0, sizeof g_dgfr);
    int lng = interval_ns > 25000000ull;
    if (lng) {
        g_dgw.long_frames++;
        if (f[DG_GAME] > 1000000) g_dgw.long_game_frames++;
    }
    int rep = frame && frame % 60 == 0;
    if (rep) {
        w = g_dgw;
        memset(&g_dgw, 0, sizeof g_dgw);
    }
    s36 = g_sync36;
    s36f = g_sync36_flag;
    unlock();
    if (lng)
        rt_log("core-diag: frame %llu: %.1f ms; core wait game %.2f host %.2f eq %.2f nocore %.2f ms; render thread game "
               "%.2f host %.2f eq %.2f nocore %.2f ms",
               (unsigned long long)frame, interval_ns / 1e6, f[0] / 1e6, f[1] / 1e6, f[2] / 1e6, f[3] / 1e6,
               fr[0] / 1e6, fr[1] / 1e6, fr[2] / 1e6, fr[3] / 1e6);
    if (!rep) return;
    char s[1024];
    int o = 0;
    for (int c = 0; c < DG_N; c++) {
        o += snprintf(s + o, sizeof s - o, "%s %.3f ms/frame %llu waits [", dg_name[c], w.ns[c] / 60e6,
                      (unsigned long long)w.n[c]);
        for (int b = 0; b < DG_NB; b++)
            o += snprintf(s + o, sizeof s - o, "%s%llu", b ? " " : "", (unsigned long long)w.hist[c][b]);
        o += snprintf(s + o, sizeof s - o, "]%s", c < DG_N - 1 ? "; " : "");
    }
    rt_log("core-diag: frames %llu-%llu: %s; long frames %llu (game wait > 1 ms in %llu); SVC 0x36 %llu (flag set %llu)",
           (unsigned long long)frame - 59, (unsigned long long)frame, s, (unsigned long long)w.long_frames,
           (unsigned long long)w.long_game_frames, (unsigned long long)s36, (unsigned long long)s36f);
    o = 0;
    for (int k = 0; k < 6; k++) {
        int bi = -1;
        for (int i = 0; i < DG_OWN; i++)
            if (w.own[i].ns && (bi < 0 || w.own[i].ns > w.own[bi].ns)) bi = i;
        if (bi < 0) break;
        o += snprintf(s + o, sizeof s - o, " %#x p%d %s %.3f;", w.own[bi].h, w.own[bi].prio, dg_name[w.own[bi].cls],
                      w.own[bi].ns / 60e6);
        w.own[bi].ns = 0;
    }
    if (o) rt_log("core-diag: owners (ms/frame):%s", s);
}

static void core_count(void) {
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    if (n > g_held_max) g_held_max = n;
}

static struct DgRel g_dg_rel;   // DKB_CORE_DIAG: the owner core_put is releasing, for core_grant

static void core_grant(int c) {
    RtThread *b = NULL, **bp = NULL;
    int bpri = 0;
    for (RtThread **p = &g_core_wait; *p; p = &(*p)->cnext) {
        if (!core_allowed(*p, c)) continue;
        int pr = prio_of(*p);
        if (!b || pr < bpri || (pr == bpri && (*p)->cseq < b->cseq)) {
            b = *p;
            bp = p;
            bpri = pr;
        }
    }
    if (!b) return;
    *bp = b->cnext;
    b->cnext = NULL;
    b->cur_core = c;
    g_core_owner[c] = b;
    core_count();
    if (g_diag > 0) b->dg_rel = g_dg_rel;
    b->cw_rel = g_put_why;
    if (rt_trace & RT_TRACE_SCHED) b->grant_at = kern_now_ns();
    plat_cond_signal(&b->cv);
}

// t gives its core up; the core it held, now free, or -1 when it held none.
static int core_release(RtThread *t) {
    if (t->cur_core < 0) return -1;
    int c = t->cur_core;
    if (t->preempt && t->pre_since) {   // sched trace: time from the preempt request to this release
        uint64_t d = kern_now_ns() - t->pre_since;
        g_pre_n++;
        g_pre_ns += d;
        if (d > g_pre_max) g_pre_max = d;
        t->npre++;
    }
    t->pre_since = 0;
    if (g_diag > 0) {
        g_dg_rel = (struct DgRel){.kick = t->dg_kick, .host_end = t->dg_host_end, .pc = t->dg_pc, .lr = t->dg_lr,
                                  .hrip = t->dg_hrip, .host = t->dg_host_kick, .pre = t->preempt, .prio = t->prio,
                                  .h = t->handle};
        t->dg_kick = t->dg_host_end = t->dg_pc = t->dg_lr = t->dg_hrip = 0;
        t->dg_host_kick = 0;
    }
    t->cur_core = -1;
    t->preempt = 0;
    g_core_owner[c] = NULL;
    return c;
}

static void core_put(RtThread *t) {
    int c = core_release(t);
    if (c >= 0) core_grant(c);
}

// Whether a waiter better than t may run on core c.
static int core_better_waiter(const RtThread *t, int c) {
    int tp = prio_of(t);
    for (const RtThread *w = g_core_wait; w; w = w->cnext)
        if (core_allowed(w, c) && prio_of(w) < tp) return 1;
    return 0;
}

// Asks holders to give their cores up, one holder per waiter: waiters best first, each claims the worst holder worse
// than itself on a core it may run on that no better waiter claimed (a request already pending is reused). Asking
// every worse holder made several drop their cores for one waiter; the spare cores went to worse waiters, which asked
// again.
static void core_kick(void) {
    RtThread *w[64];
    int wp[64], op[NCORE], n = 0, claimed = 0;
    for (int i = 0; i < NCORE; i++) g_kick_op[i] = op[i] = g_core_owner[i] ? prio_of(g_core_owner[i]) : 0;
    for (RtThread *p = g_core_wait; p && n < 64; p = p->cnext) {
        int i = n++, pp = prio_of(p);
        p->cw_prio = pp;
        for (; i > 0 && (wp[i - 1] > pp || (wp[i - 1] == pp && w[i - 1]->cseq > p->cseq)); i--) {
            w[i] = w[i - 1];
            wp[i] = wp[i - 1];
        }
        w[i] = p;
        wp[i] = pp;
    }
    for (int k = 0; k < n && claimed != (1 << NCORE) - 1; k++) {
        int v = -1;
        for (int i = 0; i < NCORE; i++) {
            RtThread *o = g_core_owner[i];
            if ((claimed >> i & 1) || !core_allowed(w[k], i) || !o || op[i] <= wp[k]) continue;
            RtThread *b = v < 0 ? NULL : g_core_owner[v];
            if (!b || op[i] > op[v] || (op[i] == op[v] && o->preempt && !b->preempt)) v = i;
        }
        if (v < 0) continue;
        claimed |= 1 << v;
        RtThread *o = g_core_owner[v];
        // A waiter that found no owner to interrupt sleeps without a timeout in core_take; it only calls ap_try again
        // when woken. So a waiter whose target changes, or whose target is marked just now, is woken to interrupt it:
        // otherwise an owner in game code with no SVC or indirect call (a long loop) keeps its core and the waiter
        // sleeps for good.
        int wake = g_ap_int && (w[k]->target != o || !o->preempt);
        w[k]->target = o;
        if (wake) plat_cond_signal(&w[k]->cv);
        if (o->preempt) continue;
        if (rt_trace & RT_TRACE_SCHED) {
            o->pre_since = kern_now_ns();
            g_pre_req++;
        }
        if (g_diag > 0) {
            o->dg_kick = kern_now_ns();
            o->dg_host_kick = dg_in_host(o);
            o->dg_host_end = 0;
        }
        __atomic_store_n(&o->preempt, 1, __ATOMIC_RELEASE);
    }
}

static void ho_arrive(RtThread *t);

static void core_take(RtThread *t) {
    if (t->cur_core >= 0) {
        if (t->ho_got) ho_arrive(t);
        return;
    }
    if (!cores_enabled()) {
        acct(t, S_RUN);
        return;
    }
    if (!(t->mask & ((1u << NCORE) - 1)))
        rt_fatal("kernel: thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++)
        if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    int blk = t->cw_blk;   // core wait counters: 1 after a blocking wait that timed out, 2 after a wake-up (block)
    uint64_t c0 = t->cw_core0;
    t->cw_blk = 0;
    t->cw_core0 = 0;
    if (c >= 0) {
        t->cur_core = c;
        g_core_owner[c] = t;
        core_count();
        acct(t, S_RUN);
        return;
    }
    acct(t, S_CORE);
    t->cseq = ++g_wseq;
    t->cnext = g_core_wait;
    g_core_wait = t;
    t->target = NULL;
    if (g_diag > 0) {
        t->dg_since = kern_now_ns();
        memset(&t->dg_rel, 0, sizeof t->dg_rel);
    }
    core_kick();
    if (g_diag > 0) t->dg_cls = dg_class0(t);
    int cls = -1;
    if (g_cw_on) {
        cls = cw_class(t, blk == 2);
        if (!c0) c0 = kern_now_ns();
        t->cw_ap_at = 0;
    }
    if (g_ap_int && t->target && t->cur_core < 0) {   // most waits end within microseconds: spin before interrupting
        uint64_t end = kern_now_ns() + AP_FIRST_NS;
        unlock();
        while (__atomic_load_n(&t->cur_core, __ATOMIC_RELAXED) < 0 && kern_now_ns() < end) plat_pause();
        lock();
        if (t->cur_core >= 0) g_ap_spin_ok++;
    } else if (g_mspin_ns && t->cur_core < 0) {
        mspin(&t->cur_core, 0, kern_now_ns() + g_mspin_ns);
    }
    int sampled = 0;
    while (t->cur_core < 0) {
        uint32_t ms = 0;   // 0: until signalled
        if (g_ap_int && ap_try(t) != AP_NONE) ms = 1;   // the owner keeps its core: again in a millisecond
        int sample = g_diag > 0 && !sampled && (t->dg_cls == DG_GAME || t->dg_cls == DG_HOST);
        if (sample && !ms) ms = 2;
        if (ms) plat_cond_wait_ms(&t->cv, &G, ms);
        else plat_cond_wait(&t->cv, &G);
        if (sample && t->cur_core < 0 && kern_now_ns() - t->dg_since >= 2000000) {
            sampled = 1;
            dg_sample(t);
        }
    }
    if (g_diag > 0) dg_done(t);
    if (cls >= 0) {
        uint64_t now = kern_now_ns();
        cw_add(t, cls, c0, now);
        if (cls == CW_WORSE_GAME || cls == CW_WORSE_WAKER) {
            g_cwf.rel[t->cw_rel]++;
            g_cwf.worse_wake += blk == 2;
        }
        if (t->cw_ap_at) {   // an asynchronous preemption was sent for this wait: from then until the core
            uint64_t d = now - t->cw_ap_at;
            g_cwf.sent_n++;
            g_cwf.sent_ns += d;
            if (d > g_cwf.sent_max) g_cwf.sent_max = d;
            t->cw_ap_at = 0;
        }
        if (t->is_main && !blk) g_cwf.main_ns += now - c0;   // inside block, cw_main_end counts the whole wait
    }
    if (t->grant_at) {
        uint64_t d = kern_now_ns() - t->grant_at;
        t->lg_ns += d;
        t->lg_n++;
        if (d > t->lg_max) t->lg_max = d;
        t->grant_at = 0;
    }
    t->target = NULL;
    acct(t, S_RUN);
}

// ---- directed core hand-off (DKB_SCHED_HANDOFF=1, off by default; "ab" switches it with the A/B blocks)
// Horizon reschedules a core as an SVC that changed the scheduling state returns (Eglantine k_scheduler.cpp,
// KScheduler::EnableScheduling and RescheduleCurrentCore when the scheduler lock is released): a thread the SVC made
// runnable that is better than the caller runs at once. Here a woken thread only asks for a core once its host thread
// has woken (a few microseconds) and core_take has found none free; core_kick then marks a holder, often the waker
// itself, which by then has left the SVC and goes on to its next SVC or indirect call (about 26 us in Bananza's mutex
// convoys: a worker at priority 54 hands a mutex to a priority-53 thread with ArbitrateUnlock and keeps the core).
// With the hand-off, wake() notes the best thread woken (ho_note), and where the waker can switch at once it gives that
// thread its core directly, while the woken thread's host thread is still on its way out of block_inner:
// - kern_svc_return, after every SVC: if the woken thread is strictly better than the caller (inheritance included),
//   may run on the caller's core and finds no free core it may use, and no thread waiting for a core that may use the
//   caller's core is as good; then any preempt request that came during the SVC is honoured (kern_checkpoint).
// - block, when the SVC that woke it then blocks the caller (WaitProcessWideKeyAtomic, ArbitrateLock,
//   WaitSynchronization, etc.): the core goes to the woken thread on the same conditions, priority aside, instead of to
//   the best core waiter.
// Departures from Horizon: the woken thread gets the waker's core rather than its own active core (the virtual cores
// let any thread take any core in its mask, as core_take does), and a caller inside an SDK spin lock (TLR+0x100
// disable_count) or with a preemption deferred to SVC 0x36 (TLR+0x102 set: pinned until then) keeps its core, as at a
// checkpoint, where Horizon's SVC return ignores the user disable count and only a pinned thread stays. No thread is
// interrupted: the switch happens on the caller's own host thread, between lifted instructions.

// t's candidate if that thread still exists, cleared (holding G).
static RtThread *ho_cand_take(RtThread *t) {
    RtThread *w = t->ho_cand;
    t->ho_cand = NULL;
    return w && h_peek(t->ho_cand_h, O_THREAD) == w && w->id == t->ho_cand_id ? w : NULL;
}

// wake(): t was just woken; remember it on the waker if it is the waker's best so far (holding G).
static void ho_note(RtThread *t) {
    t->ho_transit = 1;
    RtThread *k = tcur;
    if (!k || k == t || k->cur_core < 0 || (k->ho_cand && k->ho_cand_prio <= t->prio)) return;
    k->ho_cand = t;
    k->ho_cand_id = t->id;
    k->ho_cand_h = t->handle;
    k->ho_cand_prio = t->prio;
}

// Gives t's core to w, which t woke and which has not run since (holding G); blocking: t is about to block, so its
// priority and spin lock state do not matter. 1 if it did.
static int ho_give(RtThread *t, RtThread *w, int blocking) {
    int c = t->cur_core;
    if (c < 0 || !w->ho_transit || w->cur_core >= 0 || !core_allowed(w, c)) return 0;
    int wp = prio_of(w);
    if (!blocking && wp >= prio_of(t)) return 0;
    if (!blocking && g_ap > 0 && (ap_tlr(t)[0] || ap_tlr(t)[1])) {
        if (g_cw_on) g_cwf.ho_kept++;
        return 0;
    }
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(w, i) && !g_core_owner[i]) return 0;   // w takes a free core itself
    for (const RtThread *q = g_core_wait; q; q = q->cnext)
        if (core_allowed(q, c) && prio_of(q) <= wp) return 0;  // a core waiter as good came first
    core_release(t);
    w->cur_core = c;
    g_core_owner[c] = w;
    w->ho_got = 1;
    core_count();
    if (g_diag > 0) w->dg_rel = g_dg_rel;
    if (rt_trace & RT_TRACE_SCHED) w->grant_at = kern_now_ns();
    if (g_cw_on) {
        if (blocking) g_cwf.ho_blk++;
        else g_cwf.ho_svc++;
    }
    if (g_core_wait) core_kick();   // re-evaluate waiters that were targeting t's core
    return 1;   // wake() has signalled w's host thread already
}

// core_take of a thread that got its core in transit: what core_take does after taking a free core.
static void ho_arrive(RtThread *t) {
    t->ho_got = 0;
    t->cw_blk = 0;
    t->cw_core0 = 0;
    t->target = NULL;
    if (t->grant_at) {
        uint64_t d = kern_now_ns() - t->grant_at;
        t->lg_ns += d;
        t->lg_n++;
        if (d > t->lg_max) t->lg_max = d;
        t->grant_at = 0;
    }
    acct(t, S_RUN);
}

void kern_svc_return(void) {
    RtThread *t = tcur;
    if (!t) return;
    if (t->ho_cand) {
        lock();
        RtThread *w = ho_cand_take(t);
        if (w && ho_give(t, w, 0)) core_take(t);
        unlock();
    }
    if (__atomic_load_n(&g_cw_on, __ATOMIC_RELAXED) && __atomic_load_n(&t->preempt, __ATOMIC_RELAXED))
        __atomic_fetch_add(&g_cwf.ho_req, 1, __ATOMIC_RELAXED);
    kern_checkpoint();   // a request from a core waiter that came during the SVC
}

static uint32_t block(RtThread *t, int kind, int64_t ns) {
    t->waker = 0;
    if (ns == 0) return block_inner(t, kind, ns);
    uint64_t t0 = g_cw_on ? kern_now_ns() : 0;   // core wait counters
    if (t0 && t->is_main) {
        g_cw_epoch++;
        g_cw_main_t0 = t0;
    }
    t->cw_woke_at = 0;
    RtThread *w = t->ho_cand ? ho_cand_take(t) : NULL;
    if (!w || !ho_give(t, w, 1)) core_put(t);
    acct(t, st_of(kind));
    t->nblk++;
    uint32_t r = block_inner(t, kind, ns);
    t->ho_transit = 0;
    if (t0) {
        uint64_t now = kern_now_ns();
        t->cw_blk = 1;
        if (t->cw_woke_at) {   // woken, not timed out
            t->cw_blk = 2;
            cw_add(t, CW_WAKE, t->cw_woke_at, now);
            if (kind == W_LOCK) cw_add(t, CW_LOCK, t0, t->cw_woke_at);
        }
        t->cw_core0 = now;
    }
    core_take(t);
    if (t0 && t->is_main) cw_main_end(t, t0, kern_now_ns());
    return r;
}

int kern_core_away(void) {
    RtThread *t = tcur;
    if (!t || t->cur_core < 0) return 0;
    lock();
    core_put(t);
    acct(t, S_HOST);
    if (rt_trace & RT_TRACE_SCHED)   // the caller as an exe RVA (dkb.map)
        t->wobj = (uintptr_t)__builtin_return_address(0) - plat_exe_base();
    unlock();
    return 1;
}

void kern_core_back(int *away) {
    RtThread *t = tcur;
    if (!*away || !t) return;
    lock();
    core_take(t);
    unlock();
    *away = 0;
}

// ---- spin waits (recomp/patches.py `spin` lines, found by tools/spin_waits.py)
// Adapted from MacNativeGames games/totk/runtime/threads.c runtime_spin_wait @ a038e6b and nxnative dm/host/hle_thread.c
// dm_spin_wait @ 6d34984 (MIT, Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 seedds).
// A pass of a guest loop that polls memory until another thread changes it. On the Switch it fills only its own core,
// where a better thread pinned there preempts it; here it holds a virtual core and a host core, and pollers that take a
// lock on every pass hand its cache line between their host cores so the thread whose item it is rarely gets it
// (nxnative found this in Breath of the Wild's job queue). Every pass pauses the host core once. A wait is a run of
// passes through the same loop at most SPIN_GAP_NS apart; once one has lasted DKB_SPIN_HANDOFF_US (default 50) and a
// thread waits for a virtual core, each pass gives the core to the waiter, whatever its priority, yields the host CPU
// and takes a core again. Shorter waits keep the core: a lock held for a moment would otherwise make the next pass wait
// for another thread's next SVC (MacNativeGames). DKB_SPIN=0 keeps the core always (the gate's own cost only).
#define SPIN_GAP_NS 20000ull
static int g_spin_on = -1;
static uint64_t g_spin_handoff_ns;
static _Thread_local uint64_t sw_head, sw_t0, sw_last;

void rt_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    plat_pause();
    RtThread *t = tcur;
    if (!t) return;
    if (g_spin_on < 0) {
        const char *e = cfg_get("DKB_SPIN_HANDOFF_US");
        g_spin_handoff_ns = (uint64_t)(e && *e ? atoll(e) : 50) * 1000;
        g_spin_on = !(cfg_get("DKB_SPIN") && !cfg_bool("DKB_SPIN"));
    }
    t->sp_n++;
    t->sp_site = head;
    uint64_t now = kern_now_ns();
    if (head != sw_head || now - sw_last > SPIN_GAP_NS) {   // a new wait
        sw_head = head;
        sw_t0 = now;
    }
    sw_last = now;
    if (!g_spin_on || t->cur_core < 0 || now - sw_t0 < g_spin_handoff_ns ||
        !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED))   // a racy look: a waiter missed now is seen at the next pass
        return;
    t->sp_ho++;
    int away = kern_core_away();
    plat_yield();
    kern_core_back(&away);
    sw_last = kern_now_ns();
}

// A pending preempt request, at an SVC or an indirect call. The core goes over only if a better thread still waits
// for it, so core_put grants it to a waiter better than t; a stale request (that waiter got another core) is dropped.
void kern_checkpoint(void) {
    RtThread *t = tcur;
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_RELAXED)) return;
    lock();
    if (g_diag > 0 && t->cur) {
        t->dg_pc = t->cur->pc;
        t->dg_lr = t->cur->x[30];
    }
    int better = t->cur_core >= 0 && core_better_waiter(t, t->cur_core);
    if (better && ap_disabled(t)) {   // preemption disabled: the switch comes at SVC 0x36
        ap_tlr(t)[1] = 1;
        t->preempt = 0;
        g_ap_deferred++;
    } else if (better) {
        g_put_why = CW_REL_CHECKPOINT;
        core_put(t);   // clears preempt (after the sched trace reads it)
        g_put_why = CW_REL_YIELD;
        core_take(t);
    } else {
        t->preempt = 0;
        t->pre_since = 0;
        t->dg_kick = t->dg_host_end = 0;
        t->dg_host_kick = 0;
        g_pre_stale++;
    }
    unlock();
}

// kern_ap_stub, on the interrupted owner: where to jump back to, then the core given up as at a checkpoint.
uint64_t kern_ap_rip(void) { return tcur->ap_rip; }

void kern_ap_yield(void) {
    RtThread *t = tcur;
    lock();
    t->ap_pending = 0;
    g_ap_reached++;
    if (t->preempt && t->cur_core >= 0 && !ap_disabled(t) && core_better_waiter(t, t->cur_core)) {
        if (g_diag > 0) {   // the guest pc is the one of the last dispatch: the interrupted host rip says where it was
            t->dg_pc = 0;
            t->dg_hrip = t->ap_rip;
            t->dg_lr = t->cur->x[30];
        }
        g_ap_taken++;
        g_put_why = CW_REL_ASYNC;
        core_put(t);
        g_put_why = CW_REL_YIELD;
        core_take(t);
    }
    unlock();
}

#if defined(_WIN32) && defined(__x86_64__)
// Runs inside fault_veh on the faulting guest thread right before NtContinue resumes execution. Writing *rip here
// modifies the exception's saved context so NtContinue enters kern_ap_stub cleanly outside exception dispatch.
static void kern_ap_fault(uint64_t *rip) {
    if (!RT_KNOB_ON(g_ap_fault_mode)) return;
    RtThread *t = tcur;
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&t->host_depth, __ATOMIC_RELAXED) > 0) return;
    uint64_t r = *rip;
    if (r < g_gen_lo || r > g_gen_hi) return;
    lock();
    if (t->cur_core >= 0 && t->preempt && !t->ap_pending && __atomic_load_n(&t->host_depth, __ATOMIC_RELAXED) == 0) {
        if (ap_disabled(t)) {
            ap_tlr(t)[1] = 1;
            g_ap_deferred++;
        } else if (core_better_waiter(t, t->cur_core)) {
            t->ap_rip = r;
            t->ap_pending = 1;
            t->ap_sent_at = kern_now_ns();
            *rip = (uint64_t)(uintptr_t)kern_ap_stub;
            g_ap_fault++;
        }
    }
    unlock();
}
#endif

// SVC 0x36 SynchronizePreemptionState: a preemption deferred by disable_count happens here.
static void ap_sync(RtThread *t) {
    lock();
    if (g_ap > 0 && t->cur_core >= 0 && core_better_waiter(t, t->cur_core)) {
        g_put_why = CW_REL_CHECKPOINT;
        core_put(t);
        g_put_why = CW_REL_YIELD;
        core_take(t);
    }
    unlock();
}

void kern_core_stats(int *held_now, int *held_max) {
    lock();
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    *held_now = n;
    *held_max = g_held_max;
    unlock();
}

void kern_core_stats_reset(void) {
    lock();
    g_held_max = 0;
    core_count();
    unlock();
}

// DKB_TRACE=sched: every 120 presents, the time split of the busiest threads (running, waiting for a core, host
// calls) and of the render thread, the render and the main thread's longest waits by object and by the thread whose
// signal ended them, and the preemption latency.
#define REP_N 8
// At each present (holding G): starts recording at DKB_CRIT's first frame, stops and writes the file at its last.
// File: "CRIT", thread count, event count (u32 each); per thread handle, priority, flags (1 main, 2 presents), host
// tid (u32 each); then the CritEv records.
static void crit_frame(RtThread *self, uint64_t frame) {
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("DKB_CRIT");
        unsigned long long lo, hi;
        double wl, wh;
        if (e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh &&
            (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_wlo = (int64_t)(wl * 1e6), g_crit_whi = (int64_t)(wh * 1e6);
            g_crit_lo = g_crit_hi = UINT64_MAX;
            rt_log("crit: recording world seconds %g-%g", wl, wh);
        } else if (e && sscanf(e, "%llu-%llu", &lo, &hi) == 2 && lo < hi && (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_lo = lo;
            g_crit_hi = hi;
            rt_log("crit: recording frames %llu-%llu", lo, hi);
        }
    }
    if (g_crit && g_crit_whi && g_crit_hi == UINT64_MAX) {   // world mode: fix the frames as the clock passes lo and hi
        int64_t w = frame_step_world_us();
        if (g_crit_lo == UINT64_MAX) {
            if (w < g_crit_wlo) return;
            g_crit_lo = frame;
        }
        if (w > g_crit_whi) g_crit_hi = frame;
    }
    if (!g_crit || frame < g_crit_lo || frame > g_crit_hi) return;
    g_crit_on = 1;
    crit_add(CRIT_FRAME, self ? self->handle : 0, 0, 0, frame, kern_now_ns());
    if (frame < g_crit_hi) return;
    g_crit_on = 0;
    const char *path = cfg_get("DKB_CRIT_OUT");
    FILE *f = fopen(path && *path ? path : "crit.bin", "wb");
    if (!f) {
        rt_log("crit: cannot write %s", path && *path ? path : "crit.bin");
        return;
    }
    uint32_t nt = 0;
    for (RtThread *t = g_all; t; t = t->all) nt++;
    fwrite("CRIT", 1, 4, f);
    fwrite(&nt, 4, 1, f);
    fwrite(&g_crit_n, 4, 1, f);
    for (RtThread *t = g_all; t; t = t->all) {
        uint32_t row[4] = {t->handle, (uint32_t)t->prio, (uint32_t)(t->is_main | (t == g_render) << 1), t->tid};
        fwrite(row, 4, 4, f);
    }
    fwrite(g_crit, sizeof *g_crit, g_crit_n, f);
    fclose(f);
    rt_log("crit: %u events, %u threads, frames %llu-%llu%s", g_crit_n, nt, (unsigned long long)g_crit_lo,
           (unsigned long long)g_crit_hi, g_crit_n >= CRIT_N ? " (buffer full: the last frames are incomplete)" : "");
}

void kern_sched_report(uint64_t frame) {
    static uint64_t last;
    typedef struct {
        uint32_t h;
        int prio, render;
        uint64_t mask, entry, ns[S_N], nblk, npre, cpu, lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;
        uint64_t sp_n, sp_ho, sp_site;
    } Row;
    Row rows[REP_N + 1];
    WaitRec rw[2][4] = {0};   // the render and the main thread's longest waits
    int nr = 0;
    RtThread *self = tcur;
    lock();
    if (g_render != self) {
        g_render = self;
        memset(g_rw, 0, sizeof g_rw);
    }
    crit_frame(self, frame);
    if (frame % 120) {
        unlock();
        return;
    }
    uint64_t now = kern_now_ns(), span = last ? now - last : 0;
    last = now;
    RtThread *top[REP_N + 1] = {0};
    uint64_t busy[REP_N + 1] = {0}, all_run = 0, all_core = 0;
    for (RtThread *t = g_all; t; t = t->all) {
        acct(t, t->st);   // close the open interval
        uint64_t b = t->st_ns[S_RUN] + t->st_ns[S_CORE] + t->st_ns[S_HOST];
        all_run += t->st_ns[S_RUN];
        all_core += t->st_ns[S_CORE];
        if (t == self) continue;
        for (int i = 0; i < REP_N; i++)
            if (!top[i] || b > busy[i]) {
                memmove(top + i + 1, top + i, (REP_N - 1 - i) * sizeof *top);
                memmove(busy + i + 1, busy + i, (REP_N - 1 - i) * sizeof *busy);
                top[i] = t;
                busy[i] = b;
                break;
            }
    }
    if (self) {   // the render thread first
        memmove(top + 1, top, REP_N * sizeof *top);
        top[0] = self;
    }
    for (int i = 0; i <= REP_N && top[i]; i++) {
        RtThread *t = top[i];
        Row *r = &rows[nr++];
        r->h = t->handle;
        r->prio = t->prio;
        r->render = t == self;
        r->mask = t->mask;
        r->entry = t->entry;
        memcpy(r->ns, t->st_ns, sizeof r->ns);
        r->nblk = t->nblk;
        r->npre = t->npre;
        r->lw_ns = t->lw_ns, r->lw_n = t->lw_n, r->lw_max = t->lw_max;
        r->lg_ns = t->lg_ns, r->lg_n = t->lg_n, r->lg_max = t->lg_max;
        r->sl_n = t->sl_n, r->sl_req = t->sl_req, r->sl_act = t->sl_act;
        r->sp_n = t->sp_n, r->sp_ho = t->sp_ho, r->sp_site = t->sp_site;
        r->cpu = 0;
        uint64_t c = t->tid ? plat_thread_cpu_ns(t->tid) : 0;
        if (c) {
            r->cpu = t->cpu_last ? c - t->cpu_last : 0;
            t->cpu_last = c;
        }
    }
    for (int k = 0; k < 2; k++) {
        const WaitRec *src = k ? g_mw : g_rw;
        for (int i = 0; i < RW_N && src[i].ns; i++)
            for (int j = 0; j < 4; j++)
                if (src[i].ns > rw[k][j].ns) {
                    memmove(rw[k] + j + 1, rw[k] + j, (3 - j) * sizeof **rw);
                    rw[k][j] = src[i];
                    break;
                }
    }
    uint64_t pre_stale = g_pre_stale, pre_req = g_pre_req, pre_n = g_pre_n, pre_ns = g_pre_ns, pre_max = g_pre_max;
    uint64_t sc = g_sync_calls, sb = g_sync_blocks;
    for (RtThread *t = g_all; t; t = t->all) {
        memset(t->st_ns, 0, sizeof t->st_ns);
        t->nblk = t->npre = 0;
        t->lw_ns = t->lw_n = t->lw_max = t->lg_ns = t->lg_n = t->lg_max = t->sl_n = t->sl_req = t->sl_act = 0;
        t->sp_n = t->sp_ho = 0;   // racy against the thread's own increments: a few passes may be lost
    }
    memset(g_rw, 0, sizeof g_rw);
    memset(g_mw, 0, sizeof g_mw);
    g_pre_req = g_pre_n = g_pre_ns = g_pre_max = g_pre_stale = 0;
    g_sync_calls = g_sync_blocks = 0;
    unlock();
    if (!span) return;
    rt_log("sched: frame %llu: %.2f s, cores busy %.2f of %d, core wait %.2f thread-s, preempt req %llu released %llu "
           "avg %.2f ms max %.2f ms, WaitSynchronization %llu calls %llu blocks, stale preempt %llu%s",
           (unsigned long long)frame, span / 1e9, (double)all_run / span, NCORE, all_core / 1e9,
           (unsigned long long)pre_req, (unsigned long long)pre_n, pre_n ? pre_ns / 1e6 / pre_n : 0.0, pre_max / 1e6,
           (unsigned long long)sc, (unsigned long long)sb, (unsigned long long)pre_stale, self ? "" : " (render thread is not a guest thread)");
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        char d[128], s[256];
        int o = 0;
        for (int k = S_RUN; k < S_N; k++)
            o += snprintf(s + o, sizeof s - o, " %s %.0f%%", s_name[k], 100.0 * r->ns[k] / span);
        ldr_describe(r->entry, d, sizeof d);
        rt_log("sched: %s%#x p%d m%llx%s cpu %.0f%% blocks %llu preempted %llu (%s)", r->render ? "render " : "",
               r->h, r->prio, (unsigned long long)r->mask, s, 100.0 * r->cpu / span, (unsigned long long)r->nblk,
               (unsigned long long)r->npre, r->entry ? d : "main");
        rt_log("sched:   %#x wake to run avg %.1f us max %.0f us (%llu), core grant to run avg %.1f us max %.0f us "
               "(%llu), sleep %llu asked avg %.1f us took avg %.1f us", r->h, r->lw_n ? r->lw_ns / 1e3 / r->lw_n : 0.0,
               r->lw_max / 1e3, (unsigned long long)r->lw_n, r->lg_n ? r->lg_ns / 1e3 / r->lg_n : 0.0, r->lg_max / 1e3,
               (unsigned long long)r->lg_n, (unsigned long long)r->sl_n, r->sl_n ? r->sl_req / 1e3 / r->sl_n : 0.0,
               r->sl_n ? r->sl_act / 1e3 / r->sl_n : 0.0);
        if (r->sp_n) {
            ldr_describe(r->sp_site, d, sizeof d);
            rt_log("sched:   %#x spin wait passes %llu, core handoffs %llu, last at %s", r->h,
                   (unsigned long long)r->sp_n, (unsigned long long)r->sp_ho, d);
        }
    }
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 4 && rw[k][j].ns; j++) {
            char by[32] = "";
            if (rw[k][j].waker) snprintf(by, sizeof by, ", woken by %#x", rw[k][j].waker);
            rt_log("sched: %s wait %d: %s on %s%#llx%s: %.1f%%", k ? "main" : "render", j + 1, s_name[rw[k][j].st],
                   rw[k][j].st == S_HOST ? "exe+" : "", (unsigned long long)rw[k][j].obj, by, 100.0 * rw[k][j].ns / span);
        }
}

// ---- locks: tag word = owner handle | WAITERS_BIT
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Release the lock at addr: hand it to the best waiter, or clear it.
static void lock_handoff(uint64_t addr) {
    RtThread *w = pick(W_LOCK, addr);
    int more = 0;
    if (!w) {
        st32(addr, 0);
        return;
    }
    for (RtThread *t = g_wait; t; t = t->next)
        if (t != w && t->wkind == W_LOCK && t->waddr == addr) {
            more = 1;
            break;
        }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    if (g_cw_on) g_cwf.handoffs++;
    wake(w, 0);
}

// A condvar waiter takes its lock: directly if free (and wakes), else it becomes a lock waiter behind the others.
static void lock_acquire_for(RtThread *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) {
            if (cas32(addr, &v, tag)) {
                wake(t, 0);
                return;
            }
            continue;
        }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    acct(t, S_LOCK);
    t->wobj = v & ~WAITERS_BIT;
    t->wkind = W_LOCK;
    t->waddr = addr;
    t->wtag = tag;
    t->wseq = ++g_wseq;
}

// ---- threads
static RtThread *thr_new(int prio, int core, uint64_t mask) {
    RtThread *t = calloc(1, sizeof *t);
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    if (!t || !c) rt_fatal("kernel: thread allocation");
    memset(c, 0, sizeof *c);
    plat_cond_init(&t->cv);
    t->prio = prio;
    t->core = core;
    t->mask = mask;
    t->cur_core = -1;
    // TLR (0x200) and the "current thread object" the SDK reads through TLR+0x1F8, in one guest-visible block.
    t->tlr = mem_kernel_alloc(TLR_SIZE + THREAD_OBJ_SIZE, TLR_SIZE);
    *(uint64_t *)(t->tlr + 0x1F8) = (uint64_t)(uintptr_t)(t->tlr + TLR_SIZE);
    t->tls = mem_kernel_alloc(TLS_SIZE, 16);
    c->tpidrro = (uint64_t)(uintptr_t)t->tlr;
    c->tpidr = (uint64_t)(uintptr_t)t->tls;
    c->excl_addr = ~0ull;
    c->thread = t;
    t->cpu = t->cur = c;
    return t;
}

static void thr_link(RtThread *t) {   // holding G
    t->handle = h_new(O_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    g_created++;
}

static void thr_free(RtThread *t) {   // holding G; t has exited and its handle is closed
    for (RtThread **p = &g_all; *p; p = &(*p)->all)
        if (*p == t) {
            *p = t->all;
            break;
        }
    mem_kernel_free(t->tlr, TLR_SIZE + THREAD_OBJ_SIZE);
    mem_kernel_free(t->tls, TLS_SIZE);
    plat_aligned_free(t->cpu);
    free(t);
}

// The end of every guest thread, on its own host context after the unwind: give the core up, signal the thread
// object, free it if its handle is already closed. t is not touched after the unlock.
static void thread_finish(RtThread *t) {
    if (t->base_fiber) plat_fiber_unself();
    t->base_fiber = NULL;
    tcur = NULL;
    lock();
    core_put(t);
    acct(t, S_OFF);
    if (t->hthr) plat_intr_free(t->hthr);   // no waiter interrupts a thread without a core
    t->hthr = NULL;
    t->exited = 1;
    wake_sync_waiters();
    if (t->closed) thr_free(t);
    unlock();
}

// ExitThread: back to the thread start with longjmp. On Windows the default MinGW setjmp records the stack frame,
// which the CRT needs to validate the target when SetContextIpValidation is enabled. The non-SEH variant records a
// null frame and longjmp fails with FAST_FAIL_INVALID_SET_OF_CONTEXT on such systems.
// (__builtin_setjmp is not usable: on Win64 LLVM saves the frame address, which is not rbp when the frame register
// is offset, and the landing code then reads its locals through the wrong base.)
static __attribute__((noreturn)) void exit_now(RtThread *t) { longjmp(t->exit_jmp, 1); }

// The calling host thread, for ap_try (DKB_ASYNC_PREEMPT) and dg_sample (DKB_CORE_DIAG).
static PlatIntr *ap_open_self(void) {
    if (!cores_enabled() || (!g_ap_int && g_diag <= 0)) return NULL;
    PlatIntr *h = plat_intr_self();
    if (!h) rt_log("kernel: no interruption handle for this thread: it is not preempted asynchronously");
    return h;
}

static void thread_main(void *p) {
    RtThread *t = p;
    tcur = t;
    t->tid = plat_thread_id();
    plat_op_role(PLAT_OP_GAME);   // DKB_TRACE=perf: its host operations count as the game's (perf.c ops_frame_log)
    PlatIntr *self = ap_open_self();
    lock();
    t->hthr = self;
    while (!t->started) plat_cond_wait(&t->cv, &G);
    core_take(t);
    unlock();
    Cpu *c = t->cpu;
    c->sp = t->stack_top & ~15ull;
    if (!setjmp(t->exit_jmp)) {
#ifdef _WIN32   // the CRT needs this frame to validate longjmp when SetContextIpValidation is enabled
        if (!((_JUMP_BUFFER *)t->exit_jmp)->Frame)
            rt_fatal("kernel: setjmp recorded no frame; ExitThread would fail fast");
#endif
        rt_call(c, t->entry, 1, t->arg);
        char d[128];
        rt_fatal("guest thread %#x: entry %s returned (Horizon threads end with ExitThread)", t->handle,
                 ldr_describe(t->entry, d, sizeof d));
    }
    thread_finish(tcur);
    return;
}

uint64_t kern_threads_created(void) {
    lock();
    uint64_t n = g_created;
    unlock();
    return n;
}

Cpu *kern_cpu(void) { return tcur ? tcur->cur : NULL; }

uint32_t kern_main_thread(Cpu **cpu) {
    if (tcur) {
        if (!tcur->is_main) rt_fatal("kern_main_thread on guest thread %#x, not the main thread", tcur->handle);
        *cpu = tcur->cpu;
        return tcur->handle;
    }
    if (g_main) rt_fatal("kern_main_thread: the main thread already runs on another host thread");
    const RtGuestLayout *L = mem_layout();
    uint64_t stack = L->stack.base + L->stack.size - MAIN_STACK;
    if (!plat_mem_commit(stack, MAIN_STACK))
        rt_fatal("kernel: commit the main thread stack at %#llx failed", (unsigned long long)stack);
    RtThread *t = thr_new(MAIN_PRIO, MAIN_CORE, 1ull << MAIN_CORE);   // the kernel starts it on its ideal core alone
    t->started = 1;
    t->is_main = 1;
    t->stack_top = stack + MAIN_STACK;
    t->cpu->sp = t->stack_top;
    t->tid = plat_thread_id();
    perf_main_thread();
    plat_op_role(PLAT_OP_GAME);
    PlatIntr *self = ap_open_self();
    lock();
    t->hthr = self;
    thr_link(t);
    g_main = t;
    g_main_stack = stack;
    core_take(t);
    unlock();
    tcur = t;
    *cpu = t->cpu;
    return t->handle;
}

void kern_main_stack(uint64_t *base, uint64_t *size) {
    uint64_t b = __atomic_load_n(&g_main_stack, __ATOMIC_ACQUIRE);
    *base = b;
    *size = b ? MAIN_STACK : 0;
}

static void guest_main(void *p) {
    void (*fn)(Cpu *) = (void (*)(Cpu *))p;
    Cpu *c;
    kern_main_thread(&c);
    if (!setjmp(tcur->exit_jmp)) {
#ifdef _WIN32   // the CRT needs this frame to validate longjmp when SetContextIpValidation is enabled
        if (!((_JUMP_BUFFER *)tcur->exit_jmp)->Frame)
            rt_fatal("kernel: setjmp recorded no frame; ExitThread would fail fast");
#endif
        fn(c);
    }
    thread_finish(tcur);
    return;
}

void kern_run_on_guest_main(void (*fn)(Cpu *c)) {
    PlatThread *h = plat_thread_new_ex(guest_main, (void *)fn, "guest main", HOST_STACK);
    if (!h) rt_fatal("kernel: cannot start the guest main thread");
    plat_thread_join(h);
}

// The OS timer ends a wait late by up to PLAT_WAIT_LATE_NS (Windows' high-resolution waitable timer: 0.3-0.5 ms; the
// SDK's 20 us polling sleeps, main thread and job workers, about 1.5 per frame each, took 460-515 us). Sleeps up to
// twice that spin on the clock instead (1 ms on Windows, 120 us on Linux); the virtual core is already given up.
#define SLEEP_SPIN_NS (2 * PLAT_WAIT_LATE_NS)
static void sleep_ns(RtThread *t, int64_t ns) {
    (void)t;
    if (ns <= SLEEP_SPIN_NS) {
        uint64_t end = kern_now_ns() + (uint64_t)ns;
        while (kern_now_ns() < end) plat_pause();
        return;
    }
    plat_wait_ns((uint64_t)ns);
}

// ---- synchronization objects
static int sig_obj(uint32_t h, int *bad) {   // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) {
        *bad = 1;
        return 0;
    }
    switch (g_h[i].type) {
    case O_THREAD: return ((RtThread *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((Event *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}

int kern_thread_state(uint32_t h) {
    lock();
    RtThread *t = h_peek(h, O_THREAD);
    int r = !t ? -1 : t->exited ? KERN_THREAD_EXITED : t->wkind;
    unlock();
    return r;
}

// CloseHandle: an event goes with its last end; a thread object once it has also exited.
static uint32_t handle_close(uint32_t h) {
    unsigned i = h - 0x1000;
    int ok = 0, kind = 0;
    void *ipc = NULL;
    lock();
    if (i > 0 && i < MAX_H) {
        int type = g_h[i].type;
        void *o = g_h[i].obj;
        if (type == O_EVENT_R || type == O_EVENT_W) {
            Event *e = o;
            if (--e->refs == 0) free(e);
            ok = 1;
        } else if (type == O_SESSION || type == O_TMEM) {
            kind = type;
            ipc = o;
            ok = 1;
        } else if (type == O_THREAD) {
            RtThread *t = o;
            t->closed = 1;
            if (t->exited) thr_free(t);
            ok = 1;
        }
        if (ok) {
            g_h[i].type = O_FREE;
            g_h[i].obj = NULL;
        }
    }
    unlock();
    if (ipc) ipc_handle_closed(kind, ipc);
    return ok ? R_OK : R_BAD_HANDLE;
}

void kern_handle_close(uint32_t h) {
    if (handle_close(h)) rt_fatal("kern_handle_close(%#x): bad handle", h);
}

// Hooks of the probe's condvar negative control.
volatile int kern_test_cv_old_order;
void (*volatile kern_test_cv_hook)(void);

static void cv_hook(void) {
    void (*h)(void) = kern_test_cv_hook;
    if (h) h();
}

int kern_svc(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    if (g_diag > 0 && tcur) tcur->dg_svc = imm;
    switch (imm) {
    case 0x08: {   // CreateThread(-> w1 handle; x1 entry, x2 arg, x3 stack top, w4 priority, w5 core)
        // As on Horizon (and Eglantine, svc_thread.cpp CreateThread, k_thread.cpp KThread::Initialize), a new thread
        // may run on its ideal core only until SetThreadCoreMask widens its mask; core -2 is the process's ideal core
        // (main.npdm META, MAIN_CORE). Bananza creates every thread with -2 and gives most of them a mask, but five
        // keep the default and so share core 0 with the main thread at a lower or equal priority: they run only while
        // the main thread waits. One of them, a loader at priority 47, adds a job and its edges to the description of
        // the main thread's per-frame job graph (3.2.0 f_700056b6dc) without a lock; given cores 0-2 it did so while
        // the main thread built the graph from that description, and the build met edges to a job it had not made
        // (#317, recomp/patches-dkb458752.txt main+0x111674).
        int prio = (int)x[4], core = (int)x[5] == -2 ? MAIN_CORE : (int)x[5];
        if (!x[1] || !x[3] || prio < 28 || prio > 59 || core < 0 || core >= NCORE) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        RtThread *t = thr_new(prio, core, 1ull << core);
        t->entry = x[1];
        t->arg = x[2];
        t->stack_top = x[3] & ~15ull;
        lock();
        thr_link(t);
        unlock();
        if (!plat_thread_spawn(thread_main, t, "guest", HOST_STACK)) rt_fatal("kernel: cannot start a guest thread");
        if (rt_trace & RT_TRACE_SVC) {
            char d[128];
            rt_log("CreateThread %#x entry %s stack %#llx prio %d core %d", t->handle,
                   ldr_describe(t->entry, d, sizeof d), (unsigned long long)t->stack_top, prio, core);
        }
        x[0] = R_OK;
        x[1] = t->handle;
        return 1;
    }
    case 0x09: {   // StartThread(w0)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->started = 1;
            plat_cond_signal(&t->cv);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x0A: {   // ExitThread
        RtThread *t = cur(imm);
        if (t->fiber) {   // on a guest fiber: unwind from the thread's own context (kern_fiber_switch resumes it)
            t->exit_pending = 1;
            t->leaving = t->fiber;
            t->fiber = NULL;
            t->cur = t->cpu;
            plat_fiber_switch(t->base_fiber);
            rt_fatal("kernel: ExitThread on a fiber came back");
        }
        exit_now(t);
    }
    case 0x0B: {   // SleepThread(x0 ns); 0, -1, -2 yield
        int64_t ns = (int64_t)x[0];
        RtThread *t = cur(imm);
        if (ns > 0) {
            lock();
            core_put(t);
            acct(t, S_SLEEP);
            t->wobj = (uint64_t)ns;
            t->nblk++;
            unlock();
            uint64_t t0 = kern_now_ns();
            sleep_ns(t, ns);
            uint64_t took = kern_now_ns() - t0;
            lock();
            t->sl_n++;
            t->sl_req += (uint64_t)ns;
            t->sl_act += took;
            core_take(t);
            unlock();
            return 1;
        }
        // Yield: hand the core to a waiting thread of equal or better priority.
        lock();
        int give = 0;
        int tp = prio_of(t);
        for (RtThread *w = g_core_wait; w && !give; w = w->cnext)
            give = t->cur_core >= 0 && core_allowed(w, t->cur_core) && prio_of(w) <= tp;
        if (give) {
            core_put(t);
            core_take(t);
        }
        unlock();
        if (!give) plat_yield();
        return 1;
    }
    case 0x0C: {   // GetThreadPriority(w1 handle -> w1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = (uint64_t)t->prio;
        unlock();
        return 1;
    }
    case 0x0D: {   // SetThreadPriority(w0 handle, w1 priority)
        if ((uint32_t)x[1] < 28 || (uint32_t)x[1] > 59) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x0E: {   // GetThreadCoreMask(w2 handle -> w1 core, x2 mask)
        lock();
        RtThread *t = h_get((uint32_t)x[2], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) {
            x[1] = (uint64_t)(uint32_t)t->core;
            x[2] = t->mask;
        }
        unlock();
        return 1;
    }
    case 0x0F: {   // SetThreadCoreMask(w0 handle, w1 ideal core, x2 mask): -2 the process's ideal core and that core
                   // alone, -3 keeps the ideal core, -1 (no ideal core) prefers core 0
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            int core = (int)x[1];
            if (core >= 0) t->core = core;
            else if (core == -1) t->core = MAIN_CORE;
            if (core == -2) t->core = MAIN_CORE, t->mask = 1ull << MAIN_CORE;   // Horizon: the ideal core alone
            else if (x[2]) t->mask = x[2];
            if (t == tcur && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) {   // move off a forbidden core
                core_put(t);
                core_take(t);
            }
        }
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x10: {   // GetCurrentProcessorNumber: the virtual core held
        RtThread *t = cur(imm);
        x[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core);
        return 1;
    }
    case 0x36: {   // SynchronizePreemptionState: the SDK's spin locks call it after clearing TLR+0x100 (disable_count)
                   // when TLR+0x102 (interrupt_flag) is set; rt_svc's checkpoint has already honoured a pending preempt
        RtThread *t = cur(imm);
        volatile uint16_t *flag = (volatile uint16_t *)(t->tlr + 0x102);
        __atomic_fetch_add(&g_sync36, 1, __ATOMIC_RELAXED);
        if (*flag) __atomic_fetch_add(&g_sync36_flag, 1, __ATOMIC_RELAXED);
        *flag = 0;
        ap_sync(t);
        x[0] = R_OK;
        return 1;
    }
    case 0x25: {   // GetThreadId(w1 handle -> x1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = t->id;
        unlock();
        return 1;
    }

    case 0x1A: {   // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
        RtThread *t = cur(imm);
        uint64_t addr = x[1];
        uint32_t r = R_OK;
        lock();
        if (ld32(addr) == ((uint32_t)x[0] | WAITERS_BIT)) {
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            t->wobj = (uint32_t)x[0];
            r = block(t, W_LOCK, -1);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1B:   // ArbitrateUnlock(x0 address)
        lock();
        lock_handoff(x[0]);
        unlock();
        x[0] = R_OK;
        return 1;
    case 0x1C: {   // WaitProcessWideKeyAtomic(x0 lock, x1 key, w2 tag, x3 timeout)
        // The key is set before the lock is released. The SDK's Signal reads the key in user mode and skips svc 0x1D
        // when it is 0, and user mode takes the lock without G: releasing first lets another thread take the lock,
        // change the condition, read key 0 and wake nobody, after which this thread waits forever (MNG threads.c
        // 439-446). Linking into the wait list happens in block, still holding G, so svc 0x1D cannot come between.
        RtThread *t = cur(imm);
        uint64_t addr = x[0], key = x[1];
        lock();
        if (!kern_test_cv_old_order) {
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            lock_handoff(addr);
            cv_hook();
        } else {   // the probe's negative control: MNG's pre-fix order
            lock_handoff(addr);
            cv_hook();
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
        }
        t->wobj = key;
        uint32_t r = block(t, W_CONDVAR, (int64_t)x[3]);   // on timeout user mode takes the lock itself
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1D: {   // SignalProcessWideKey(x0 key, w1 count; <= 0 all)
        uint64_t key = x[0];
        int cnt = (int)x[1], n = 0;
        lock();
        for (RtThread *w; (cnt <= 0 || n < cnt) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0;
        for (RtThread *w = g_wait; w && !left; w = w->next) left = w->wkind == W_CONDVAR && w->wkey == key;
        if (!left) st32(key, 0);
        unlock();
        x[0] = R_OK;
        return 1;
    }

    case 0x34: {   // WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 if less, 1 decrement if less, 2 if equal
        RtThread *t = cur(imm);
        uint64_t addr = x[0];
        int type = (int)x[1];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        uint32_t r, v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1)
            while (!cas32(addr, &v, v - 1))
                if (!((int32_t)v < val)) {
                    ok = 0;
                    break;
                }
        if (!ok) r = R_BAD_STATE;
        else {
            t->waddr = addr;
            t->wobj = addr;
            r = block(t, W_ADDRESS, (int64_t)x[3]);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x35: {   // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal,
                   // 2 if equal modify by the number of waiters
        uint64_t addr = x[0];
        int type = (int)x[1], cnt = (int)x[3];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        int nw = 0;
        for (RtThread *w = g_wait; w; w = w->next) nw += w->wkind == W_ADDRESS && w->waddr == addr;
        uint32_t r = R_OK;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r)
            for (int n = 0; cnt <= 0 || n < cnt; n++) {
                RtThread *w = pick(W_ADDRESS, addr);
                if (!w) break;
                wake(w, 0);
            }
        unlock();
        x[0] = r;
        return 1;
    }

    case 0x45: {   // CreateEvent(-> w1 writable, w2 readable)
        Event *e = calloc(1, sizeof *e);
        if (!e) rt_fatal("kernel: event allocation");
        e->refs = 2;
        lock();
        uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e);
        unlock();
        x[0] = R_OK;
        x[1] = hw;
        x[2] = hr;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   // SignalEvent(writable), ClearEvent(either end), ResetSignal(readable)
        uint32_t h = (uint32_t)x[0];
        uint32_t r = R_OK;
        lock();
        Event *e = imm == 0x11 ? h_peek(h, O_EVENT_W) : imm == 0x17 ? h_peek(h, O_EVENT_R)
                 : h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R);
        if (!e) {
            r = imm == 0x17 && h_peek(h, O_THREAD) ? R_BAD_STATE : R_BAD_HANDLE;
            if (r == R_BAD_HANDLE) rt_log("kernel: event svc %#x: invalid handle %#x", imm, h);
        } else if (imm == 0x11) {
            e->signaled = 1;
            wake_sync_waiters();
        } else if (imm == 0x17 && !e->signaled) {
            r = R_BAD_STATE;
        } else {
            e->signaled = 0;
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x18: {   // WaitSynchronization(-> w1 index; x1 handles, w2 count, x3 timeout)
        RtThread *t = cur(imm);
        const uint32_t *hs = (const uint32_t *)(uintptr_t)x[1];
        int n = (int)x[2];
        int64_t ns = (int64_t)x[3];
        if (n < 0 || n > 64 || (!hs && n)) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        uint64_t start = kern_now_ns();
        uint32_t r;
        int idx = -1;
        lock();
        g_sync_calls++;
        for (;;) {
            if (t->cancel) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++)
                if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                r = R_BAD_HANDLE;
                break;
            }
            if (idx >= 0) {
                r = R_OK;
                break;
            }
            int64_t left = ns;
            if (ns > 0) {
                uint64_t spent = kern_now_ns() - start;
                if (spent >= (uint64_t)ns) {
                    r = R_TIMEOUT;
                    break;
                }
                left = ns - (int64_t)spent;
            }
            t->wobj = n ? hs[0] | (uint64_t)n << 32 : 0;
            g_sync_blocks++;
            if (block(t, W_SYNC, left) == R_TIMEOUT) {
                r = R_TIMEOUT;
                break;
            }
        }
        unlock();
        x[0] = r;
        x[1] = idx >= 0 ? (uint64_t)idx : 0;
        return 1;
    }
    case 0x19: {   // CancelSynchronization(w0 thread)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->cancel = 1;
            if (t->wkind == W_SYNC) wake(t, (uint32_t)-1);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x16:   // CloseHandle(w0)
        x[0] = handle_close((uint32_t)x[0]);
        return 1;
    }
    return 0;
}

// ---- handles and events for host services (ipc.c and the service overrides)
uint32_t kern_handle_new(int kind, void *obj) {
    if (kind != KERN_SESSION && kind != KERN_TMEM) rt_fatal("kern_handle_new: kind %d", kind);
    lock();
    uint32_t h = h_new(kind, obj);
    unlock();
    return h;
}

void *kern_handle_get(uint32_t h, int kind) {
    lock();
    void *o = h_peek(h, kind);
    unlock();
    return o;
}

struct IpcSession *kern_session_acquire(uint32_t h) {
    lock();
    struct IpcSession *s = h_peek(h, O_SESSION);
    if (s) ipc_session_retain(s);
    unlock();
    return s;
}

uint32_t kern_event_new(void) {
    Event *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("kernel: event allocation");
    e->refs = 1;
    lock();
    uint32_t hw = h_new(O_EVENT_W, e);
    unlock();
    return hw;
}

// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t kern_event_readable(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_readable: %#x is not a writable event handle", hw);
    e->refs++;
    uint32_t hr = h_new(O_EVENT_R, e);
    unlock();
    return hr;
}

void kern_event_signal(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_signal: %#x is not a writable event handle", hw);
    e->signaled = 1;
    wake_sync_waiters();
    unlock();
}

// System events of host services: the SDK's own nn::os::CreateSystemEvent creates the kernel event (SVC 0x45) on a
// guest thread; the writable handle is kept, so any thread (host audio tick included) can signal it later.
static struct { uint64_t ev; uint32_t wh; } g_se[512];
static int g_nse;
static PlatMutex g_se_lock = PLAT_MUTEX_INIT;

static uint32_t se_find(uint64_t ev) {
    plat_mutex_lock_shared(&g_se_lock);
    uint32_t wh = 0;
    for (int i = 0; i < g_nse && !wh; i++)
        if (g_se[i].ev == ev) wh = g_se[i].wh;
    plat_mutex_unlock_shared(&g_se_lock);
    return wh;
}

// The game destroyed the event at ev (nn::os::DestroySystemEvent closes its handles) and is about to create one there
// again: the next kern_bind_system_event binds the new one.
void kern_forget_system_event(uint64_t ev) {
    plat_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++)
        if (g_se[i].ev == ev) g_se[i--] = g_se[--g_nse];
    plat_mutex_unlock(&g_se_lock);
}

void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear) {
    if (se_find(ev)) return;
    if (!tcur || !c) rt_fatal("kern_bind_system_event(%#llx) on a host thread", (unsigned long long)ev);
    static uint64_t create, getw;
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        getw = ldr_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
        if (!create || !getw) rt_fatal("kern_bind_system_event: the SDK's system event functions are not loaded");
    }
    uint32_t r = (uint32_t)rt_call(c, create, 3, ev, (uint64_t)(autoclear ? 1 : 0), (uint64_t)1);
    if (r) rt_fatal("kern_bind_system_event: CreateSystemEvent(%#llx) failed %#x", (unsigned long long)ev, r);
    uint32_t wh = (uint32_t)rt_call(c, getw, 1, ev);
    plat_mutex_lock(&g_se_lock);
    if (g_nse == (int)(sizeof g_se / sizeof *g_se)) rt_fatal("kern_bind_system_event: table full");
    g_se[g_nse].ev = ev;
    g_se[g_nse++].wh = wh;
    plat_mutex_unlock(&g_se_lock);
}

// An event not bound yet is bound on the spot with manual clear, which needs a guest thread.
void kern_signal_system_event(uint64_t ev) {
    uint32_t wh = se_find(ev);
    if (!wh) {
        kern_bind_system_event(kern_cpu(), ev, 0);
        wh = se_find(ev);
    }
    kern_event_signal(wh);
}

// ---- fibers
// The SDK switches fibers by swapping register context, which breaks "RET = C return". Each guest fiber gets a host
// fiber (own 256 MiB reserved host stack) and its own Cpu, so a suspended fiber's registers and host frames stay
// untouched while others run, and a fiber may resume on another thread. A switch carries the thread state (TLR, TLS
// pointer, FPCR/FPSR) over to the target's Cpu and clears the exclusive monitor.
enum { F_NEW, F_STARTED, F_DONE };
struct Fiber {
    uint64_t addr, func, arg, stack, size;
    Cpu *cpu;
    PlatFiber *host;
    int state;
    volatile int running;
};
static Fiber *g_fibers[FIBER_MAX];
static PlatMutex g_fiber_lock = PLAT_MUTEX_INIT;

static Fiber *fiber_find(uint64_t addr) {
    Fiber *f = NULL;
    plat_mutex_lock_shared(&g_fiber_lock);
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) f = g_fibers[i];
    plat_mutex_unlock_shared(&g_fiber_lock);
    return f;
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to);

// After a host fiber switch: release the context left behind, and finish an ExitThread issued on a fiber.
static void fiber_resumed(void) {
    RtThread *t = tcur;
    rt_fpcr_sync(t->cur);   // the host fiber resumes with its own MXCSR (platform.h fibers switch it)
    if (t->leaving) {
        __atomic_store_n(&t->leaving->running, 0, __ATOMIC_RELEASE);
        t->leaving = NULL;
    }
    if (t->exit_pending && !t->fiber) exit_now(t);
}

static void fiber_main(void *p) {
    Fiber *f = p;
    fiber_resumed();
    Cpu *c = f->cpu;
    tcur->host_depth = 0;   // a new fiber starts at the top level; each context restores its own depth on resume
    uint64_t next = rt_call(c, f->func, 1, f->arg);
    f->state = F_DONE;
    Fiber *n = NULL;
    if (next && !(n = fiber_find(next)))
        rt_fatal("fiber %#llx returned %#llx, which is not an initialized fiber", (unsigned long long)f->addr,
                 (unsigned long long)next);
    fiber_switch_to(tcur, c, n);   // NULL: back to the thread's own context
    rt_fatal("fiber %#llx resumed after it finished", (unsigned long long)f->addr);
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to) {
    if (!t->base_fiber) {
        t->base_fiber = plat_fiber_self();
        if (!t->base_fiber) rt_fatal("kernel: cannot make the thread a fiber");
    }
    Cpu *dst = to ? to->cpu : t->cpu;
    if (to) {
        if (to->state == F_DONE) rt_fatal("SwitchToFiber(%#llx): the fiber has finished", (unsigned long long)to->addr);
        int idle = 0;
        if (!__atomic_compare_exchange_n(&to->running, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rt_fatal("SwitchToFiber(%#llx): the fiber is running", (unsigned long long)to->addr);
        if (to->state == F_NEW) {
            dst->sp = (to->stack + to->size) & ~15ull;
            to->state = F_STARTED;
        }
    }
    dst->tpidr = from->tpidr;
    dst->tpidrro = from->tpidrro;
    dst->fpcr = from->fpcr;
    dst->fpsr = from->fpsr;
    dst->excl_addr = ~0ull;
    dst->thread = t;
    t->leaving = t->fiber;
    t->fiber = to;
    t->cur = dst;
    int depth = t->host_depth;
    plat_fiber_switch(to ? to->host : t->base_fiber);
    t->host_depth = depth;
    fiber_resumed();
}

// InitializeFiber(x0 FiberType*, x1 function, x2 argument, x3 stack, x4 stack size, w5 flags)
void kern_fiber_initialize(Cpu *c) {
    uint64_t addr = c->x[0];
    Fiber *f = fiber_find(addr);
    if (f && f->running) rt_fatal("InitializeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    if (!f) {
        f = calloc(1, sizeof *f);
        Cpu *fc = plat_aligned_alloc(sizeof(Cpu), 64);
        if (!f || !fc) rt_fatal("kernel: fiber allocation");
        f->cpu = fc;
        plat_mutex_lock(&g_fiber_lock);
        int i = 0;
        while (i < FIBER_MAX && g_fibers[i]) i++;
        if (i == FIBER_MAX) rt_fatal("kernel: more than %d fibers", FIBER_MAX);
        g_fibers[i] = f;
        plat_mutex_unlock(&g_fiber_lock);
    } else if (f->host) {
        plat_fiber_free(f->host);
    }
    memset(f->cpu, 0, sizeof *f->cpu);
    f->cpu->excl_addr = ~0ull;
    f->addr = addr;
    f->func = c->x[1];
    f->arg = c->x[2];
    f->stack = c->x[3];
    f->size = c->x[4];
    f->state = F_NEW;
    f->running = 0;
    f->host = plat_fiber_new(HOST_STACK, fiber_main, f);
    if (!f->host) rt_fatal("kernel: cannot create a host fiber");
}

// FinalizeFiber(x0 FiberType*)
void kern_fiber_finalize(Cpu *c) {
    uint64_t addr = c->x[0];
    plat_mutex_lock(&g_fiber_lock);
    Fiber *f = NULL;
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) {
            f = g_fibers[i];
            g_fibers[i] = NULL;
        }
    plat_mutex_unlock(&g_fiber_lock);
    if (!f) rt_fatal("FinalizeFiber(%#llx): not an initialized fiber", (unsigned long long)addr);
    if (f->running) rt_fatal("FinalizeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    plat_fiber_free(f->host);
    plat_aligned_free(f->cpu);
    free(f);
}

// SwitchToFiber(x0 FiberType*)
void kern_fiber_switch(Cpu *c) {
    RtThread *t = tcur;
    if (!t) rt_fatal("SwitchToFiber on a host thread");
    Fiber *to = fiber_find(c->x[0]);
    if (!to) rt_fatal("SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)c->x[0]);
    if (to == t->fiber) return;
    fiber_switch_to(t, c, to);
}

// GetCurrentFiber() -> x0, NULL on the thread's own context
void kern_fiber_current(Cpu *c) {
    RtThread *t = tcur;
    c->x[0] = t && t->fiber ? t->fiber->addr : 0;
}

HOST_OVERRIDE(KERN_SYM_INITIALIZE_FIBER, kern_fiber_initialize);
HOST_OVERRIDE(KERN_SYM_FINALIZE_FIBER, kern_fiber_finalize);
HOST_OVERRIDE(KERN_SYM_SWITCH_TO_FIBER, kern_fiber_switch);
HOST_OVERRIDE(KERN_SYM_GET_CURRENT_FIBER, kern_fiber_current);
