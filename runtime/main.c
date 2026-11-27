// dkb.exe entry: reserves the guest address space, parses the command line and runs one mode.
//
//   dkb.exe [--exefs DIR] [--romfs DIR] [--profile DIR] <mode>
//     --selftest        layout, dispatch table and host-function registry; needs no game files
//     --probe-loader    load and relocate the modules, check them against the measured 1.2.1 values
//     --probe-kernel, --probe-ipc, --probe-ipc-lifetime, --probe-services, --probe-fs    host probes
//     --probe-dpi       the process DPI awareness (per-monitor v2) and the primary monitor's size; no game files
//     --probe-commit-later   memory.c's background commits of staged pools against a heap shrink (Windows, about
//                       5 s); no game files
//     --probe-gfx-abi   NVN object layouts against nvn.h's reserved sizes; needs no game files
//     --probe-nvn, --probe-nvn-tex, --probe-window, --probe-shader, --probe-vkmem, --probe-vkexec    graphics probes
//     --probe-astc      the compute ASTC decoder against astc_decode_ldr; needs no game files
//     --probe-movie     host VP9 movie::Decoder on one RomFS movie; skips without game data
//     --probe-splash    the start-up screen drawn in a set of states and window sizes into PNGs (DKB_SPLASH_SHOTS
//                       names the folder, splash-probe by default); needs a Vulkan device, no game files
//     --probe-aoc       mount the add-on content next to the exe and read it back; DKB_AOC_SOURCE=<romfs folder> also
//                       compares the bytes with the folder the pack was made from
//     --boot            run the game to the first NVN call
//     --seconds S       run the game (implies --boot's start-up) until the world clock (game time since the loaded
//                       save's first frame) passed S seconds, then exit 0 at that present
//     --shot PNG        with --seconds: save that frame; env DKB_GPU=null|vulkan selects the back end
//     (no mode)         run the game
//
// Also here: the runtime services the generated code calls besides rt_dispatch and rt_svc (rt_brk, rt_udf,
// rt_ticks, the function profiler hook), logging, SHA-256, and the crash reporter (a vectored exception handler that
// names the lifted function, the guest registers and the guest frame-pointer chain).
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aoc_romfs.h"
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"
#include "splash.h"
#include "vk_driver_env.h"

#define MAX_MODULES 16

// ---- logging, tracing, hashing
unsigned rt_trace;

// Log file. A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also
// writes its log to logs\dkb.log next to dkb.exe, so a player has one to attach to a report; run_game.py and
// `2> file` keep their own file. Each start moves the earlier logs to dkb.1.log .. dkb.4.log. A file stops at 64 MiB
// (DKB_TRACE can write that much). Lines from before log_open wait in a static buffer: mem_init runs first so that
// nothing else lands in the guest address space, and an earlier open would put the CRT's heap there.
#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    int state = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE);
    if (state == LOG_EARLY) {
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
    } else if (state == LOG_FILE) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else if (at <= LOG_CAP) {   // the first write past the cap says so, once
            static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only\n";
            plat_append_write(g_log_file, cut, sizeof cut - 1);
        }
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

// Opens the log file for a game run (game = 1) and writes the buffered lines to it; anything else drops them. Runs on
// the main thread before the boot starts other threads.
static void log_open(int game) {
    char dir[PLAT_PATH_MAX * 3], from[PLAT_PATH_MAX * 3 + 16], to[PLAT_PATH_MAX * 3 + 16];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        const char *base = "dkb";
        for (int i = LOG_KEEP - 1; i > 0; i--) {   // dkb.3.log -> dkb.4.log, ..., dkb.log -> dkb.1.log
            if (i > 1) snprintf(from, sizeof from, "%s/%s.%d.log", dir, base, i - 1);
            else snprintf(from, sizeof from, "%s/%s.log", dir, base);
            snprintf(to, sizeof to, "%s/%s.%d.log", dir, base, i);
            if (plat_file_exists(from)) plat_file_replace(from, to);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/%s.log", dir, base);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
        return;
    }
    size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
    if (n > sizeof g_log_early) n = sizeof g_log_early;
    plat_append_write(g_log_file, g_log_early, n);
    g_log_bytes = n;
    __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
    rt_log("version: %s (%s %s)", rt_port_version, rt_game_version ? "TotK" : "Bananza", rt_game_display);
    rt_log("log: %s", g_log_path);
}

