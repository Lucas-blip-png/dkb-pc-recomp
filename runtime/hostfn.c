// Adapted from LAS Recomp runtime/hle.h and runtime/dispatch.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai).
// Host functions at synthetic guest addresses: the HOST_OVERRIDE / HOST_OBSERVER
// linker tables, the registry behind them, and the calls between host and guest (rt_call, rt_original).
//
// Every registered function dispatches through hostfn_entry, which counts the call, traces the first one
// (DKB_TRACE=host) and tail-calls the real function. The registry is fixed by hostfn_seal, which builds the dispatch
// table; guest threads start after that, so lookups need no lock.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define MAX_HOSTFN 4096   // dispatch.c holds HOST_CAP / 2 host functions
#define MAX_ADDED 64

#ifdef _WIN32
// PE/COFF: the linker sorts .ovr$a < .ovr$m (entries, host.h) < .ovr$z by the text after '$'.
__attribute__((section(".ovr$a"), used)) static const RtHostEntry ovr_start[1];
__attribute__((section(".ovr$z"), used)) static const RtHostEntry ovr_stop[1];
__attribute__((section(".obs$a"), used)) static const RtHostEntry obs_start[1];
__attribute__((section(".obs$z"), used)) static const RtHostEntry obs_stop[1];
#elif defined(__APPLE__)
// Mach-O: ld64 defines section$start$<segment>$<section> and section$end$<segment>$<section> around a section that
// at least one object file fills. Weak, so a build without observers has NULL bounds on both ends.
extern const RtHostEntry mach_ovr_start[] __asm__("section$start$__DATA$__dkb_ovr") __attribute__((weak));
extern const RtHostEntry mach_ovr_stop[] __asm__("section$end$__DATA$__dkb_ovr") __attribute__((weak));
extern const RtHostEntry mach_obs_start[] __asm__("section$start$__DATA$__dkb_obs") __attribute__((weak));
extern const RtHostEntry mach_obs_stop[] __asm__("section$end$__DATA$__dkb_obs") __attribute__((weak));
#define ovr_start mach_ovr_start
#define ovr_stop mach_ovr_stop
#define obs_start mach_obs_start
#define obs_stop mach_obs_stop
#else
// ELF: the linker defines __start_<section> and __stop_<section> around a section named like a C identifier. Weak: a
// build without observers has no such section, and both bounds are then NULL.
extern const RtHostEntry __start_dkb_ovr[] __attribute__((weak)), __stop_dkb_ovr[] __attribute__((weak));
extern const RtHostEntry __start_dkb_obs[] __attribute__((weak)), __stop_dkb_obs[] __attribute__((weak));
#define ovr_start __start_dkb_ovr
#define ovr_stop __stop_dkb_ovr
#define obs_start __start_dkb_obs
#define obs_stop __stop_dkb_obs
#endif

typedef struct {
    const char *name;
    GuestFn fn;
    uint64_t original;   // observers: the guest definition rt_original calls
    uint64_t calls;
    int blocking;        // runs with the virtual core given up (hostfn_blocking)
} Slot;
static Slot slots[MAX_HOSTFN];
// Read-only hot dispatch table populated before hostfn_seal: kept separate from slots[].calls so guest cores
// reading fn and blocking never suffer cache-line invalidation from call counting.
typedef struct {
    GuestFn fn;
    int blocking;
} HotSlot;
static HotSlot hot_slots[MAX_HOSTFN];
static int nslots;
static RtHostEntry added[MAX_ADDED];
static int nadded;
static int sealed;

// Per-thread call total for the fast dispatch path: one 64-byte-aligned node per thread eliminates global atomic
// contention on every host call while keeping hostfn_calls() exact.
typedef struct HostTls {
    uint64_t total;
    struct HostTls *next;
} HostTls;
static _Thread_local HostTls *tl_host;
static HostTls *tl_head;
static PlatMutex tl_lock = PLAT_MUTEX_INIT;
static int fast_mode = -1, fast_applied = -1;

