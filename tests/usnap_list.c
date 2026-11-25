// Uniform range lists of DKB_USNAP_LIST (vk_usnap.c nvn_ubo_list_build / collect / list_check, nvn_api_cmd.c
// take_recording with OP_CALL list composition), through the real recorder. tests/usnap_list_test.py cuts the list,
// sort, collect and check functions out of runtime/vk_usnap.c into usnap_list_cut.c. No game, renderer or GPU.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_api.h"
#include "platform/platform.h"

#include "usnap_list_cut.c"
int rt_ab(void) { return 0; }   // nvn_api_cmd.c's DKB_CMD_REC_FAST
int rt_knob(const char *name, const char *def) { (void)name; return !def || strcmp(def, "0") != 0; }

#define PROC(n) void nvnp_##n(Cpu *c)
PROC(nvnCommandBufferInitialize);
PROC(nvnCommandBufferFinalize);
PROC(nvnCommandBufferAddCommandMemory);
PROC(nvnCommandBufferAddControlMemory);
PROC(nvnCommandBufferBeginRecording);
PROC(nvnCommandBufferEndRecording);
PROC(nvnCommandBufferBindUniformBuffer);
PROC(nvnCommandBufferBindUniformBuffers);
PROC(nvnCommandBufferBindStorageBuffer);
PROC(nvnCommandBufferCallCommands);
PROC(nvnCommandBufferPushGraphicsConstantsDeferred);
#undef PROC

static int checks, failures;
#define CHECK(cond, text) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", text); } } while (0)
#define UP(p) ((uint64_t)(uintptr_t)(p))
static uint64_t call(void (*fn)(Cpu *), const uint64_t args[8]) {
    Cpu c = {0};
    memcpy(c.x, args, sizeof c.x[0] * 8);
    fn(&c);
    return c.x[0];
}
#define CALL(n, ...) call(nvnp_##n, (const uint64_t[8]){__VA_ARGS__})

// The recorder and the submit preparation are linked; these replace the host services they reach. Single-threaded.
unsigned rt_trace;
static uint64_t replayed;
int nvn_ubo_lists_on(void) { return 1; }   // DKB_USNAP_LIST on: overrides the recorder's weak default
void nvn_called(uint8_t *seen_, const char *name) { (void)seen_; (void)name; }
void nvn_mode_trace(unsigned kind, uint64_t cb, uint64_t handle, uint32_t value, uint64_t submit) {
    (void)kind; (void)cb; (void)handle; (void)value; (void)submit;
}
int hdr_trace_hit(const Cpu *c, const char *name) { (void)c; (void)name; return 0; }
void hdr_trace_dump(const char *name, const void *p) { (void)name; (void)p; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }
uint64_t gfx_replayed_submits(void) { return replayed; }
// DKB_CALL_REF from USNAP_LIST_CALL_REF (tests/usnap_list_test.py runs the fixture with 0 and 1): the recorder reads it
// once.
int cfg_bool(const char *name) {
    if (!strcmp(name, "DKB_CALL_REF")) return getenv("USNAP_LIST_CALL_REF") && atoi(getenv("USNAP_LIST_CALL_REF"));
    rt_fatal("unexpected setting lookup: %s", name);
    return 0;
}
const GfxBackend *gfx_backend(void) { rt_fatal("unexpected sync wait in the uniform list fixture"); return NULL; }
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    (void)c; (void)fn; (void)n;
    rt_fatal("unexpected guest callback in the uniform list fixture");
}

static uint64_t rnd(void) { static uint64_t x = 88172645463325252ull; x ^= x << 13, x ^= x >> 7, x ^= x << 17; return x; }

typedef struct { uint64_t addr, size; int c4; } Bind;

