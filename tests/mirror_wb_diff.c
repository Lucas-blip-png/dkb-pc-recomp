// CPU unit tests for Issue #485 mirror coherency fixes in runtime/vk_memory.c and runtime/vk_exec_draw.c:
//   1. wb_diff_copy: 64-byte memcmp skip, 8-byte word skip, byte/sub-word diff preserving concurrent CPU writes,
//      particle slot + splash counter regressions, unaligned edges, and modified-page tracking in staged_copy_back.
//   2. Unsized (size == 0) SSBO bindings in ssbo_info / bind_range: VkDescriptorBufferInfo.range, write-back,
//      uniform-snapshot drop, and note_written all agree and cap at 8 MiB past b->addr (matching Eglantine).
//   3. Multi-part PreMap invariants (alignment, upload refresh, mid-submission cpu_read 0 -> 1 st_skipped_pre
//      fallback, WB_EARLY pre_cut_copy V0 -> V1 -> V2 telescoping, explicit held tags across two held parts and an
//      ordinary split between them, late/unprovable tag raw fallback, and DKB_MIRROR_DIFF=0 bypass) and st_w_merge_scan.
// tests/mirror_wb_diff_test.py pastes the production types and functions from runtime/vk_memory.c and
// runtime/vk_exec_draw.c into the markers below.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "nvn_api.h"
#include "gfx_spirv.h"
#include "msort.h"
#include "platform/platform.h"

static int checks, failures;
#define CHECK(cond) do {                                                                                                \
    checks++;                                                                                                           \
    if (!(cond)) {                                                                                                      \
        failures++;                                                                                                     \
        fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                                 \
    }                                                                                                                   \
} while (0)

unsigned rt_trace;
void rt_log(const char *fmt, ...) { (void)fmt; }
int rt_knob(const char *name, const char *def) { (void)name; return def && !strcmp(def, "1"); }   // st_sort_job: MEM_FAST
int rt_ab(void) { return 0; }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_mutex_trylock(PlatMutex *m) { (void)m; return 1; }
void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }
int plat_mutex_trylock_shared(PlatMutex *m) { (void)m; return 1; }
static uint64_t qpc(void) { return 0; }

/* MEMORY_TYPES */

static VkCtx fake_vk_ctx;
VkCtx *vk_ctx(void) { return &fake_vk_ctx; }

int vk_mirror_on = 1;
static int mirror_diff_cur = 1;
static uint32_t msub = 1;
static int staged_fault = 1;
static int fetch_test;
static PlatMutex st_lock = PLAT_MUTEX_INIT;
static PlatMutex own_lock = PLAT_MUTEX_INIT;
static StJob *st_jobs, *st_cur;
static MirrorAcc mrep;
static volatile int64_t own_fb_ranges, own_fb_bytes, own_protects, own_cb_opened, own_cb_kept, st_sort_ns;

static int mirror_lock_if_shared(void) { return 0; }
static void mirror_unlock(int locked) { (void)locked; }

static int mock_ops_ready = 1;
static int mirror_ops_init(VkCtx *c) { (void)c; return mock_ops_ready; }

typedef struct AllocPB {
    PreBuf pb;
    uint8_t *backing;
    uint64_t fence;
} AllocPB;
static AllocPB alloc_pbs[64];
static int nalloc_pbs;

static PreBuf *pre_buf_acquire(VkCtx *c, VkDeviceSize min_bytes, int host, uint64_t fence) {
    (void)c;
    CHECK(nalloc_pbs < 64);
    VkDeviceSize want = 4096;
    while (want < min_bytes) want *= 2;
    AllocPB *a = &alloc_pbs[nalloc_pbs++];
    memset(a, 0, sizeof *a);
    a->backing = calloc(1, (size_t)want);
    CHECK(a->backing != NULL);
    a->fence = fence;
    a->pb.refs = 1;
    a->pb.host = host ? 1 : 0;
    a->pb.buf = (VkBuffer)(uintptr_t)(0x5000 + nalloc_pbs);
    a->pb.map = a->backing;
    a->pb.bytes = want;
    a->pb.used = 0;
    return &a->pb;
}

static uint8_t *mock_staging_buf;
void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc) {
    free(mock_staging_buf);
    mock_staging_buf = calloc(1, (size_t)size);
    CHECK(mock_staging_buf != NULL);
    *buf = (VkBuffer)(uintptr_t)0x9000;
    *alloc = (VmaAllocation)(uintptr_t)0x9001;
    return mock_staging_buf;
}

static int mock_copy_cmds;
static VkBufferCopy mock_last_copies[32];
static int nmock_last_copies;
void vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions) {
    (void)cb; (void)src; (void)dst;
    mock_copy_cmds++;
    nmock_last_copies = 0;
    for (uint32_t i = 0; i < count && nmock_last_copies < 32; i++)
        mock_last_copies[nmock_last_copies++] = regions[i];
}

static int mock_guard_states[16];
static int mock_thaw_calls;
int mem_guard_state(uint64_t addr) {
    uint64_t pg = (addr >> 12) & 15;
    return mock_guard_states[pg];
}
int mem_guard_tryclaim(uint64_t addr) { (void)addr; return 0; }
void mem_guard_clear(uint64_t addr) { (void)addr; }
int mem_guard_own(uint64_t addr) { (void)addr; return 0; }
void mem_thaw_keep(uint64_t addr, uint64_t size) { (void)addr; (void)size; mock_thaw_calls++; }
void mem_thaw(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
VkResult vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size) {
    (void)allocator; (void)allocation; (void)offset; (void)size;
    return VK_SUCCESS;
}
void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) { (void)fence; (void)fn; (void)arg; }
static void own_cut(PoolRec *r, const struct KeptRange *gone, int ngone) { (void)r; (void)gone; (void)ngone; }
static void own_merge(PoolRec *r, const StRange *add, int n) { (void)r; (void)add; (void)n; }
static void st_sort_job(StJob *j);
static void st_sort_queue(StJob *j) { st_sort_job(j); }

/* MEMORY_FUNCTIONS */