static GuestFn table_find(const RtHostEntry *start, const RtHostEntry *stop, const char *name) {
    __asm__("" : "+r"(start), "+r"(stop));   // separate objects: keep the compiler from reasoning about them
    for (const RtHostEntry *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

GuestFn hostfn_override(const char *name) {
    for (int i = 0; i < nadded; i++)
        if (!strcmp(added[i].name, name)) return added[i].fn;
    return table_find(ovr_start, ovr_stop, name);
}

GuestFn hostfn_observer(const char *name) { return table_find(obs_start, obs_stop, name); }

// Linked HOST_OVERRIDE / HOST_OBSERVER entries (the loader reports how many it installed).
int hostfn_table_size(int observers) {
    const RtHostEntry *p = observers ? obs_start : ovr_start, *e = observers ? obs_stop : ovr_stop;
    __asm__("" : "+r"(p), "+r"(e));
    int n = 0;
    for (; p < e; p++) n += p->name != NULL;
    return n;
}

const char *hostfn_table_name(int observers, int i) {
    const RtHostEntry *p = observers ? obs_start : ovr_start, *e = observers ? obs_stop : ovr_stop;
    __asm__("" : "+r"(p), "+r"(e));
    for (; p < e; p++)
        if (p->name && i-- == 0) return p->name;
    return NULL;
}

void hostfn_add_override(const char *name, GuestFn fn) {
    if (ldr_count()) rt_fatal("hostfn_add_override(%s) after ldr_load", name);
    if (nadded == MAX_ADDED) rt_fatal("hostfn_add_override: more than %d", MAX_ADDED);
    added[nadded++] = (RtHostEntry){name, fn};
}

static void hostfn_entry(Cpu *c);

static uint64_t slot_addr(int i) { return RT_HOSTFN_BASE + 16ull * (uint64_t)i; }

uint64_t hostfn_register(const char *name, GuestFn fn) {
    if (sealed) rt_fatal("hostfn_register(%s) after hostfn_seal", name);
    if (!name || !fn) rt_fatal("hostfn_register: empty entry");
    for (int i = 0; i < nslots; i++)
        if (slots[i].fn == fn && !strcmp(slots[i].name, name)) return slot_addr(i);
    if (nslots == MAX_HOSTFN) rt_fatal("hostfn_register: more than %d host functions", MAX_HOSTFN);
    uint64_t a = slot_addr(nslots);
    if (a >= RT_RET_HOST) rt_fatal("hostfn_register: host-function window full");
    int blocking = !strncmp(name, "_ZN2nn2fs", 9);
    slots[nslots] = (Slot){.name = name, .fn = fn, .blocking = blocking};
    hot_slots[nslots] = (HotSlot){.fn = fn, .blocking = blocking};
    rt_register_host(a, hostfn_entry);
    nslots++;
    return a;
}

// Loader: an observer slot and the guest definition it wraps.
uint64_t hostfn_register_observer(const char *name, GuestFn fn, uint64_t original) {
    uint64_t a = hostfn_register(name, fn);
    slots[(a - RT_HOSTFN_BASE) >> 4].original = original;
    return a;
}

const char *hostfn_name(uint64_t addr) {
    if (addr == RT_RET_HOST) return "<return to host>";
    uint64_t i = (addr - RT_HOSTFN_BASE) >> 4;
    if ((addr & 15) || i >= (uint64_t)nslots) return NULL;
    return slots[i].name;
}

__attribute__((noinline, noreturn, cold)) static void bad_entry(Cpu *c) {
    char lr[96];
    rt_fatal("host function call at %#llx: no host function registered there (lr %s)", (unsigned long long)c->pc,
             ldr_describe(c->x[30], lr, sizeof lr));
}

// kern_host_track: the call counted in the thread's host depth. Not a tail call, so the host stack holds this frame
// while fn runs; longjmp out of fn (ExitThread) leaves the depth raised on a thread that ends.
__attribute__((noinline)) static void hostfn_tracked(Cpu *c, GuestFn fn) {
    void *t = c->thread;
    int *d = kern_host_depth(t);
    __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    fn(c);
    int n = *d - 1;
    __atomic_store_n(d, n, __ATOMIC_RELAXED);
    if (!n && kern_preempt_pending(t)) kern_host_left(t);
}

// nn::fs: the call waits for the disk and the OS, and on the Switch it is an IPC request that blocks its thread, so
// the thread gives its virtual core up for it. DKB_CORE_DIAG found the game's loader threads (priority 58) holding
// a core through CloseFile and CreateCacheStorage while job workers and better threads waited 2-6 ms for it.
__attribute__((noinline)) static void hostfn_blocking(Cpu *c, GuestFn fn) {
    int away = kern_core_away();
    if (kern_host_track && c->thread) hostfn_tracked(c, fn);
    else fn(c);
    kern_core_back(&away);
}

static void hostfn_entry(Cpu *c) {
    uint64_t i = (c->pc - RT_HOSTFN_BASE) >> 4;
    if (__builtin_expect((c->pc & 15) || i >= (uint64_t)nslots, 0)) bad_entry(c);
    Slot *s = &slots[i];
    uint64_t n = __atomic_fetch_add(&s->calls, 1, __ATOMIC_RELAXED);
    if (__builtin_expect(!n && (rt_trace & RT_TRACE_HOSTCALL), 0)) {
        char lr[96];
        rt_log("host call %s from %s", s->name, ldr_describe(c->x[30] - 4, lr, sizeof lr));
    }
    if (s->blocking) {
        hostfn_blocking(c, s->fn);
        return;
    }
    if (__builtin_expect(kern_host_track && c->thread, 0)) {
        hostfn_tracked(c, s->fn);
        return;
    }
    __attribute__((musttail)) return s->fn(c);
}

__attribute__((noinline, cold)) static HostTls *host_tls_get(void) {
    HostTls *tls = plat_aligned_alloc(64, 64);
    if (!tls) rt_fatal("hostfn: out of memory allocating thread counter");
    memset(tls, 0, 64);
    plat_mutex_lock(&tl_lock);
    tls->next = tl_head;
    tl_head = tls;
    plat_mutex_unlock(&tl_lock);
    tl_host = tls;
    return tls;
}

__attribute__((noinline, cold)) static void hostfn_trace_count(Cpu *c, uint64_t i) {
    uint64_t n = __atomic_fetch_add(&slots[i].calls, 1, __ATOMIC_RELAXED);
    if (!n) {
        char lr[96];
        rt_log("host call %s from %s", slots[i].name, ldr_describe(c->x[30] - 4, lr, sizeof lr));
    }
}

// Fast host-function dispatch (DKB_HOSTFN_FAST=1): reads the read-only hot_slots[] table, counts calls in a
// thread-private cache line (falling back to per-slot atomics only when DKB_TRACE=host is on), and inlines the
// common kern_host_track depth update instead of calling out-of-line hostfn_tracked.
static void hostfn_entry_fast(Cpu *c) {
    uint64_t i = (c->pc - RT_HOSTFN_BASE) >> 4;
    if (__builtin_expect((c->pc & 15) || i >= (uint64_t)nslots, 0)) bad_entry(c);
    HotSlot s = hot_slots[i];
    if (__builtin_expect(rt_trace & RT_TRACE_HOSTCALL, 0)) {
        hostfn_trace_count(c, i);
    } else {
        HostTls *tls = tl_host;
        if (__builtin_expect(!tls, 0)) tls = host_tls_get();
        __atomic_store_n(&tls->total, tls->total + 1, __ATOMIC_RELAXED);
    }
    if (__builtin_expect(s.blocking, 0)) {
        hostfn_blocking(c, s.fn);
        return;
    }
    void *t = c->thread;
    if (__builtin_expect(kern_host_track && t != NULL, 1)) {
        int *d = kern_host_depth(t);
        __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
        s.fn(c);
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (__builtin_expect(!n && kern_preempt_pending(t), 0)) kern_host_left(t);
        return;
    }
    __attribute__((musttail)) return s.fn(c);
}

// A guest jump to the rt_call return sentinel: guest code used x30 as data, which a lifted RET never does.
static void return_sentinel(Cpu *c) {
    rt_fatal("guest code jumped to the host return address %#llx (x0 %#llx)", (unsigned long long)RT_RET_HOST,
             (unsigned long long)c->x[0]);
}

void hostfn_seal(void) {
    static RtModule mods[16];   // kept by the dispatcher
    if (sealed) return;
    rt_register_host(RT_RET_HOST, return_sentinel);
    sealed = 1;
    int n = rt_linked_modules(mods, 16);
    rt_dispatch_init(mods, n);
    fast_mode = rt_knob("HOSTFN_FAST", "1");
    if (fast_mode == 1) {
        fast_applied = 1;
        rt_dispatch_set_hostfn(hostfn_entry_fast, (uint64_t)nslots);
    } else {
        fast_applied = 0;
        if (fast_mode == 2) rt_log("hostfn: fast dispatch in the B blocks of DKB_AB only (DKB_HOSTFN_FAST=ab)");
    }
}

void hostfn_ab_sync(int ab_side) {
    if (__builtin_expect(fast_mode != 2, 1)) return;
    int on = ab_side != 0;
    if (on == fast_applied) return;
    fast_applied = on;
    rt_dispatch_set_hostfn(hostfn_entry_fast, on ? (uint64_t)nslots : 0);
}

uint64_t hostfn_calls(void) {
    uint64_t n = 0;
    for (int i = 0; i < nslots; i++) n += __atomic_load_n(&slots[i].calls, __ATOMIC_RELAXED);
    plat_mutex_lock(&tl_lock);
    for (HostTls *p = tl_head; p; p = p->next) n += __atomic_load_n(&p->total, __ATOMIC_RELAXED);
    plat_mutex_unlock(&tl_lock);
    return n;
}

static int by_calls(const void *a, const void *b) {
    uint64_t x = (*(const Slot *const *)a)->calls, y = (*(const Slot *const *)b)->calls;
    return x < y ? 1 : x > y ? -1 : 0;
}

void hostfn_report(void) {
    static const Slot *order[MAX_HOSTFN];
    int n = 0;
    for (int i = 0; i < nslots; i++)
        if (__atomic_load_n(&slots[i].calls, __ATOMIC_RELAXED)) order[n++] = &slots[i];
    qsort(order, n, sizeof *order, by_calls);
    rt_log("host calls: %llu in %d of %d host functions", (unsigned long long)hostfn_calls(), n, nslots);
    for (int i = 0; i < n; i++) rt_log("  %10llu  %s", (unsigned long long)order[i]->calls, order[i]->name);
}

uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    if (n < 0 || n > 8) rt_fatal("rt_call: %d arguments", n);
    if (!sealed) hostfn_seal();
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) c->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    uint64_t lr = c->x[30], pc = c->pc;
    c->x[30] = RT_RET_HOST;
    c->pc = fn;
    rt_fpcr_sync(c);   // guest code may start here on a host thread or fiber that has not run it
    rt_dispatch(c);
    c->x[30] = lr;
    c->pc = pc;
    return c->x[0];
}

