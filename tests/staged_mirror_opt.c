// Unit tests for the post-#442 staged-mirror optimizations in runtime/vk_memory.c:
//   1. cpu_written_chunk bitmap tracking (ww_stamp, vk_guest_written, written_batch, vk_gpu_written_into,
//      cpu_written_mark_range), decommit_hook clearing (mem_decommit_cb), and staged_runs() bit-scanning.
//   2. staged_split_own() O(1) fast path when r->cpu_read == NULL against the page-by-page loop, including
//      overlapping, touching, clipped, and out-of-order ranges, plus mixed pools with fetched pages.
//
// tests/staged_mirror_opt_test.py pastes the production types and functions from runtime/vk_memory.c below.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "msort.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "platform/platform.h"

unsigned rt_trace;
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

uint64_t kern_now_ns(void) { return 0; }
void perf_lock_wait(int lock, uint64_t ns) { (void)lock; (void)ns; }

// Single-threaded lock stubs that also check lock balance and non-reentrancy.
static int mutex_excl, mutex_shared;
void plat_mutex_lock(PlatMutex *m) {
    if (m->p) rt_fatal("lock reentered exclusively");
    m->p = (void *)1;
    mutex_excl++;
}
int plat_mutex_trylock(PlatMutex *m) {
    if (m->p) return 0;
    m->p = (void *)1;
    mutex_excl++;
    return 1;
}
void plat_mutex_unlock(PlatMutex *m) {
    if (m->p != (void *)1) rt_fatal("exclusive unlock mismatch");
    m->p = NULL;
    mutex_excl--;
}
void plat_mutex_lock_shared(PlatMutex *m) {
    if (m->p == (void *)1) rt_fatal("shared lock while held exclusively");
    m->p = (void *)((uintptr_t)m->p + 2);
    mutex_shared++;
}
int plat_mutex_trylock_shared(PlatMutex *m) {
    if (m->p == (void *)1) return 0;
    m->p = (void *)((uintptr_t)m->p + 2);
    mutex_shared++;
    return 1;
}
void plat_mutex_unlock_shared(PlatMutex *m) {
    if ((uintptr_t)m->p < 2 || ((uintptr_t)m->p & 1)) rt_fatal("shared unlock mismatch");
    m->p = (void *)((uintptr_t)m->p - 2);
    mutex_shared--;
}

static uint64_t qpc(void) { return 0; }

// Minimal knob helpers for written_batch (RT_KNOB_ON and nvn_gpu_only come from host.h / nvn_api.h).
int rt_knob(const char *name, const char *def) { (void)name; (void)def; return 1; }
int rt_ab(void) { return 1; }

// Mock write-watch backing for mem_written_pages (used by ww_stamp and mem_decommit_cb).
#define MOCK_WW_MAX 4096
static uint64_t mock_ww_pages[MOCK_WW_MAX];
static uint64_t mock_ww_n;
static int mock_ww_untracked;
static int mock_ww_stuck;
static uint64_t mock_ww_calls;

static void mock_ww_reset(void) {
    mock_ww_n = 0;
    mock_ww_untracked = 0;
    mock_ww_stuck = 0;
    mock_ww_calls = 0;
}

static void mock_ww_add(uint64_t page) {
    if (mock_ww_n >= MOCK_WW_MAX) rt_fatal("mock_ww overflow");
    mock_ww_pages[mock_ww_n++] = page & ~0xfffull;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    mock_ww_calls++;
    if (mock_ww_untracked) return UINT64_MAX;
    if (!max || !size) return 0;
    if (mock_ww_stuck) {
        // Pathological non-advancing return to verify mem_decommit_cb's next <= cur guard terminates.
        for (uint64_t i = 0; i < max; i++) pages[i] = addr > 0x1000 ? addr - 0x1000 : 0;
        return max;
    }
    qsort(mock_ww_pages, (size_t)mock_ww_n, sizeof mock_ww_pages[0], cmp_u64);
    uint64_t lo = addr & ~0xfffull, hi = addr + size;
    uint64_t out = 0, kept = 0;
    for (uint64_t i = 0; i < mock_ww_n; i++) {
        uint64_t p = mock_ww_pages[i];
        if (p >= lo && p < hi && out < max) {
            if (!out || pages[out - 1] != p) pages[out++] = p;
        } else {
            mock_ww_pages[kept++] = p;
        }
    }
    mock_ww_n = kept;
    return out;
}