static void reset_pre_state(void) {
    mirror_diff_cur = 1;
    msub = 1;
    wpre_map.n = spre_map.n = 0;
    wpre_map.cur = spre_map.cur = NULL;
    npre_pend = pre_end_done = 0;
    pre_sealed_next = 0;
    pre_walk_writes = pre_seen_walk_writes = 0;
    pre_worker_seq = pre_seen_worker_seq = 0;
    pre_has_deferred = pre_cache_valid = 0;
    wbl_scan = stw_scan = 0;
    nwbl = nstw = npend = 0;
    wb_tag = 0;
    for (int i = 0; i < nalloc_pbs; i++) free(alloc_pbs[i].backing);
    nalloc_pbs = 0;
    pre_nwraw = pre_nsraw = 0;
}

// Whether [lo, hi) of r lies in raw-only segments (no snapshot buffer), and none of its bytes in a snapshot segment.
static int pre_raw_only(const PreMap *m, const PoolRec *r, uint64_t lo, uint64_t hi) {
    int raw = 0;
    for (int i = 0; i < m->n; i++) {
        if (m->seg[i].r != r || m->seg[i].off >= hi || m->seg[i].off + m->seg[i].size <= lo) continue;
        if (m->seg[i].pb) return 0;
        raw = 1;
    }
    return raw && pre_map_covers(m, r, lo, hi);
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void test_wb_diff_copy_and_pages(void) {
    // 1. 64-byte fast skip: identical buffers return 0 and leave concurrent CPU writes untouched.
    uint8_t dst[4096], before[4096], after[4096];
    for (size_t i = 0; i < sizeof dst; i++) {
        before[i] = (uint8_t)(i * 3 + 1);
        after[i] = before[i];
        dst[i] = (uint8_t)(0xa5u ^ (i & 0xffu));   // concurrent CPU bytes in guest memory
    }
    CHECK(wb_diff_copy(dst, after, before, sizeof dst) == 0);
    for (size_t i = 0; i < sizeof dst; i++)
        CHECK(dst[i] == (uint8_t)(0xa5u ^ (i & 0xffu)));

    // 2. Issue #485 particle slot regression (0x1c247da00): GPU updates pos.xyz (bytes 0..11), leaves spawnTime
    //    (bytes 12..15) equal to before, while CPU concurrently wrote spawnTime = 430.0f (0x43d70000).
    memset(before, 0, 64);
    memset(after, 0, 64);
    memset(dst, 0, 64);
    const uint32_t gpu_xyz[3] = {0x00000000u, 0xbcc8c7e6u, 0xbb8d8d00u};
    const uint32_t cpu_spawn = 0x43d70000u;   // 430.0f
    memcpy(after, gpu_xyz, 12);
    memcpy(dst + 12, &cpu_spawn, 4);
    CHECK(wb_diff_copy(dst, after, before, 64) == 1);
    CHECK(memcmp(dst, gpu_xyz, 12) == 0);
    uint32_t got_spawn = 0;
    memcpy(&got_spawn, dst + 12, 4);
    CHECK(got_spawn == cpu_spawn);

    // 3. Issue #485 splash counter regression (0x24ce265c0 at +0x40 after 0x24ce26580): GPU writes +0x00, leaves
    //    +0x40 untouched at old value 1 while CPU zeroed +0x40 from 1 -> 0; counter stays 0, never re-armed to 1.
    memset(before, 0, 128);
    memset(after, 0, 128);
    memset(dst, 0, 128);
    uint32_t one = 1, active = 7;
    memcpy(before + 0x40, &one, 4);
    memcpy(after + 0x40, &one, 4);   // untouched in mirror (still 1)
    memcpy(after + 0x00, &active, 4);
    // dst + 0x40 is 0 (CPU consumed emission request)
    CHECK(wb_diff_copy(dst, after, before, 128) == 1);
    uint32_t got_active = 0, got_counter = 99;
    memcpy(&got_active, dst + 0x00, 4);
    memcpy(&got_counter, dst + 0x40, 4);
    CHECK(got_active == 7 && got_counter == 0);

    // 4. Sub-word / single-byte writes inside an 8-byte word preserve all 7 adjacent concurrent CPU bytes.
    for (int b = 0; b < 8; b++) {
        memset(before, 0x11, 64);
        memset(after, 0x11, 64);
        for (int j = 0; j < 64; j++) dst[j] = (uint8_t)(0x80 + j);
        after[16 + b] = 0x7f;
        CHECK(wb_diff_copy(dst, after, before, 64) == 1);
        for (int j = 0; j < 64; j++) {
            uint8_t want = (j == 16 + b) ? 0x7f : (uint8_t)(0x80 + j);
            CHECK(dst[j] == want);
        }
    }

    // 5. Exhaustive lengths 0..513 with unaligned starts against a byte-exact oracle.
    uint32_t rng = 0x48500485u;
    uint8_t ref[520];
    for (size_t len = 0; len <= 513; len++) {
        size_t pad = len & 7;
        for (size_t i = 0; i < len + pad; i++) {
            uint32_t r = xorshift32(&rng);
            before[i] = (uint8_t)r;
            after[i] = ((r >> 8) & 15) == 0 ? (uint8_t)(r >> 16) : before[i];
            dst[i] = (uint8_t)(r >> 24);
            ref[i] = dst[i];
        }
        int want_chg = 0;
        for (size_t i = 0; i < len; i++) {
            if (after[pad + i] != before[pad + i]) {
                ref[pad + i] = after[pad + i];
                want_chg = 1;
            }
        }
        int got_chg = wb_diff_copy(dst + pad, after + pad, before + pad, len);
        CHECK(got_chg == want_chg);
        CHECK(memcmp(dst, ref, len + pad) == 0);
    }

    // 6. staged_copy_back tracks cpu_written (cw) and thaws ONLY pages where at least 1 byte differed.
    uint8_t *guest = calloc(4, 4096), *rb = calloc(4, 4096), *snap = calloc(4, 4096);
    CHECK(guest && rb && snap);
    memset(guest, 0x5a, 4 * 4096);
    memset(rb, 0x22, 4 * 4096);
    memset(snap, 0x22, 4 * 4096);
    // Page 0: unchanged; Page 1: byte +0x100 changed; Page 2: unchanged; Page 3: byte +0xffc changed.
    rb[1 * 4096 + 0x100] = 0x99;
    rb[3 * 4096 + 0xffc] = 0x77;
    PreBuf pb = {.refs = 1, .host = 1, .map = snap, .bytes = 4 * 4096, .used = 4 * 4096};
    PoolRec pool = {.kind = POOL_IMPORT, .lo = (uintptr_t)guest, .size = 4 * 4096,
                    .import_lo = (uintptr_t)guest, .import_hi = (uintptr_t)guest + 4 * 4096, .sfeed = 1};
    StRange wr = {.r = &pool, .lo = pool.lo, .serial = 0, .off = 0, .size = 4 * 4096, .roff = 0, .poff = 1, .pb = &pb};
    StJob job = {.fence = 1, .w = &wr, .n = 1, .map = rb, .bytes = 4 * 4096};
    memset(mock_guard_states, 0, sizeof mock_guard_states);
    mock_thaw_calls = 0;
    struct KeptRange *cw = NULL;
    int ncw = 0, ccw = 0;
    staged_copy_back(&pool, &job, 0, 1, &cw, &ncw, &ccw);
    CHECK(mock_thaw_calls == 2);
    CHECK(ncw == 2);
    if (ncw == 2) {
        CHECK(cw[0].lo == pool.import_lo + 1 * 4096 && cw[0].hi == pool.import_lo + 2 * 4096);
        CHECK(cw[1].lo == pool.import_lo + 3 * 4096 && cw[1].hi == pool.import_lo + 4 * 4096);
    }
    // Untouched pages 0 & 2 and untouched bytes on pages 1 & 3 retain concurrent CPU pattern 0x5a.
    CHECK(guest[0 * 4096 + 0x10] == 0x5a && guest[2 * 4096 + 0x10] == 0x5a);
    CHECK(guest[1 * 4096 + 0x100] == 0x99 && guest[1 * 4096 + 0x101] == 0x5a);
    CHECK(guest[3 * 4096 + 0xffc] == 0x77 && guest[3 * 4096 + 0xffb] == 0x5a);
    free(cw);
    free(guest);
    free(rb);
    free(snap);
}

static void test_pre_map_and_telescoping(void) {
    reset_pre_state();
    PoolRec pool = {.kind = POOL_IMPORT, .lo = 0x100000, .size = 0x20000, .import_lo = 0x100000, .import_hi = 0x120000};

    // 1. Alignment invariant: (poff & 3) == (off & 3) across arbitrary unaligned offsets.
    const uint64_t offs[] = {1, 2, 3, 4, 7, 0x102, 0x3ff};
    for (size_t i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        uint64_t poff = 99;
        PreBuf *pb = pre_alloc(&fake_vk_ctx, &wpre_map, offs[i], 19, 1, 1, &poff);
        CHECK(pb != NULL && (poff & 3ull) == (offs[i] & 3ull));
    }

    // 2. Multi-part coverage + upload refresh into already-covered range.
    reset_pre_state();
    MRange p1 = {&pool, 0x1000, 0x2000, 0};
    pre_cover_ranges(&fake_vk_ctx, &wpre_map, &p1, 1, 1, 10, 0);
    CHECK(wpre_map.n == 1 && npre_pend == 1);
    uint64_t base_poff = wpre_map.seg[0].poff;
    PreBuf *base_pb = wpre_map.seg[0].pb;
    npre_pend = 0;   // Part 1 prologue recorded its initial snapshot

    // Part 2 writes overlapping [0x1800, 0x3800) and uploads new CPU bytes to [0x1400, 0x1c00).
    MRange up2 = {&pool, 0x1400, 0x800, 0};
    pre_refresh_batch(&wpre_map, &up2, 1);
    CHECK(npre_pend == 1);
    CHECK(pre_pend[0].off == 0x1400 && pre_pend[0].poff == base_poff + 0x400 && pre_pend[0].size == 0x800);
    MRange p2 = {&pool, 0x1800, 0x2000, 0};
    pre_cover_ranges(&fake_vk_ctx, &wpre_map, &p2, 1, 1, 10, 0);
    // Only the newly uncovered tail [0x3000, 0x3800) is allocated; [0x1000, 0x3000) keeps base_pb/base_poff.
    CHECK(pre_map_covers(&wpre_map, &pool, 0x1000, 0x3800));
    CHECK(wpre_map.seg[0].off == 0x1000 && wpre_map.seg[0].pb == base_pb && wpre_map.seg[0].poff == base_poff);

    // 3. Mid-submission staged_fetch (cpu_read[pg] flipping 0 -> 1 after Part 1 modified mbuf without a snapshot).
    reset_pre_state();
    staged_fault = 1;
    uint8_t cr[4] = {1, 0, 1, 0};
    PoolRec st_pool = {.kind = POOL_IMPORT, .lo = 0x200000, .size = 0x4000,
                       .import_lo = 0x200000, .import_hi = 0x204000, .sfeed = 1, .cpu_read = cr};
    StJob job = {.fence = 11, .sorted = 1};
    st_cur = &job;
    // Part 1 writes all 4 pages while cr = {1, 0, 1, 0}.
    wb_range_add(&stw, &nstw, &cstw, &st_pool, 0, 0x4000);
    pre_walk_writes++;
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) == 1);
    pre_prepare_part(UINT32_MAX, 11);
    CHECK(pre_map_covers(&spre_map, &st_pool, 0x0000, 0x1000));
    CHECK(!pre_map_covers(&spre_map, &st_pool, 0x1000, 0x2000));
    CHECK(pre_map_covers(&spre_map, &st_pool, 0x2000, 0x3000));
    CHECK(!pre_map_covers(&spre_map, &st_pool, 0x3000, 0x4000));
    npre_pend = 0;
    // Mid-submission fault flips cr[1] from 0 -> 1 and marks cr_flip_sub = msub before submission end!
    cr[1] = 1;
    st_pool.cr_flip_sub = msub;
    staged_split_own();
    pre_reserve_end(11);
    // Also exercise pre_refresh_staged on page 0 ([0x000, 0x400) cut + [0x400, 0x800) merge).
    npre_pend = 0;
    MRange st_cut_r = {&st_pool, 0x000, 0x400, 0};
    StMerge st_mrg_r = {&st_pool, spre_map.seg[0].pb, 0x400, 0, 0x400, 0};
    pre_refresh_staged(&st_cut_r, 1, &st_mrg_r, 1);
    CHECK(npre_pend == 1 && pre_pend[0].off == 0 && pre_pend[0].size == 0x800);
    mock_copy_cmds = 0;
    staged_wb_record((VkCommandBuffer)(uintptr_t)1);
    CHECK(mock_copy_cmds == 1 && ms_get(&st_pool) == MS_NONE);
    // Page 3 (cr == 0) went to job.o; pages 0, 1, 2 went to job.w, with page 1 forced to poff == 0 (raw memcpy).
    CHECK(job.no == 1 && job.o[0].off == 0x3000 && job.o[0].size == 0x1000);
    CHECK(job.n == 3);
    if (job.n == 3) {
        CHECK(job.w[0].off == 0x0000 && job.w[0].size == 0x1000 && job.w[0].poff != 0 && job.w[0].pb != NULL);
        CHECK(job.w[1].off == 0x1000 && job.w[1].size == 0x1000 && job.w[1].poff == 0 && job.w[1].pb == NULL);
        CHECK(job.w[2].off == 0x2000 && job.w[2].size == 0x1000 && job.w[2].poff != 0 && job.w[2].pb != NULL);
    }
    free(job.w);
    free(job.o);
    st_cur = NULL;

    // 4. DKB_WB_EARLY pre_cut_copy telescoping: V0 -> V1 -> V2 (with V2 == V0) + untouched word U0 with CPU write.
    reset_pre_state();
    wb_tag = 0;
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x1000, 0x1000);
    pre_walk_writes++;
    pre_prepare_part(0, 12);
    vk_mem_mirror_part_done(0);
    CHECK(wpre_map.n == 1);
    PreMap j1_pre = {0};
    MRange take1 = {&pool, 0x1000, 0x1000, 0};
    pre_cut_copy(&wpre_map, &take1, 1, &j1_pre);
    CHECK(j1_pre.n == 1 && wpre_map.n == 0);   // cut out of wpre_map so Part 2 takes a fresh snapshot!

    wb_tag = 1;
    nwbl = wbl_scan = 0;
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x1000, 0x1000);
    pre_walk_writes++;
    pre_prepare_part(1, 12);
    vk_mem_mirror_part_done(1);
    CHECK(wpre_map.n == 1);
    CHECK(wpre_map.seg[0].pb != j1_pre.seg[0].pb || wpre_map.seg[0].poff != j1_pre.seg[0].poff);

    // Verify telescoping values under both normal queue order (wb1 before P2) and delayed WB.t order (wb1 after P2).
    const uint32_t V0 = 0x11111111u, V1 = 0x22222222u, V2 = 0x11111111u;   // V2 == V0!
    const uint32_t U0 = 0xaaaaaaaau, U_CPU = 0xbbbbbbbbu;
    for (int delayed = 0; delayed <= 1; delayed++) {
        uint32_t snap1[2] = {V0, U0};
        uint32_t snap2[2] = {V1, U0};   // P1 ran before pro2, so pro2 always captures V1
        uint32_t guest_w[2] = {V0, U_CPU};
        uint32_t mbuf_at_wb1[2] = {delayed ? V2 : V1, U0};
        uint32_t mbuf_at_wb2[2] = {V2, U0};
        wb_diff_copy((uint8_t *)guest_w, (const uint8_t *)mbuf_at_wb1, (const uint8_t *)snap1, sizeof guest_w);
        wb_diff_copy((uint8_t *)guest_w, (const uint8_t *)mbuf_at_wb2, (const uint8_t *)snap2, sizeof guest_w);
        CHECK(guest_w[0] == V2 && guest_w[1] == U_CPU);
    }
    free(j1_pre.seg);

    // 5. Explicit held tags + ordinary split between two held parts + unprovable/late tag fallback:
    //    Held part 1 (tag 0) writes [0x1000, 0x2000), walk writes [0x3000, 0x4000) in tag 1 before an ordinary split,
    //    then Held part 2 (tag 2) writes [0x5000, 0x6000) while walk continues in tag 3 with [0x7000, 0x8000).
    //    Each prologue receives its explicit upto tag; no FIFO can steal a held part's tag.
    reset_pre_state();
    CHECK(vk_mem_mirror_pending() == 0 && vk_mem_mirror_pending_for(UINT32_MAX) == 0 && !pre_need_scan());
    // Held part 1 (tag 0)
    wb_tag = 0;
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x1000, 0x1000);
    pre_walk_writes++;
    uint32_t h1_tag = 0;
    CHECK(vk_mem_wb_tag(1) == 0);
    // Walk after H1 (tag 1, before ordinary split)
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x3000, 0x1000);
    pre_walk_writes++;
    // H1 finishes with its explicit tag (0): snapshots ONLY [0x1000, 0x2000), leaving tag 1 uncovered.
    CHECK(vk_mem_mirror_pending_for(h1_tag) == 1);
    pre_prepare_part(h1_tag, 13);
    vk_mem_mirror_part_done(h1_tag);
    CHECK(pre_map_covers(&wpre_map, &pool, 0x1000, 0x2000));
    CHECK(!pre_map_covers(&wpre_map, &pool, 0x3000, 0x4000));
    CHECK(pre_sealed_next == 1 && pre_has_deferred == 1);
    npre_pend = 0;
    // Ordinary split between H1 and H2 (upto == UINT32_MAX, wb_tag == 1): snapshots [0x3000, 0x4000) and seals tag 1.
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) == 1);
    pre_prepare_part(UINT32_MAX, 13);
    vk_mem_mirror_part_done(UINT32_MAX);
    CHECK(pre_map_covers(&wpre_map, &pool, 0x3000, 0x4000));
    CHECK(pre_sealed_next == 2 && !pre_need_scan());
    CHECK(vk_mem_wb_tag(2) == 1);
    npre_pend = 0;
    // Held part 2 (tag 2) + walk continuation (tag 3) + a stray/late write carrying already-submitted tag 0!
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x5000, 0x1000);
    pre_walk_writes++;
    uint32_t h2_tag = 2;
    CHECK(vk_mem_wb_tag(3) == 2);
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x7000, 0x1000);
    wbl[nwbl++] = (MRange){&pool, 0x9000, 0x1000, 0};   // tag 0 < pre_sealed_next (2): cannot prove pre-write!
    pre_walk_writes += 2;
    // H2 finishes with explicit tag 2: snapshots ONLY [0x5000, 0x6000); skips tag 3 (deferred) AND tag 0 (already submitted).
    CHECK(vk_mem_mirror_pending_for(h2_tag) == 1);
    pre_prepare_part(h2_tag, 13);
    vk_mem_mirror_part_done(h2_tag);
    CHECK(pre_map_covers(&wpre_map, &pool, 0x5000, 0x6000));
    CHECK(!pre_map_covers(&wpre_map, &pool, 0x7000, 0x8000));
    CHECK(pre_raw_only(&wpre_map, &pool, 0x9000, 0xa000));   // a raw-only segment: no snapshot
    CHECK(pre_sealed_next == 3);
    npre_pend = 0;
    // Submission end (pre_reserve_end): snapshots tag 3 [0x7000, 0x8000), STILL refuses already-submitted tag 0 [0x9000, 0xa000)
    // so [0x9000, 0xa000) falls back to a full raw write-back with no snapshot.
    pre_reserve_end(13);
    CHECK(pre_map_covers(&wpre_map, &pool, 0x7000, 0x8000));
    CHECK(pre_raw_only(&wpre_map, &pool, 0x9000, 0xa000));
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) > 0);
    vk_mem_mirror_part_done(UINT32_MAX);
    CHECK(pre_end_done == 0 && wpre_map.n == 0);

    // 5a. A skipped range (tag below pre_sealed_next) stays raw even where a later provable write overlaps it: the
    //     overlap [0x9800, 0xa000) must not get a snapshot (it would hold the earlier part's bytes already), and the rest
    //     of the later write, [0xa000, 0xa800), is snapshotted as usual.
    reset_pre_state();
    pre_sealed_next = 1;
    wbl[nwbl++] = (MRange){&pool, 0x9000, 0x1000, 0};   // tag 0: already submitted
    wbl[nwbl++] = (MRange){&pool, 0x9800, 0x1000, 1};   // tag 1: provable, overlaps the skipped one
    pre_walk_writes += 2;
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) == 1);
    pre_prepare_part(UINT32_MAX, 15);
    CHECK(pre_raw_only(&wpre_map, &pool, 0x9000, 0xa000));
    CHECK(pre_map_covers(&wpre_map, &pool, 0x9000, 0xa800) && !pre_raw_only(&wpre_map, &pool, 0xa000, 0xa800));
    CHECK(npre_pend == 1 && pre_pend[0].off == 0xa000 && pre_pend[0].size == 0x800);
    npre_pend = 0;
    // The raw segment alone: nothing to snapshot, but the part still adds it.
    reset_pre_state();
    pre_sealed_next = 1;
    wbl[nwbl++] = (MRange){&pool, 0xb000, 0x1000, 0};
    pre_walk_writes++;
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) == 1);
    pre_prepare_part(UINT32_MAX, 16);
    CHECK(pre_raw_only(&wpre_map, &pool, 0xb000, 0xc000) && npre_pend == 0);

    // 5a'. A late raw write (tag 0, already submitted) lands on an existing snapshot segment: the overlap turns raw-only
    //      (the later snapshot already holds its bytes), the rest of the segment keeps its snapshot.
    reset_pre_state();
    pre_sealed_next = 1;
    wbl[nwbl++] = (MRange){&pool, 0xd000, 0x2000, 1};   // tag 1: snapshotted
    pre_walk_writes++;
    pre_prepare_part(UINT32_MAX, 17);
    CHECK(pre_map_covers(&wpre_map, &pool, 0xd000, 0xf000) && !pre_raw_only(&wpre_map, &pool, 0xd000, 0xf000));
    npre_pend = 0;
    wbl[nwbl++] = (MRange){&pool, 0xd800, 0x1000, 0};   // tag 0: noted late
    pre_walk_writes++;
    pre_prepare_part(UINT32_MAX, 18);
    CHECK(pre_raw_only(&wpre_map, &pool, 0xd800, 0xe800));
    CHECK(!pre_raw_only(&wpre_map, &pool, 0xd000, 0xd800) && !pre_raw_only(&wpre_map, &pool, 0xe800, 0xf000));
    CHECK(pre_map_covers(&wpre_map, &pool, 0xd000, 0xf000));

    // 5a''. The staged twin: a tag-1 staged write is snapshotted, then a late tag-0 staged write (already submitted)
    //       lands on it; the overlap turns raw-only, the rest keeps its snapshot.
    reset_pre_state();
    staged_fault = 1;
    pre_sealed_next = 1;
    uint8_t cr_all[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    PoolRec spool = {.kind = POOL_IMPORT, .lo = 0x400000, .size = 0x8000, .import_lo = 0x400000, .import_hi = 0x408000,
                     .sfeed = 1, .cpu_read = cr_all};
    wb_range_add(&stw, &nstw, &cstw, &spool, 0x1000, 0x3000);
    stw[nstw - 1].tag = 1;
    pre_walk_writes++;
    pre_prepare_part(UINT32_MAX, 19);
    CHECK(pre_map_covers(&spre_map, &spool, 0x1000, 0x4000) && !pre_raw_only(&spre_map, &spool, 0x1000, 0x4000));
    npre_pend = 0;
    wb_range_add(&stw, &nstw, &cstw, &spool, 0x2000, 0x1000);
    stw[nstw - 1].tag = 0;
    pre_walk_writes++;
    pre_prepare_part(UINT32_MAX, 20);
    CHECK(pre_raw_only(&spre_map, &spool, 0x2000, 0x3000));
    CHECK(!pre_raw_only(&spre_map, &spool, 0x1000, 0x2000) && !pre_raw_only(&spre_map, &spool, 0x3000, 0x4000));
    npre_pend = 0;

    // 5b. Two held parts simultaneously in flight (H1 tag 0, H2 tag 1) + cheap DKB_MIRROR_DIFF=0 bypass.
    reset_pre_state();
    wb_tag = 0;
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x1000, 0x1000);
    pre_walk_writes++;
    vk_mem_wb_tag(1);
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x2000, 0x1000);
    pre_walk_writes++;
    vk_mem_wb_tag(2);
    CHECK(vk_mem_mirror_pending_for(0) == 1);
    pre_prepare_part(0, 14);
    vk_mem_mirror_part_done(0);
    CHECK(pre_map_covers(&wpre_map, &pool, 0x1000, 0x2000) && !pre_map_covers(&wpre_map, &pool, 0x2000, 0x3000));
    CHECK(vk_mem_mirror_pending_for(1) == 1);
    pre_prepare_part(1, 14);
    vk_mem_mirror_part_done(1);
    CHECK(pre_map_covers(&wpre_map, &pool, 0x2000, 0x3000) && !pre_need_scan());
    // With mirror_diff_cur == 0, vk_mem_mirror_pending_for only reflects npend.
    mirror_diff_cur = 0;
    wb_range_add(&wbl, &nwbl, &cwbl, &pool, 0x4000, 0x1000);
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) == 0);
    range_add(&pend, &npend, &cpend, &pool, 0, 64);
    CHECK(vk_mem_mirror_pending() == 1 && vk_mem_mirror_pending_for(UINT32_MAX) == 1);
    npend = 0;
    free(pend);
    pend = NULL, cpend = 0;

    // 6. When pre_end_done && !npre_pend (all ranges already snapshotted earlier) and npend == 0,
    //    vk_mem_mirror_pending_for returns 0 and vk_mem_mirror_part_done resets wpre_map, spre_map, and pre_end_done.
    reset_pre_state();
    MRange r_early = {&pool, 0x1000, 0x1000, 0};
    pre_cover_ranges(&fake_vk_ctx, &wpre_map, &r_early, 1, 1, 20, 0);
    npre_pend = 0;
    pre_end_done = 1;
    CHECK(wpre_map.n == 1 && wpre_map.cur != NULL);
    CHECK(vk_mem_mirror_pending_for(UINT32_MAX) == 0);
    vk_mem_mirror_part_done(UINT32_MAX);
    CHECK(pre_end_done == 0 && wpre_map.n == 0 && wpre_map.cur == NULL && spre_map.n == 0 && spre_map.cur == NULL);
}