uint64_t rt_original(Cpu *c) {
    uint64_t i = (c->pc - RT_HOSTFN_BASE) >> 4;
    if ((c->pc & 15) || i >= (uint64_t)nslots || !slots[i].original)
        rt_fatal("rt_original at %#llx: not inside an observer with a guest definition", (unsigned long long)c->pc);
    uint64_t lr = c->x[30], pc = c->pc;
    c->x[30] = RT_RET_HOST;
    c->pc = slots[i].original;
    rt_dispatch(c);
    c->x[30] = lr;
    c->pc = pc;
    return c->x[0];
}

// ---- setjmp and longjmp
// The lifted guest longjmp ends in `br x30` to the instruction after its setjmp call, the middle of a function, which
// rt_dispatch cannot enter, and the host frames of everything called since would stay on the stack. recomp/gen.py
// lifts a call to the setjmp import as rt_setjmp_arm, then the host setjmp on jb in the calling function's frame, then
// the guest setjmp, which fills env. The longjmp import becomes ov_longjmp: it loads the guest registers from env and
// longjmps to jb, and the calling function reloads its registers and goes on after the call.
// Update 3.2.0 lays out Thai text with them (main imports setjmp and longjmp; 1.0.0's main does not); the SDK's
// libpng has them for its errors.
// env as the SDK's setjmp fills it (1.0.0 and 3.2.0): x19-x30 at 0, sp at 0x68, d8-d15 at 0x70.
enum { ENV_X30 = 0x58, ENV_SP = 0x68, ENV_D8 = 0x70 };