/* PRODUCTION_TYPES */

static uint64_t ww_query(uint64_t addr, uint64_t size, int who) {
    (void)who;
    return mem_written_pages(addr, size, gen_pages, sizeof gen_pages / sizeof *gen_pages);
}

static PlatMutex pool_lock = PLAT_MUTEX_INIT;
static void pool_lock_shared(void) { plat_mutex_lock_shared(&pool_lock); }
static pidx pool_index;

int vk_mirror_on = 1;
static int shadow_on;
static uint32_t ww_ep = 1;
static uint32_t reg_last[RT_ADDRESS_LIMIT >> 21];

static MirrorAcc mrep;
static PlatMutex st_lock = PLAT_MUTEX_INIT;
static StJob *st_cur;
static MRange *stw;
static int nstw, cstw;
static int fetch_test;
static volatile int64_t st_sort_ns;
static volatile int64_t own_fb_ranges, own_fb_bytes;
static int sort_queued_count;

static void st_sort_queue(StJob *j) { (void)j; sort_queued_count++; }

void vk_gpu_written(uint64_t addr, uint64_t size) { vk_guest_written(addr, size); }

/* PRODUCTION_FUNCTIONS */

static int cpu_written_bit(uint64_t page) {
    uint64_t ci = page >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS || !cpu_written_chunk[ci]) return 0;
    uint64_t pi = (page >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1);
    return (int)((cpu_written_chunk[ci][pi >> 6] >> (pi & 63)) & 1u);
}