static void test_st_w_merge_scan(void) {
    reset_pre_state();
    staged_fault = 1;
    mock_ops_ready = 1;
    struct KeptRange own_r = {0x0000, 0x1000};   // page 0 GPU-owned on r->own
    PoolRec pool = {.kind = POOL_IMPORT, .lo = 0x300000, .size = 0x8000,
                    .import_lo = 0x300000, .import_hi = 0x308000, .sfeed = 1, .own = &own_r, .nown = 1};
    PreBuf pb1 = {.refs = 1, .host = 1, .buf = (VkBuffer)(uintptr_t)0x6001, .bytes = 0x4000};
    StRange w1 = {.r = &pool, .lo = pool.lo, .off = 0x1000, .size = 0x1000, .roff = 0, .poff = 0x200 + 1, .pb = &pb1};
    StRange o1 = {.r = &pool, .lo = pool.lo, .off = 0x2000, .size = 0x1000};
    StJob j1 = {.fence = 5, .ended = 1, .sorted = 1, .w = &w1, .n = 1, .o = &o1, .no = 1};
    st_jobs = &j1;

    // Upload covers [0x0000, 0x4000):
    // - [0x0000, 0x1000): r->own -> blocked, NOT merged
    // - [0x1000, 0x2000): j1.w (not own) -> blocked by st_cut, QUEUED in mrg with wait_fence == 0!
    // - [0x2000, 0x3000): j1.o -> blocked, NOT merged
    // - [0x3000, 0x4000): unblocked -> copied by st_cut
    struct KeptRange *blk = NULL;
    MRange *cut = NULL;
    StMerge *mrg = NULL;
    int nblk = 0, cblk = 0, ncut = 0, ccut = 0, nmrg = 0, cmrg = 0;
    uint64_t total = 0, skipped = 0, wait_fence = 0;
    st_blocked(&pool, 0, 0x4000, 6, &blk, &nblk, &cblk);
    st_cut(&pool, 0, 0x4000, blk, nblk, &cut, &ncut, &ccut, &total, &skipped);
    CHECK(total == 0x1000 && skipped == 0x3000 && ncut == 1 && cut[0].off == 0x3000);
    st_w_merge_scan(&pool, 0, 0x4000, 6, 1, &mrg, &nmrg, &cmrg, &wait_fence);
    CHECK(wait_fence == 0 && nmrg == 1);
    if (nmrg == 1) {
        CHECK(mrg[0].r == &pool && mrg[0].pb == &pb1);
        CHECK(mrg[0].off == 0x1000 && mrg[0].poff == 0x200 && mrg[0].size == 0x1000);
    }

    // Two overlapping in-flight jobs on [0x1000, 0x2000) -> falls back to wait_fence = max(5, 7) = 7.
    StRange w2 = {.r = &pool, .lo = pool.lo, .off = 0x1800, .size = 0x800, .roff = 0, .poff = 0x400 + 1, .pb = &pb1};
    StJob j2 = {.fence = 7, .ended = 1, .sorted = 1, .w = &w2, .n = 1, .next = &j1};
    st_jobs = &j2;
    nmrg = 0, wait_fence = 0;
    st_w_merge_scan(&pool, 0x1000, 0x2000, 8, 1, &mrg, &nmrg, &cmrg, &wait_fence);
    CHECK(wait_fence == 7);

    // Single job with missing snapshot (poff == 0) -> falls back to wait_fence = 5.
    st_jobs = &j1;
    w1.poff = 0, w1.pb = NULL;
    nmrg = 0, wait_fence = 0;
    st_w_merge_scan(&pool, 0x1000, 0x2000, 6, 1, &mrg, &nmrg, &cmrg, &wait_fence);
    CHECK(wait_fence == 5);

    // Exact word-merge semantics of mirror_merge.comp: GPU-modified slot stays, untouched slot takes new CPU emission.
    uint32_t pre_words[4] = {10, 20, 0, 0};     // before J_old: slot 0 had {10,20}, slot 1 empty {0,0}
    uint32_t mbuf_words[4] = {15, 25, 0, 0};    // J_old updated slot 0 -> {15,25}, left slot 1 at {0,0}
    uint32_t cpu_words[4] = {10, 20, 99, 430};  // Frame N CPU left slot 0 stale, emitted slot 1 {99,430}
    for (int i = 0; i < 4; i++)
        if (mbuf_words[i] == pre_words[i]) mbuf_words[i] = cpu_words[i];
    CHECK(mbuf_words[0] == 15 && mbuf_words[1] == 25 && mbuf_words[2] == 99 && mbuf_words[3] == 430);

    st_jobs = NULL;
    free(blk);
    free(cut);
    free(mrg);
}