// The armed setjmps, by Cpu and the sp and return address the guest setjmp stores, so a longjmp finds its setjmp
// through a copy of env as well. Not thread-local: a guest fiber may resume on another host thread. An arm drops the
// Cpu's entries below its sp (frames that have returned), so only setjmps that are still live take slots; a longjmp
// to a setjmp whose function returned is undefined in C too. depth is the thread's host depth at the setjmp
// (kern_host_track), which ov_longjmp restores, since the hostfn_tracked frames it unwinds never lower it.
// ponytail: one lock and a scan of 64 entries per setjmp; per-Cpu lists if setjmp ever shows up in a profile.
#define JMP_SLOTS 64
static struct { Cpu *c; uint64_t sp, lr; void *jb; int depth; } jmps[JMP_SLOTS];
static unsigned jmp_next;
static PlatMutex jmp_lock = PLAT_MUTEX_INIT;

// _longjmp on POSIX: the plain longjmp would restore a signal mask _setjmp never saved (gen.py, RT_SETJMP).
#ifdef _WIN32
#define RT_LONGJMP longjmp
#else
#define RT_LONGJMP _longjmp
#endif

static int *host_depth(Cpu *c) { return kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL; }

void rt_setjmp_arm(Cpu *c, void *jb) {
    uint64_t sp = c->sp, lr = c->x[30];
    int *d = host_depth(c);
    plat_mutex_lock(&jmp_lock);
    int k = -1, idle = -1;
    for (int i = 0; i < JMP_SLOTS; i++) {
        if (jmps[i].c == c && jmps[i].sp < sp) jmps[i].c = NULL;
        if (jmps[i].c == c && jmps[i].sp == sp && jmps[i].lr == lr) k = i;
        if (!jmps[i].c && idle < 0) idle = i;
    }
    if (k < 0) k = idle >= 0 ? idle : (int)(jmp_next++ % JMP_SLOTS);
    jmps[k].c = c;
    jmps[k].sp = sp;
    jmps[k].lr = lr;
    jmps[k].jb = jb;
    jmps[k].depth = d ? *d : 0;
    plat_mutex_unlock(&jmp_lock);
}