static void parse_trace(void) {
    const char *e = cfg_get("DKB_TRACE");   // no default: nothing is traced when unset
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"codec", RT_TRACE_CODEC}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("DKB_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, codec, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

// ---- configuration
static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)
volatile int g_fnprof = 0;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr, (void)lr, (void)x0; }

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz, the Switch 2's at 31.25 MHz (Bananza's SDK converts with 31250000 /
// 10^9 in nn::os::detail::TickManager::ConvertToTick). With 19.2 MHz ticks Bananza measured its frames 1.6 times too
// long and paced itself at 20 FPS. The scale is set before main, so no thread reads it half written.
static PlatScale tick_scale;

uint32_t rt_tick_hz(void) { return rt_game_version ? 19200000u : 31250000u; }

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(rt_tick_hz(), plat_counter_hz()); }

uint64_t rt_ticks(void) { return plat_scaled(tick_scale, plat_counter()); }
uint64_t rt_ticks_to_ns(uint64_t t) { return (uint64_t)((unsigned __int128)t * 1000000000u / rt_tick_hz()); }
uint64_t rt_ns_to_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * rt_tick_hz() / 1000000000u); }

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it; 0 outside the lifted
// code.
static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    // The lifted code is one range with the host code outside it (kernel.c ap_init checks that); an address above the
    // last function start is taken for host code, which costs only the last function's body.
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