// ---- Part A: size == 0 SSBO bind_range and ssbo_info checks (extracted from runtime/vk_exec_draw.c)

typedef struct { int cls, use; uint64_t bytes; } RsCount;
typedef struct DrawCtx {
    int helper, slice;
    struct {
        int volatile_, ncnt, cnt_over;
        RsCount cnt[16];
    } RS;
} DrawCtx;
#define DC_OF(ex) ((DrawCtx *)(ex)->draw)
#define IS_HELPER(dc) ((dc)->helper)
#define IN_SLICE(dc) ((dc)->slice)

#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_GS 2
#define NVN_STAGE_CS 5

VkCaps vk_caps;
int vk_mirror_active = 1;
static struct { uint64_t ubo_max, sbo_max, ubo_align, sbo_align; } LIM = {65536, 0x80000000ull, 16, 16};
static void limits_init(void) {}
enum { W_SSBO_ALIGN, W_SSBO_SLOT };
#define warn_once(id, ...) ((void)(id))

static uint64_t mock_pool_base = 0x200000000ull;
static uint64_t mock_pool_size = 450ull << 20;   // 450 MiB pool (as in Bananza's 0x234c80000 arena)
static VkBuffer mock_mirror_buf = (VkBuffer)(uintptr_t)0xdeadbeefull;