static void ov_longjmp(Cpu *c) {
    uint64_t env = c->x[0], sp = LD64(env + ENV_SP), lr = LD64(env + ENV_X30);
    uint32_t val = (uint32_t)c->x[1];
    void *jb = NULL;
    int depth = 0;
    plat_mutex_lock(&jmp_lock);
    for (int i = 0; i < JMP_SLOTS && !jb; i++)
        if (jmps[i].c == c && jmps[i].sp == sp && jmps[i].lr == lr) jb = jmps[i].jb, depth = jmps[i].depth;
    plat_mutex_unlock(&jmp_lock);
    char at[96];
    if (!jb)
        rt_fatal("longjmp to env %#llx (sp %#llx, setjmp return %s): no setjmp armed there on this thread",
                 (unsigned long long)env, (unsigned long long)sp, rt_guest_name(lr, at, sizeof at));
    // hostfn_tracked counts this call (+1). Any other host call between the setjmp and here would be skipped with
    // its locks held or its rt_call lr/pc save not restored, so it is fatal rather than unwound.
    int *d = host_depth(c);
    if (d && *d != depth + 1)
        rt_fatal("longjmp to setjmp return %s would cross %d host call(s) still in progress",
                 rt_guest_name(lr, at, sizeof at), *d - depth - 1);
    for (int i = 0; i < 12; i++) c->x[19 + i] = LD64(env + 8 * i);
    c->sp = sp;
    for (int i = 0; i < 8; i++) c->v[8 + i] = LD64(env + ENV_D8 + 8 * i);   // LDP Dn clears the upper half
    c->x[0] = val ? val : 1;
    if (d) {
        __atomic_store_n(d, depth, __ATOMIC_RELAXED);   // what hostfn_tracked's return would have done
        if (!depth && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    RT_LONGJMP(*(jmp_buf *)jb, 1);
}
HOST_OVERRIDE("longjmp", ov_longjmp);
HOST_OVERRIDE("_longjmp", ov_longjmp);