// A binding as the walk makes it (vk_usnap.c collect): none without an address or a size, aligned down, at most
// MAX_RANGE from the address, with RNG_C4 on VS/FS slot 1 bindings of at most 256 bytes.
static int norm(Bind b, Rng *out) {
    if (!b.addr || !b.size) return 0;
    uint64_t n = b.size < MAX_RANGE ? b.size : MAX_RANGE;
    uint64_t c4 = (b.c4 && b.size <= 256) ? RNG_C4 : 0;
    *out = (Rng){(b.addr & ~(uint64_t)(ALIGN - 1)) | c4, b.addr + n};
    return 1;
}
static int by_lo_hi(const void *a, const void *b) {
    const Rng *x = a, *y = b;
    uint64_t xl = x->lo & ~RNG_C4, yl = y->lo & ~RNG_C4;
    return xl != yl ? (xl < yl ? -1 : 1) : x->hi != y->hi ? (x->hi < y->hi ? -1 : 1) : 0;
}
// The expected list: every binding normalised, sorted, and the ranges that overlap or touch merged (brute force).
static size_t expect(const Bind *b, size_t n, Rng *out) {
    size_t k = 0, m = 0;
    for (size_t i = 0; i < n; i++) k += norm(b[i], &out[k]);
    qsort(out, k, sizeof *out, by_lo_hi);
    for (size_t i = 0; i < k; i++) {
        if (m && (out[i].lo & ~RNG_C4) <= out[m - 1].hi) {
            out[m - 1].lo |= out[i].lo & RNG_C4;
            if (out[i].hi > out[m - 1].hi) out[m - 1].hi = out[i].hi;
        } else {
            out[m++] = out[i];
        }
    }
    return m;
}
static int list_is(const nvn_range_list *l, const Rng *want, size_t n) {
    if (!l || l->n != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (l->r[i].lo != want[i].lo || l->r[i].hi != want[i].hi) return 0;
    for (size_t i = 1; i < n; i++)   // sorted, and no two overlap or touch: each range is there once
        if ((l->r[i].lo & ~RNG_C4) <= l->r[i - 1].hi) return 0;
    return 1;
}

// Bindings that repeat, overlap, touch, cross a 256-byte line, pass 64 KiB, or have no address or no size.
static Bind random_bind(void) {
    uint64_t slot = rnd() % 64, addr = 0x10000000ull + slot * 0x1000 + (rnd() % 4 ? (rnd() % 16) * 64 : rnd() % 4096);
    uint64_t size = rnd() % 8 ? 16 + rnd() % 2048 : rnd() % 2 ? 0x20000 : 0;
    if (rnd() % 3 == 0) size = ALIGN - addr % ALIGN + ALIGN * (rnd() % 3);   // ends on a line another range starts on
    if (rnd() % 64 == 0) addr = 0;
    if (rnd() % 128 == 0) size = ~0ull - rnd() % 64;   // a size that wraps the address: clamped like any other
    return (Bind){addr, size, (int)(rnd() % 3 == 0)};
}

static void begin(NVNcommandBuffer *cmd, void *control, size_t control_size) {
    CALL(nvnCommandBufferInitialize, UP(cmd), 0);
    CALL(nvnCommandBufferAddCommandMemory, UP(cmd), 0, 0, 0x10000);
    CALL(nvnCommandBufferAddControlMemory, UP(cmd), UP(control), control_size);
}
static void bind_all(NVNcommandBuffer *cmd, const Bind *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i].c4) {
            CALL(nvnCommandBufferBindUniformBuffer, UP(cmd), (uint64_t)(i & 1), 1, b[i].addr, b[i].size);
        } else if (i % 3 == 0 && i + 1 < n && !b[i + 1].c4) {   // two non-c4 at once through BindUniformBuffers
            NVNbufferRange r[2] = {{b[i].addr, b[i].size}, {b[i + 1].addr, b[i + 1].size}};
            CALL(nvnCommandBufferBindUniformBuffers, UP(cmd), 0, (uint64_t)(2 + (i % 10)), 2, UP(r));
            i++;
        } else {
            CALL(nvnCommandBufferBindUniformBuffer, UP(cmd), 1, (uint64_t)(2 + (i % 12)), b[i].addr, b[i].size);
        }
        if (i % 5 == 0) CALL(nvnCommandBufferBindStorageBuffer, UP(cmd), 0, 0, 0x7000000ull, 64);   // not a uniform
    }
}

static Bind binds[8000], called[2000];
static Rng want[9000];

// Recordings of every size on one command buffer: small ones are copied, large ones hand their buffer over. Each
// copy's list is the brute-force one, and an empty recording has an empty list, not none.
static void recorded_lists(void) {
    NVNcommandBuffer cmd = {0};
    _Alignas(16) uint8_t control[0x8000];
    begin(&cmd, control, sizeof control);
    uint64_t live0 = nvn_ubo_lists_live();
    int handovers = 0, copies = 0, heap_lists = 0;
    static const size_t sizes[] = {0, 1, 7, 300, 4000, 4000, 1500, 2, 6000, 0, 4000};
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        size_t n = sizes[s];
        for (size_t i = 0; i < n; i++) binds[i] = random_bind();
        CALL(nvnCommandBufferBeginRecording, UP(&cmd));
        bind_all(&cmd, binds, n);
        uint64_t seg = UP(((const cmd_t *)(const void *)&cmd)->seg);
        uint64_t h = CALL(nvnCommandBufferEndRecording, UP(&cmd));
        if (h == seg) handovers++;
        else copies++;
        size_t m = expect(binds, n, want);
        heap_lists += m > 2;
        char text[96];
        snprintf(text, sizeof text, "the list of a recording of %zu bindings is the merged, sorted ranges", n);
        CHECK(list_is(nvn_handle_ubo_list(h), want, m), text);
    }
    CHECK(handovers > 0 && copies > 0, "both the handed-over and the copied recordings are covered");
    CHECK(nvn_ubo_lists_live() == live0 + (uint64_t)heap_lists, "one heap list block per recording with >2 uniform ranges");
    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    CHECK(nvn_ubo_lists_live() == live0, "releasing the copies frees their lists");
    CALL(nvnCommandBufferFinalize, UP(&cmd));
}