static void crash_handler(const PlatCrash *x) {
    // The thread that writes the report, by id. A second fault on that thread, inside the report, gets no report but
    // still a dump. A fault on another thread waits for the report to end in plat_exit: exiting at once cut the first
    // thread's register dump short (#443), since several guest threads often fault on the same bad data together.
    static volatile int32_t owner;
    int32_t me = (int32_t)plat_thread_id();
    int32_t prev = plat_cas32(&owner, me, 0);
    if (prev == me) {
        plat_crash_dump();
        plat_exit(71);
    }
    if (prev) {
        for (int i = 0; i < 300; i++) plat_sleep_ms(100);   // 30 s: the report or its dump hung
        plat_exit(71);
    }
    // A stack overflow leaves too little stack for the report (rt_vlogf alone takes 2 KB): dump first there, from the
    // dump's own thread, while the report may still die.
    const char *dump = x->code == 0xc00000fdu ? plat_crash_dump() : NULL;
    char d[160];
    uintptr_t rip = x->rip;
    // x->module names the DLL of a fault outside this executable (the last-chance filter: a driver, a Vulkan layer);
    // its offset is then into that module, and the lifted-function lookup below finds nothing.
    rt_logf("FATAL: %s (%#x) at host %p (%s+%#llx)", x->what, x->code, (void *)rip, x->module ? x->module : "dkb",
            (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
#ifdef _WIN32
    uint64_t lazy_at;
    uint32_t lazy_err = plat_mem_lazy_failure(&lazy_at);
    if (lazy_err)
        rt_logf("  lazy heap commit failed: error %u at %#llx, commit limit reached?\n", lazy_err, (unsigned long long)lazy_at);
#endif
    uint64_t f = x->module ? 0 : lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    for (int i = 0; i < x->nframes; i++) {   // abort and invalid parameter: where the host code called it
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: dkb+%#llx", i, (unsigned long long)(x->frames[i] - (rip - x->offset)));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    Cpu *c = kern_cpu();
    if (c) rt_guest_dump(c);
    else rt_logf("  not on a guest thread\n");
    fflush(stderr);
    // The dump comes last: it suspends the other threads and can hang on a lock one of them holds, and by now the
    // report is in the log. It holds game-derived memory (thread stacks): the line says so, since players copy this
    // log into issues.
    if (!dump) dump = plat_crash_dump();
    if (dump) {
        rt_logf("  minidump: %s (keep it on this PC: send it only privately, when asked for)\n", dump);
        fflush(stderr);
    }
    plat_exit(71);
}

// ---- self-test: layout, dispatch table, host-function registry, without game files
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static volatile int host_probe_hits;
static void host_probe(Cpu *c) { host_probe_hits++; c->x[0] = 42; }

static void check_cpu(void) {
#ifdef __aarch64__
    // The portable FP/SIMD lowering (simd.h) runs on every AArch64 CPU: NEON is part of the base architecture there.
    printf("cpu: AArch64 (portable FP/SIMD lowering)\n");
#else
    __builtin_cpu_init();
#if defined(__AVX2__)
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    const char *level = "x86-64-v3 (AVX2, FMA, BMI2)";
#else
    // A --march v2 build (tools/build_game.py) needs only SSE4.2 and POPCNT, as require_cpu checks at start (#318)
    int ok = __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
    const char *level = "x86-64-v2 (SSE4.2, POPCNT)";
#endif
    printf("cpu: %s %s\n", level, ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for %s", level);
#endif
}

// Layout: ascending, no overlap, below 2^39, and inside the window mem_init reserved.
static void check_layout(void) {
    const RtLayoutEntry *lay = rt_layout;
    int n = rt_layout_count;
    printf("layout: %d modules\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-8s base %#llx  size %#10llx  end %#llx\n", lay[i].name, (unsigned long long)lay[i].base,
               (unsigned long long)lay[i].size, (unsigned long long)(lay[i].base + lay[i].size));
        CHECK(lay[i].size > 0, "%s has size 0", lay[i].name);
        CHECK(lay[i].base % 0x10000 == 0, "%s base is not 64 KiB aligned", lay[i].name);
        CHECK(lay[i].base + lay[i].size <= RT_KERNEL_BASE, "%s reaches into the kernel region", lay[i].name);
        if (i) {
            CHECK(lay[i].base > lay[i - 1].base, "%s base is not above %s", lay[i].name, lay[i - 1].name);
            CHECK(lay[i - 1].base + lay[i - 1].size <= lay[i].base, "%s overlaps %s", lay[i - 1].name, lay[i].name);
        }
        uint64_t end;
        int ours = plat_mem_state(lay[i].base, lay[i].base + 1, &end) == PLAT_MEM_RESERVED;
        CHECK(ours, "%s is not inside the reserved module window", lay[i].name);
    }
    const RtGuestLayout *g = mem_layout();
    printf("guest memory: aslr %#llx+%#llx stack %#llx heap %#llx alias %#llx+%#llx kernel %#llx host functions %#llx\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->aslr.size, (unsigned long long)g->stack.base,
           (unsigned long long)g->heap.base, (unsigned long long)g->alias.base, (unsigned long long)g->alias.size,
           (unsigned long long)g->kernel.base, (unsigned long long)g->hostfn.base);
    // No overlap whatever the order: Windows and Linux put the stack and heap block below the modules and the alias
    // block above it, macOS has nothing mappable below the modules and puts both above the host functions.
    struct { const char *name; uint64_t base, size; } r[] = {
        {"stack", g->stack.base, g->stack.size},
        {"heap", g->heap.base, g->heap.size},
        {"alias", g->alias.base, g->alias.size},
        {"kernel", g->kernel.base, g->kernel.size},
        {"host functions", g->hostfn.base, g->hostfn.size},
        {"modules", lay[0].base, lay[n - 1].base + lay[n - 1].size - lay[0].base},
    };
    for (int i = 0; i < (int)(sizeof r / sizeof *r); i++) {
        CHECK(r[i].base < RT_ADDRESS_LIMIT && r[i].size <= RT_ADDRESS_LIMIT - r[i].base,
              "%s is not below 2^39", r[i].name);
        for (int j = i + 1; j < (int)(sizeof r / sizeof *r); j++)
            if (r[i].base < r[j].base + r[j].size && r[j].base < r[i].base + r[i].size)
                CHECK(0, "%s and %s overlap", r[i].name, r[j].name);
    }
}

// Every function start dispatches to its own function. The word after a start (unless that is a start too), a
// misaligned address and the word below the module dispatch to nothing.
static void check_modules(const RtModule *mods, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        const RtLayoutEntry *l = NULL;
        for (int k = 0; k < rt_layout_count; k++)
            if (!strcmp(rt_layout[k].name, m->name)) l = &rt_layout[k];
        CHECK(l && l->base == m->base && l->size == m->size, "%s: linked base/size differ from the layout", m->name);

        uint64_t bad = 0, bad_neg = 0;
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            if (rt_dispatch_lookup(a) != m->fns[k] && bad++ < 5)
                printf("  FAIL: %s_fn_addrs[%llu] %#llx does not dispatch to its function\n", m->name,
                       (unsigned long long)k, (unsigned long long)a);
            int next_is_start = k + 1 < m->count && m->addrs[k + 1] == a + 4;
            if ((!next_is_start && rt_dispatch_lookup(a + 4)) || rt_dispatch_lookup(a + 2)) bad_neg++;
        }
        if (rt_dispatch_lookup(m->base - 4)) bad_neg++;
        failures += bad > 0;
        CHECK(!bad_neg, "%s: %llu addresses that are not function starts dispatch to a function", m->name,
              (unsigned long long)bad_neg);
        uint64_t lo = m->count ? m->addrs[0] : 0, hi = m->count ? m->addrs[m->count - 1] : 0;
        printf("  %-8s %8llu functions, first %#llx, last %#llx: %s\n", m->name, (unsigned long long)m->count,
               (unsigned long long)lo, (unsigned long long)hi, bad || bad_neg ? "FAIL" : "ok");
        total += m->count;
    }
    printf("  total    %8llu functions\n", (unsigned long long)total);
}

static void bench(const RtModule *mods, int n) {
    uint64_t total = 0, sink = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        for (uint64_t k = 0; k < mods[i].count; k++) sink += (uintptr_t)rt_dispatch_lookup(mods[i].addrs[k]);
        total += mods[i].count;
    }
    double dt = now_s() - t0;
    __asm__ volatile("" ::"r"(sink));
    if (total) printf("lookup: %.1f ns per function start (address order)\n", dt * 1e9 / (double)total);
}