static uint64_t got_wrote_off, got_wrote_n;
static uint64_t got_kept_off, got_kept_n;
static uint64_t got_usnap_addr, got_usnap_n;
static uint64_t got_note_addr, got_note_n;
static uint64_t got_sync_addr, got_sync_n;
static int got_wrote_calls, got_kept_calls, got_usnap_calls, got_note_calls, got_sync_calls, mock_gs_wb;

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) {
    if (addr < mock_pool_base || addr >= mock_pool_base + mock_pool_size) return 0;
    uint64_t off = addr - mock_pool_base;
    if (size > mock_pool_size - off) return 0;
    *out = (VkMemSpan){
        .buffer = mock_mirror_buf,
        .offset = off,
        .avail = mock_pool_size - off,
        .pool = (void *)1,
        .cls = VK_MCLS_HOST_CACHED,
    };
    return 1;
}
void vk_mem_use(VkMemSpan *sp) { (void)sp; }
void vk_mem_bound_note(void *pool) { (void)pool; }
void vk_mem_count(int cls, int use, uint64_t bytes) { (void)cls; (void)use; (void)bytes; }
void vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size) {
    got_wrote_calls++;
    got_wrote_off = sp->offset;
    got_wrote_n = size;
}
int vk_mem_gpu_kept(const VkMemSpan *sp, uint64_t size) {
    got_kept_calls++;
    got_kept_off = sp->offset;
    got_kept_n = size;
    return 1;
}
void vk_usnap_drop(uint64_t addr, uint64_t size) {
    got_usnap_calls++;
    got_usnap_addr = addr;
    got_usnap_n = size;
}
int vk_usnap_covers(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
void vk_exec_par_order_hazard(void) {}
enum { VK_SPEC_WRITE = 1 };
static int vk_spec_fail(int why) { (void)why; return 1; }
static void count_use(DrawCtx *dc, int cls, int use, uint64_t n) { (void)dc; (void)cls; (void)use; (void)n; }
static void *scratch(VkExec *ex, size_t n, VkBuffer *b, VkDeviceSize *o) {
    (void)ex; (void)n; *b = VK_NULL_HANDLE; *o = 0; return NULL;
}
static int gs_storage_writeback(void) { return mock_gs_wb; }
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) { (void)st; *ubo = 0; return (int)r->handle; }
static p_bind_buf guarded_storage_binding(VkExec *ex, int st, const GfxShaderRes *r) {
    (void)ex; (void)st; (void)r; return (p_bind_buf){0};
}
static int ubo_ptr_res(const GfxShaderRes *r) { return r->cbuf_slot >= 0; }
static uint64_t ubo_ptr(VkExec *ex, int st, const GfxShaderRes *r) {
    (void)ex; (void)st; (void)r; return mock_pool_base + 0x181b6680ull;
}
static void note_written(VkExec *ex, uint64_t addr, uint64_t size, VkBuffer buf) {
    (void)ex; (void)buf;
    got_note_calls++;
    got_note_addr = addr;
    got_note_n = size;
}
int vk_sync_noting(void) { return 1; }
static int find_stage_origin(DrawCtx *dc, const prog_t *p, int st, const uint8_t **ctl, const uint8_t **code,
                             uint32_t *len, uint64_t *o) {
    (void)dc; (void)p; (void)st;
    *ctl = NULL; *code = NULL; *len = 0; *o = 0x1234;
    return 1;
}
void vk_sync_note_write(uint64_t addr, uint64_t size, uint64_t origin) {
    (void)origin;
    got_sync_calls++;
    got_sync_addr = addr;
    got_sync_n = size;
}
static VkDescriptorBufferInfo zero_info(uint64_t max) {
    (void)max;
    return (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)1, 0, 65536};
}