// A list by hand: bound out of order, one twice, two that touch at a 256-byte line (merged, as the walk's regions
// are), one inside another, one past 64 KiB, and none for no address or no size.
static void fixed_list(void) {
    NVNcommandBuffer cmd = {0};
    _Alignas(16) uint8_t control[0x1000];
    begin(&cmd, control, sizeof control);
    static const Bind b[] = {{0x30000310, 0x20, 0}, {0x30000000, 0x100, 0}, {0x30000100, 0x40, 0}, {0x30000310, 0x20, 0},
                             {0x30000020, 0x10, 0}, {0x30001000, 0x30000, 0}, {0, 0x100, 0}, {0x30000400, 0, 0}};
    static const Rng r[] = {{0x30000000, 0x30000140}, {0x30000300, 0x30000330}, {0x30001000, 0x30011000}};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, b, sizeof b / sizeof *b);
    uint64_t h = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(list_is(nvn_handle_ubo_list(h), r, sizeof r / sizeof *r), "a list by hand: sorted, merged, clamped");
    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    CALL(nvnCommandBufferFinalize, UP(&cmd));
}

// CallCommands with and without DKB_CALL_REF: callee lists compose across OP_CALL (including repeated callees,
// empty callees, merged > 64 KiB callee ranges, and nested chains up to CALL_REF_DEPTH) and pass list_check.
static void called_lists(void) {
    NVNcommandBuffer cmd = {0};
    _Alignas(16) uint8_t control[0x4000];
    begin(&cmd, control, sizeof control);

    // Empty callee (0 bindings -> &ubo_empty): calling it must keep the caller's list valid.
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    uint64_t empty_h = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(list_is(nvn_handle_ubo_list(empty_h), NULL, 0), "empty callee has an empty list");

    // Callee with two overlapping 64 KiB bindings that merge into a single 96 KiB range (> MAX_RANGE).
    static const Bind wide_b[] = {{0x40000000ull, 0x10000, 0}, {0x40008000ull, 0x10000, 0}};
    static const Rng wide_r[] = {{0x40000000ull, 0x40018000ull}};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, wide_b, 2);
    uint64_t wide_h = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(list_is(nvn_handle_ubo_list(wide_h), wide_r, 1), "callee merges overlapping 64 KiB bindings into 96 KiB");

    // Build a 6-level OP_CALL chain (matching CALL_REF_DEPTH) with repeated consecutive and non-consecutive calls.
    size_t total = 0;
    uint64_t level[7];
    level[0] = wide_h;
    called[total++] = wide_b[0];
    called[total++] = wide_b[1];
    for (int d = 1; d <= 6; d++) {
        size_t chunk = 40;
        for (size_t i = 0; i < chunk; i++) called[total + i] = random_bind();
        CALL(nvnCommandBufferBeginRecording, UP(&cmd));
        bind_all(&cmd, called + total, chunk / 2);
        // Consecutive duplicate calls (exercises last_cp skip) plus empty callee and repeated non-consecutive call.
        uint64_t seq[4] = {level[d - 1], level[d - 1], empty_h, level[d - 1]};
        CALL(nvnCommandBufferCallCommands, UP(&cmd), 4, UP(seq));
        bind_all(&cmd, called + total + chunk / 2, chunk - chunk / 2);
        level[d] = CALL(nvnCommandBufferEndRecording, UP(&cmd));
        total += chunk;
        size_t m = expect(called, total, want);
        CHECK(list_is(nvn_handle_ubo_list(level[d]), want, m),
              "nested OP_CALL caller composes callee lists and preserves >64 KiB merged ranges");
    }
    if (nvn_call_ref_on()) CHECK(nvn_copy_depth(level[6]) == 6, "nested call chain reaches CALL_REF_DEPTH 6");

    // list_check against true full op walk (allow_callee_lists = 0) must report 0 bad lists.
    U.fast = 1;
    U.list_bad = 0;
    list_check(level, 7);
    CHECK(U.list_bad == 0, "list_check agrees with full op walk across nested OP_CALL tree");

    // Also test collect(..., allow_callee_lists = 1) vs collect(..., allow_callee_lists = 0) on the top handle.
    U.ncol = 0;
    memset(seen, 0, sizeof seen);
    collect(nvn_op_first(level[6]), 0, 1);
    sort_col();
    size_t m_fast = rng_merge(U.col, (size_t)U.ncol);
    size_t m_exp = expect(called, total, want);
    CHECK(m_fast == m_exp && !memcmp(U.col, want, m_exp * sizeof *want),
          "collect with callee lists matches full walk on nested OP_CALL tree");

    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    CALL(nvnCommandBufferFinalize, UP(&cmd));
}