// Memory SVCs without modules: heap, physical memory in the alias region, a stack move, QueryMemory, GetInfo.
static void check_memory(void) {
    const RtGuestLayout *g = mem_layout();
    RtMemoryInfo q;
    uint64_t heap = 0, v = 0;
    int before = failures;
    CHECK(mem_set_heap_size(0x400000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 4 MiB");
    memset((void *)(uintptr_t)heap, 0xAB, 0x400000);
    mem_query(heap + 0x1000, &q);
    CHECK(q.base == heap && q.size == 0x400000 && q.state == 5 && q.perm == 3, "QueryMemory on the heap");
    CHECK(mem_set_heap_size(0x300000, &heap) != 0, "SetHeapSize accepts a size that is not a 2 MiB multiple");

    uint64_t a = g->alias.base + 0x100000;
    CHECK(mem_map_physical(a, 0x10000) == 0, "MapPhysicalMemory");
    ST64(a + 0x8000, 0x1234);
    CHECK(mem_map_physical(a - 0x4000, 0x20000) == 0 && LD64(a + 0x8000) == 0x1234,
          "MapPhysicalMemory over a mapped range lost its contents");
    mem_query(a, &q);
    CHECK(q.base == a - 0x4000 && q.size == 0x20000 && q.state == 5, "QueryMemory on mapped alias pages");
    mem_info(7, &v);
    CHECK(v == 0x400000 + 0x20000, "GetInfo 7 (used memory) %#llx", (unsigned long long)v);

    uint64_t stack = g->stack.base + 0x100000;
    ST64(a + 8, 0x99);
    CHECK(mem_map(stack, a, 0x4000) == 0 && LD64(stack + 8) == 0x99, "MapMemory alias -> stack did not copy");
    ST64(stack + 0x10, 0x5678);
    mem_query(stack, &q);
    CHECK(q.base == stack && q.size == 0x4000 && q.state == 0xB, "QueryMemory on a moved stack");
    mem_query(a, &q);
    CHECK(q.base == a && q.state == 5 && q.perm == 0, "QueryMemory on a moved stack's source");
    CHECK(mem_unmap_physical(a, 0x4000) != 0, "UnmapPhysicalMemory under a moved stack");
    CHECK(mem_unmap(stack, a, 0x4000) == 0 && LD64(a + 0x10) == 0x5678, "UnmapMemory did not copy the stack back");

    CHECK(mem_unmap_physical(a - 0x4000, 0x20000) == 0, "UnmapPhysicalMemory");
    mem_query(a, &q);
    CHECK(q.state == 0 && q.base == g->alias.base && q.size == g->alias.size, "QueryMemory on the free alias region");
    CHECK(mem_map_physical(a, 0x1000) == 0 && LD64(a + 0x10) == 0 && mem_unmap_physical(a, 0x1000) == 0,
          "remapped physical memory is not zeroed");
    CHECK(mem_info(6, &v) && v == g->pool, "GetInfo 6 (total memory) %#llx", (unsigned long long)v);
    uint64_t beyond_pool = (g->pool + 0x1000ull) & ~0xfffull;
    CHECK(beyond_pool > g->pool && a >= g->alias.base && a - g->alias.base <= g->alias.size &&
          beyond_pool <= g->alias.size - (a - g->alias.base) && mem_map_physical(a, beyond_pool) == 0x10801,
          "MapPhysicalMemory beyond the pool is not LimitReached");
    mem_query(RT_HOSTFN_BASE + 0x40, &q);
    CHECK(q.state == 0x10 && q.base == RT_HOSTFN_BASE, "QueryMemory on the host-function window");
    mem_query(g->alias.base + g->alias.size + 0x1000, &q);
    // The alias region can end exactly at 2^39 (macOS has no room to put it lower), and then that address is past the
    // guest address space: inaccessible rather than the host's, both of which mean the guest may not use it.
    CHECK((q.state == 3 && q.base == g->alias.base + g->alias.size) ||
          (q.state == 0x10 && q.base == RT_ADDRESS_LIMIT), "QueryMemory on host memory");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("memory: heap, physical memory, stack move, QueryMemory, GetInfo: %s\n", failures > before ? "FAIL" : "ok");
}

#ifdef _WIN32
// memory.c mem_commit_later against a heap shrink (DKB_HEAP_LAZY, Windows only). The background thread commits a
// staged pool's untouched pages about 2 s after the request, so the heap may shrink in between, or while it commits the
// range slice by slice. Neither may leave a committed page beyond the new heap end, nor mark one in the commit map: a
// later grow and mem_touch of the range must find every page reserved and commit it. Only plat_mem_state looks at the
// pages: a read of a reserved heap page would commit it through the fault handler. About 5 s, so a probe of its own
// (--probe-commit-later) rather than part of --selftest, which the setup runs at every install.
static int cl_range_wait(uint64_t *got) {   // until the thread has taken one more range, at most 10 s
    uint64_t s[4];
    for (int i = 0; i < 200; i++) {
        mem_commit_later_stats(s);
        if (s[0]) { *got = s[1]; return 1; }
        plat_sleep_ms(50);
    }
    return 0;
}

static int all_state(uint64_t a, uint64_t e, int committed) {   // every page of [a, e) committed, or every one reserved
    for (uint64_t x = a, y; x < e; x = y) {
        int st = plat_mem_state(x, e, &y);
        if (committed ? st < 0 : st != PLAT_MEM_RESERVED) return 0;
    }
    return 1;
}

static int probe_commit_later(void) {
    if (!cfg_bool("DKB_HEAP_LAZY")) {
        printf("commit-later: skipped (DKB_HEAP_LAZY=0)\n");
        return 0;
    }
    int before = failures;
    uint64_t heap = 0, s[4], got = 0;
    const uint64_t M = 1ull << 20;
    mem_commit_later_stats(s);   // clears the counts of anything before
    // The shrink comes before the range is due: the thread finds nothing of it left in the heap.
    CHECK(mem_set_heap_size(8 * M, &heap) == 0 && all_state(heap + 4 * M, heap + 8 * M, 0), "SetHeapSize 8 MiB (lazy)");
    mem_commit_later(heap + 4 * M, 4 * M);
    CHECK(mem_set_heap_size(4 * M, &heap) == 0, "SetHeapSize 4 MiB under a queued background commit");
    CHECK(cl_range_wait(&got), "the background commit thread took no range in 10 s");
    CHECK(all_state(heap + 4 * M, heap + 8 * M, 0), "a background commit queued before a shrink committed pages beyond the heap");
    CHECK(mem_set_heap_size(8 * M, &heap) == 0, "SetHeapSize 8 MiB again");
    mem_touch(heap + 4 * M, 4 * M);
    CHECK(all_state(heap + 4 * M, heap + 8 * M, 1), "mem_touch skipped pages after a shrink under a queued background commit");
    // The shrink comes while the thread commits the range, 2 MiB at a time: the slices before it are decommitted by it,
    // those after it stop at the new end.
    CHECK(mem_set_heap_size(64 * M, &heap) == 0 && all_state(heap + 8 * M, heap + 64 * M, 0), "SetHeapSize 64 MiB (lazy)");
    mem_commit_later(heap + 8 * M, 56 * M);
    int seen = 0, mid = 0;
    // Spins on the first slice's state (a VirtualQuery each) for at most 10 s: a sleep could outlast the whole range.
    for (uint64_t t_end = plat_now_ms() + 10000; !seen && plat_now_ms() < t_end;) {
        uint64_t y;
        seen = plat_mem_state(heap + 8 * M, heap + 8 * M + 0x10000, &y) >= 0;   // the thread is inside the range
    }
    if (seen) {
        uint64_t y;
        mid = plat_mem_state(heap + 64 * M - 0x10000, heap + 64 * M, &y) == PLAT_MEM_RESERVED;   // last slice not yet
    }
    CHECK(seen && mem_set_heap_size(32 * M, &heap) == 0, "SetHeapSize 32 MiB while a background commit runs");
    CHECK(cl_range_wait(&got), "the background commit thread did not finish its range in 10 s");
    CHECK(all_state(heap + 32 * M, heap + 64 * M, 0), "a background commit cut by a shrink committed pages beyond the heap");
    CHECK(mem_set_heap_size(64 * M, &heap) == 0, "SetHeapSize 64 MiB again");
    mem_touch(heap + 8 * M, 56 * M);
    CHECK(all_state(heap + 8 * M, heap + 64 * M, 1), "mem_touch skipped pages after a shrink inside a background commit");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("commit-later: background commits against a heap shrink (queued, and %s): %s\n",
           mid ? "inside the range" : "after the range: the thread was faster than the check, try again",
           failures > before ? "FAIL" : "ok");
    return failures > before;
}
#endif

static int selftest(void) {
    static RtModule mods[MAX_MODULES];
    int n = rt_linked_modules(mods, MAX_MODULES);

    check_cpu();
    check_layout();
    uint64_t probe = hostfn_register("<selftest probe>", host_probe);

    double t0 = now_s();
    hostfn_seal();
    double dt = now_s() - t0;
    RtDispatchStats s = rt_dispatch_stats();
    printf("dispatch: %d modules linked, table covers %#llx-%#llx: %llu slots, %.1f MiB reserved, built in %.0f ms\n",
           n, (unsigned long long)s.lo, (unsigned long long)s.hi, (unsigned long long)s.slots,
           (double)s.table_bytes / (1 << 20), dt * 1e3);
    check_modules(mods, n);
    CHECK(s.functions > 0, "no functions linked");
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    uint64_t r = rt_call(c, probe, 1, (uint64_t)7);
    CHECK(rt_dispatch_lookup(probe) && r == 42 && host_probe_hits == 1 && c->x[30] == 0,
          "a registered host function does not dispatch through rt_call");
    CHECK(!rt_dispatch_lookup(probe + 16), "an unregistered host-function address dispatches");
    CHECK(rt_dispatch_lookup(RT_RET_HOST), "the rt_call return sentinel is not registered");
    plat_aligned_free(c);
    bench(mods, n);

    void *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK((uintptr_t)k1 - RT_KERNEL_BASE < RT_KERNEL_SIZE && !((uintptr_t)k2 & 0xFFF) && k1 != k2,
          "kernel-region allocation");
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "a freed kernel-region block is not reused");
    check_memory();
    CHECK(!vk_backend_selftest(), "the replay write-back deduplication");
    CHECK(!splash_selftest(), "the start-up screen's estimate, texts and test sequence");

    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    if (proc >= 0) printf("process: working set %.0f MiB\n", proc);
    mem_commit_report("selftest");
    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR] [--set KEY=VALUE ...] [--version] [--help] [--selftest | --probe-loader | "
                    "--probe-cfg | --probe-dpi | --probe-commit-later (Windows) | --probe-kernel | --probe-ipc | --probe-ipc-lifetime | --probe-services | --probe-fs | --probe-gfx-abi | "
                    "--probe-nvn | --probe-nvn-tex | --probe-window | --probe-shader | --probe-shader-geometry | --probe-vkmem | "
                    "--probe-vkexec | --probe-vkgeometry | --probe-vkgeometry-passthrough | --probe-vkgeometry-passthrough-ordering | --probe-vkclear | --probe-vkcopy-astc | --probe-vkpool | --probe-vkreset | --probe-astc | --probe-movie | --probe-splash | --probe-aoc | --prepare-shaders | --translate-shaders | --warm-shaders | --warm-shaders-if-needed | --boot |--seconds S [--shot PNG]]\n", argv0);
    return 2;
}