static uint32_t own_chunk_val(uint64_t page) {
    uint64_t ci = page >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS || !own_chunk[ci]) return 0;
    return own_chunk[ci][(page >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
}

static void reset_gen_tables(void) {
    for (uint64_t i = 0; i < GEN_CHUNKS; i++) {
        free(gen_chunk[i]), gen_chunk[i] = NULL;
        free(own_chunk[i]), own_chunk[i] = NULL;
        free(cpu_written_chunk[i]), cpu_written_chunk[i] = NULL;
    }
    gen_now = 0;
    memset(reg_max, 0, sizeof reg_max);
    memset(reg_last, 0, sizeof reg_last);
}

// Naive page-by-page reference oracle for staged_runs.
static uint64_t ref_staged_runs(PoolRec *r, MRange **out, int *nout, int *cout) {
    uint64_t copied = 0;
    for (uint64_t p = r->import_lo; p < r->import_hi; p += 0x1000) {
        if (cpu_written_bit(p)) {
            range_add(out, nout, cout, r, p - r->import_lo, 0x1000);
            copied += 0x1000;
        }
    }
    return copied;
}

static void test_cpu_written_and_decommit(void) {
    reset_gen_tables();
    mock_ww_reset();

    // Two pools: a staged mirror (sfeed=1) and a regular non-staged mirror (sfeed=0), plus an unmirrored address.
    PoolRec staged = {
        .kind = POOL_IMPORT, .lo = 0x100000000ull, .size = 0x800000ull,
        .import_lo = 0x100000000ull, .import_hi = 0x100800000ull,
        .mstate = MS_ON, .mbuf = (VkBuffer)(uintptr_t)0x1111, .sfeed = 1, .mgen = 0,
    };
    PoolRec regular = {
        .kind = POOL_IMPORT, .lo = 0x101000000ull, .size = 0x100000ull,
        .import_lo = 0x101000000ull, .import_hi = 0x101100000ull,
        .mstate = MS_ON, .mbuf = (VkBuffer)(uintptr_t)0x2222, .sfeed = 0, .mgen = 0,
    };
    pidx_ent e1 = {.lo = staged.lo, .hi = staged.lo + staged.size, .buf = &staged, .key = &staged};
    pidx_ent e2 = {.lo = regular.lo, .hi = regular.lo + regular.size, .buf = &regular, .key = &regular};
    CHECK(pidx_insert(&pool_index, &e1));
    CHECK(pidx_insert(&pool_index, &e2));

    // 1. Fresh pool has zero staged_runs bytes.
    MRange *runs = NULL;
    int nruns = 0, cruns = 0;
    CHECK(staged_runs(&staged, &runs, &nruns, &cruns) == 0 && nruns == 0);

    // 2. GPU writes into staged mirror (sfeed=1) via written_batch and vk_gpu_written_into MUST stamp gen_chunk
    //    and own_chunk, but MUST NOT set cpu_written_chunk; staged_runs skips them completely.
    VkGpuWrite gw[3] = {
        {.addr = staged.lo + 0x2000, .size = 0x5000, .buf = staged.mbuf},
        {.addr = regular.lo + 0x1000, .size = 0x3000, .buf = regular.mbuf},
        {.addr = 0x102000000ull, .size = 0x2000, .buf = (VkBuffer)(uintptr_t)0x3333}, // non-mirror
    };
    written_batch(gw, 3);
    vk_gpu_written_into(staged.lo + 0x10000, 0x4000, staged.mbuf);
    vk_gpu_written_into(regular.lo + 0x10000, 0x2000, regular.mbuf);

    for (uint64_t off = 0x2000; off < 0x7000; off += 0x1000) {
        CHECK(gen_slot(staged.lo + off, 0) && *gen_slot(staged.lo + off, 0) > 0);
        CHECK(own_chunk_val(staged.lo + off) > 0);
        CHECK(!cpu_written_bit(staged.lo + off));
    }
    for (uint64_t off = 0x10000; off < 0x14000; off += 0x1000) {
        CHECK(gen_slot(staged.lo + off, 0) && *gen_slot(staged.lo + off, 0) > 0);
        CHECK(own_chunk_val(staged.lo + off) > 0);
        CHECK(!cpu_written_bit(staged.lo + off));
    }
    nruns = 0;
    CHECK(staged_runs(&staged, &runs, &nruns, &cruns) == 0 && nruns == 0);

    // Non-staged mirror (sfeed=0) and non-mirror writes DO set cpu_written.
    for (uint64_t off = 0x1000; off < 0x4000; off += 0x1000) CHECK(cpu_written_bit(regular.lo + off));
    for (uint64_t off = 0x10000; off < 0x12000; off += 0x1000) CHECK(cpu_written_bit(regular.lo + off));
    CHECK(cpu_written_bit(0x102000000ull) && cpu_written_bit(0x102001000ull));

    // 3. ww_stamp marks CPU-written pages (including multi-batch >1024 pages).
    mock_ww_reset();
    for (uint64_t k = 0; k < 1100; k++) mock_ww_add(staged.lo + 0x40000 + k * 0x1000);
    // Also add isolated pages at offset 0x0 and 0x2000 (overlapping earlier GPU write).
    mock_ww_add(staged.lo + 0x0);
    mock_ww_add(staged.lo + 0x2000);
    plat_mutex_lock(&gen_lock);
    CHECK(ww_stamp(staged.lo, staged.lo + staged.size, ++gen_now, 0) == 1);
    plat_mutex_unlock(&gen_lock);
    CHECK(mock_ww_calls == 2); // 1024 + 78
    CHECK(cpu_written_bit(staged.lo + 0x0));
    CHECK(cpu_written_bit(staged.lo + 0x2000));
    CHECK(!cpu_written_bit(staged.lo + 0x3000)); // GPU-only stays clear
    for (uint64_t k = 0; k < 1100; k++) CHECK(cpu_written_bit(staged.lo + 0x40000 + k * 0x1000));

    // 4. cpu_written_mark_range (used by staged_wb_done, staged_fetch, own_settle_failed) and vk_guest_written.
    cpu_written_mark_range(staged.lo + 0x20100, 0x2f01); // spans pages 0x20000, 0x21000, 0x22000, 0x23000
    for (uint64_t off = 0x20000; off < 0x24000; off += 0x1000) CHECK(cpu_written_bit(staged.lo + off));
    CHECK(!cpu_written_bit(staged.lo + 0x1f000) && !cpu_written_bit(staged.lo + 0x24000));
    vk_guest_written(staged.lo + 0x30000, 0x2000);
    CHECK(cpu_written_bit(staged.lo + 0x30000) && cpu_written_bit(staged.lo + 0x31000));

    // Verify staged_runs matches naive oracle before decommit.
    MRange *ref = NULL;
    int nref = 0, cref = 0;
    nruns = 0;
    uint64_t got_bytes = staged_runs(&staged, &runs, &nruns, &cruns);
    uint64_t want_bytes = ref_staged_runs(&staged, &ref, &nref, &cref);
    CHECK(got_bytes == want_bytes && nruns == nref);
    for (int i = 0; i < nruns && i < nref; i++)
        CHECK(runs[i].r == ref[i].r && runs[i].off == ref[i].off && runs[i].size == ref[i].size);

    // 5. mem_decommit_cb drains stale write-watch bits (multi-batch), clears cpu_written and own_chunk in range,
    //    and preserves adjacent bits outside range.
    mock_ww_reset();
    for (uint64_t k = 0; k < 1050; k++) mock_ww_add(staged.lo + 0x40000 + k * 0x1000);
    // Keep one dirty page outside decommit range to ensure drain only touches [a, a+n).
    mock_ww_add(staged.lo + 0x0);
    // Decommit [staged.lo + 0x21000, staged.lo + 0x40000 + 1099 * 0x1000):
    //   - partial head inside word containing 0x21000 (keeps 0x20000 set!)
    //   - full middle words
    //   - partial tail leaving 0x40000 + 1099*0x1000 set!
    uint64_t dlo = staged.lo + 0x21000, dhi = staged.lo + 0x40000 + 1099 * 0x1000;
    mem_decommit_cb(dlo, dhi - dlo);
    CHECK(mock_ww_n == 1 && mock_ww_pages[0] == staged.lo + 0x0); // stale bits inside [dlo, dhi) drained
    CHECK(cpu_written_bit(staged.lo + 0x20000));
    CHECK(!cpu_written_bit(staged.lo + 0x21000));
    CHECK(!cpu_written_bit(staged.lo + 0x30000));
    CHECK(!cpu_written_bit(dhi - 0x1000));
    CHECK(cpu_written_bit(dhi)); // page right at dhi preserved

    // Single-word partial clear (w0 == w1) and own_chunk zeroing.
    CHECK(own_chunk_val(staged.lo + 0x2000) > 0 && own_chunk_val(staged.lo + 0x3000) > 0 && own_chunk_val(staged.lo + 0x6000) > 0);
    mem_decommit_cb(staged.lo + 0x2000, 0x3000); // pages 2,3,4
    CHECK(!cpu_written_bit(staged.lo + 0x2000));
    CHECK(own_chunk_val(staged.lo + 0x2000) == 0 && own_chunk_val(staged.lo + 0x3000) == 0 && own_chunk_val(staged.lo + 0x4000) == 0);
    CHECK(own_chunk_val(staged.lo + 0x5000) > 0 && own_chunk_val(staged.lo + 0x6000) > 0);
    CHECK(cpu_written_bit(staged.lo + 0x0));

    // Exact 64-page single-word clear (w0 == w1, cnt == 64).
    uint64_t wbase = 0x104000000ull; // page-aligned and 64-page aligned
    cpu_written_mark_range(wbase, 66 * 0x1000);
    mem_decommit_cb(wbase, 64 * 0x1000);
    for (uint64_t k = 0; k < 64; k++) CHECK(!cpu_written_bit(wbase + k * 0x1000));
    CHECK(cpu_written_bit(wbase + 64 * 0x1000) && cpu_written_bit(wbase + 65 * 0x1000));

    // Cross-chunk (256 MiB boundary) mark, staged_runs, and decommit!
    uint64_t boundary = 1ull << GEN_CHUNK_SHIFT; // 0x10000000
    cpu_written_mark_range(boundary - 5 * 0x1000, 10 * 0x1000);
    PoolRec cross = {
        .kind = POOL_IMPORT, .lo = boundary - 3 * 0x1000, .size = 6 * 0x1000,
        .import_lo = boundary - 3 * 0x1000, .import_hi = boundary + 3 * 0x1000,
    };
    nruns = 0;
    CHECK(staged_runs(&cross, &runs, &nruns, &cruns) == 6 * 0x1000 && nruns == 1 && runs[0].off == 0 && runs[0].size == 6 * 0x1000);
    mem_decommit_cb(boundary - 2 * 0x1000, 4 * 0x1000);
    CHECK(cpu_written_bit(boundary - 3 * 0x1000) && !cpu_written_bit(boundary - 2 * 0x1000));
    CHECK(!cpu_written_bit(boundary) && !cpu_written_bit(boundary + 0x1000) && cpu_written_bit(boundary + 2 * 0x1000));
    nruns = 0;
    CHECK(staged_runs(&cross, &runs, &nruns, &cruns) == 2 * 0x1000 && nruns == 2);
    CHECK(runs[0].off == 0 && runs[0].size == 0x1000 && runs[1].off == 5 * 0x1000 && runs[1].size == 0x1000);

    // Untracked range (UINT64_MAX) and non-advancing guard in mem_decommit_cb must still clear bits and terminate.
    cpu_written_mark_range(0x105000000ull, 0x4000);
    mock_ww_reset();
    mock_ww_untracked = 1;
    mem_decommit_cb(0x105000000ull, 0x2000);
    CHECK(!cpu_written_bit(0x105000000ull) && !cpu_written_bit(0x105001000ull) && cpu_written_bit(0x105002000ull));
    mock_ww_reset();
    mock_ww_stuck = 1;
    mem_decommit_cb(0x105002000ull, 0x2000);
    CHECK(!cpu_written_bit(0x105002000ull) && !cpu_written_bit(0x105003000ull));
    mock_ww_reset();

    // 6. Sub-word boundary clamping fuzz for staged_runs against naive oracle (same word, multi-word, multi-chunk).
    uint64_t rng = 0x123456789abcdef0ull;
    for (int tc = 0; tc < 200; tc++) {
        reset_gen_tables();
        rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
        uint64_t base = (rng & 1) ? ((1ull << GEN_CHUNK_SHIFT) - 70 * 0x1000) : 0x200000000ull;
        uint64_t p_lo = (rng >> 8) & 127;
        uint64_t p_len = 1 + ((rng >> 16) & 255);
        PoolRec pr = {
            .kind = POOL_IMPORT, .lo = base + p_lo * 0x1000, .size = p_len * 0x1000,
            .import_lo = base + p_lo * 0x1000, .import_hi = base + (p_lo + p_len) * 0x1000,
        };
        // Mark pages including outside [import_lo, import_hi) (before and after) to test strict sub-word clamping!
        for (uint64_t k = 0; k < p_lo + p_len + 16; k++) {
            rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
            if ((rng & 3) == 0 || k + 1 == p_lo || k == p_lo + p_len)
                cpu_written_mark_range(base + k * 0x1000, 0x1000);
        }
        nruns = 0, nref = 0;
        uint64_t g = staged_runs(&pr, &runs, &nruns, &cruns);
        uint64_t w = ref_staged_runs(&pr, &ref, &nref, &cref);
        CHECK(g == w && nruns == nref);
        for (int i = 0; i < nruns && i < nref; i++)
            CHECK(runs[i].off == ref[i].off && runs[i].size == ref[i].size);
    }

    CHECK(pidx_remove(&pool_index, &staged, NULL));
    CHECK(pidx_remove(&pool_index, &regular, NULL));
    free(runs);
    free(ref);
    reset_gen_tables();
}

static uint64_t xorshift(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *s = x;
}

static void test_staged_split_own(void) {
    enum { NP = 4, MAX_IN = 64 };
    PoolRec pools_fast[NP], pools_slow[NP];
    uint8_t *zero_cr[NP];
    uint64_t sizes[NP] = {0x4000, 0x10000, 0x23000, 0x8000};
    for (int p = 0; p < NP; p++) {
        memset(&pools_fast[p], 0, sizeof pools_fast[p]);
        pools_fast[p].lo = 0x100000000ull * (uint64_t)(p + 1);
        pools_fast[p].size = sizes[p];
        pools_fast[p].import_lo = pools_fast[p].lo;
        pools_fast[p].import_hi = pools_fast[p].lo + sizes[p];
        pools_fast[p].storage_serial = (uint64_t)(100 + p);
        pools_fast[p].cpu_read = NULL; // O(1) fast path

        pools_slow[p] = pools_fast[p];
        zero_cr[p] = calloc((size_t)(sizes[p] >> 12) + 1, 1);
        pools_slow[p].cpu_read = zero_cr[p]; // all-zero page-by-page path
    }

    uint64_t seed = 0x9e3779b97f4a7c15ull;
    for (int tc = 0; tc < 5000; tc++) {
        int nin = (int)(xorshift(&seed) % MAX_IN) + 1;
        MRange in_fast[MAX_IN], in_slow[MAX_IN];
        for (int i = 0; i < nin; i++) {
            int p = (int)(xorshift(&seed) % NP);
            uint64_t lim = sizes[p];
            uint64_t off, sz;
            unsigned kind = (unsigned)(xorshift(&seed) % 8);
            if (kind == 0 && i > 0 && in_fast[i - 1].r == &pools_fast[p]) {
                // Touching end of previous range
                off = in_fast[i - 1].off + in_fast[i - 1].size;
                sz = (xorshift(&seed) % 0x3000) + 1;
            } else if (kind == 1 && i > 0 && in_fast[i - 1].r == &pools_fast[p]) {
                // Overlapping inside previous range (both extending past and contained inside)
                uint64_t prev_sz = in_fast[i - 1].size ? in_fast[i - 1].size : 1;
                off = in_fast[i - 1].off + (xorshift(&seed) % prev_sz);
                sz = (xorshift(&seed) % (prev_sz + 0x3000)) + 1;
            } else if (kind == 2 && i > 0 && in_fast[i - 1].r == &pools_fast[p] && in_fast[i - 1].off > 0) {
                // Out-of-order range starting BEFORE previous range (possibly overlapping it)
                off = xorshift(&seed) % in_fast[i - 1].off;
                sz = (xorshift(&seed) % (lim - off + 0x2000)) + 1;
            } else if (kind == 3) {
                // Clipped at/beyond lim (including off >= lim -> empty after clamp)
                off = lim - 0x400 + (xorshift(&seed) % 0x800);
                sz = (xorshift(&seed) % 0x2000);
            } else {
                // Arbitrary sub-page or multi-page range
                off = xorshift(&seed) % (lim + 0x100);
                sz = xorshift(&seed) % 0x5000;
            }
            in_fast[i] = (MRange){&pools_fast[p], off, sz, 0};
            in_slow[i] = (MRange){&pools_slow[p], off, sz, 0};
        }

        // Run fast path (r->cpu_read == NULL)
        StJob jf = {0}, js = {0};
        fetch_test = 0;
        memset(&mrep, 0, sizeof mrep);
        nstw = 0;
        for (int i = 0; i < nin; i++) {
            if (nstw == cstw) grow((void **)&stw, &cstw, sizeof *stw);
            stw[nstw++] = in_fast[i];
        }
        st_cur = &jf;
        staged_split_own();
        CHECK(nstw == 0);
        uint64_t fast_pieces = mrep.st_sp_pieces;

        // Run slow path (r->cpu_read != NULL, all zeros)
        memset(&mrep, 0, sizeof mrep);
        nstw = 0;
        for (int i = 0; i < nin; i++) {
            if (nstw == cstw) grow((void **)&stw, &cstw, sizeof *stw);
            stw[nstw++] = in_slow[i];
        }
        st_cur = &js;
        staged_split_own();
        CHECK(nstw == 0);

        // Pre-sort exact equivalence (element-for-element)!
        CHECK(jf.no == js.no);
        for (int k = 0; k < jf.no && k < js.no; k++) {
            int pf = (int)(jf.o[k].r - pools_fast), ps = (int)(js.o[k].r - pools_slow);
            CHECK(pf == ps && jf.o[k].lo == js.o[k].lo && jf.o[k].serial == js.o[k].serial &&
                  jf.o[k].off == js.o[k].off && jf.o[k].size == js.o[k].size);
        }
        // Fast path must do O(1) pieces (<= slow path pieces).
        CHECK(fast_pieces <= mrep.st_sp_pieces);

        // Post-sort exact equivalence (after st_sort_job merges overlaps/out-of-order ranges).
        st_sort_job(&jf);
        st_sort_job(&js);
        CHECK(jf.no == js.no);
        for (int k = 0; k < jf.no && k < js.no; k++) {
            int pf = (int)(jf.o[k].r - pools_fast), ps = (int)(js.o[k].r - pools_slow);
            CHECK(pf == ps && jf.o[k].off == js.o[k].off && jf.o[k].size == js.o[k].size);
        }

        free(jf.o);
        free(js.o);
    }

    // Mixed test: pool 0 has cpu_read == NULL (all owned), pool 1 has page 2 marked CPU-read (splits into owned + stw).
    zero_cr[1][2] = 1;
    StJob jm = {0};
    nstw = 0;
    if (cstw < 4) grow((void **)&stw, &cstw, sizeof *stw);
    stw[nstw++] = (MRange){&pools_fast[0], 0x100, 0x2500, 0};
    stw[nstw++] = (MRange){&pools_slow[1], 0x1800, 0x2000, 0}; // spans pages 1, 2 (cpu_read=1), 3
    st_cur = &jm;
    staged_split_own();
    CHECK(nstw == 1 && stw[0].r == &pools_slow[1] && stw[0].off == 0x2000 && stw[0].size == 0x1000);
    st_sort_job(&jm);
    CHECK(jm.no == 3);
    // Pool 0 [0x100, 0x2600), Pool 1 [0x1800, 0x2000) and [0x3000, 0x3800)
    int got0 = 0, got1a = 0, got1b = 0;
    for (int k = 0; k < jm.no; k++) {
        if (jm.o[k].r == &pools_fast[0] && jm.o[k].off == 0x100 && jm.o[k].size == 0x2500) got0++;
        if (jm.o[k].r == &pools_slow[1] && jm.o[k].off == 0x1800 && jm.o[k].size == 0x800) got1a++;
        if (jm.o[k].r == &pools_slow[1] && jm.o[k].off == 0x3000 && jm.o[k].size == 0x800) got1b++;
    }
    CHECK(got0 == 1 && got1a == 1 && got1b == 1);
    CHECK(sort_queued_count > 0);
    free(jm.o);

    for (int p = 0; p < NP; p++) free(zero_cr[p]);
    free(stw), stw = NULL, nstw = cstw = 0;
}

static void test_st_sort_job_radix(void) {
    // Compare st_sort_job (radix fast path + >64-pool and >4 GiB offset fallbacks) against sort_st_ranges + merge.
    enum { MAX_P = 80 };
    static PoolRec pools[MAX_P];
    memset(pools, 0, sizeof pools);
    for (int i = 0; i < MAX_P; i++) pools[i].lo = 0x100000000ull + (uint64_t)i * 0x10000000ull, pools[i].storage_serial = (uint64_t)(i + 1);
    static StRange *ref_buf;
    static size_t ref_cap;
    const int counts[] = {0, 1, 17, 32, 33, 500, 4000};
    const int npools[] = {1, 4, 16, 72};
    uint64_t rng = 0x123456789abcdef0ull;
    for (size_t ci = 0; ci < sizeof counts / sizeof *counts; ci++) {
        for (size_t pi = 0; pi < sizeof npools / sizeof *npools; pi++) {
            int n = counts[ci], np = npools[pi];
            StRange *a = n ? malloc((size_t)n * sizeof *a) : NULL;
            StRange *b = n ? malloc((size_t)n * sizeof *b) : NULL;
            for (int i = 0; i < n; i++) {
                rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
                int p = (int)(rng % (uint64_t)np);
                uint64_t off = ((rng >> 16) % 0x1000000ull) & ~15ull;
                if (pi == 1 && ci == 5 && i == 0) off = 0x1000000000ull;   // >4 GiB span fallback case
                uint64_t sz = 16 + ((rng >> 40) % 4096ull);
                a[i] = b[i] = (StRange){&pools[p], pools[p].lo, pools[p].storage_serial, off, sz, 0};
            }
            StJob jf = {.o = a, .no = n};
            st_sort_job(&jf);
            sort_st_ranges(b, (size_t)n, &ref_buf, &ref_cap);
            int o = 0;
            for (int i = 0; i < n; i++) {
                StRange *l = o ? &b[o - 1] : NULL;
                if (l && l->r == b[i].r && b[i].off <= l->off + l->size) {
                    if (b[i].off + b[i].size > l->off + l->size) l->size = b[i].off + b[i].size - l->off;
                } else b[o++] = b[i];
            }
            CHECK(jf.no == o);
            for (int i = 0; i < jf.no && i < o; i++)
                CHECK(jf.o[i].r == b[i].r && jf.o[i].off == b[i].off && jf.o[i].size == b[i].size);
            free(a), free(b);
        }
    }
    free(ref_buf), ref_buf = NULL, ref_cap = 0;
}

int main(void) {
    test_cpu_written_and_decommit();
    test_staged_split_own();
    test_st_sort_job_radix();
    CHECK(mutex_excl == 0 && mutex_shared == 0);
    free(pool_index.e), free(pool_index.maxhi);
    printf("staged-mirror-opt: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