// Asynchronous replay: a second submission of a copy still queued gets a clone, which has no list (the replay walks
// it, using callee lists for OP_CALL children). A copy released while queued keeps its list until it is swept, and the
// clone frees none.
static void async_lists(void) {
    NVNcommandBuffer cmd = {0}, other = {0};
    _Alignas(16) uint8_t control[0x1000], control2[0x1000];
    uint32_t source[4] = {1, 2, 3, 4};
    begin(&cmd, control, sizeof control);
    begin(&other, control2, sizeof control2);
    replayed = nvn_submit_count();
    for (size_t i = 0; i < 40; i++) binds[i] = random_bind();
    binds[0] = (Bind){0x20000040ull, 64, 0};   // guarantee >= 3 disjoint ranges so h uses a heap list
    binds[1] = (Bind){0x21000040ull, 64, 0};
    binds[2] = (Bind){0x22000040ull, 64, 0};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, binds, 40);
    CALL(nvnCommandBufferPushGraphicsConstantsDeferred, UP(&cmd), 0, 0, 16, UP(source));   // makes the copy clonable
    uint64_t h = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    uint64_t live0 = nvn_ubo_lists_live(), first, second;
    nvn_submit_prepare_async(&h, &first, 1);
    nvn_submit_prepare_async(&h, &second, 1);
    CHECK(first == h && second != h, "the second submission of a queued copy is a clone");
    CHECK(nvn_handle_ubo_list(second) == NULL, "a clone has no list");
    size_t m = expect(binds, 40, want);
    CHECK(list_is(nvn_handle_ubo_list(h), want, m), "the original keeps its list");
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);   // released while queued
    CHECK(!nvn_handle_live(h) && nvn_ubo_lists_live() == live0, "a released copy that is still queued keeps its list");
    CHECK(list_is(nvn_handle_ubo_list(h), want, m), "the queued copy's list stays readable");
    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&other), UP(control2), sizeof control2);   // sweeps the replayed ones
    CHECK(nvn_ubo_lists_live() == live0 - 1, "the swept copy frees its list, the clone none");
    CALL(nvnCommandBufferFinalize, UP(&cmd));
    CALL(nvnCommandBufferFinalize, UP(&other));
}

// Radix sort (k >= 256, 1/2/3 passes) vs merge sort fallback (k < 256 or span > 33 bits) parity in nvn_ubo_list_build.
static void radix_and_span_cases(void) {
    static const struct { size_t n; uint64_t stride; const char *label; } cases[] = {
        {100, 0x1000ull, "small recording (<256, merge sort)"},
        {600, 0x100ull, "medium recording (>=256, 1-pass radix)"},
        {600, 0x40000ull, "medium recording (>=256, 2-pass radix)"},
        {600, 0x20000000ull, "medium recording (>=256, 3-pass radix)"},
        {600, 0x1000000000ull, "large span (>33 bits / 2 TiB, merge sort fallback)"},
    };
    static Rng in[1024], ref[1024];
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        size_t n = cases[c].n;
        for (size_t i = 0; i < n; i++) {
            uint64_t base = 0x10000000ull + (rnd() % 512) * cases[c].stride;
            base &= ~(uint64_t)(ALIGN - 1);
            uint64_t sz = 64 + (rnd() % 4096);
            in[i] = ref[i] = (Rng){base | (rnd() & RNG_C4), base + sz};
        }
        qsort(ref, n, sizeof *ref, by_lo_hi);
        size_t m_ref = rng_merge(ref, n);
        size_t m_got = nvn_ubo_list_build(in, n);
        CHECK(m_got == m_ref && !memcmp(in, ref, m_ref * sizeof *ref), cases[c].label);
    }
}

// vk_usnap_begin with lists: each recording's list appended to U.col, sorted (sort_col) and merged, must give the
// regions the walk gives (add_range of every binding, sort_col, merge), with and without USNAP_FAST.
static Rng a_out[20000];
static int union_cases(void) {
    int fails = 0;
    for (int round = 0; round < 40; round++) {
        int k = 1 + (int)(rnd() % 24);
        size_t start[25] = {0}, total = 0;
        for (int r = 0; r < k; r++) {
            size_t n = rnd() % 4 == 0 ? 0 : rnd() % 300;
            for (size_t i = 0; i < n; i++) binds[total + i] = random_bind();
            total += n, start[r + 1] = total;
        }
        for (int fast = 0; fast < 2; fast++) {
            U.fast = fast, U.ncol = 0;
            memset(seen, 0, sizeof seen);
            for (size_t i = 0; i < total; i++) {
                Rng r;
                if (norm(binds[i], &r)) add_range(r);
            }
            sort_col();
            size_t na = rng_merge(U.col, (size_t)U.ncol);
            memcpy(a_out, U.col, na * sizeof *a_out);
            U.ncol = 0;
            for (int r = 0; r < k; r++) {
                static nvn_range raw[8000];
                size_t n = start[r + 1] - start[r], kn = 0;
                for (size_t i = 0; i < n; i++) {
                    Rng nr;
                    if (norm(binds[start[r] + i], &nr)) raw[kn++] = nr;
                }
                size_t m = nvn_ubo_list_build(raw, kn);
                while (U.ccol - U.ncol < (int)m) U.col = grow(U.col, &U.ccol, sizeof *U.col);
                memcpy(U.col + U.ncol, raw, m * sizeof *raw);
                U.ncol += (int)m;
            }
            sort_col();
            size_t nb = rng_merge(U.col, (size_t)U.ncol);
            checks++;
            if (na != nb || memcmp(a_out, U.col, na * sizeof *a_out))
                fails++, printf("FAIL: round %d fast %d: %zu regions from the lists, %zu from the walk\n", round, fast,
                                nb, na);
        }
    }
    U.fast = 0, U.ncol = 0;
    return fails;
}