// Paths for a copied folder (tools/package_build.py). Per path: DKB_EXEFS, DKB_ROMFS, DKB_PROFILE_DIR, else the
// folder next to dkb.exe (exefs with main.npdm, romfs with romfs.bin, any profile folder) if it is there, else the
// path baked in at build time. A relative value is relative to dkb.exe's folder.
static char pp_exefs[PLAT_PATH_MAX * 2], pp_romfs[PLAT_PATH_MAX * 2], pp_profile[PLAT_PATH_MAX * 2];

static const char *portable_path(const char *dir, const char *var, const char *sub, const char *must, char *out, size_t cap,
                                 const char *fallback) {
    const char *e = cfg_get(var);
    if (e && *e) {
        int abs_path = e[0] == '\\' || e[0] == '/' || e[1] == ':';
        if (abs_path) snprintf(out, cap, "%s", e);
        else snprintf(out, cap, "%s/%s", dir, e);
        return out;
    }
    char probe[PLAT_PATH_MAX * 3];
    snprintf(out, cap, "%s/%s", dir, sub);
    if (!must) return plat_dir_exists(out) ? out : fallback;   // the folder itself is the sign
    snprintf(probe, sizeof probe, "%s/%s", out, must);
    return plat_file_exists(probe) ? out : fallback;
}