/* DRAW_FUNCTIONS */

static void reset_draw_spies(void) {
    got_wrote_calls = got_kept_calls = got_usnap_calls = got_note_calls = got_sync_calls = 0;
    got_wrote_off = got_wrote_n = got_kept_off = got_kept_n = 0;
    got_usnap_addr = got_usnap_n = got_note_addr = got_note_n = got_sync_addr = got_sync_n = 0;
}

static void test_size_zero_ssbo_bounds(void) {
    DrawCtx dc = {0};
    prog_t dummy_prog = {0};
    VkExec ex = {.draw = &dc, .prog = &dummy_prog};
    const uint64_t EightMiB = 8ull << 20;

    // 1. Unsized SSBO (size == 0) in 450 MiB pool:
    //    VkDescriptorBufferInfo.range, vk_mem_gpu_wrote, vk_usnap_drop, note_written, and vk_sync_note_write all agree
    //    and cap to 8 MiB (matching Eglantine BufferCache::StorageBufferBinding).
    reset_draw_spies();
    uint64_t addr = mock_pool_base + (24ull << 20);   // 426 MiB remaining
    ex.ssbo[NVN_STAGE_CS][14] = (p_bind_buf){.addr = addr, .size = 0};
    GfxShaderRes res = {.handle = 14, .cbuf_slot = -1};
    VkDescriptorBufferInfo bi = ssbo_info(&ex, NVN_STAGE_CS, &res, 1);
    CHECK(bi.buffer == mock_mirror_buf);
    CHECK(bi.range == EightMiB);
    CHECK(got_wrote_calls == 1 && got_wrote_off == (24ull << 20) && got_wrote_n == EightMiB);
    CHECK(got_usnap_calls == 1 && got_usnap_addr == addr && got_usnap_n == EightMiB);
    CHECK(got_note_calls == 1 && got_note_addr == addr && got_note_n == (EightMiB | VK_WB_LAZY));
    CHECK(got_sync_calls == 1 && got_sync_addr == addr && got_sync_n == EightMiB);

    // 1b. Unsized SSBO with alignment lead (sbuf_align = 16, addr + 4): caps to 8 MiB + lead from aligned base.
    reset_draw_spies();
    vk_caps.sbuf_align = 16;
    ex.ssbo[NVN_STAGE_CS][14] = (p_bind_buf){.addr = addr + 4, .size = 0};
    bi = ssbo_info(&ex, NVN_STAGE_CS, &res, 1);
    CHECK(bi.range == EightMiB + 4);
    CHECK(got_wrote_calls == 1 && got_wrote_off == (24ull << 20) && got_wrote_n == EightMiB + 4);
    vk_caps.sbuf_align = 0;

    // 2. Geometry stage kept storage (written -> how == 2) with size == 0 also caps bi.range and vk_mem_gpu_kept to 8 MiB.
    reset_draw_spies();
    mock_gs_wb = 0;
    ex.ssbo[NVN_STAGE_GS][3] = (p_bind_buf){.addr = addr, .size = 0};
    GfxShaderRes gs_res = {.handle = 3, .cbuf_slot = -1};
    bi = ssbo_info(&ex, NVN_STAGE_GS, &gs_res, 1);
    CHECK(bi.range == EightMiB);
    CHECK(got_kept_calls == 1 && got_kept_off == (24ull << 20) && got_kept_n == EightMiB && got_wrote_calls == 0);

    // 3. Unsized SSBO near the end of the pool (256 KiB left < 8 MiB): both range and wb_n clamp to 256 KiB.
    reset_draw_spies();
    uint64_t tail_addr = mock_pool_base + mock_pool_size - (256ull << 10);
    ex.ssbo[NVN_STAGE_CS][15] = (p_bind_buf){.addr = tail_addr, .size = 0};
    GfxShaderRes tail_res = {.handle = 15, .cbuf_slot = -1};
    bi = ssbo_info(&ex, NVN_STAGE_CS, &tail_res, 1);
    CHECK(bi.range == (256ull << 10));
    CHECK(got_wrote_calls == 1 && got_wrote_n == (256ull << 10));
    CHECK(got_note_calls == 1 && got_note_n == ((256ull << 10) | VK_WB_LAZY));

    // 4. Explicitly sized SSBO > 8 MiB (12 MiB) keeps its full explicit 12 MiB write-back extent!
    reset_draw_spies();
    ex.ssbo[NVN_STAGE_CS][4] = (p_bind_buf){.addr = addr, .size = 12ull << 20};
    GfxShaderRes big_res = {.handle = 4, .cbuf_slot = -1};
    bi = ssbo_info(&ex, NVN_STAGE_CS, &big_res, 1);
    CHECK(bi.range == (12ull << 20));
    CHECK(got_wrote_calls == 1 && got_wrote_n == (12ull << 20));
    CHECK(got_usnap_calls == 1 && got_usnap_n == (12ull << 20));
    CHECK(got_note_calls == 1 && got_note_n == ((12ull << 20) | VK_WB_LAZY));
}

int main(void) {
    test_wb_diff_copy_and_pages();
    test_pre_map_and_telescoping();
    test_st_w_merge_scan();
    test_size_zero_ssbo_bounds();
    reset_pre_state();
    free(mock_staging_buf);
    printf("mirror_wb_diff: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