// Inline lists (n <= 2) vs heap lists (n >= 3) and lazy seen[] transition past 16 bindings.
static void inline_and_lazy_seen_cases(void) {
    NVNcommandBuffer cmd = {0};
    _Alignas(16) uint8_t control[0x4000];
    begin(&cmd, control, sizeof control);
    uint64_t live0 = nvn_ubo_lists_live();

    // n == 1: inline in copy_prefix, 0 heap allocations.
    static const Bind b1[] = {{0x50000120ull, 64, 0}};
    static const Rng r1[] = {{0x50000100ull, 0x50000160ull}};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, b1, 1);
    uint64_t h1 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(list_is(nvn_handle_ubo_list(h1), r1, 1), "1-range recording matches inline list");
    CHECK(nvn_ubo_lists_live() == live0, "1-range inline list takes no heap allocation");

    // n == 2 (from 5 out-of-order/duplicate/touching bindings): inline in copy_prefix, 0 heap allocations.
    static const Bind b2[] = {{0x51000400ull, 64, 0}, {0x51000000ull, 256, 0}, {0x51000100ull, 64, 0},
                              {0x51000400ull, 64, 0}, {0x51000020ull, 32, 0}};
    static const Rng r2[] = {{0x51000000ull, 0x51000140ull}, {0x51000400ull, 0x51000440ull}};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, b2, 5);
    uint64_t h2 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(list_is(nvn_handle_ubo_list(h2), r2, 2), "2-range recording matches inline list");
    CHECK(nvn_ubo_lists_live() == live0, "2-range inline list takes no heap allocation");

    // n == 3: heap-allocated, live increments by 1.
    static const Bind b3[] = {{0x52002000ull, 64, 0}, {0x52000000ull, 64, 0}, {0x52001000ull, 64, 0}};
    static const Rng r3[] = {{0x52000000ull, 0x52000040ull}, {0x52001000ull, 0x52001040ull},
                             {0x52002000ull, 0x52002040ull}};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, b3, 3);
    uint64_t h3 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(list_is(nvn_handle_ubo_list(h3), r3, 3), "3-range recording matches heap list");
    CHECK(nvn_ubo_lists_live() == live0 + 1, "3-range list allocates one heap block");

    // Lazy seen[] transition across index 16: duplicates before 16, exact trigger at 16, duplicates after 16.
    Bind b17[32];
    size_t k = 0;
    for (int i = 0; i < 12; i++) b17[k++] = (Bind){0x53000000ull + (uint64_t)i * 0x1000, 64, 0};
    for (int i = 0; i < 4; i++) b17[k++] = b17[i];   // duplicates before index 16 (nubo reaches 16)
    b17[k++] = b17[2];                                // duplicate right at transition (nubo == 16 -> seen_on = 1)
    for (int i = 12; i < 20; i++) b17[k++] = (Bind){0x53000000ull + (uint64_t)i * 0x1000, 64, 0};
    for (int i = 0; i < 7; i++) b17[k++] = b17[i * 2];   // duplicates after seen_on == 1
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, b17, k);
    uint64_t h17 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    size_t m17 = expect(b17, k, want);
    CHECK(list_is(nvn_handle_ubo_list(h17), want, m17), "lazy seen transition past 16 ranges matches brute-force");
    CHECK(nvn_ubo_lists_live() == live0 + 2, "heap list count includes >2-range recording after lazy seen");

    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    CHECK(nvn_ubo_lists_live() == live0, "releasing inline and heap copies returns live heap lists to baseline");
    CALL(nvnCommandBufferFinalize, UP(&cmd));
}

// A recycled block from the free cache (blk_free) must have its prefix's ubo pointer cleared before reuse in rec_buffer.
static void recycled_block_clears_ubo(void) {
    NVNcommandBuffer cmd = {0};
    _Alignas(16) uint8_t control[0x4000];
    begin(&cmd, control, sizeof control);
    // 2500 uniform binds (~85 KiB) lands in the 128 KiB block class and hands over its recording buffer as the copy.
    for (size_t i = 0; i < 2500; i++)
        binds[i] = (Bind){0x60000000ull + (i % 8) * 0x1000, 64, 0};
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, binds, 2500);
    uint64_t seg1 = UP(((const cmd_t *)(const void *)&cmd)->seg);
    uint64_t h1 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(h1 == seg1, "recycled-block test hands over the 128 KiB recording buffer");
    CHECK(nvn_handle_ubo_list(h1) != NULL, "handed-over copy has a non-NULL uniform list before release");
    // Release the copy so its 128 KiB block returns to the top of the free cache with its prior prefix memory intact.
    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    // A second handover of the same 128 KiB class calls rec_buffer, popping h1's block from the free cache as cmd.seg.
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    bind_all(&cmd, binds, 2500);
    uint64_t seg2 = UP(((const cmd_t *)(const void *)&cmd)->seg);
    uint64_t h2 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    uint64_t recycled = UP(((const cmd_t *)(const void *)&cmd)->seg);
    CHECK(h2 == seg2, "second recording also hands over its 128 KiB buffer");
    CHECK(recycled == h1, "rec_buffer pops the just-released 128 KiB block from the free cache");
    CHECK(nvn_handle_ubo_list(recycled) == NULL, "rec_buffer clears stale ubo pointer on a recycled block");
    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    CALL(nvnCommandBufferFinalize, UP(&cmd));
}