static void portable_paths(RtConfig *c) {
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    char *cut = n && n < sizeof exe ? plat_last_sep(exe) : NULL;
    if (!cut) return;
    *cut = 0;
    c->exefs = portable_path(exe, "DKB_EXEFS", "exefs", "main.npdm", pp_exefs, sizeof pp_exefs, c->exefs);
    c->romfs = portable_path(exe, "DKB_ROMFS", "romfs", "romfs.bin", pp_romfs, sizeof pp_romfs, c->romfs);
    c->profile = portable_path(exe, "DKB_PROFILE_DIR", "profile", NULL, pp_profile, sizeof pp_profile, c->profile);
}

// The lifted code is built for the x86-64 level tools/build_game.py chose; on a CPU below it the first AVX2 or FMA
// instruction would fault, so say so and stop.
static void require_cpu(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
#if defined(__AVX2__)
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    const char *level = "x86-64-v3 (AVX2, FMA, BMI2)", *hint = "Rebuild with --march v2 (the setup does this on its own).";
#else
    int ok = __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
    const char *level = "x86-64-v2 (SSE4.2)", *hint = "This CPU is too old for the game.";
#endif
    if (!ok) {
        fprintf(stderr, "This build needs a CPU with %s, which this one lacks. %s\n", level, hint);
        exit(1);
    }
#endif
}