// RNG_C4 propagation through ubo_buf_add (before, across, and after UBO_SEEN_MIN), nvn_ubo_list_build (insertion,
// presorted, merge, radix, >33-bit span), rng_merge (overlapping & touching), sort_col (presorted, radix, qsort),
// and list_check.
static void c4_flag_cases(void) {
    NVNcommandBuffer cmd = {0};
    _Alignas(16) uint8_t control[0x4000];
    begin(&cmd, control, sizeof control);

    // (a) Non-c4 then c4 on identical range: both below UBO_SEEN_MIN and after seen_on == 1, plus right at the
    //     lazy seen[] transition (index 16). Also check c4 then non-c4, size > 256 at slot 1, and compute stage slot 1.
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    // Range A (0x70000000): non-c4 first, then c4 after seen_on == 1 -> must upgrade and carry RNG_C4.
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 0, 0, 0x70000000ull, 128);
    // Range B (0x70001000): c4 first, then non-c4 -> must keep RNG_C4.
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 1, 1, 0x70001000ull, 256);
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 0, 2, 0x70001000ull, 256);
    // Range C (0x70002000): slot 1 with size > 256 -> NOT c4.
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 0, 1, 0x70002000ull, 512);
    // Range D (0x70003000): compute stage (stage 5) slot 1 size <= 256 -> NOT c4.
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 5, 1, 0x70003000ull, 128);
    // Range E (0x70004000): non-c4 before UBO_SEEN_MIN, upgraded right at index 16 transition.
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 1, 3, 0x70004000ull, 64);
    // Push 11 more unique non-c4 ranges so nubo reaches 16 (5+1+10 = 16 -> next bind triggers seen[] lazy init).
    for (int i = 0; i < 10; i++)
        CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 0, 4, 0x70010000ull + (uint64_t)i * 0x1000, 64);
    // Now nubo == 16: upgrade Range E right at the lazy seen[] transition!
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 0, 1, 0x70004000ull, 64);
    // Now seen_on == 1: upgrade Range A after seen_on == 1 (slot already holds non-c4 Range A)!
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 1, 1, 0x70000000ull, 128);
    // Repeat non-c4 on Range A after c4 upgrade: must remain c4.
    CALL(nvnCommandBufferBindUniformBuffer, UP(&cmd), 0, 0, 0x70000000ull, 128);
    uint64_t hc4 = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    const nvn_range_list *lc4 = nvn_handle_ubo_list(hc4);
    CHECK(lc4 && lc4->n == 15, "c4 recording produced 15 disjoint ranges");
    if (lc4 && lc4->n == 15) {
        CHECK(lc4->r[0].lo == (0x70000000ull | RNG_C4) && lc4->r[0].hi == 0x70000080ull,
              "non-c4 then c4 after seen_on preserves RNG_C4");
        CHECK(lc4->r[1].lo == (0x70001000ull | RNG_C4) && lc4->r[1].hi == 0x70001100ull,
              "c4 then non-c4 preserves RNG_C4");
        CHECK(lc4->r[2].lo == 0x70002000ull && lc4->r[2].hi == 0x70002200ull,
              "slot 1 size > 256 does not set RNG_C4");
        CHECK(lc4->r[3].lo == 0x70003000ull && lc4->r[3].hi == 0x70003080ull,
              "compute stage slot 1 does not set RNG_C4");
        CHECK(lc4->r[4].lo == (0x70004000ull | RNG_C4) && lc4->r[4].hi == 0x70004040ull,
              "non-c4 then c4 right at UBO_SEEN_MIN transition preserves RNG_C4");
    }
    // list_check verifies take_recording matches collect() on both bounds and RNG_C4 bits.
    U.fast = 1;
    U.list_bad = 0;
    list_check(&hc4, 1);
    CHECK(U.list_bad == 0, "list_check agrees on bounds and RNG_C4 bits");

    // (b) Overlapping and touching non-c4 + c4 ranges merge via rng_merge with RNG_C4 ORed.
    Rng ov[4] = {
        {0x80000000ull, 0x80000100ull},                 // non-c4
        {0x80000100ull | RNG_C4, 0x80000180ull},        // touching at 0x80000100 with RNG_C4
        {0x80001000ull, 0x80001400ull},                 // wide non-c4
        {0x80001100ull | RNG_C4, 0x80001200ull},        // overlapping interior c4
    };
    size_t mov = rng_merge(ov, 4);
    CHECK(mov == 2 &&
          ov[0].lo == (0x80000000ull | RNG_C4) && ov[0].hi == 0x80000180ull &&
          ov[1].lo == (0x80001000ull | RNG_C4) && ov[1].hi == 0x80001400ull,
          "rng_merge merges touching and overlapping non-c4 + c4 ranges with RNG_C4 ORed");

    // (c) Presorted, insertion sort (k <= 16), and radix sort (k >= 256) in nvn_ubo_list_build + sort_col:
    //     same base address with c4 before non-c4 must still sort by lo & ~RNG_C4 and preserve RNG_C4.
    Rng ins[5] = {
        {0x81002000ull, 0x81002080ull},
        {0x81001000ull | RNG_C4, 0x81001080ull},
        {0x81000000ull, 0x81000100ull},
        {0x81001000ull, 0x81001100ull},
        {0x81002000ull | RNG_C4, 0x81002040ull},
    };
    size_t mins = nvn_ubo_list_build(ins, 5);
    CHECK(mins == 3 &&
          ins[0].lo == 0x81000000ull && ins[0].hi == 0x81000100ull &&
          ins[1].lo == (0x81001000ull | RNG_C4) && ins[1].hi == 0x81001100ull &&
          ins[2].lo == (0x81002000ull | RNG_C4) && ins[2].hi == 0x81002080ull,
          "insertion sort in nvn_ubo_list_build masks RNG_C4 and ORs on merge");

    // Presorted fast path (k == 20 > 16) with c4 before non-c4 at the same masked lo.
    Rng pre[20];
    for (int i = 0; i < 10; i++) {
        uint64_t b = 0x82000000ull + (uint64_t)i * 0x1000;
        pre[2 * i] = (Rng){b | RNG_C4, b + 0x40};
        pre[2 * i + 1] = (Rng){b, b + 0x80};
    }
    size_t mpre = nvn_ubo_list_build(pre, 20);
    int pre_ok = mpre == 10;
    for (int i = 0; pre_ok && i < 10; i++) {
        uint64_t b = 0x82000000ull + (uint64_t)i * 0x1000;
        pre_ok &= pre[i].lo == (b | RNG_C4) && pre[i].hi == b + 0x80;
    }
    CHECK(pre_ok, "presorted path in nvn_ubo_list_build masks RNG_C4 and preserves flag");

    // sort_col: presorted, radix, and >33-bit qsort paths with mixed RNG_C4.
    U.fast = 1;
    // Presorted path in sort_col (c4 before non-c4 at identical masked lo).
    U.ncol = 0;
    while (U.ccol < 4) U.col = grow(U.col, &U.ccol, sizeof *U.col);
    U.col[0] = (Rng){0x83000000ull | RNG_C4, 0x83000040ull};
    U.col[1] = (Rng){0x83000000ull, 0x83000080ull};
    U.col[2] = (Rng){0x83001000ull, 0x83001040ull};
    U.col[3] = (Rng){0x83001000ull | RNG_C4, 0x83001080ull};
    U.ncol = 4;
    sort_col();
    size_t msc = rng_merge(U.col, (size_t)U.ncol);
    CHECK(msc == 2 &&
          U.col[0].lo == (0x83000000ull | RNG_C4) && U.col[0].hi == 0x83000080ull &&
          U.col[1].lo == (0x83001000ull | RNG_C4) && U.col[1].hi == 0x83001080ull,
          "sort_col presorted path masks RNG_C4 and preserves flag");

    // qsort fallback (>33-bit span) in sort_col with mixed RNG_C4.
    U.col[0] = (Rng){0x40000000000ull | RNG_C4, 0x40000000080ull};
    U.col[1] = (Rng){0x100000000ull, 0x100000080ull};
    U.col[2] = (Rng){0x100000000ull | RNG_C4, 0x100000100ull};
    U.col[3] = (Rng){0x40000000000ull, 0x40000000100ull};
    U.ncol = 4;
    sort_col();
    msc = rng_merge(U.col, (size_t)U.ncol);
    CHECK(msc == 2 &&
          U.col[0].lo == (0x100000000ull | RNG_C4) && U.col[0].hi == 0x100000100ull &&
          U.col[1].lo == (0x40000000000ull | RNG_C4) && U.col[1].hi == 0x40000000100ull,
          "sort_col qsort fallback masks RNG_C4 and preserves flag");

    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(control), sizeof control);
    CALL(nvnCommandBufferFinalize, UP(&cmd));
}

// Incremental callee tracking (DKB_CMD_TRACK_FAST): inline (<=4) and heap (>4) callee arrays, inlining a deferred copy
// that itself holds OP_CALL records, async cloning a copy with >4 callees, and abandoning recordings via BeginRecording
// or Finalize must maintain exact callee refcounts and matching UBO lists.
static void callee_track_cases(void) {
    NVNcommandBuffer leaf_cmd = {0}, cmd = {0}, other = {0};
    _Alignas(16) uint8_t leaf_ctl[0x2000], ctl[0x4000], ctl2[0x1000];
    uint32_t src[4] = {10, 20, 30, 40};
    begin(&leaf_cmd, leaf_ctl, sizeof leaf_ctl);
    begin(&cmd, ctl, sizeof ctl);
    begin(&other, ctl2, sizeof ctl2);
    replayed = nvn_submit_count();

    uint64_t leaves[6];
    Bind leaf_b[6];
    for (int i = 0; i < 6; i++) {
        leaf_b[i] = (Bind){0x90000000ull + (uint64_t)i * 0x2000, 0x100, i == 0};
        CALL(nvnCommandBufferBeginRecording, UP(&leaf_cmd));
        bind_all(&leaf_cmd, &leaf_b[i], 1);
        leaves[i] = CALL(nvnCommandBufferEndRecording, UP(&leaf_cmd));
    }

    // 1. Copy with 6 callees (>4, heap callees array) + deferred push so it is both inlined and clonable.
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    CALL(nvnCommandBufferCallCommands, UP(&cmd), 6, UP(leaves));
    CALL(nvnCommandBufferPushGraphicsConstantsDeferred, UP(&cmd), 0, 0, 16, UP(src));
    uint64_t h_def = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    if (nvn_call_ref_on()) {
        CHECK(nvn_copy_refs(leaves[0]) == 1 && nvn_copy_refs(leaves[5]) == 1,
              "recording with 6 callees takes 1 ref per callee");
    }

    // 2. Caller that inlines h_def (deferred -> body copied in, callees appended to caller's tracker).
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    CALL(nvnCommandBufferCallCommands, UP(&cmd), 1, UP(&h_def));
    uint64_t h_inl = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    if (nvn_call_ref_on()) {
        CHECK(nvn_copy_refs(leaves[0]) == 2 && nvn_copy_refs(leaves[5]) == 2,
              "inlining a deferred copy with 6 callees increments each callee ref");
    }
    U.fast = 1;
    U.list_bad = 0;
    list_check(&h_inl, 1);
    CHECK(U.list_bad == 0, "list_check agrees after inlining a deferred copy with callees");

    // 3. Async clone of h_def (heap callees): clone duplicates callees array and increments refs, then sweeps cleanly.
    uint64_t s1, s2;
    nvn_submit_prepare_async(&h_def, &s1, 1);
    nvn_submit_prepare_async(&h_def, &s2, 1);
    CHECK(s1 == h_def && s2 != h_def, "second async submit of deferred copy with >4 callees clones it");
    if (nvn_call_ref_on()) {
        CHECK(nvn_copy_refs(leaves[0]) == 3 && nvn_copy_refs(leaves[5]) == 3,
              "async clone of copy with >4 callees increments callee refs");
    }

    // 4. Abandon recording via BeginRecording and via Finalize: both must release tracked callee refs.
    CALL(nvnCommandBufferBeginRecording, UP(&other));
    CALL(nvnCommandBufferCallCommands, UP(&other), 6, UP(leaves));
    if (nvn_call_ref_on()) CHECK(nvn_copy_refs(leaves[0]) == 4, "active recording holds callee ref before abandon");
    CALL(nvnCommandBufferBeginRecording, UP(&other));   // abandons previous recording
    if (nvn_call_ref_on()) CHECK(nvn_copy_refs(leaves[0]) == 3, "BeginRecording while recording drops tracked callee refs");
    CALL(nvnCommandBufferCallCommands, UP(&other), 2, UP(leaves));
    if (nvn_call_ref_on()) CHECK(nvn_copy_refs(leaves[0]) == 4, "inline callee count (<=4) tracked before Finalize");
    CALL(nvnCommandBufferFinalize, UP(&other));   // abandons recording on Finalize
    if (nvn_call_ref_on()) CHECK(nvn_copy_refs(leaves[0]) == 3, "Finalize while recording drops tracked callee refs");

    // Release callers + leaves and sweep: every ref and zombie must drain to 0.
    replayed = nvn_submit_count();
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(ctl), sizeof ctl);
    if (nvn_call_ref_on()) {
        CHECK(nvn_copy_refs(leaves[0]) == 0 && nvn_copy_refs(leaves[5]) == 0,
              "releasing callers and sweeping async clone returns all callee refs to 0");
    }
    CALL(nvnCommandBufferAddControlMemory, UP(&leaf_cmd), UP(leaf_ctl), sizeof leaf_ctl);
    CHECK(nvn_copy_zombies() == 0, "all copies and clones freed with zero zombies left");
    CALL(nvnCommandBufferFinalize, UP(&cmd));
    CALL(nvnCommandBufferFinalize, UP(&leaf_cmd));
}

int main(void) {
    fixed_list();
    recorded_lists();
    recycled_block_clears_ubo();
    inline_and_lazy_seen_cases();
    called_lists();
    async_lists();
    radix_and_span_cases();
    c4_flag_cases();
    callee_track_cases();
    failures += union_cases();
    printf("usnap lists (DKB_CALL_REF %d): %d checks, %d failures\n", nvn_call_ref_on(), checks, failures);
    return failures != 0;
}