int main(int argc, char **argv) {
    require_cpu();
    // --set KEY=VALUE first: mem_init and the lines below already read settings.
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--set") && !cfg_set_arg(argv[++i])) {
            fprintf(stderr, "--set %s: not KEY=VALUE with a known setting (dkb.ini.example lists them)\n", argv[i]);
            return 2;
        }
    mem_init();   // first: claim the guest address space before anything else can land there; reads dkb.ini
    // Right after the settings can be read and before anything loads or calls the Vulkan loader (SDL's video init,
    // vk_init): the loader reads VK_DRIVER_FILES when it first lists the drivers.
    vk_driver_env_apply();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    // Build the host-to-guest function table now, not in the crash handler: its malloc there faults again after heap
    // damage, and the report then stops before the lifted function and the guest frames (#148).
    lifted_function(0);
    parse_trace();
    prof_start();
    // Timed guest waits (kernel.c block_inner) sleep in whole milliseconds; at the default 15.6 ms tick a 1 ms wait
    // costs a full tick. DKB_TIMER_1MS=0 keeps the default for comparison.
    if (cfg_bool("DKB_TIMER_1MS")) plat_timer_1ms();
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile,
                     .gpu = cfg_get("DKB_GPU")};
    portable_paths(&cfg);   // command-line paths below still win
    if (cfg.gpu && strcmp(cfg.gpu, "null") && strcmp(cfg.gpu, "vulkan")) {
        rt_logf("DKB_GPU must be null or vulkan, not '%s'\n", cfg.gpu);
        return 2;
    }
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (path) {
            if (++i == argc) return usage(argv[0]);
            *path = argv[i];
        } else if (!strcmp(argv[i], "--seconds")) {
            char *end;
            if (++i == argc) return usage(argv[0]);
            double s = strtod(argv[i], &end);
            if (*end || !(s > 0) || s > 1e6) return usage(argv[0]);
            cfg.seconds = s;
        } else if (!strcmp(argv[i], "--version")) {
            // The port version (VERSION, plus the commit of a checkout) and the game it was built for, so a report says
            // which build it came from.
            printf("%s %s (%s %s)\n", "dkb", rt_port_version,
                   rt_game_version ? "Tears of the Kingdom" : "Donkey Kong Bananza", rt_game_display);
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("%s [--exefs DIR] [--romfs DIR] [--profile DIR] [--set KEY=VALUE ...] [--seconds S [--shot PNG]]\n"
                   "  --set KEY=VALUE  a dkb.ini setting for this run only (dkb.ini.example lists the keys); the file "
                   "is not changed\n"
                   "  --version        the port version and the game it was built for\n"
                   "  --seconds S      play for S seconds and exit; --shot PNG saves the last frame\n"
                   "Settings: dkb.ini next to the exe, or the environment (DKB_ and the key in capitals).\n",
                   argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--set")) {
            if (++i == argc) return usage(argv[0]);   // applied at the start of main
        } else if (!strcmp(argv[i], "--shot")) {
            if (++i == argc) return usage(argv[0]);
            cfg.shot = argv[i];
        } else if (!mode && !strncmp(argv[i], "--", 2)) {
            mode = argv[i];
        } else {
            fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[i]);
            return usage(argv[0]);
        }
    }
    if (cfg.shot && !cfg.seconds) return usage(argv[0]);
    if (cfg.seconds && mode) return usage(argv[0]);
    log_open(!mode);
    if (!mode) {
        // console = 0: a frontend that starts the game gets no console window in front of the game's (#238).
        if (!cfg_bool("DKB_CONSOLE")) {
            const char *why = plat_console_hide();
            if (why) rt_log("console: left as is: %s", why);   // tells #364-style reports which case they hit
        }
        cfg.boot = cfg.seconds > 0;   // --seconds starts the game like --boot, with its checkpoints
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--selftest")) return selftest();
    if (!strcmp(mode, "--probe-loader")) return probe_loader();
    if (!strcmp(mode, "--probe-cfg")) return probe_cfg();
#ifdef _WIN32
    if (!strcmp(mode, "--probe-dpi")) return probe_dpi();   // the Win32 DPI awareness of dkb.manifest
#endif
#ifdef _WIN32
    if (!strcmp(mode, "--probe-commit-later")) return probe_commit_later();
#endif
    if (!strcmp(mode, "--probe-kernel")) return probe_kernel();
    if (!strcmp(mode, "--probe-ipc")) return probe_ipc();
    if (!strcmp(mode, "--probe-ipc-lifetime")) return probe_ipc_lifetime();
    if (!strcmp(mode, "--probe-services")) return probe_services();
    if (!strcmp(mode, "--probe-fs")) return probe_fs();
    if (!strcmp(mode, "--probe-gfx-abi")) return probe_gfx_abi();
    if (!strcmp(mode, "--probe-nvn")) return probe_nvn();
    if (!strcmp(mode, "--probe-nvn-tex")) return probe_nvn_tex();
    if (!strcmp(mode, "--probe-window")) return probe_window();
    if (!strcmp(mode, "--probe-shader")) return probe_shader();
    if (!strcmp(mode, "--probe-shader-geometry")) return probe_geometry_shader();
    if (!strcmp(mode, "--probe-vkmem")) return probe_vkmem();
    if (!strcmp(mode, "--probe-vkexec")) return probe_vkexec();
    if (!strcmp(mode, "--probe-vkgeometry")) return probe_vkgeometry();
    if (!strcmp(mode, "--probe-vkgeometry-passthrough")) return probe_vkgeometry_passthrough();
    if (!strcmp(mode, "--probe-vkgeometry-passthrough-ordering")) return probe_vkgeometry_ordering();
    if (!strcmp(mode, "--probe-vkclear")) return probe_vkclear();
    if (!strcmp(mode, "--probe-vkcopy-astc")) return probe_vkcopy_astc();
    if (!strcmp(mode, "--probe-vkpool")) return probe_vkpool();
    if (!strcmp(mode, "--probe-vkreset")) return probe_vkreset();
    if (!strcmp(mode, "--probe-astc")) return probe_astc();
    if (!strcmp(mode, "--probe-vkdevice")) return probe_vkdevice();
    if (!strcmp(mode, "--probe-movie")) return probe_movie();
    if (!strcmp(mode, "--probe-splash")) return probe_splash();
    if (!strcmp(mode, "--probe-aoc")) return probe_aoc();
    if (!strcmp(mode, "--prepare-shaders")) return gfx_shader_tool(GFX_TOOL_PREPARE_SHADERS);
    if (!strcmp(mode, "--translate-shaders")) return gfx_shader_tool(GFX_TOOL_TRANSLATE_SHADERS);
    if (!strcmp(mode, "--warm-shaders")) return gfx_shader_tool(GFX_TOOL_WARM_SHADERS);
    if (!strcmp(mode, "--warm-shaders-if-needed")) return gfx_shader_tool(GFX_TOOL_WARM_SHADERS_IF_NEEDED);
    if (!strcmp(mode, "--boot")) { cfg.boot = 1; rt_boot(); return 0; }
    fprintf(stderr, "%s: unknown option '%s'\n", argv[0], mode);   // a mistyped or unsupported flag is named (#402)
    return usage(argv[0]);
}
