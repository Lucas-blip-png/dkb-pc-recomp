// NVN front end, command-buffer slice: command buffers, their memory, recording
// as op records (nvn_obj.h), recording copies behind command handles, and the submit-time resolution of deferred binds
// (nvn_submit_prepare, called by nvnQueueSubmitCommands in nvn_api_core.c). Ported from MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile),
// lines 647-1062, 1261-1286, 1486-1760 and 1975-1990, and the *Deferred and memory-callback entry points of
// totk_nvn_ext.c in the same directory. Metal, mach, the translation thread, Apple diagnostics, the per-frame
// statistics and the long-session sweeps (copy marks in control memory, texture pin sweep) dropped.
//
// Records are written into a growable host buffer per command buffer, never into the game's command memory: games hand
// the same command memory to several command buffers recording at once (budgeted for real NVN encodings), and our
// records are larger. The game's command and control memory is accounted only. EndRecording copies the finished
// recording into host memory; the handle is that copy (a flat OP_BEGIN ... OP_END sequence) and lives until the game
// adds the same control memory again. CallCommands and CopyCommands inline the called recording, or with DKB_CALL_REF
// record one OP_CALL naming its copy (inline_recording); copies hold no OP_JUMP.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_mode_trace.h"
#include "platform/platform.h"

#define CMD_MAGIC MAGIC('C', 'M', 'D', 'B')
#define COPY_MAGIC 0x59504f43444d43ull     // "CMDCOPY": prefix of a live recording copy
#define REC_MAGIC 0x4345524d444d43ull      // "CMDMREC": prefix of a live recording buffer
#define OP_RESERVE 24                       // room always kept at the end of the recording buffer
#define MIN_COMMAND_SIZE 512                // NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE (nvn_api_core.c devinfo)
#define MIN_CONTROL_SIZE 256                // NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE
#define CTL_PER_RECORDING 64                // control memory one recording is reported to use
#define COPY_PAGE_SHIFT 16                  // recording copy index granularity (release_copies)

static cmd_t *cmd_of(const Cpu *c) { return arg_p(c, 0); }

// ---------- memory callbacks
// NVN calls the game's memory callback when a recording needs command or control memory and none is left. Records
// live in host memory here, so the callback is called only where the hardware certainly would: a recording starts
// without any command memory, or ends without any control memory. It runs on the calling guest thread through
// rt_call; whatever memory it adds is accounted. Set{Command,Control}MemoryCallbackEnabled(false) suppress it.
enum { CB_NO_COMMAND = 1, CB_NO_CONTROL = 2 };
static PlatMutex cb_lock = PLAT_MUTEX_INIT;
static nvn_hmap cb_off = {.klen = 8};       // command buffer -> disabled callback kinds | 0x100

static unsigned cb_disabled(const cmd_t *o) {
    plat_mutex_lock_shared(&cb_lock);
    uintptr_t v = (uintptr_t)nvn_hm_get(&cb_off, &o);
    plat_mutex_unlock_shared(&cb_lock);
    return (unsigned)v & 0xff;
}

static void cb_set_disabled(const cmd_t *o, unsigned kind, int disabled) {
    plat_mutex_lock(&cb_lock);
    unsigned v = (unsigned)(uintptr_t)nvn_hm_get(&cb_off, &o) & 0xff;
    v = disabled ? v | kind : v & ~kind;
    if (v) nvn_hm_put(&cb_off, &o, (void *)(uintptr_t)(v | 0x100));
    else nvn_hm_del(&cb_off, &o);
    plat_mutex_unlock(&cb_lock);
}

static void memory_event(Cpu *c, cmd_t *o, int event, uint64_t min) {
    if (!o->cb || (cb_disabled(o) & (event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? CB_NO_COMMAND
                                                                                                     : CB_NO_CONTROL)))
        return;
    NVN_LOG("command buffer %p: memory callback %p (%s memory, %llu bytes)", (void *)o, (void *)o->cb,
            event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? "command" : "control",
            (unsigned long long)min);
    rt_call(c, (uint64_t)(uintptr_t)o->cb, 4, (uint64_t)(uintptr_t)o, (uint64_t)event, min,
            (uint64_t)(uintptr_t)o->cb_data);
}

// ---------- recording buffer
#define SEG_TOK(o) ((uint32_t)(((uintptr_t)(o) >> 3) & 0x7fffffffu))
#define SEG_OWNED(o) ((o)->seg && (o)->seg_tok == SEG_TOK(o))
// DKB_CMD_REC_FAST (default 1, "ab" alternates on DKB_AB): batched CallCommands/CopyCommands locking and ref-counter
// updates, thread-local generation-tagged UBO dedup + fast-path take_recording, off-lock copy freeing in
// release_copies, and offset-indexed deferred bind resolution at submit.
static int cmd_rec_fast_mode = -1;
static inline int cmd_rec_fast(void) {
    int m = __atomic_load_n(&cmd_rec_fast_mode, __ATOMIC_RELAXED);
    if (__builtin_expect(m < 0, 0)) {
        m = rt_knob("CMD_REC_FAST", "1");
        __atomic_store_n(&cmd_rec_fast_mode, m, __ATOMIC_RELAXED);
    }
    return RT_KNOB_ON(m);
}

// Offsets of deferred OP_PUSH_CONST / OP_BIND_{TEX,IMG,SAMP} records inside a recording copy, gathered once during
// take_recording so submit preparation resolves them directly instead of rescanning the whole copy.
typedef struct { uint32_t n; uint32_t offs[]; } def_off_list;

// The prefix of a copy and of a recording buffer (see "recording copies" below).
// ubo (DKB_USNAP_LIST): a copy's merged uniform ranges (nvn_handle_ubo_list), NULL without; set by take_recording,
// NULL in a recording buffer. Lists of 1 or 2 ranges live inline in ubo_inl to avoid heap churn on small leaves.
typedef union {
    nvn_range_list hdr;
    struct { uint64_t n; nvn_range r[2]; } inl;
} ubo_inline_list;
_Static_assert(offsetof(nvn_range_list, n) == 0 && offsetof(nvn_range_list, r) == offsetof(ubo_inline_list, inl.r),
               "inline uniform list matches nvn_range_list layout");
typedef struct copy_prefix copy_prefix;
typedef struct rec_track rec_track;
struct copy_prefix {
    uint64_t magic, bytes, made_at, used_at, deferred, block;
    uint32_t refs;
    uint16_t flags, depth;
    nvn_range_list *ubo;
    def_off_list *def_list;
    uint32_t ncallees, pad_;
    union {
        rec_track *track;             // REC_MAGIC: incremental per-recording metadata and callee/UBO tracker
        copy_prefix **callees;        // COPY_MAGIC with COPY_CALLEES and ncallees > 4: heap array
        copy_prefix *callee_inl[4];   // COPY_MAGIC with COPY_CALLEES and ncallees <= 4: inline storage
    } u;
    ubo_inline_list ubo_inl;
};
_Static_assert(sizeof(copy_prefix) % 8 == 0, "records stay 8-byte aligned");

// used_at only moves forward: a thread that read nsubmits earlier must not lower the value of a later submission (a
// zombie copy is freed once used_at is replayed).
static inline void used_at_max(copy_prefix *pf, uint64_t v) {
    uint64_t cur = __atomic_load_n(&pf->used_at, __ATOMIC_RELAXED);
    while (cur < v && !__atomic_compare_exchange_n(&pf->used_at, &cur, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
_Static_assert(OP_COUNT_ <= 64, "recording scan bitmask fits in uint64_t");
#define COPY_CALLS 1u     // copy_prefix.flags (see "recording copies" below)
#define COPY_WAITS 2u
#define COPY_CALLEES 4u   // copy's OP_CALL targets are stored in ncallees / u.callee_inl / u.callees
static inline copy_prefix *const *copy_callees(const copy_prefix *pf) {
    return pf->ncallees <= 4 ? pf->u.callee_inl : pf->u.callees;
}
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) {
    op_hdr h = {op, size};
    memcpy(p, &h, sizeof h);
}

// DKB_CMD_TRACK_FAST (default 1): track uniform bindings, OP_CALL callees, waits, call depth and deferred records
// incrementally at record time so EndRecording (take_recording) and copy_free / drop_recording_calls avoid walking
// every op record in the finished buffer. Read directly from the environment so standalone recorder tests that only
// stub cfg_bool("DKB_CALL_REF") keep working.
static int cmd_track_fast(void) {
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *e = getenv("DKB_CMD_TRACK_FAST");
        v = !(e && e[0] == '0' && e[1] == '\0');
        __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    }
    return v;
}

// The recording buffer is a copy block (copy_block below) with the copy's prefix in front, so EndRecording can hand
// it over as the copy (take_recording). rec_buffer: one with room for at least cap bytes after the prefix, and the
// room in *room.
static uint8_t *rec_buffer(size_t cap, size_t *room);
static void rec_buffer_free(uint8_t *seg);
static int rec_owned(const cmd_t *o);
static void drop_recording_calls(cmd_t *o);

// Diagnostic for a seg that is no buffer of ours: a command buffer object the game overwrote or copied shows a wrong
// magic or more used than its old size.
static void seg_check(const cmd_t *o, size_t need) {
    if (!o->seg) return;
    const copy_prefix *pf = (const copy_prefix *)o->seg - 1;
    size_t size = (size_t)(o->end - o->seg), used = (size_t)(o->cur - o->seg);
    if (o->cur < o->seg || o->cur > o->end || size + sizeof *pf != (size_t)pf->block)
        rt_fatal("nvn: corrupt recording buffer (command buffer %p, magic %#x, buffer %p of %zu bytes, %zu used, %zu "
                 "asked, block %llu)", (const void *)o, o->magic, (const void *)o->seg, size, used, need,
                 (unsigned long long)pf->block);
}

static void cmd_reserve(cmd_t *o, size_t need) {
    if (o->seg && SEG_OWNED(o) && o->cur + need + OP_RESERVE <= o->end) return;
    int twin = o->seg && !SEG_OWNED(o);
    if (twin) {   // a copied cmd_t: its seg may be a handed-over copy or already freed, so it is not read
        rt_log("nvn: command buffer %p records on a buffer it does not own; starting a new one", (void *)o);
        drop_recording_calls(o);   // not its own to read: the lost records' counts stay taken
        o->seg = o->cur = o->end = NULL;
    } else {
        seg_check(o, need);
    }
    size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 48 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    size_t room;
    rec_track *tr = NULL;
    if (o->seg) {
        copy_prefix *old_pf = (copy_prefix *)o->seg - 1;
        tr = old_pf->u.track;
        old_pf->u.track = NULL;
    }
    uint8_t *b = rec_buffer(cap, &room);
    ((copy_prefix *)b - 1)->u.track = tr;
    if (used) memcpy(b, o->seg, used);
    rec_buffer_free(o->seg);
    o->seg = b;
    o->seg_tok = SEG_TOK(o);
    o->cur = b + used;
    o->end = b + room;
    if (twin && o->recording) {   // the lost records are gone; keep what follows a valid recording
        put_hdr(o->cur, OP_BEGIN, 8);
        o->cur += 8;
    }
}

// Space for one record (header included); the header's op is filled by the caller.
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_reserve(o, size);
    uint8_t *p = o->cur;
    o->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}

// The game sums nvnCommandBufferGetCommandMemoryUsed over recordings to size resource heaps, so usage must be what the
// real NVN push-buffer encoding takes, not our record size. Known encodings (Maxwell methods, from a real recording):
// DrawElements 12 words, DrawElementsBaseVertex 11, BindVertexAttribState 1+16, BindVertexStreamState 16+n,
// BindVertexBuffer 6 words each.
static void hw_fix(cmd_t *o, size_t bytes) {
    o->hw_used += bytes;
    o->hw_used -= o->last_rec;
    o->last_rec = bytes;
}

// A zeroed payload of one record.
static void *rec(cmd_t *o, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(o, OP, T) ((T *)rec((o), (OP), sizeof(T)))
// Array-carrying binds take the prefix fields plus n elements; the replay reads only the first n.
#define RECN(o, OP, T, ARR, N) ((T *)rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))

static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) {
    *has = v != NULL;
    if (v) *dst = *(const view_t *)v;
}

// ---------- texture pins
// Recordings refer to textures by pointer, but the game may finalize or reuse an NVNtexture after recording and before
// the submit (NVN itself captured only GPU state). Records therefore point at an immutable host copy per texture
// initialization (tex_t.serial). The copies are kept for the whole run (MacNativeGames sweeps them for long sessions).
static PlatMutex pin_lock = PLAT_MUTEX_INIT;
static nvn_hmap pins = {.klen = 8};         // serial -> tex_t copy

static const tex_t *tex_pin(const NVNtexture *nt) {
    const tex_t *t = (const tex_t *)nt;
    if (!t) return NULL;
    uint64_t serial = t->serial;
    if (!serial) rt_fatal("nvn: a recording references the uninitialized texture object %p", (const void *)t);
    plat_mutex_lock_shared(&pin_lock);
    const tex_t *r = nvn_hm_get(&pins, &serial);
    plat_mutex_unlock_shared(&pin_lock);
    if (r) return r;
    plat_mutex_lock(&pin_lock);
    r = nvn_hm_get(&pins, &serial);
    if (!r) {
        tex_t *copy = malloc(sizeof *copy);
        if (!copy) rt_fatal("nvn: texture pin copy");
        *copy = *t;
        nvn_hm_put(&pins, &serial, copy);
        r = copy;
    }
    plat_mutex_unlock(&pin_lock);
    return r;
}

// ---------- recording copies
// A handle points just past a copy_prefix. Copies are indexed by the 64 KiB block of their control-memory position, so
// adding control memory again releases the copies recorded there without walking every live copy. 4 KiB pages cost
// about 500 lookups per AddControlMemory under the exclusive lock (1.4.3 field: 53 calls a frame, 2 MiB ranges, 3.7
// copies per page), and the recording threads waited on it.
// deferred: 0 unknown, 1 none, 2 some. block: the size of the block the prefix starts (copy_block). refs
// (DKB_CALL_REF): the OP_CALL records in other copies that name this one; flags bit 0 (COPY_CALLS): this copy holds
// OP_CALL records; bit 1 (COPY_WAITS): it, or a copy it calls, holds an OP_WAIT_SYNC record; depth: the longest chain of
// OP_CALL records from this copy down (0 without any). A copy never changes after take_recording, so the submit scans
// (wait_target, resolve_deferred) skip a copy whose prefix says it holds nothing for them.

// The blocks of copies and recording buffers of 64 KiB and more (the prefix included) come in power-of-two sizes and
// go back to a cache when released, up to BLK_CACHE bytes, instead of to the heap. The CRT hands blocks of about
// 512 KiB and more to VirtualAlloc and VirtualFree, so every copy wrote into new pages and took a demand-zero fault on
// each: in the Hilltop Layer of Surf Hills (3.2.0), where the game records about 28,500 draws a frame, the copies came
// to 185 KiB on average and the game took 470,000 page faults a second.
#define BLK_MIN_SHIFT 16
#define BLK_CLASSES 12               // 64 KiB to 128 MiB; larger copies use the heap
#define BLK_CACHE (256ull << 20)     // bytes kept in the cache at most
static PlatMutex blk_lock = PLAT_MUTEX_INIT;
static void **blk_free[BLK_CLASSES];
static uint32_t blk_n[BLK_CLASSES], blk_cap[BLK_CLASSES];
static uint64_t blk_cached;

static int blk_class(size_t total) {   // -1: from the heap
    if (total < (1u << BLK_MIN_SHIFT)) return -1;
    int c = 0;
    while (((size_t)1 << (BLK_MIN_SHIFT + c)) < total) c++;
    return c < BLK_CLASSES ? c : -1;
}

// A block of at least total bytes; its prefix's block field holds its size, and the other fields are left to the caller.
static copy_prefix *copy_block(size_t total) {
    int c = blk_class(total);
    void *p = NULL;
    if (c >= 0) {
        total = (size_t)1 << (BLK_MIN_SHIFT + c);
        plat_mutex_lock(&blk_lock);
        if (blk_n[c]) p = blk_free[c][--blk_n[c]], blk_cached -= total;
        plat_mutex_unlock(&blk_lock);
        if (!p) p = malloc(total);
    } else {
        p = malloc(total);
    }
    if (!p) rt_fatal("nvn: command copy allocation (%zu bytes)", total);
    ((copy_prefix *)p)->block = total;
    return p;
}

static void copy_block_free(copy_prefix *pf) {
    if (!pf) return;
    size_t size = (size_t)pf->block;
    int c = blk_class(size);
    if (c >= 0 && size == (size_t)1 << (BLK_MIN_SHIFT + c)) {
        plat_mutex_lock(&blk_lock);
        if (blk_cached + size <= BLK_CACHE) {
            if (blk_n[c] == blk_cap[c]) {
                uint32_t cap = blk_cap[c] ? blk_cap[c] * 2 : 16;
                void **v = realloc(blk_free[c], sizeof *v * cap);
                if (!v) rt_fatal("nvn: out of memory (command copy cache)");
                blk_free[c] = v, blk_cap[c] = cap;
            }
            blk_free[c][blk_n[c]++] = pf, blk_cached += size;
            pf = NULL;
        }
        plat_mutex_unlock(&blk_lock);
    }
    free(pf);
}

// Returns a batch of released blocks to the cache under one blk_lock hold, freeing any uncached ones outside the lock.
static void copy_block_free_many(copy_prefix **pfs, size_t n) {
    if (!n) return;
    int any_cached = 0;
    for (size_t i = 0; i < n; i++) {
        copy_prefix *pf = pfs[i];
        if (!pf) continue;
        size_t size = (size_t)pf->block;
        int c = blk_class(size);
        if (c >= 0 && size == (size_t)1 << (BLK_MIN_SHIFT + c)) any_cached = 1;
        else free(pf), pfs[i] = NULL;
    }
    if (any_cached) {
        plat_mutex_lock(&blk_lock);
        for (size_t i = 0; i < n; i++) {
            copy_prefix *pf = pfs[i];
            if (!pf) continue;
            size_t size = (size_t)pf->block;
            int c = blk_class(size);
            if (blk_cached + size <= BLK_CACHE) {
                if (blk_n[c] == blk_cap[c]) {
                    uint32_t cap = blk_cap[c] ? blk_cap[c] * 2 : 16;
                    void **v = realloc(blk_free[c], sizeof *v * cap);
                    if (!v) rt_fatal("nvn: out of memory (command copy cache)");
                    blk_free[c] = v, blk_cap[c] = cap;
                }
                blk_free[c][blk_n[c]++] = pf, blk_cached += size;
                pfs[i] = NULL;
            }
        }
        plat_mutex_unlock(&blk_lock);
        for (size_t i = 0; i < n; i++) free(pfs[i]);
    }
}

// The game may copy a cmd_t (see nvnCommandBufferInitialize), so two objects can hold one seg. Only the object named in
// the cmd_t's seg_tok (its own address) writes into it or frees it; another one takes a block of its own (or copies and leaves the
// shared one alone). Safe rather than fatal: a twin that records on is a game quirk we can serve, and the owner's
// buffer, or the copy handed over from it, stays intact. Once handed over, the buffer belongs to the copy list: the command buffer takes a new seg.
static void rec_track_free(rec_track *tr);

static uint8_t *rec_buffer(size_t cap, size_t *room) {
    copy_prefix *b = copy_block(sizeof *b + cap);
    b->magic = REC_MAGIC;
    b->ubo = NULL;
    b->def_list = NULL;
    b->ncallees = 0;
    b->u.track = NULL;
    *room = (size_t)b->block - sizeof *b;
    return (uint8_t *)(b + 1);
}

// From the object alone: a twin's seg may be freed, so the prefix is never read to decide.
static int rec_owned(const cmd_t *o) { return SEG_OWNED(o); }

static void rec_buffer_free(uint8_t *seg) {
    if (!seg) return;
    copy_prefix *pf = (copy_prefix *)seg - 1;
    rec_track *tr = pf->u.track;
    pf->u.track = NULL;
    pf->magic = 0;   // a stale REC_MAGIC must not look live
    rec_track_free(tr);
    copy_block_free(pf);
}
typedef struct { uintptr_t lo, hi; uint8_t *copy; } cmd_copy;
typedef struct { cmd_copy *v; uint32_t n, cap; } copy_bucket;
static PlatMutex copy_lock = PLAT_MUTEX_INIT;    // guards the two maps, the counters and every prefix
static nvn_hmap copy_pages = {.klen = 8};   // page -> copy_bucket
static nvn_hmap copy_live = {.klen = 8};    // handle -> copy_prefix of a live copy
static uint64_t ncopies, copy_bytes, copies_made, nsubmits;
// Asynchronous replay: copies released by the game, and per-submission clones, that a queued submission still reads.
// Freed once gfx_replayed_submits() reaches their used_at. With DKB_CALL_REF also copies released while an OP_CALL
// record still names them, with either replay; they are freed once no record names them (refs 0).
static copy_prefix **zombies;
static uint32_t nzombies, cap_zombies;

// ---------- uniform range lists (nvn_api.h nvn_ubo_list_build, vk_usnap.c DKB_USNAP_LIST)
// Without a back end that builds them (tests, the null back end) a copy has none.
__attribute__((weak)) int nvn_ubo_lists_on(void) { return 0; }
__attribute__((weak)) size_t nvn_ubo_list_build(nvn_range *r, size_t n) {   // only called once lists are on
    (void)r, (void)n;
    rt_fatal("nvn: uniform range lists are on, but no back end builds them");
    return 0;
}

// A copy whose recording binds no uniform range shares this list (never freed).
static nvn_range_list ubo_empty;
static uint64_t ubo_live;   // lists held by copies, live or released and waiting (zombies); atomic

#define UBO_ALIGN 256u
_Static_assert(UBO_ALIGN == 256u, "rlo >> 8 matches rlo / UBO_ALIGN");
#define UBO_MAX_RANGE (64u << 10)
#define UBO_RNG_C4 1ull   // low bit of lo (0 after UBO_ALIGN): matches vk_usnap.c RNG_C4 for VS/FS c4 bindings
#define UBO_SEEN_BITS 8
#define UBO_SEEN_MIN 16

static inline int ubo_lists_cached(void) {
    static int lists = -1;   // nvn_ubo_lists_on, read once; any recording thread may store it first (same value)
    int on = __atomic_load_n(&lists, __ATOMIC_RELAXED);
    if (on < 0) on = nvn_ubo_lists_on() != 0, __atomic_store_n(&lists, on, __ATOMIC_RELAXED);
    return on;
}

// Incremental per-recording tracker attached to a live REC_MAGIC buffer when DKB_CMD_TRACK_FAST is on. Moves with the
// command buffer across cmd_reserve reallocations and RECORDING_HANDOVER so its scratch arrays and generation-tagged
// L1 UBO dedup cache are reused across recordings with zero per-recording allocations or 4 KiB memset clears.
struct rec_track {
    nvn_range *ubos;
    size_t nubos, cubos;
    copy_prefix **callees;
    uint32_t ncallees, cap_callees;
    const copy_prefix *last_cp;
    uint16_t flags, depth;
    uint8_t valid, ubo_ok, deferred, seen_on;
    uint32_t gen;
    uint32_t seen_gen[1u << UBO_SEEN_BITS];
    nvn_range seen[1u << UBO_SEEN_BITS];
};

static rec_track *rec_track_new(void) {
    rec_track *tr = calloc(1, sizeof *tr);
    if (!tr) rt_fatal("nvn: out of memory (recording tracker)");
    return tr;
}

static void rec_track_free(rec_track *tr) {
    if (!tr) return;
    free(tr->ubos);
    free(tr->callees);
    free(tr);
}

static void rec_track_begin(cmd_t *o) {
    if (!cmd_track_fast() || !rec_owned(o)) return;
    copy_prefix *pf = (copy_prefix *)o->seg - 1;
    rec_track *tr = pf->u.track;
    if (!tr) tr = pf->u.track = rec_track_new();
    tr->nubos = 0;
    tr->ncallees = 0;
    tr->last_cp = NULL;
    tr->flags = 0;
    tr->depth = 0;
    tr->valid = 1;
    tr->ubo_ok = (uint8_t)ubo_lists_cached();
    tr->deferred = 0;
    tr->seen_on = 0;
}

static inline rec_track *rec_track_of(const cmd_t *o) {
    if (!cmd_track_fast() || !o->recording || !rec_owned(o)) return NULL;
    rec_track *tr = ((const copy_prefix *)o->seg - 1)->u.track;
    return (tr && tr->valid) ? tr : NULL;
}

static inline void track_ubo_add(rec_track *tr, nvn_range r) {
    uint64_t rlo = r.lo & ~UBO_RNG_C4;
    if (tr->nubos) {
        nvn_range *prev = &tr->ubos[tr->nubos - 1];
        if ((prev->lo & ~UBO_RNG_C4) == rlo && prev->hi == r.hi) {
            if ((prev->lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
            prev->lo |= UBO_RNG_C4;
            if (tr->seen_on) {
                uint32_t idx = (uint32_t)(((rlo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
                if (tr->seen_gen[idx] == tr->gen && (tr->seen[idx].lo & ~UBO_RNG_C4) == rlo && tr->seen[idx].hi == r.hi)
                    tr->seen[idx].lo |= UBO_RNG_C4;
            }
            return;
        }
    }
    if (tr->seen_on) {
        uint32_t idx = (uint32_t)(((rlo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
        if (tr->seen_gen[idx] == tr->gen && (tr->seen[idx].lo & ~UBO_RNG_C4) == rlo && tr->seen[idx].hi == r.hi) {
            if ((tr->seen[idx].lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
            tr->seen[idx].lo |= UBO_RNG_C4;
        } else {
            tr->seen_gen[idx] = tr->gen;
            tr->seen[idx] = r;
        }
    } else if (tr->nubos >= UBO_SEEN_MIN) {
        if (!++tr->gen) {
            memset(tr->seen_gen, 0, sizeof tr->seen_gen);
            tr->gen = 1;
        }
        uint32_t g = tr->gen;
        for (size_t i = 0; i < tr->nubos; i++) {
            nvn_range e = tr->ubos[i];
            uint64_t elo = e.lo & ~UBO_RNG_C4;
            uint32_t eidx = (uint32_t)(((elo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
            if (tr->seen_gen[eidx] == g && (tr->seen[eidx].lo & ~UBO_RNG_C4) == elo && tr->seen[eidx].hi == e.hi)
                tr->seen[eidx].lo |= e.lo & UBO_RNG_C4;
            else {
                tr->seen_gen[eidx] = g;
                tr->seen[eidx] = e;
            }
        }
        tr->seen_on = 1;
        uint32_t idx = (uint32_t)(((rlo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
        if (tr->seen_gen[idx] == g && (tr->seen[idx].lo & ~UBO_RNG_C4) == rlo && tr->seen[idx].hi == r.hi) {
            if ((tr->seen[idx].lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
            tr->seen[idx].lo |= UBO_RNG_C4;
        } else {
            tr->seen_gen[idx] = g;
            tr->seen[idx] = r;
        }
    }
    if (tr->nubos == tr->cubos) {
        size_t c = tr->cubos ? tr->cubos * 2 : 128;
        nvn_range *q = realloc(tr->ubos, c * sizeof *q);
        if (!q) rt_fatal("nvn: out of memory (uniform bindings of a recording)");
        tr->ubos = q;
        tr->cubos = c;
    }
    tr->ubos[tr->nubos++] = r;
}

static inline void track_ubo_bind(rec_track *tr, int stage, int index, uint64_t addr, uint64_t size) {
    if (!tr->ubo_ok || !addr || !size) return;
    uint64_t sz = size < UBO_MAX_RANGE ? size : UBO_MAX_RANGE;
    uint64_t c4 = (index == 1 && (stage == NVN_SHADER_STAGE_VERTEX || stage == NVN_SHADER_STAGE_FRAGMENT) && size <= 256)
                      ? UBO_RNG_C4
                      : 0;
    track_ubo_add(tr, (nvn_range){(addr & ~(uint64_t)(UBO_ALIGN - 1)) | c4, addr + sz});
}

static inline void track_callee_add(rec_track *tr, copy_prefix *cp) {
    if (tr->ncallees == tr->cap_callees) {
        uint32_t c = tr->cap_callees ? tr->cap_callees * 2 : 16;
        copy_prefix **v = realloc(tr->callees, c * sizeof *v);
        if (!v) rt_fatal("nvn: out of memory (recording callees)");
        tr->callees = v;
        tr->cap_callees = c;
    }
    tr->callees[tr->ncallees++] = cp;
}

static void track_callees_append(rec_track *tr, copy_prefix *const *cs, uint32_t n) {
    if (!n) return;
    if (tr->ncallees + n > tr->cap_callees) {
        uint32_t c = tr->cap_callees ? tr->cap_callees * 2 : 16;
        while (c < tr->ncallees + n) c *= 2;
        copy_prefix **v = realloc(tr->callees, c * sizeof *v);
        if (!v) rt_fatal("nvn: out of memory (recording callees)");
        tr->callees = v;
        tr->cap_callees = c;
    }
    memcpy(tr->callees + tr->ncallees, cs, n * sizeof *cs);
    tr->ncallees += n;
}

static inline void track_compose_ubo(rec_track *tr, const copy_prefix *cp) {
    if (!tr->ubo_ok || cp == tr->last_cp) return;
    tr->last_cp = cp;
    const nvn_range_list *cl = cp->ubo;
    if (!cl) {
        tr->ubo_ok = 0;
        return;
    }
    for (uint64_t j = 0; j < cl->n; j++) track_ubo_add(tr, cl->r[j]);
}

// Appends normalized range r to the recording thread's scratch buffer. Small leaf recordings stay under UBO_SEEN_MIN
// without zeroing the 4 KiB seen cache; once UBO_SEEN_MIN ranges accumulate, seen is initialized lazily from them.
static inline void ubo_buf_add(nvn_range r, nvn_range seen[1 << UBO_SEEN_BITS], int *seen_on,
                               nvn_range **buf, size_t *n, size_t *cap) {
    if (*seen_on) {
        uint64_t rlo = r.lo & ~UBO_RNG_C4;
        nvn_range *s = &seen[((rlo / UBO_ALIGN) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS)];
        if ((s->lo & ~UBO_RNG_C4) == rlo && s->hi == r.hi) {
            if ((s->lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
            s->lo |= UBO_RNG_C4;
        } else {
            *s = r;
        }
    } else if (*n >= UBO_SEEN_MIN) {
        memset(seen, 0, sizeof(nvn_range) * (1u << UBO_SEEN_BITS));
        for (size_t i = 0; i < *n; i++) {
            nvn_range e = (*buf)[i];
            uint64_t elo = e.lo & ~UBO_RNG_C4;
            nvn_range *s = &seen[((elo / UBO_ALIGN) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS)];
            if ((s->lo & ~UBO_RNG_C4) == elo && s->hi == e.hi) s->lo |= e.lo & UBO_RNG_C4;
            else *s = e;
        }
        *seen_on = 1;
        uint64_t rlo = r.lo & ~UBO_RNG_C4;
        nvn_range *s = &seen[((rlo / UBO_ALIGN) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS)];
        if ((s->lo & ~UBO_RNG_C4) == rlo && s->hi == r.hi) {
            if ((s->lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
            s->lo |= UBO_RNG_C4;
        } else {
            *s = r;
        }
    }
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 1024;
        nvn_range *q = realloc(*buf, c * sizeof *q);
        if (!q) rt_fatal("nvn: out of memory (uniform bindings of a recording)");
        *buf = q, *cap = c;
    }
    (*buf)[(*n)++] = r;
}

// Thread-local generation-tagged dedup table for DKB_CMD_REC_FAST: keeps seen[] off the stack (avoiding a 4 KiB
// stack probe on every EndRecording) and replaces per-recording 4 KiB memset with a 16-bit generation bump.
static _Thread_local nvn_range tl_ubo_seen[1 << UBO_SEEN_BITS];
static _Thread_local uint16_t tl_ubo_gen[1 << UBO_SEEN_BITS];
static _Thread_local uint16_t tl_ubo_cur_gen;

static __attribute__((noinline)) void ubo_buf_grow(nvn_range **buf, size_t *cap, size_t need) {
    size_t c = *cap ? *cap * 2 : 1024;
    while (c < need) c *= 2;
    nvn_range *q = realloc(*buf, c * sizeof *q);
    if (!q) rt_fatal("nvn: out of memory (uniform bindings of a recording)");
    *buf = q, *cap = c;
}

static __attribute__((noinline)) uint16_t ubo_seen_init_fast(const nvn_range *buf, size_t n) {
    uint16_t gen = ++tl_ubo_cur_gen;
    if (!gen) {
        memset(tl_ubo_gen, 0, sizeof tl_ubo_gen);
        gen = tl_ubo_cur_gen = 1;
    }
    for (size_t i = 0; i < n; i++) {
        nvn_range e = buf[i];
        uint64_t elo = e.lo & ~UBO_RNG_C4;
        uint32_t idx = (uint32_t)(((elo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
        if (tl_ubo_gen[idx] == gen) {
            nvn_range *s = &tl_ubo_seen[idx];
            if ((s->lo & ~UBO_RNG_C4) == elo && s->hi == e.hi) s->lo |= e.lo & UBO_RNG_C4;
            else *s = e;
        } else {
            tl_ubo_gen[idx] = gen;
            tl_ubo_seen[idx] = e;
        }
    }
    return gen;
}

static inline void ubo_buf_add_fast(nvn_range r, uint16_t *seen_gen, nvn_range **buf, size_t *n, size_t *cap) {
    uint16_t gen = *seen_gen;
    if (__builtin_expect(gen != 0, 1)) {
        uint64_t rlo = r.lo & ~UBO_RNG_C4;
        uint32_t idx = (uint32_t)(((rlo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
        if (tl_ubo_gen[idx] == gen) {
            nvn_range *s = &tl_ubo_seen[idx];
            if ((s->lo & ~UBO_RNG_C4) == rlo && s->hi == r.hi) {
                if ((s->lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
                s->lo |= UBO_RNG_C4;
            } else {
                *s = r;
            }
        } else {
            tl_ubo_gen[idx] = gen;
            tl_ubo_seen[idx] = r;
        }
    } else if (*n >= UBO_SEEN_MIN) {
        gen = *seen_gen = ubo_seen_init_fast(*buf, *n);
        uint64_t rlo = r.lo & ~UBO_RNG_C4;
        uint32_t idx = (uint32_t)(((rlo >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - UBO_SEEN_BITS));
        if (tl_ubo_gen[idx] == gen) {
            nvn_range *s = &tl_ubo_seen[idx];
            if ((s->lo & ~UBO_RNG_C4) == rlo && s->hi == r.hi) {
                if ((s->lo & UBO_RNG_C4) || !(r.lo & UBO_RNG_C4)) return;
                s->lo |= UBO_RNG_C4;
            } else {
                *s = r;
            }
        } else {
            tl_ubo_gen[idx] = gen;
            tl_ubo_seen[idx] = r;
        }
    }
    if (__builtin_expect(*n == *cap, 0)) ubo_buf_grow(buf, cap, *n + 1);
    (*buf)[(*n)++] = r;
}

// Stores the merged list of the n normalized ranges in r on pf: empty shares ubo_empty, 1-2 ranges live inline in
// pf->ubo_inl, and 3+ ranges take a heap block counted in ubo_live.
static void ubo_list_store(copy_prefix *pf, const nvn_range *r, size_t m) {
    if (!m) {
        pf->ubo = &ubo_empty;
        return;
    }
    if (m <= 2) {
        pf->ubo_inl.inl.n = m;
        memcpy(pf->ubo_inl.inl.r, r, m * sizeof *r);
        pf->ubo = &pf->ubo_inl.hdr;
        return;
    }
    nvn_range_list *l = malloc(sizeof *l + m * sizeof *l->r);
    if (!l) rt_fatal("nvn: out of memory (uniform range list of %zu ranges)", m);
    l->n = m;
    memcpy(l->r, r, m * sizeof *r);
    __atomic_add_fetch(&ubo_live, 1, __ATOMIC_RELAXED);
    pf->ubo = l;
}

static void ubo_list_set(copy_prefix *pf, nvn_range *r, size_t n) {
    ubo_list_store(pf, r, n ? nvn_ubo_list_build(r, n) : 0);
}

static void ubo_list_set_fast(copy_prefix *pf, nvn_range *r, size_t n, int sorted_merged) {
    size_t m;
    if (!n) {
        m = 0;
    } else if (sorted_merged) {
        m = n;
    } else if (n == 1) {
        m = r[0].hi > (r[0].lo & ~UBO_RNG_C4) ? 1 : 0;
    } else if (n == 2) {
        size_t k = 0;
        if (r[0].hi > (r[0].lo & ~UBO_RNG_C4)) r[k++] = r[0];
        if (r[1].hi > (r[1].lo & ~UBO_RNG_C4)) r[k++] = r[1];
        if (k == 2) {
            if ((r[0].lo & ~UBO_RNG_C4) > (r[1].lo & ~UBO_RNG_C4)) {
                nvn_range t = r[0];
                r[0] = r[1], r[1] = t;
            }
            if ((r[1].lo & ~UBO_RNG_C4) <= r[0].hi) {
                r[0].lo |= r[1].lo & UBO_RNG_C4;
                if (r[1].hi > r[0].hi) r[0].hi = r[1].hi;
                k = 1;
            }
        }
        m = k;
    } else {
        m = nvn_ubo_list_build(r, n);
    }
    ubo_list_store(pf, r, m);
}

static void ubo_list_free(copy_prefix *pf) {
    nvn_range_list *l = pf->ubo;
    if (!l || l == &ubo_empty || l == &pf->ubo_inl.hdr) return;
    free(l);
    __atomic_sub_fetch(&ubo_live, 1, __ATOMIC_RELAXED);
}

uint64_t nvn_ubo_lists_live(void) { return __atomic_load_n(&ubo_live, __ATOMIC_RELAXED); }

// The handle's list while its copy lives, which is as long as a submission of it may replay: the replay reads the
// copy's records without copy_lock in the same way.
const nvn_range_list *nvn_handle_ubo_list(uint64_t handle) {
    return handle ? ((const copy_prefix *)(uintptr_t)handle - 1)->ubo : NULL;
}

static void zombie_add(copy_prefix *pf) {   // copy_lock held
    if (nzombies == cap_zombies) {
        cap_zombies = cap_zombies ? cap_zombies * 2 : 64;
        zombies = realloc(zombies, sizeof *zombies * cap_zombies);
        if (!zombies) rt_fatal("nvn: out of memory (released recording copies)");
    }
    zombies[nzombies++] = pf;
}

// DKB_CALL_REF (default 1): CallCommands and CopyCommands of a recording without deferred binds or pushes record an
// OP_CALL to its copy instead of copying its body into the caller (Hill: about 18,000 calls and 11 ms of the game's
// main thread a frame). The replay, the WaitSync scan and the deferred-bind resolution already follow OP_CALL. The
// called copy counts the OP_CALL records that name it (refs) and is freed only once that count is 0 and no queued
// submission reads it; a freed copy gives its callees' counts back, and so does a recording abandoned by
// BeginRecording or Finalize. One that cannot be read any more (a twin's buffer, Initialize over a recording command
// buffer) keeps its counts, so its callees stay allocated; the log line counts those. A copy with deferred records is
// still copied in, because its records are resolved in place at each submit. A copy is referenced only while its depth
// is below CALL_REF_DEPTH and copied in beyond it, so the game's own nesting never takes a copy deeper than that: the
// replay (vk_exec.c walk) stops past 8 levels, the WaitSync scan and the deferred-bind resolution past 16.
#define CALL_REF_DEPTH 6
static int call_ref(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("DKB_CALL_REF");
        if (on) rt_log("nvn: DKB_CALL_REF: CallCommands references recordings without deferred records");
    }
    return on;
}
int nvn_call_ref_on(void) { return call_ref(); }
static uint64_t n_call_refs, n_call_inlined, n_call_deep, n_ref_leaks;

// Adds delta to the count of every copy that an OP_CALL record in [p, end) names (a copy's records or an inlined body).
static void calls_adjust(const uint8_t *p, const uint8_t *end, int delta) {
    while (p < end) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8) break;
        if (h.op == OP_CALL && h.size >= 16) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            __atomic_add_fetch(&((copy_prefix *)(uintptr_t)t - 1)->refs, (uint32_t)delta, __ATOMIC_RELAXED);
        }
        if (h.op == OP_END) break;
        p += h.size;
    }
}

static void copy_calls_adjust(const copy_prefix *pf, int delta) {
    if (pf->flags & COPY_CALLEES) {
        copy_prefix *const *cs = copy_callees(pf);
        for (uint32_t i = 0; i < pf->ncallees; i++)
            __atomic_add_fetch(&cs[i]->refs, (uint32_t)delta, __ATOMIC_RELAXED);
    } else {
        calls_adjust((const uint8_t *)(pf + 1), (const uint8_t *)(pf + 1) + pf->bytes, delta);
    }
}

// A released copy or a clone, with its uniform list (copy_lock held; no OP_CALL record names pf any more). A recording
// buffer has no list: rec_buffer_free.
static void copy_free(copy_prefix *pf) {
    if (pf->flags & COPY_CALLS) copy_calls_adjust(pf, -1);
    if ((pf->flags & COPY_CALLEES) && pf->ncallees > 4) free(pf->u.callees);
    free(pf->def_list);
    ubo_list_free(pf);
    copy_block_free(pf);
}

// Deferred copy freeing (DKB_CMD_REC_FAST): unlinks ready-to-free copies under copy_lock (still running calls_adjust
// for COPY_CALLS so nested zombies drain in one sweep), then runs free() and blk_lock outside copy_lock.
static _Thread_local copy_prefix **tl_free_pf;
static _Thread_local uint32_t tl_free_pf_cap;
static _Thread_local void **tl_free_ptr;
static _Thread_local uint32_t tl_free_ptr_cap;

static void defer_pf(copy_prefix *pf, uint32_t *n) {
    if (*n == tl_free_pf_cap) {
        uint32_t cap = tl_free_pf_cap ? tl_free_pf_cap * 2 : 64;
        copy_prefix **v = realloc(tl_free_pf, sizeof *v * cap);
        if (!v) rt_fatal("nvn: out of memory (deferred copy release)");
        tl_free_pf = v, tl_free_pf_cap = cap;
    }
    tl_free_pf[(*n)++] = pf;
}

static void defer_ptr(void *p, uint32_t *n) {
    if (!p) return;
    if (*n == tl_free_ptr_cap) {
        uint32_t cap = tl_free_ptr_cap ? tl_free_ptr_cap * 2 : 64;
        void **v = realloc(tl_free_ptr, sizeof *v * cap);
        if (!v) rt_fatal("nvn: out of memory (deferred bucket release)");
        tl_free_ptr = v, tl_free_ptr_cap = cap;
    }
    tl_free_ptr[(*n)++] = p;
}

static void flush_deferred_frees(uint32_t npf, uint32_t nptr) {
    uint64_t heap_ubo = 0;
    for (uint32_t i = 0; i < npf; i++) {
        copy_prefix *pf = tl_free_pf[i];
        if ((pf->flags & COPY_CALLEES) && pf->ncallees > 4) free(pf->u.callees);
        free(pf->def_list);
        nvn_range_list *l = pf->ubo;
        if (l && l != &ubo_empty && l != &pf->ubo_inl.hdr) {
            free(l);
            heap_ubo++;
        }
    }
    if (heap_ubo) __atomic_sub_fetch(&ubo_live, heap_ubo, __ATOMIC_RELAXED);
    copy_block_free_many(tl_free_pf, npf);
    for (uint32_t i = 0; i < nptr; i++) free(tl_free_ptr[i]);
}

// Again while it freed a copy that names others: their counts may have reached 0, so a nest goes in one sweep.
static void zombie_sweep(uint64_t replayed) {   // copy_lock held
    for (int again = 1; again;) {
        again = 0;
        for (uint32_t i = 0; i < nzombies;) {
            if (zombies[i]->used_at <= replayed && !__atomic_load_n(&zombies[i]->refs, __ATOMIC_RELAXED)) {
                copy_prefix *pf = zombies[i];
                zombies[i] = zombies[--nzombies];
                again |= pf->flags & COPY_CALLS;
                copy_free(pf);
            } else {
                i++;
            }
        }
    }
}

static void zombie_sweep_defer(uint64_t replayed, uint32_t *npf) {   // copy_lock held
    for (int again = 1; again;) {
        again = 0;
        for (uint32_t i = 0; i < nzombies;) {
            if (zombies[i]->used_at <= replayed && !__atomic_load_n(&zombies[i]->refs, __ATOMIC_RELAXED)) {
                copy_prefix *pf = zombies[i];
                zombies[i] = zombies[--nzombies];
                if (pf->flags & COPY_CALLS) {
                    again = 1;
                    copy_calls_adjust(pf, -1);
                }
                defer_pf(pf, npf);
            } else {
                i++;
            }
        }
    }
}

// The recording o abandons gives back the counts its OP_CALL records took, when its buffer is its own to read; else
// they stay taken (counted).
static void drop_recording_calls(cmd_t *o) {
    if (!call_ref() || !o->recording || !o->seg) return;
    if (rec_owned(o)) {
        rec_track *tr = ((copy_prefix *)o->seg - 1)->u.track;
        if (tr && tr->valid) {
            for (uint32_t i = 0; i < tr->ncallees; i++)
                __atomic_sub_fetch(&tr->callees[i]->refs, 1, __ATOMIC_RELAXED);
            tr->ncallees = 0;
            tr->valid = 0;
        } else {
            calls_adjust(o->seg, o->cur, -1);
        }
    } else {
        __atomic_add_fetch(&n_ref_leaks, 1, __ATOMIC_RELAXED);
    }
}

// For the tests: a copy's count and depth (its prefix is read as is, so a released copy must still be a zombie), and
// the zombie list's length.
uint32_t nvn_copy_refs(uint64_t handle) { return __atomic_load_n(&((const copy_prefix *)(uintptr_t)handle - 1)->refs, __ATOMIC_RELAXED); }
uint32_t nvn_copy_depth(uint64_t handle) { return ((const copy_prefix *)(uintptr_t)handle - 1)->depth; }
uint32_t nvn_copy_zombies(void) {
    plat_mutex_lock_shared(&copy_lock);
    uint32_t n = nzombies;
    plat_mutex_unlock_shared(&copy_lock);
    return n;
}

static copy_prefix *copy_find(uint64_t handle) {   // copy_lock held
    return nvn_hm_get(&copy_live, &handle);
}

int nvn_handle_live(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    int live = copy_find(handle) != NULL;
    plat_mutex_unlock_shared(&copy_lock);
    return live;
}

uint64_t nvn_handle_bytes(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    const copy_prefix *pf = copy_find(handle);
    uint64_t n = pf ? pf->bytes : 0;
    plat_mutex_unlock_shared(&copy_lock);
    return n;
}

// Marks a live copy used by submission now; its size from OP_BEGIN to OP_END inclusive.
static uint64_t copy_touch(uint64_t handle, uint64_t now, const char *what) {
    plat_mutex_lock_shared(&copy_lock);
    copy_prefix *pf = copy_find(handle);
    uint64_t bytes = 0;
    if (pf) used_at_max(pf, now), bytes = pf->bytes;
    plat_mutex_unlock_shared(&copy_lock);
    if (!pf) rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                      (unsigned long long)handle);
    return bytes;
}

// The recording [seg, cur), OP_BEGIN to OP_END (the recording buffer is one block: recording emits no OP_JUMP), checked
// record by record, as a copy. One of RECORDING_HANDOVER bytes or more is its recording buffer itself, a copy_block
// with the prefix in front, and the command buffer takes a block of the same size from the cache for its next
// recording; a smaller one is copied into a block of its own, as a handed-over buffer of 64 KiB or more would stay
// allocated for as long as the game keeps the handle. Growing the copy record by record cost the game's main thread
// about 1 ms a frame in reallocations and small copies (1.4.3, field); copying every recording at its end took about
// two fifths of the main thread's running time in the Hilltop Layer of Surf Hills (3.2.0, 185 KiB a copy on average).
// With uniform range lists on (nvn_ubo_lists_on), the check also gathers the copy's normalized uniform bindings (and
// composes OP_CALL callees' merged lists under DKB_CALL_REF, deduplicated through a stack-local L1 cache and
// consecutive-callee skip) into the calling thread's buffer, and the copy keeps their merged list (ubo_list_set). The
// records are final here and the submit changes no uniform binding (resolve_deferred). If any callee lacks a list, the
// caller gets NULL and the snapshot walks it (still taking lists for callees that have them).
#define RECORDING_HANDOVER (32u << 10)
static _Thread_local nvn_range *tl_ubo_buf;   // the gathered bindings, kept for the thread's next recording
static _Thread_local size_t tl_ubo_cap;
static _Thread_local uint32_t *tl_def_offs;
static _Thread_local size_t tl_def_cap;

static __attribute__((noinline)) void def_offs_push(uint32_t off, uint32_t *ndef) {
    if (*ndef == tl_def_cap) {
        size_t c = tl_def_cap ? tl_def_cap * 2 : 64;
        uint32_t *q = realloc(tl_def_offs, c * sizeof *q);
        if (!q) rt_fatal("nvn: out of memory (deferred record offsets)");
        tl_def_offs = q, tl_def_cap = c;
    }
    tl_def_offs[(*ndef)++] = off;
}

static __attribute__((noinline)) uint8_t *take_recording_slow(cmd_t *o, int on) {
    const uint8_t *p = o->seg;
    size_t n = (size_t)(o->cur - o->seg);
    int owned = rec_owned(o);
    rec_track *tr = owned ? ((copy_prefix *)o->seg - 1)->u.track : NULL;
    int ubo_ok = on, seen_on = 0;
    size_t nubo = 0;
    const copy_prefix *last_cp = NULL;
    nvn_range seen[1 << UBO_SEEN_BITS];
    int calls = 0, deferred = 0, depth = 0, waits = 0;
    for (size_t at = 0;;) {   // the records stay checked: a corrupt one stops here, not in the replay
        op_hdr h;
        memcpy(&h, p + at, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_ || h.op == OP_JUMP || (h.op == OP_CALL && !call_ref()) ||
            at + h.size > n || (!at && h.op != OP_BEGIN))
            rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)(p + at), h.op, h.size);
        if (h.op == OP_CALL && h.size >= 16) {   // the callee stays allocated: this record holds a count of it
            uint64_t t;
            memcpy(&t, p + at + 8, 8);
            const copy_prefix *cp = (const copy_prefix *)(uintptr_t)t - 1;
            int d = cp->depth + 1;
            calls = 1, depth = d > depth ? d : depth, waits |= (cp->flags & COPY_WAITS) != 0;
            if (ubo_ok && cp != last_cp) {
                last_cp = cp;
                const nvn_range_list *cl = cp->ubo;
                if (!cl) ubo_ok = 0;
                else for (uint64_t j = 0; j < cl->n; j++)
                    ubo_buf_add(cl->r[j], seen, &seen_on, &tl_ubo_buf, &nubo, &tl_ubo_cap);
            }
        } else if (h.op == OP_WAIT_SYNC) {
            waits = 1;
        } else if (h.op == OP_PUSH_CONST && h.size >= 8 + offsetof(p_push_const, w))
            deferred |= ((const p_push_const *)(const void *)(p + at + 8))->src != 0;
        else if ((h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) &&
                 h.size >= 8 + sizeof(p_bind_handles) - sizeof(uint64_t) * 32)
            deferred |= ((const p_bind_handles *)(const void *)(p + at + 8))->src != 0;
        else if (ubo_ok && h.op == OP_BIND_UBO) {
            if (h.size < sizeof h + sizeof(p_bind_buf))
                rt_fatal("nvn: short uniform binding at %p (size %u)", (const void *)(p + at), h.size);
            p_bind_buf b;
            memcpy(&b, p + at + sizeof h, sizeof b);
            if (b.addr && b.size) {
                uint64_t sz = b.size < UBO_MAX_RANGE ? b.size : UBO_MAX_RANGE;
                uint64_t c4 = (b.index == 1 &&
                               (b.stage == NVN_SHADER_STAGE_VERTEX || b.stage == NVN_SHADER_STAGE_FRAGMENT) &&
                               b.size <= 256)
                                  ? UBO_RNG_C4
                                  : 0;
                ubo_buf_add((nvn_range){(b.addr & ~(uint64_t)(UBO_ALIGN - 1)) | c4, b.addr + sz},
                            seen, &seen_on, &tl_ubo_buf, &nubo, &tl_ubo_cap);
            }
        }
        at += h.size;
        if (h.op == OP_END) {
            if (at != n) rt_fatal("nvn: recording at %p ends at %zu of %zu bytes", (const void *)p, at, n);
            break;
        }
    }
    copy_prefix *pf;
    // Hand the buffer over only when it is the block a copy would get anyway (else a 40 KiB recording could pin the
    // 8 MiB the buffer grew to) and this object owns it. The next buffer is sized to the same class.
    if (n >= RECORDING_HANDOVER && owned &&
        blk_class(sizeof *pf + n) == blk_class((size_t)((copy_prefix *)o->seg - 1)->block)) {
        pf = (copy_prefix *)o->seg - 1;
        rec_track *saved_tr = pf->u.track;
        pf->u.track = NULL;
        size_t room;
        o->seg = o->cur = rec_buffer((size_t)pf->block - sizeof *pf, &room);
        o->end = o->seg + room;
        ((copy_prefix *)o->seg - 1)->u.track = saved_tr;
    } else {
        pf = copy_block(sizeof *pf + n);
        memcpy(pf + 1, p, n);
    }
    if (tr) tr->valid = 0;
    uint64_t block = pf->block;
    // With DKB_CALL_REF the deferred records are known from here on (inline_recording needs it before any submit).
    *pf = (copy_prefix){COPY_MAGIC, n, 0, 0, call_ref() ? (uint64_t)(deferred ? 2 : 1) : 0, block, 0,
                        (uint16_t)((calls ? COPY_CALLS : 0) | (waits ? COPY_WAITS : 0)), (uint16_t)depth, NULL, NULL,
                        0, 0, {NULL}, {{0}}};
    if (ubo_ok) ubo_list_set(pf, tl_ubo_buf, nubo);
    return (uint8_t *)(pf + 1);
}

static uint8_t *take_recording(cmd_t *o) {
    const uint8_t *p = o->seg;
    size_t n = (size_t)(o->cur - o->seg);
    int owned = rec_owned(o);
    rec_track *tr = owned ? ((copy_prefix *)o->seg - 1)->u.track : NULL;
    if (cmd_track_fast() && tr && tr->valid) {
        if (n < 16 || (n & 7))
            rt_fatal("nvn: corrupt recording at %p (size %zu)", (const void *)p, n);
        op_hdr first, last;
        memcpy(&first, p, sizeof first);
        memcpy(&last, p + n - sizeof last, sizeof last);
        if (first.op != OP_BEGIN || first.size != 8 || last.op != OP_END || last.size != 8)
            rt_fatal("nvn: corrupt recording boundaries at %p (first %u/%u last %u/%u)", (const void *)p,
                     first.op, first.size, last.op, last.size);
        copy_prefix *pf;
        if (n >= RECORDING_HANDOVER &&
            blk_class(sizeof *pf + n) == blk_class((size_t)((copy_prefix *)o->seg - 1)->block)) {
            pf = (copy_prefix *)o->seg - 1;
            pf->u.track = NULL;
            size_t room;
            o->seg = o->cur = rec_buffer((size_t)pf->block - sizeof *pf, &room);
            o->end = o->seg + room;
            ((copy_prefix *)o->seg - 1)->u.track = tr;
        } else {
            pf = copy_block(sizeof *pf + n);
            memcpy(pf + 1, p, n);
        }
        uint64_t block = pf->block;
        uint32_t nc = tr->ncallees;
        *pf = (copy_prefix){COPY_MAGIC, n, 0, 0, call_ref() ? (uint64_t)(tr->deferred ? 2 : 1) : 0, block, 0,
                            (uint16_t)((tr->flags & (COPY_CALLS | COPY_WAITS)) | COPY_CALLEES), tr->depth, NULL, NULL,
                            nc, 0, {NULL}, {{0}}};
        if (nc <= 4) {
            if (nc) memcpy(pf->u.callee_inl, tr->callees, nc * sizeof *pf->u.callee_inl);
        } else {
            copy_prefix **v = malloc(nc * sizeof *v);
            if (!v) rt_fatal("nvn: out of memory (recording callees)");
            memcpy(v, tr->callees, nc * sizeof *v);
            pf->u.callees = v;
        }
        if (tr->ubo_ok) ubo_list_set_fast(pf, tr->ubos, tr->nubos, 0);
        tr->valid = 0;
        return (uint8_t *)(pf + 1);
    }
    int on = ubo_lists_cached();
    if (!cmd_rec_fast()) return take_recording_slow(o, on);
    const int cref = call_ref();
    int ubo_ok = on, single_callee = 0;
    uint16_t seen_gen = 0;
    size_t nubo = 0;
    uint32_t ndef = 0;
    int def_ok = 1;
    const copy_prefix *last_cp = NULL;
    nvn_range *ubo_buf = tl_ubo_buf;
    size_t ubo_cap = tl_ubo_cap;
    int calls = 0, deferred = 0, depth = 0, waits = 0;
    if (__builtin_expect(n < 8, 0))
        rt_fatal("nvn: corrupt recording at %p (empty)", (const void *)p);
    op_hdr first_h;
    memcpy(&first_h, p, sizeof first_h);
    if (__builtin_expect(first_h.size < 8 || (first_h.size & 7) || first_h.op != OP_BEGIN || first_h.size > n, 0))
        rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)p, first_h.op, first_h.size);
    uint64_t scan_mask = (1ull << OP_END) | (1ull << OP_CALL) | (1ull << OP_WAIT_SYNC) | (1ull << OP_PUSH_CONST) |
                         (1ull << OP_BIND_TEX) | (1ull << OP_BIND_IMG) | (1ull << OP_BIND_SAMP) |
                         (ubo_ok ? (1ull << OP_BIND_UBO) : 0ull);
    for (size_t at = first_h.size;;) {
        if (__builtin_expect(at + 8 > n, 0))
            rt_fatal("nvn: corrupt recording at %p (truncated at %zu of %zu)", (const void *)(p + at), at, n);
        op_hdr h;
        memcpy(&h, p + at, sizeof h);
        if (__builtin_expect(h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_ || h.op == OP_JUMP || at + h.size > n, 0))
            rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)(p + at), h.op, h.size);
        if ((1ull << h.op) & scan_mask) {
            switch (h.op) {
            case OP_BIND_UBO: {
                if (__builtin_expect(h.size < sizeof h + sizeof(p_bind_buf), 0))
                    rt_fatal("nvn: short uniform binding at %p (size %u)", (const void *)(p + at), h.size);
                p_bind_buf b;
                memcpy(&b, p + at + sizeof h, sizeof b);
                if (b.addr && b.size) {
                    single_callee = 0;
                    uint64_t sz = b.size < UBO_MAX_RANGE ? b.size : UBO_MAX_RANGE;
                    uint64_t c4 = (b.index == 1 &&
                                   (b.stage == NVN_SHADER_STAGE_VERTEX || b.stage == NVN_SHADER_STAGE_FRAGMENT) &&
                                   b.size <= 256)
                                      ? UBO_RNG_C4
                                      : 0;
                    ubo_buf_add_fast((nvn_range){(b.addr & ~(uint64_t)(UBO_ALIGN - 1)) | c4, b.addr + sz},
                                     &seen_gen, &ubo_buf, &nubo, &ubo_cap);
                }
                break;
            }
            case OP_CALL: {
                if (__builtin_expect(!cref, 0))
                    rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)(p + at), h.op, h.size);
                if (h.size >= 16) {
                    uint64_t t;
                    memcpy(&t, p + at + 8, 8);
                    const copy_prefix *cp = (const copy_prefix *)(uintptr_t)t - 1;
                    int d = cp->depth + 1;
                    calls = 1, depth = d > depth ? d : depth, waits |= (cp->flags & COPY_WAITS) != 0;
                    if (cp->deferred != 1) def_ok = 0;
                    if (ubo_ok && cp != last_cp) {
                        last_cp = cp;
                        const nvn_range_list *cl = cp->ubo;
                        if (!cl) {
                            ubo_ok = 0;
                            single_callee = 0;
                            scan_mask &= ~(1ull << OP_BIND_UBO);
                        } else if (cl->n) {
                            if (!nubo) {
                                if (__builtin_expect(cl->n > ubo_cap, 0)) ubo_buf_grow(&ubo_buf, &ubo_cap, (size_t)cl->n);
                                memcpy(ubo_buf, cl->r, (size_t)cl->n * sizeof *cl->r);
                                nubo = (size_t)cl->n;
                                single_callee = 1;
                            } else {
                                single_callee = 0;
                                for (uint64_t j = 0; j < cl->n; j++)
                                    ubo_buf_add_fast(cl->r[j], &seen_gen, &ubo_buf, &nubo, &ubo_cap);
                            }
                        }
                    }
                }
                break;
            }
            case OP_PUSH_CONST:
                if (h.size >= 8 + offsetof(p_push_const, w) &&
                    ((const p_push_const *)(const void *)(p + at + 8))->src != 0) {
                    deferred = 1;
                    if (at <= UINT32_MAX) def_offs_push((uint32_t)at, &ndef);
                    else def_ok = 0;
                }
                break;
            case OP_BIND_TEX:
            case OP_BIND_IMG:
            case OP_BIND_SAMP:
                if (h.size >= 8 + sizeof(p_bind_handles) - sizeof(uint64_t) * 32 &&
                    ((const p_bind_handles *)(const void *)(p + at + 8))->src != 0) {
                    deferred = 1;
                    if (at <= UINT32_MAX) def_offs_push((uint32_t)at, &ndef);
                    else def_ok = 0;
                }
                break;
            case OP_WAIT_SYNC:
                waits = 1;
                break;
            case OP_END:
                at += h.size;
                if (__builtin_expect(at != n, 0))
                    rt_fatal("nvn: recording at %p ends at %zu of %zu bytes", (const void *)p, at, n);
                goto scan_done;
            default:
                break;
            }
        }
        at += h.size;
    }
scan_done:
    tl_ubo_buf = ubo_buf;
    tl_ubo_cap = ubo_cap;
    copy_prefix *pf;
    if (n >= RECORDING_HANDOVER && owned &&
        blk_class(sizeof *pf + n) == blk_class((size_t)((copy_prefix *)o->seg - 1)->block)) {
        pf = (copy_prefix *)o->seg - 1;
        rec_track *saved_tr = pf->u.track;
        pf->u.track = NULL;
        size_t room;
        o->seg = o->cur = rec_buffer((size_t)pf->block - sizeof *pf, &room);
        o->end = o->seg + room;
        ((copy_prefix *)o->seg - 1)->u.track = saved_tr;
    } else {
        pf = copy_block(sizeof *pf + n);
        memcpy(pf + 1, p, n);
    }
    if (tr) tr->valid = 0;
    uint64_t block = pf->block;
    def_off_list *dl = NULL;
    if (ndef && def_ok) {
        dl = malloc(sizeof *dl + (size_t)ndef * sizeof *dl->offs);
        if (!dl) rt_fatal("nvn: out of memory (deferred record list of %u)", ndef);
        dl->n = ndef;
        memcpy(dl->offs, tl_def_offs, (size_t)ndef * sizeof *dl->offs);
    }
    *pf = (copy_prefix){COPY_MAGIC, n, 0, 0, cref ? (uint64_t)(deferred ? 2 : 1) : 0, block, 0,
                        (uint16_t)((calls ? COPY_CALLS : 0) | (waits ? COPY_WAITS : 0)), (uint16_t)depth, NULL, dl,
                        0, 0, {NULL}, {{0}}};
    if (ubo_ok) ubo_list_set_fast(pf, ubo_buf, nubo, single_callee);
    return (uint8_t *)(pf + 1);
}

static void copy_add(uintptr_t lo, uintptr_t hi, uint8_t *copy) {
    copy_prefix *pf = (copy_prefix *)copy - 1;
    uint64_t handle = (uintptr_t)copy;
    uintptr_t page = lo >> COPY_PAGE_SHIFT;
    plat_mutex_lock(&copy_lock);
    pf->made_at = nsubmits;
    copy_bucket *b = nvn_hm_get(&copy_pages, &page);
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!b) rt_fatal("nvn: command copy index");
        nvn_hm_put(&copy_pages, &page, b);
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = realloc(b->v, b->cap * sizeof *b->v);
        if (!b->v) rt_fatal("nvn: command copy index");
    }
    b->v[b->n++] = (cmd_copy){lo, hi, copy};
    nvn_hm_put(&copy_live, &handle, pf);
    ncopies++;
    copy_bytes += pf->bytes;
    if (++copies_made % 20000 == 0)
        NVN_LOG("recording copies: %llu made, %llu live, %.1f MiB, submit %llu", (unsigned long long)copies_made,
                (unsigned long long)ncopies, copy_bytes / 1048576.0, (unsigned long long)nsubmits);
    plat_mutex_unlock(&copy_lock);
}

// Adding control memory again retires every recording copy made in it (the driver's handle lived there).
static void release_copies(uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    uint64_t replayed = gfx_replayed_submits();
    if (cmd_rec_fast()) {
        uint32_t npf = 0, nptr = 0;
        plat_mutex_lock(&copy_lock);
        if (nzombies) zombie_sweep_defer(replayed, &npf);
        for (uintptr_t pg = lo >> COPY_PAGE_SHIFT; pg <= (hi - 1) >> COPY_PAGE_SHIFT; pg++) {
            copy_bucket *b = nvn_hm_get(&copy_pages, &pg);
            if (!b) continue;
            for (uint32_t i = 0; i < b->n;) {
                cmd_copy *k = &b->v[i];
                if (!(k->lo < hi && lo < k->hi)) { i++; continue; }
                uint64_t handle = (uintptr_t)k->copy;
                copy_prefix *pf = (copy_prefix *)k->copy - 1;
                nvn_hm_del(&copy_live, &handle);
                ncopies--;
                copy_bytes -= pf->bytes;
                pf->magic = 0;
                if (pf->used_at > replayed || __atomic_load_n(&pf->refs, __ATOMIC_RELAXED)) {
                    zombie_add(pf);   // still read
                } else {
                    if (pf->flags & COPY_CALLS) copy_calls_adjust(pf, -1);
                    defer_pf(pf, &npf);
                }
                b->v[i] = b->v[--b->n];
            }
            if (!b->n) {
                nvn_hm_del(&copy_pages, &pg);
                defer_ptr(b->v, &nptr);
                defer_ptr(b, &nptr);
            }
        }
        plat_mutex_unlock(&copy_lock);
        if (npf || nptr) flush_deferred_frees(npf, nptr);
        return;
    }
    plat_mutex_lock(&copy_lock);
    if (nzombies) zombie_sweep(replayed);
    for (uintptr_t pg = lo >> COPY_PAGE_SHIFT; pg <= (hi - 1) >> COPY_PAGE_SHIFT; pg++) {
        copy_bucket *b = nvn_hm_get(&copy_pages, &pg);
        if (!b) continue;
        for (uint32_t i = 0; i < b->n;) {
            cmd_copy *k = &b->v[i];
            if (!(k->lo < hi && lo < k->hi)) { i++; continue; }
            uint64_t handle = (uintptr_t)k->copy;
            copy_prefix *pf = (copy_prefix *)k->copy - 1;
            nvn_hm_del(&copy_live, &handle);
            ncopies--;
            copy_bytes -= pf->bytes;
            pf->magic = 0;
            if (pf->used_at > replayed || __atomic_load_n(&pf->refs, __ATOMIC_RELAXED)) zombie_add(pf);   // still read
            else copy_free(pf);
            b->v[i] = b->v[--b->n];
        }
        if (!b->n) {
            nvn_hm_del(&copy_pages, &pg);
            free(b->v);
            free(b);
        }
    }
    plat_mutex_unlock(&copy_lock);
}

// ---------- CommandBuffer: object and memory
// NVNboolean nvnCommandBufferInitialize(NVNcommandBuffer *cmd, NVNdevice *device): the inline fast-path fields are kept
// at ~0 / 0 so NVN 55 inline code never finds room and calls the API (nvn_obj.h cmd_t)
NVN_API(nvnCommandBufferInitialize) {
    cmd_t *o = cmd_of(c);
    // Initialize over an object that was never finalized drops its recording buffer (seg lives in host memory). Logged
    // only: the old bytes may be a copy whose buffer another object owns, so freeing it here is not safe.
    static uint32_t reinits;
    if (o->magic == CMD_MAGIC && o->seg && __atomic_fetch_add(&reinits, 1, __ATOMIC_RELAXED) < 8)
        rt_log("nvn: Initialize over the command buffer %p, which was not finalized; its %zu-byte recording buffer %p "
               "is dropped", (void *)o, (size_t)(o->end - o->seg), (void *)o->seg);
    if (o->magic == CMD_MAGIC && o->recording && o->seg && call_ref())   // the bytes may be stale: counted, not read
        __atomic_add_fetch(&n_ref_leaks, 1, __ATOMIC_RELAXED);
    memset(o, 0, sizeof *o);
    o->magic = CMD_MAGIC;
    o->dev = arg_p(c, 1);
    o->inline_cur = ~0ull;
    o->inline_end = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
    nvn_mode_trace(NVN_MODE_INIT, (uintptr_t)o, 0, 0, 0);
    ret_x(c, 1);
}
// void nvnCommandBufferFinalize(NVNcommandBuffer *cmd): handles outlive their recorder
NVN_API(nvnCommandBufferFinalize) {
    cmd_t *o = cmd_of(c);
    drop_recording_calls(o);
    if (rec_owned(o)) {
        seg_check(o, 0);
        rec_buffer_free(o->seg);
    }
    o->seg = o->cur = o->end = NULL;
    o->seg_tok = 0;
    o->magic = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
    nvn_mode_trace(NVN_MODE_FINALIZE, (uintptr_t)o, 0, 0, 0);
}
// void nvnCommandBufferSetMemoryCallback(NVNcommandBuffer *cmd, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC callback)
NVN_API(nvnCommandBufferSetMemoryCallback) { cmd_of(c)->cb = arg_p(c, 1); }
// void nvnCommandBufferSetMemoryCallbackData(NVNcommandBuffer *cmd, void *data)
NVN_API(nvnCommandBufferSetMemoryCallbackData) { cmd_of(c)->cb_data = arg_p(c, 1); }
// PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC nvnCommandBufferGetMemoryCallback(const NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferGetMemoryCallback) { ret_x(c, (uintptr_t)cmd_of(c)->cb); }
// void *nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer *cmd) (nvn.h declares it void)
NVN_API(nvnCommandBufferGetMemoryCallbackData) { ret_x(c, (uintptr_t)cmd_of(c)->cb_data); }
// void nvnCommandBufferSetCommandMemoryCallbackEnabled / SetControlMemoryCallbackEnabled(NVNcommandBuffer *cmd,
// NVNboolean enabled) (NVN 55, no prototype in nvn.h)
NVN_API(nvnCommandBufferSetCommandMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_COMMAND, !nvn_arg_bool(c, 1)); }
NVN_API(nvnCommandBufferSetControlMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_CONTROL, !nvn_arg_bool(c, 1)); }
// void nvnCommandBufferAddCommandMemory(NVNcommandBuffer *cmd, const NVNmemoryPool *pool, ptrdiff_t offset, size_t size):
// accounting only; command memory can be shared by independent recordings with distinct control memory
NVN_API(nvnCommandBufferAddCommandMemory) {
    cmd_t *o = cmd_of(c);
    const pool_t *p = arg_p(c, 1);
    o->cmd_base = (uintptr_t)(p ? p->mem : NULL) + (uintptr_t)arg_x(c, 2);
    o->game_size = arg_x(c, 3);
    o->cmd_used = 0;
    o->hw_used = 0;
}
// void nvnCommandBufferAddControlMemory(NVNcommandBuffer *cmd, void *memory, size_t size): the SDK encodes handles in
// control memory (sdk+0x444530), so reusing that storage retires the copies recorded there
NVN_API(nvnCommandBufferAddControlMemory) {
    cmd_t *o = cmd_of(c);
    void *m = arg_p(c, 1);
    size_t n = arg_x(c, 2);
    release_copies((uintptr_t)m, (uintptr_t)m + n);
    o->ctl = m;
    o->ctl_size = n;
    o->ctl_used = 0;
}
// size_t nvnCommandBufferGet{Control,Command}Memory{Size,Used,Free}(const NVNcommandBuffer *cmd): usage is bounded by
// the memory the game gave, so the game never sees more used than it provided
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
NVN_API(nvnCommandBufferGetControlMemorySize) { ret_x(c, cmd_of(c)->ctl_size); }
NVN_API(nvnCommandBufferGetControlMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetControlMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->ctl_size - min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetCommandMemorySize) { ret_x(c, cmd_of(c)->game_size); }
NVN_API(nvnCommandBufferGetCommandMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->hw_used, o->game_size)); }
NVN_API(nvnCommandBufferGetCommandMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->game_size - min_size(o->hw_used, o->game_size)); }

// ---------- CommandBuffer: recording
// void nvnCommandBufferBeginRecording(NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferBeginRecording) {
    cmd_t *o = cmd_of(c);
    if (o->recording) {
        rt_log("nvn: BeginRecording on command buffer %p while it records", (void *)o);
        drop_recording_calls(o);
    }
    o->recording = 1;
    if (o->seg && !rec_owned(o)) o->seg = o->cur = o->end = NULL;   // a twin's or handed-over buffer: take a block of our own
    o->cur = o->seg;   // the previous recording was copied or handed over at EndRecording (take_recording)
    o->rec_start = o->cmd_used;
    put_hdr(cmd_alloc(o, 8), OP_BEGIN, 8);
    o->hw_used -= 8;   // BEGIN and END have no hardware equivalent
    o->hw_start = o->hw_used;
    rec_track_begin(o);
    nvn_mode_trace(NVN_MODE_BEGIN, (uintptr_t)o, 0, 0, 0);
    if (!o->game_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, MIN_COMMAND_SIZE);
}
// NVNcommandHandle nvnCommandBufferEndRecording(NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferEndRecording) {
    cmd_t *o = cmd_of(c);
    if (!o->recording || !o->seg) rt_fatal("nvn: EndRecording on command buffer %p, which does not record", (void *)o);
    if (!o->ctl_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY, MIN_CONTROL_SIZE);
    put_hdr(cmd_alloc(o, 8), OP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    uint8_t *copy = take_recording(o);
    // Finalizing the recorder does not retire its handle; only reusing the control memory it was recorded in does.
    uintptr_t lo = (uintptr_t)o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    uintptr_t hi = o->ctl ? lo + 1 : 0;
    o->ctl_used += CTL_PER_RECORDING;
    copy_add(lo, hi, copy);
    nvn_mode_trace(NVN_MODE_END, (uintptr_t)o, (uintptr_t)copy, 0, 0);
    ret_x(c, (uintptr_t)copy);
}
// NVNboolean nvnCommandBufferIsRecording(const NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferIsRecording) { ret_x(c, cmd_of(c)->recording != 0); }

// With DKB_CALL_REF (the default), a call to a recording without deferred records whose copy nests less than
// CALL_REF_DEPTH deep becomes one OP_CALL naming that copy, which counts the reference and so outlives the game's
// release of its control memory. Every other call copies the called recording into the caller at the call position:
// the game recycles command memory in small steps, so a handle may be gone by the time the caller is submitted.
// Deferred binds stay deferred. A copy is OP_BEGIN, its records and OP_END (take_recording checked them), so the
// records go over as one block; usage grows by the same bytes as record by record.
static void inline_recording(cmd_t *o, uint64_t handle, const char *what) {
    if (!handle) rt_fatal("nvn: %s with a null handle", what);
    uint64_t bytes;
    if (call_ref()) {   // under the shared lock: a release cannot free the copy between the check and the count
        plat_mutex_lock_shared(&copy_lock);
        copy_prefix *pf = copy_find(handle);
        if (!pf) {
            plat_mutex_unlock_shared(&copy_lock);
            rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                     (unsigned long long)handle);
        }
        used_at_max(pf, __atomic_load_n(&nsubmits, __ATOMIC_RELAXED));
        bytes = pf->bytes;
        const int shallow = pf->depth < CALL_REF_DEPTH;
        const int ref = pf->deferred == 1 && bytes > 16 && o->recording && shallow;
        if (ref) {
            __atomic_add_fetch(&pf->refs, 1, __ATOMIC_RELAXED);
            plat_mutex_unlock_shared(&copy_lock);
            ((p_jump *)rec(o, OP_CALL, sizeof(p_jump)))->target = handle;
            o->hw_used += (size_t)bytes - 16 - (8 + sizeof(p_jump));   // the game sees the usage a copy would have taken
            o->last_rec = 0;
            rec_track *tr = rec_track_of(o);
            if (tr) {
                tr->flags |= (uint16_t)(COPY_CALLS | (pf->flags & COPY_WAITS));
                uint16_t d = (uint16_t)(pf->depth + 1);
                if (d > tr->depth) tr->depth = d;
                track_callee_add(tr, pf);
                track_compose_ubo(tr, pf);
            }
            if ((__atomic_add_fetch(&n_call_refs, 1, __ATOMIC_RELAXED) & 0xfffff) == 0)
                rt_log("nvn: DKB_CALL_REF: %llu calls referenced, %llu copied (deferred records), %llu copied (nested %d "
                       "deep), %llu abandoned recordings kept their counts, %u released copies still named or read",
                       (unsigned long long)n_call_refs, (unsigned long long)__atomic_load_n(&n_call_inlined, __ATOMIC_RELAXED),
                       (unsigned long long)__atomic_load_n(&n_call_deep, __ATOMIC_RELAXED), CALL_REF_DEPTH,
                       (unsigned long long)__atomic_load_n(&n_ref_leaks, __ATOMIC_RELAXED), nvn_copy_zombies());
            return;
        }
        __atomic_add_fetch(shallow ? &n_call_inlined : &n_call_deep, 1, __ATOMIC_RELAXED);
        // Copied in while the lock is held, so the callees its OP_CALL records name cannot be freed before they are
        // counted again. Records copied into a command buffer that does not record are dropped by its next
        // BeginRecording, so they take no counts.
        if (bytes > 16) {
            size_t body = (size_t)bytes - 16;
            cmd_reserve(o, body);
            memcpy(o->cur, (const uint8_t *)(uintptr_t)handle + 8, body);
            rec_track *tr = rec_track_of(o);
            if ((pf->flags & COPY_CALLS) && o->recording) {
                if (pf->flags & COPY_CALLEES) {
                    copy_prefix *const *cs = copy_callees(pf);
                    for (uint32_t i = 0; i < pf->ncallees; i++)
                        __atomic_add_fetch(&cs[i]->refs, 1, __ATOMIC_RELAXED);
                    if (tr) {
                        tr->flags |= COPY_CALLS;
                        if (pf->depth > tr->depth) tr->depth = pf->depth;
                        track_callees_append(tr, cs, pf->ncallees);
                    }
                } else {
                    calls_adjust(o->cur, o->cur + body, +1);
                    if (tr) tr->valid = 0, tr = NULL;
                }
            }
            if (tr) {
                tr->flags |= (uint16_t)(pf->flags & COPY_WAITS);
                if (pf->deferred == 2) tr->deferred = 1;
                else if (pf->deferred == 0) tr->valid = 0;
                if (tr->valid) track_compose_ubo(tr, pf);
            }
            o->cur += body;
            o->cmd_used += body;
            o->hw_used += body;
            o->last_rec = 0;
        }
        plat_mutex_unlock_shared(&copy_lock);
        return;
    }
    bytes = copy_touch(handle, __atomic_load_n(&nsubmits, __ATOMIC_RELAXED), what);
    if (bytes <= 16) return;
    size_t body = (size_t)bytes - 16;
    cmd_reserve(o, body);
    memcpy(o->cur, (const uint8_t *)(uintptr_t)handle + 8, body);
    rec_track *tr = rec_track_of(o);
    if (tr) {
        const copy_prefix *pf = (const copy_prefix *)(uintptr_t)handle - 1;
        tr->flags |= (uint16_t)(pf->flags & COPY_WAITS);
        track_compose_ubo(tr, pf);
    }
    o->cur += body;
    o->cmd_used += body;
    o->hw_used += body;
    o->last_rec = 0;
}
static _Thread_local uint32_t tl_call_refs;

static void call_commands_fast(cmd_t *o, const uint64_t *h, int n, const char *what) {
    int i = 0;
    uint32_t refs_added = 0;
    rec_track *tr = rec_track_of(o);
    while (i < n) {
        int chunk = n - i;
        if (chunk > 64) chunk = 64;
        if (o->recording) cmd_reserve(o, (size_t)chunk * 16);
        plat_mutex_lock_shared(&copy_lock);
        uint64_t sub = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
        for (int k = 0; k < chunk; k++, i++) {
            uint64_t handle = h[i];
            if (__builtin_expect(!handle, 0)) {
                plat_mutex_unlock_shared(&copy_lock);
                rt_fatal("nvn: %s with a null handle", what);
            }
            copy_prefix *pf = copy_find(handle);
            if (__builtin_expect(!pf, 0)) {
                plat_mutex_unlock_shared(&copy_lock);
                rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                         (unsigned long long)handle);
            }
            used_at_max(pf, sub);
            uint64_t bytes = pf->bytes;
            const int shallow = pf->depth < CALL_REF_DEPTH;
            if (__builtin_expect(pf->deferred == 1 && bytes > 16 && o->recording && shallow, 1)) {
                __atomic_add_fetch(&pf->refs, 1, __ATOMIC_RELAXED);
                if (__builtin_expect(o->cur + 16 + OP_RESERVE > o->end, 0)) cmd_reserve(o, 16);
                _Static_assert(sizeof(p_jump) == 8, "the 16-byte OP_CALL written below is an 8-byte header and a p_jump");
                uint8_t *dst = o->cur;
                put_hdr(dst, OP_CALL, 16);
                memcpy(dst + 8, &handle, 8);
                o->cur = dst + 16;
                o->cmd_used += 16;
                o->hw_used += (size_t)bytes - 16;
                o->last_rec = 0;
                if (tr) {
                    tr->flags |= (uint16_t)(COPY_CALLS | (pf->flags & COPY_WAITS));
                    uint16_t d = (uint16_t)(pf->depth + 1);
                    if (d > tr->depth) tr->depth = d;
                    track_callee_add(tr, pf);
                    track_compose_ubo(tr, pf);
                }
                refs_added++;
            } else {
                __atomic_add_fetch(shallow ? &n_call_inlined : &n_call_deep, 1, __ATOMIC_RELAXED);
                if (bytes > 16) {
                    size_t body = (size_t)bytes - 16;
                    cmd_reserve(o, body);
                    memcpy(o->cur, (const uint8_t *)(uintptr_t)handle + 8, body);
                    if ((pf->flags & COPY_CALLS) && o->recording) {
                        if (pf->flags & COPY_CALLEES) {
                            copy_prefix *const *cs = copy_callees(pf);
                            for (uint32_t j = 0; j < pf->ncallees; j++)
                                __atomic_add_fetch(&cs[j]->refs, 1, __ATOMIC_RELAXED);
                            if (tr) {
                                tr->flags |= COPY_CALLS;
                                if (pf->depth > tr->depth) tr->depth = pf->depth;
                                track_callees_append(tr, cs, pf->ncallees);
                            }
                        } else {
                            calls_adjust(o->cur, o->cur + body, +1);
                            if (tr) tr->valid = 0, tr = NULL;
                        }
                    }
                    if (tr) {
                        tr->flags |= (uint16_t)(pf->flags & COPY_WAITS);
                        if (pf->deferred == 2) tr->deferred = 1;
                        else if (pf->deferred == 0) tr->valid = 0, tr = NULL;
                        if (tr) track_compose_ubo(tr, pf);
                    }
                    o->cur += body;
                    o->cmd_used += body;
                    o->hw_used += body;
                    o->last_rec = 0;
                }
            }
        }
        plat_mutex_unlock_shared(&copy_lock);
    }
    if (refs_added) {
        tl_call_refs += refs_added;
        if (__builtin_expect(tl_call_refs >= 4096, 0)) {
            uint32_t d = tl_call_refs;
            tl_call_refs = 0;
            uint64_t cur = __atomic_add_fetch(&n_call_refs, d, __ATOMIC_RELAXED);
            if (((cur - d) >> 20) != (cur >> 20))
                rt_log("nvn: DKB_CALL_REF: %llu calls referenced, %llu copied (deferred records), %llu copied (nested %d "
                       "deep), %llu abandoned recordings kept their counts, %u released copies still named or read",
                       (unsigned long long)cur, (unsigned long long)__atomic_load_n(&n_call_inlined, __ATOMIC_RELAXED),
                       (unsigned long long)__atomic_load_n(&n_call_deep, __ATOMIC_RELAXED), CALL_REF_DEPTH,
                       (unsigned long long)__atomic_load_n(&n_ref_leaks, __ATOMIC_RELAXED), nvn_copy_zombies());
        }
    }
}

// void nvnCommandBuffer{Call,Copy}Commands(NVNcommandBuffer *cmd, int n, const NVNcommandHandle *handles)
NVN_API(nvnCommandBufferCallCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    if (n > 0 && cmd_rec_fast() && call_ref()) {
        call_commands_fast(o, h, n, "CallCommands");
        return;
    }
    for (int i = 0; i < n; i++) inline_recording(o, h[i], "CallCommands");
}
NVN_API(nvnCommandBufferCopyCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    if (n > 0 && cmd_rec_fast() && call_ref()) {
        call_commands_fast(o, h, n, "CopyCommands");
        return;
    }
    for (int i = 0; i < n; i++) inline_recording(o, h[i], "CopyCommands");
}

// ---------- CommandBuffer: commands with CPU-visible results (performed by the back end in GPU order)
// void nvnCommandBufferSignalEvent(NVNcommandBuffer *cmd, const NVNevent *event, NVNeventSignalMode mode,
// NVNeventSignalLocation location, int flags, uint32_t value)
NVN_API(nvnCommandBufferSignalEvent) {
    const ev_t *e = arg_p(c, 1);
    *REC(cmd_of(c), OP_SIGNAL_EVENT, p_signal_event) = (p_signal_event){e->val, (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 5)};
}
// void nvnCommandBufferResetCounter(NVNcommandBuffer *cmd, NVNcounterType type)
NVN_API(nvnCommandBufferResetCounter) { REC(cmd_of(c), OP_RESET_COUNTER, p_reset_counter)->type = (uint32_t)arg_x(c, 1); }
// void nvnCommandBufferReportCounter(NVNcommandBuffer *cmd, NVNcounterType type, NVNbufferAddress address)
NVN_API(nvnCommandBufferReportCounter) {
    *REC(cmd_of(c), OP_REPORT_COUNTER, p_report_counter) = (p_report_counter){(uint32_t)arg_x(c, 1), arg_x(c, 2)};
}
// void nvnCommandBufferCopyBufferToBuffer(NVNcommandBuffer *cmd, NVNbufferAddress src, NVNbufferAddress dst,
// size_t size, int flags)
NVN_API(nvnCommandBufferCopyBufferToBuffer) {
    *REC(cmd_of(c), OP_COPY_B2B, p_copy_b2b) = (p_copy_b2b){arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
}
// void nvnCommandBufferUpdateUniformBuffer(NVNcommandBuffer *cmd, NVNbufferAddress buffer, size_t buffer_size,
// ptrdiff_t offset, size_t size, const void *data): the data is copied at record time (the caller may change it
// afterwards) and written into the buffer at replay
NVN_API(nvnCommandBufferUpdateUniformBuffer) {
    uint64_t dst = arg_x(c, 1) + arg_x(c, 3), n = arg_x(c, 4);
    const void *data = arg_p(c, 5);
    if (n > (16u << 20)) rt_fatal("nvn: UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);
    p_update_ub *r = rec(cmd_of(c), OP_UPDATE_UB, offsetof(p_update_ub, data) + n);
    r->dst = dst;
    r->size = n;
    memcpy(r->data, data, n);
}

// ---------- CommandBuffer: render state, payloads of nvn_obj.h
// void nvnCommandBufferSetRenderTargets(NVNcommandBuffer *cmd, int n, const NVNtexture *const *colors,
// const NVNtextureView *const *color_views, const NVNtexture *depth, const NVNtextureView *depth_view)
NVN_API(nvnCommandBufferSetRenderTargets) {
    int n = (int)arg_x(c, 1);
    const NVNtexture *const *ct = arg_p(c, 2);
    const NVNtextureView *const *cv = arg_p(c, 3);
    const NVNtexture *dt = arg_p(c, 4);
    const NVNtextureView *dv = arg_p(c, 5);
    if (n < 0 || n > 8) {
        rt_log("nvn: SetRenderTargets with %d colour targets", n);
        n = n < 0 ? 0 : 8;
    }
    p_set_rt *r = REC(cmd_of(c), OP_SET_RT, p_set_rt);
    r->n = n;
    for (int i = 0; i < n; i++) {
        r->color[i] = ct ? tex_pin(ct[i]) : NULL;
        copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL);
    }
    r->depth = tex_pin(dt);
    copy_view(&r->dview, &r->has_dview, dv);
}
// void nvnCommandBufferClearColor(NVNcommandBuffer *cmd, int index, const float *rgba, int mask)
NVN_API(nvnCommandBufferClearColor) {
    p_clear_color *r = REC(cmd_of(c), OP_CLEAR_COLOR, p_clear_color);
    r->index = (int)arg_x(c, 1);
    r->mask = (int)arg_x(c, 3);
    memcpy(r->rgba, arg_p(c, 2), 16);
}
// void nvnCommandBufferClearDepthStencil(NVNcommandBuffer *cmd, float depth, NVNboolean depth_mask, int stencil,
// int stencil_mask)
NVN_API(nvnCommandBufferClearDepthStencil) {
    *REC(cmd_of(c), OP_CLEAR_DS, p_clear_ds) = (p_clear_ds){arg_s(c, 0), nvn_arg_bool(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3)};
}
static void set_viewports(cmd_t *o, int first, int n, const float *v) {
    if (first < 0 || n < 0 || first + n > 16) {
        rt_log("nvn: SetViewports first %d n %d", first, n);
        return;
    }
    p_viewports *r = REC(o, OP_VIEWPORTS, p_viewports);
    r->first = first;
    r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
// void nvnCommandBufferSetViewports(NVNcommandBuffer *cmd, int first, int n, const float *xywh)
NVN_API(nvnCommandBufferSetViewports) { set_viewports(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), arg_p(c, 3)); }
// void nvnCommandBufferSetViewport(NVNcommandBuffer *cmd, int x, int y, int w, int h)
NVN_API(nvnCommandBufferSetViewport) {
    float v[4] = {(float)(int)arg_x(c, 1), (float)(int)arg_x(c, 2), (float)(int)arg_x(c, 3), (float)(int)arg_x(c, 4)};
    set_viewports(cmd_of(c), 0, 1, v);
}
static void set_scissor(cmd_t *o, int x, int y, int w, int h) { *REC(o, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
// void nvnCommandBufferSetScissor(NVNcommandBuffer *cmd, int x, int y, int w, int h)
NVN_API(nvnCommandBufferSetScissor) {
    set_scissor(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4));
}
// void nvnCommandBufferSetScissors(NVNcommandBuffer *cmd, int first, int n, const int *xywh): the replay keeps one
// scissor (index 0); other indices matter only to shaders that select a viewport index, so differing ones are reported
NVN_API(nvnCommandBufferSetScissors) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const int *r = arg_p(c, 3);
    if (first < 0 || n < 0 || first + n > 16) rt_fatal("nvn: SetScissors first %d n %d", first, n);
    for (int i = 1; i < n; i++)
        if (memcmp(r + 4 * i, r, 16)) {
            static int once;
            if (!once++) rt_log("nvn: SetScissors: differing scissor %d ignored (single-scissor replay)", first + i);
        }
    if (first == 0 && n > 0) set_scissor(cmd_of(c), r[0], r[1], r[2], r[3]);
}
static void set_depth_range(cmd_t *o, float n, float f) { *REC(o, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
// void nvnCommandBufferSetDepthRange(NVNcommandBuffer *cmd, float near, float far)
NVN_API(nvnCommandBufferSetDepthRange) { set_depth_range(cmd_of(c), arg_s(c, 0), arg_s(c, 1)); }
// void nvnCommandBufferSetDepthRanges(NVNcommandBuffer *cmd, int first, int n, const float *ranges)
NVN_API(nvnCommandBufferSetDepthRanges) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const float *r = arg_p(c, 3);
    if (first < 0 || n < 0 || first + n > 16) rt_fatal("nvn: SetDepthRanges first %d n %d", first, n);
    for (int i = 1; i < n; i++)
        if (memcmp(r + 2 * i, r, 8)) {
            static int once;
            if (!once++) rt_log("nvn: SetDepthRanges: differing range %d ignored (single-range replay)", first + i);
        }
    if (first == 0 && n > 0) set_depth_range(cmd_of(c), r[0], r[1]);
}
// void nvnCommandBufferSetDepthClamp / SetRenderEnable / SetRasterizerDiscard(NVNcommandBuffer *cmd, NVNboolean enable)
NVN_API(nvnCommandBufferSetDepthClamp) { REC(cmd_of(c), OP_DEPTH_CLAMP, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnCommandBufferSetRenderEnable) { REC(cmd_of(c), OP_RENDER_ENABLE, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
// Rasterizer discard runs vertex work without rasterising; with no transform feedback that equals not drawing.
NVN_API(nvnCommandBufferSetRasterizerDiscard) { REC(cmd_of(c), OP_RASTER_DISCARD, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
// void nvnCommandBufferSetPrimitiveRestart(NVNcommandBuffer *cmd, NVNboolean enable, int index): TOTK draws its
// terrain and cliff strips with 16-bit indices and restart at 0xffff; the replay applies it per indexed draw (op_draw_idx)
NVN_API(nvnCommandBufferSetPrimitiveRestart) {
    *REC(cmd_of(c), OP_PRIM_RESTART, p_prim_restart) = (p_prim_restart){nvn_arg_bool(c, 1) != 0, (uint32_t)arg_x(c, 2)};
}
// void nvnCommandBufferSetRenderEnableConditional(NVNcommandBuffer *cmd, NVNconditionalRenderMode mode,
// NVNbufferAddress counters): rendering is enabled when the two NVNcounterData.counter values at addr / addr+16
// compare equal (mode 0) or not equal (mode 1). TOTK gates most of its ClearColor commands with it; the replay
// evaluates it in GPU order.
NVN_API(nvnCommandBufferSetRenderEnableConditional) {
    int m = (int)arg_x(c, 1);
    if (m != 0 && m != 1) rt_fatal("nvn: SetRenderEnableConditional: unknown mode %d", m);
    p_render_cond *r = REC(cmd_of(c), OP_RENDER_COND, p_render_cond);
    r->mode = m;
    r->addr = arg_x(c, 2);
}
// void nvnCommandBufferBindProgram(NVNcommandBuffer *cmd, const NVNprogram *program, int stages)
NVN_API(nvnCommandBufferBindProgram) {
    *REC(cmd_of(c), OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){arg_p(c, 1), (int)arg_x(c, 2)};
}
// void nvnCommandBufferBind{Blend,Color,ChannelMask,Multisample,Polygon,DepthStencil}State(NVNcommandBuffer *cmd,
// const NVN*State *state): the state object's bytes are copied (p_bind_state.kind order)
static void bind_state(const Cpu *c, int kind, size_t n) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    r->kind = kind;
    memcpy(r->bytes, arg_p(c, 1), n);
}
NVN_API(nvnCommandBufferBindBlendState) { bind_state(c, 0, sizeof(blend_st)); }
NVN_API(nvnCommandBufferBindColorState) { bind_state(c, 1, sizeof(color_st)); }
NVN_API(nvnCommandBufferBindChannelMaskState) { bind_state(c, 2, sizeof(chmask_st)); }
NVN_API(nvnCommandBufferBindMultisampleState) { bind_state(c, 3, sizeof(ms_st)); }
NVN_API(nvnCommandBufferBindPolygonState) { bind_state(c, 4, sizeof(poly_st)); }
NVN_API(nvnCommandBufferBindDepthStencilState) { bind_state(c, 5, sizeof(ds_st)); }
// void nvnCommandBufferSetPointSize(NVNcommandBuffer *cmd, float size): recorded as bind-state kind 6 with the float as payload
NVN_API(nvnCommandBufferSetPointSize) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    float s = arg_s(c, 0);
    r->kind = 6;
    memcpy(r->bytes, &s, sizeof s);
}
// void nvnCommandBufferSetAlphaRef(NVNcommandBuffer *cmd, float ref): recorded as bind-state kind 7 (spirv_alpha_test)
NVN_API(nvnCommandBufferSetAlphaRef) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    float s = arg_s(c, 0);
    r->kind = 7;
    memcpy(r->bytes, &s, sizeof s);
}
// void nvnCommandBufferBindVertexAttribState(NVNcommandBuffer *cmd, int n, const NVNvertexAttribState *states)
NVN_API(nvnCommandBufferBindVertexAttribState) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) { rt_log("nvn: BindVertexAttribState n %d", n); return; }
    p_bind_vattr *r = RECN(o, OP_BIND_VATTR, p_bind_vattr, st, n);
    r->first = 0;
    r->n = n;
    memcpy(r->st, arg_p(c, 2), 4 * (size_t)n);
    hw_fix(o, 68);   // the driver always writes all 16 attribute formats: 17 words
}
// void nvnCommandBufferBindVertexStreamState(NVNcommandBuffer *cmd, int n, const NVNvertexStreamState *states)
NVN_API(nvnCommandBufferBindVertexStreamState) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) { rt_log("nvn: BindVertexStreamState n %d", n); return; }
    p_bind_vstream *r = RECN(o, OP_BIND_VSTREAM, p_bind_vstream, st, n);
    r->first = 0;
    r->n = n;
    memcpy(r->st, arg_p(c, 2), 8 * (size_t)n);
    hw_fix(o, 4 * (16 + (size_t)n));
}
static void bind_vertex_buffers(cmd_t *o, int first, int n, const NVNbufferRange *rg) {
    if (first < 0 || n < 0 || first + n > 16) { rt_log("nvn: BindVertexBuffers first %d n %d", first, n); return; }
    p_bind_vbuf *r = RECN(o, OP_BIND_VBUF, p_bind_vbuf, r, n);
    r->first = first;
    r->n = n;
    memcpy(r->r, rg, sizeof *rg * (size_t)n);
    hw_fix(o, 24 * (size_t)n);
}
// void nvnCommandBufferBindVertexBuffers(NVNcommandBuffer *cmd, int first, int n, const NVNbufferRange *ranges)
NVN_API(nvnCommandBufferBindVertexBuffers) { bind_vertex_buffers(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), arg_p(c, 3)); }
// void nvnCommandBufferBindVertexBuffer(NVNcommandBuffer *cmd, int index, NVNbufferAddress address, size_t size)
NVN_API(nvnCommandBufferBindVertexBuffer) {
    NVNbufferRange r = {arg_x(c, 2), arg_x(c, 3)};
    bind_vertex_buffers(cmd_of(c), (int)arg_x(c, 1), 1, &r);
}
// Uniform and storage buffers: one record per binding (CB_SIZE/ADDRESS 4 words + CB_BIND 1 on hardware).
static void bind_buffers(const Cpu *c, uint32_t op) {   // (cmd, stage, first, n, const NVNbufferRange *ranges)
    cmd_t *o = cmd_of(c);
    int st = (int)arg_x(c, 1), first = (int)arg_x(c, 2), n = (int)arg_x(c, 3);
    const NVNbufferRange *rg = arg_p(c, 4);
    for (int i = 0; i < n; i++) {
        *REC(o, op, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
        hw_fix(o, 20);
        if (op == OP_BIND_UBO) {
            rec_track *tr = rec_track_of(o);
            if (tr) track_ubo_bind(tr, st, first + i, rg[i].address, rg[i].size);
        }
    }
}
static void bind_buffer(const Cpu *c, uint32_t op) {    // (cmd, stage, index, NVNbufferAddress address, size_t size)
    cmd_t *o = cmd_of(c);
    int st = (int)arg_x(c, 1), idx = (int)arg_x(c, 2);
    uint64_t addr = arg_x(c, 3), sz = arg_x(c, 4);
    *REC(o, op, p_bind_buf) = (p_bind_buf){st, idx, addr, sz};
    hw_fix(o, 20);
    if (op == OP_BIND_UBO) {
        rec_track *tr = rec_track_of(o);
        if (tr) track_ubo_bind(tr, st, idx, addr, sz);
    }
}
NVN_API(nvnCommandBufferBindUniformBuffer) { bind_buffer(c, OP_BIND_UBO); }
NVN_API(nvnCommandBufferBindUniformBuffers) { bind_buffers(c, OP_BIND_UBO); }
NVN_API(nvnCommandBufferBindStorageBuffer) { bind_buffer(c, OP_BIND_SSBO); }
NVN_API(nvnCommandBufferBindStorageBuffers) { bind_buffers(c, OP_BIND_SSBO); }
// void nvnCommandBufferPush{Graphics,Compute}Constants(NVNcommandBuffer *, int table, int offset, int bytes,
// const void *data), NVN 2. The driver (sdk+0x5d34b0 graphics, sdk+0x5d3a30 compute) clamps bytes to 0x100, drops
// calls under 4 bytes and writes words: graphics as SET_ROOT_TABLE_SELECTOR (method 0x504, table & 7 | offset << 8)
// plus LOAD_ROOT_TABLE (0x508), compute through MME macro 0x53. Graphics shaders read table 0 as constant bank 24:
// the full-screen blits get their texel size and size this way (1/w, 1/h, w, h at 0x0..0xc in Bananza's runs). Where
// compute shaders read their pushes is not known yet; no Bananza compute stage reads bank 24.
// The Deferred entry points (sdk+0x5d3670 graphics, sdk+0x5d3df0 compute) have the same five arguments, but store the
// direct data pointer and the low byte of offset without reading data. The submit interpreter reloads that pointer.
// Keep the host's 256-byte table bound for both pipelines; the deferred compute helper itself does not clamp bytes.
static void push_constants(const Cpu *c, int compute, int deferred) {
    cmd_t *o = cmd_of(c);
    int table = (int)(arg_x(c, 1) & 7), offset = deferred ? (uint8_t)arg_x(c, 2) : (int)arg_x(c, 2);
    uint32_t bytes = (uint32_t)arg_x(c, 3);
    if (bytes > 0x100) bytes = 0x100;
    if (bytes < 4) return;
    if (offset < 0 || offset + bytes > 0x100 || (offset & 3)) {
        rt_log("nvn: Push%sConstants offset %#x bytes %#x past the 256-byte root table", compute ? "Compute" : "Graphics",
               offset, bytes);
        return;
    }
    uint32_t n = bytes / 4;
    p_push_const *r = RECN(o, OP_PUSH_CONST, p_push_const, w, n);   // holds only n words of w: set fields one by one
    r->compute = compute, r->table = table, r->offset = offset, r->bytes = (int32_t)(n * 4);
    r->src = deferred ? arg_x(c, 4) : 0;
    if (!deferred) memcpy(r->w, arg_p(c, 4), n * 4);
    else if (r->src) {
        rec_track *tr = rec_track_of(o);
        if (tr) tr->deferred = 1;
    }
    hw_fix(o, 4 * ((size_t)n + 2));   // method header, selector or macro parameter, n words
}
NVN_API(nvnCommandBufferPushGraphicsConstants) { push_constants(c, 0, 0); }
NVN_API(nvnCommandBufferPushComputeConstants) { push_constants(c, 1, 0); }
NVN_API(nvnCommandBufferPushGraphicsConstantsDeferred) { push_constants(c, 0, 1); }
NVN_API(nvnCommandBufferPushComputeConstantsDeferred) { push_constants(c, 1, 1); }
// Texture, image and separate handle binds. A deferred bind (NVN 55 *Deferred entry points, the fallback of the inline
// fast paths) keeps the pointer to the game's handle array: the original driver (sdk+0x46cc00 BindTexturesDeferred)
// reads the handles when the command buffer is submitted, and the game fills its material handle tables after
// recording and before the submit (nvn_submit_prepare). hw_bytes: the hardware size of the plain variant.
static void bind_handles(cmd_t *o, uint32_t op, int st, int first, int n, const uint64_t *h, int deferred, size_t hw_bytes) {
    if (n < 0 || n > 32) { rt_log("nvn: bind of %d handles", n); return; }
    p_bind_handles *r = RECN(o, op, p_bind_handles, h, n);
    r->stage = st;
    r->first = first;
    r->n = n;
    r->src = deferred ? (uint64_t)(uintptr_t)h : 0;
    memcpy(r->h, h, 8 * (size_t)n);
    if (r->src) {
        rec_track *tr = rec_track_of(o);
        if (tr) tr->deferred = 1;
    }
    hw_fix(o, hw_bytes ? hw_bytes : 16 + 4 * (size_t)n);   // macro 7 + method 0x1a2c + CB_POS + one word per binding
}
static void bind_handle_array(const Cpu *c, uint32_t op, int deferred, int wide) {   // (cmd, stage, first, n, handles)
    int n = (int)arg_x(c, 3);
    bind_handles(cmd_of(c), op, (int)arg_x(c, 1), (int)arg_x(c, 2), n, arg_p(c, 4), deferred,
                 wide && n >= 0 && n <= 32 ? 16 + 8 * (size_t)n : 0);
}
static void bind_one_handle(const Cpu *c, uint32_t op, size_t hw_bytes) {   // (cmd, stage, index, handle)
    uint64_t v = arg_x(c, 3);
    bind_handles(cmd_of(c), op, (int)arg_x(c, 1), (int)arg_x(c, 2), 1, &v, 0, hw_bytes);
}
// Combined texture handles share the texture slots with separate textures (sampler id in bits 20 and up). Images
// (writable textures): handle = texture pool index; the game does not call RegisterImage, images and textures share
// their registration.
NVN_API(nvnCommandBufferBindTexture) { bind_one_handle(c, OP_BIND_TEX, 0); }
NVN_API(nvnCommandBufferBindTextures) { bind_handle_array(c, OP_BIND_TEX, 0, 0); }
NVN_API(nvnCommandBufferBindImage) { bind_one_handle(c, OP_BIND_IMG, 24); }
NVN_API(nvnCommandBufferBindImages) { bind_handle_array(c, OP_BIND_IMG, 0, 1); }
NVN_API(nvnCommandBufferBindSeparateTexture) { bind_one_handle(c, OP_BIND_TEX, 24); }
NVN_API(nvnCommandBufferBindSeparateTextures) { bind_handle_array(c, OP_BIND_TEX, 0, 1); }
NVN_API(nvnCommandBufferBindSeparateSampler) { bind_one_handle(c, OP_BIND_SAMP, 24); }
NVN_API(nvnCommandBufferBindSeparateSamplers) { bind_handle_array(c, OP_BIND_SAMP, 0, 1); }
// void nvnCommandBufferBind{Textures,Images,SeparateTextures,SeparateSamplers}Deferred(NVNcommandBuffer *cmd,
// NVNshaderStage stage, int first, int n, const T *handles) (NVN 55, no prototype in nvn.h)
NVN_API(nvnCommandBufferBindTexturesDeferred) { bind_handle_array(c, OP_BIND_TEX, 1, 0); }
NVN_API(nvnCommandBufferBindImagesDeferred) { bind_handle_array(c, OP_BIND_IMG, 1, 1); }
NVN_API(nvnCommandBufferBindSeparateTexturesDeferred) { bind_handle_array(c, OP_BIND_TEX, 1, 1); }
NVN_API(nvnCommandBufferBindSeparateSamplersDeferred) { bind_handle_array(c, OP_BIND_SAMP, 1, 1); }
// void nvnCommandBufferBind{Uniform,Storage}BuffersDeferred(NVNcommandBuffer *cmd, NVNshaderStage stage, int first,
// int n, const NVNbufferRange *ranges): buffer ranges still bind at record time
NVN_API(nvnCommandBufferBindUniformBuffersDeferred) {
    static int once;
    if (!once++) NVN_LOG("BindUniformBuffersDeferred: buffer ranges bind at record time");
    bind_buffers(c, OP_BIND_UBO);
}
NVN_API(nvnCommandBufferBindStorageBuffersDeferred) {
    static int once;
    if (!once++) NVN_LOG("BindStorageBuffersDeferred: buffer ranges bind at record time");
    bind_buffers(c, OP_BIND_SSBO);
}
// void nvnCommandBufferSetStencil{ValueMask,Mask,Ref}(NVNcommandBuffer *cmd, NVNface face, int value)
NVN_API(nvnCommandBufferSetStencilValueMask) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), (int)arg_x(c, 2), 0, 0, 0};
}
NVN_API(nvnCommandBufferSetStencilMask) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), 0, (int)arg_x(c, 2), 0, 1};
}
NVN_API(nvnCommandBufferSetStencilRef) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), 0, 0, (int)arg_x(c, 2), 2};
}
// void nvnCommandBufferSetStencilCullCriteria(NVNcommandBuffer *, NVNstencilFunc, int ref, int mask): a hint for the
// hardware's stencil culling; the stencil test itself still decides, so the Vulkan replay needs nothing (Bananza)
NVN_API(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
// void nvnCommandBufferSetPolygonOffsetClamp(NVNcommandBuffer *cmd, float factor, float units, float clamp)
NVN_API(nvnCommandBufferSetPolygonOffsetClamp) {
    *REC(cmd_of(c), OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){arg_s(c, 0), arg_s(c, 1), arg_s(c, 2)};
}
// void nvnCommandBufferDrawArrays(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, int first, int count)
NVN_API(nvnCommandBufferDrawArrays) {
    *REC(cmd_of(c), OP_DRAW, p_draw) = (p_draw){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), 0, 1};
}
// void nvnCommandBufferDrawArraysInstanced(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, int first, int count,
// int base_instance, int instances): zero instances draw nothing
NVN_API(nvnCommandBufferDrawArraysInstanced) {
    int inst = (int)arg_x(c, 5);
    if (inst <= 0) return;
    *REC(cmd_of(c), OP_DRAW, p_draw) = (p_draw){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4), inst};
}
// void nvnCommandBufferDrawElements(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, NVNindexType type, int count,
// NVNbufferAddress indices)
NVN_API(nvnCommandBufferDrawElements) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), 0, 0, 1, arg_x(c, 4)};
    hw_fix(o, 48);
}
// void nvnCommandBufferDrawElementsBaseVertex(..., NVNbufferAddress indices, int base_vertex)
NVN_API(nvnCommandBufferDrawElementsBaseVertex) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) =
        (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 5), 0, 1, arg_x(c, 4)};
    hw_fix(o, 44);
}
// void nvnCommandBufferDrawElementsInstanced(..., NVNbufferAddress indices, int base_vertex, int base_instance,
// int instances): assumed to use the 6-parameter draw macro of the base-vertex form (unverified)
NVN_API(nvnCommandBufferDrawElementsInstanced) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3),
                                                    (int)arg_x(c, 5), (int)arg_x(c, 6), (int)arg_x(c, 7), arg_x(c, 4)};
    hw_fix(o, 44);
}
// void nvnCommandBufferDrawElementsIndirect(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, NVNindexType type,
// NVNbufferAddress indices, NVNbufferAddress indirect): the arguments at indirect are often GPU-written, so the replay
// draws with vkCmdDrawIndexedIndirect from the pool buffer (nxnative hle_nvn.c, SavageWindRecomp cmdbuf.c)
NVN_API(nvnCommandBufferDrawElementsIndirect) {
    *REC(cmd_of(c), OP_DRAW_IDX_IND, p_draw_idx_ind) = (p_draw_idx_ind){(int)arg_x(c, 1), (int)arg_x(c, 2), arg_x(c, 3), arg_x(c, 4)};
}
// void nvnCommandBufferDispatchCompute(NVNcommandBuffer *cmd, int x, int y, int z): caller = the game's return address
NVN_API(nvnCommandBufferDispatchCompute) {
    *REC(cmd_of(c), OP_DISPATCH, p_dispatch) = (p_dispatch){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), c->x[30]};
}
// void nvnCommandBufferDispatchComputeIndirect(NVNcommandBuffer *cmd, NVNbufferAddress indirect): vkCmdDispatchIndirect
NVN_API(nvnCommandBufferDispatchComputeIndirect) {
    *REC(cmd_of(c), OP_DISPATCH_IND, p_dispatch_ind) = (p_dispatch_ind){arg_x(c, 1), c->x[30]};
}
// void nvnCommandBufferClearBuffer(NVNcommandBuffer *cmd, NVNbufferAddress addr, size_t size, uint32_t value)
NVN_API(nvnCommandBufferClearBuffer) {
    *REC(cmd_of(c), OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){arg_x(c, 1), arg_x(c, 2), (uint32_t)arg_x(c, 3), 0};
}
// void nvnCommandBufferReportValue(NVNcommandBuffer *cmd, uint32_t value, NVNbufferAddress addr): the value lands when
// the GPU reaches this point. Recorded as a 4-byte ClearBuffer, so it stays in GPU order with the ClearBuffer resets of
// the same counter (main+0x7b9274 modes 1-3) and works in CPU-invisible pools; the CPU sees it after the fence, as a
// counter report.
NVN_API(nvnCommandBufferReportValue) {
    *REC(cmd_of(c), OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){arg_x(c, 2), 4, (uint32_t)arg_x(c, 1), 0};
}
// Colour reduction is a Maxwell bandwidth hint for blending into compressed targets: no effect on the result.
// void nvnCommandBufferSetColorReductionEnable(cmd, NVNboolean enable), SetColorReductionThresholds(cmd, class, const int *)
NVN_API(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN_API(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
// void nvnCommandBufferBarrier(NVNcommandBuffer *cmd, int bits)
NVN_API(nvnCommandBufferBarrier) { REC(cmd_of(c), OP_BARRIER, p_barrier)->bits = (int)arg_x(c, 1); }
// void nvnCommandBufferSetTexturePool / SetSamplerPool(NVNcommandBuffer *cmd, const NVNtexturePool / NVNsamplerPool *)
NVN_API(nvnCommandBufferSetTexturePool) {
    dpool_ref pool;
    if (!nvn_dpool_snapshot(arg_p(c, 1), 1u << 20, &pool)) rt_fatal("nvn: invalid texture descriptor pool");
    REC(cmd_of(c), OP_SET_TEX_POOL, p_set_pool)->pool = pool;
}
NVN_API(nvnCommandBufferSetSamplerPool) {
    dpool_ref pool;
    if (!nvn_dpool_snapshot(arg_p(c, 1), 1u << 12, &pool)) rt_fatal("nvn: invalid sampler descriptor pool");
    REC(cmd_of(c), OP_SET_SAMP_POOL, p_set_pool)->pool = pool;
}
// void nvnCommandBufferCopyBufferToTexture(NVNcommandBuffer *cmd, NVNbufferAddress src, const NVNtexture *dst,
// const NVNtextureView *view, const NVNcopyRegion *region, int flags)
NVN_API(nvnCommandBufferCopyBufferToTexture) {
    p_copy_b2t *o = REC(cmd_of(c), OP_COPY_B2T, p_copy_b2t);
    o->src = arg_x(c, 1);
    o->tex = tex_pin(arg_p(c, 2));
    copy_view(&o->view, &o->has_view, arg_p(c, 3));
    o->r = *(const NVNcopyRegion *)arg_p(c, 4);
    o->flags = (int)arg_x(c, 5);
}
// void nvnCommandBufferCopyTextureToBuffer(NVNcommandBuffer *cmd, const NVNtexture *src, const NVNtextureView *view,
// const NVNcopyRegion *region, NVNbufferAddress dst, int flags): p_copy_b2t with src = the destination address
NVN_API(nvnCommandBufferCopyTextureToBuffer) {
    p_copy_b2t *o = REC(cmd_of(c), OP_COPY_T2B, p_copy_b2t);
    o->src = arg_x(c, 4);
    o->tex = tex_pin(arg_p(c, 1));
    copy_view(&o->view, &o->has_view, arg_p(c, 2));
    o->r = *(const NVNcopyRegion *)arg_p(c, 3);
    o->flags = (int)arg_x(c, 5);
}
// void nvnCommandBufferClearTexture{,ui}(NVNcommandBuffer *cmd, const NVNtexture *tex, const NVNtextureView *view,
// const NVNcopyRegion *region, const float / uint32_t *rgba, int mask)
static void clear_texture(const Cpu *c, int is_uint) {
    p_clear_tex *o = REC(cmd_of(c), OP_CLEAR_TEX, p_clear_tex);
    o->tex = tex_pin(arg_p(c, 1));
    copy_view(&o->view, &o->has_view, arg_p(c, 2));
    o->r = *(const NVNcopyRegion *)arg_p(c, 3);
    o->mask = (int)arg_x(c, 5);
    o->is_uint = (uint8_t)is_uint;
    memcpy(o->c.u, arg_p(c, 4), 16);
}
NVN_API(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN_API(nvnCommandBufferClearTextureui) { clear_texture(c, 1); }
// void nvnCommandBufferCopyTextureToTexture(NVNcommandBuffer *cmd, const NVNtexture *src, const NVNtextureView *src_view,
// const NVNcopyRegion *src_region, const NVNtexture *dst, const NVNtextureView *dst_view,
// const NVNcopyRegion *dst_region, int flags)
NVN_API(nvnCommandBufferCopyTextureToTexture) {
    p_copy_t2t *o = REC(cmd_of(c), OP_COPY_T2T, p_copy_t2t);
    o->src = tex_pin(arg_p(c, 1));
    o->dst = tex_pin(arg_p(c, 4));
    copy_view(&o->sv, &o->has_sv, arg_p(c, 2));
    copy_view(&o->dv, &o->has_dv, arg_p(c, 5));
    o->sr = *(const NVNcopyRegion *)arg_p(c, 3);
    o->dr = *(const NVNcopyRegion *)arg_p(c, 6);
    o->flags = (int)arg_x(c, 7);
}
// void nvnCommandBufferDownsample(NVNcommandBuffer *cmd, const NVNtexture *src, const NVNtexture *dst): level 0 of src
// into level 0 of dst, whole extents, filtered. Recorded as a linear-filter texture copy: copy_t2t resolves a
// multisample source into an equal-size target and blits (2x2 average at half size) between sizes. As nxnative
// hle_nvn.c and SavageWindRecomp cmdbuf.c record it; docs/maxwell.md 1.19.
NVN_API(nvnCommandBufferDownsample) {
    p_copy_t2t *o = REC(cmd_of(c), OP_COPY_T2T, p_copy_t2t);
    o->src = tex_pin(arg_p(c, 1));
    o->dst = tex_pin(arg_p(c, 2));
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
}
// void nvnCommandBufferSetCopyRowStride(NVNcommandBuffer *cmd, ptrdiff_t stride)
NVN_API(nvnCommandBufferSetCopyRowStride) { REC(cmd_of(c), OP_COPY_STRIDE, p_copy_stride)->stride = (int64_t)arg_x(c, 1); }
// void nvnCommandBufferSetBlendColor(NVNcommandBuffer *cmd, const float *rgba)
NVN_API(nvnCommandBufferSetBlendColor) { memcpy(REC(cmd_of(c), OP_BLEND_COLOR, p_blend_color)->rgba, arg_p(c, 1), 16); }

// void nvnCommandBufferWaitSync(NVNcommandBuffer *cmd, const NVNsync *sync): the SDK retains the pointer at record
// time and reads the sync's current producer at submit. Preparation fulfils the wait before consumer CPU snapshots.
NVN_API(nvnCommandBufferWaitSync) {
    cmd_t *o = cmd_of(c);
    REC(o, OP_WAIT_SYNC, p_wait_sync)->sync = arg_p(c, 1);
    rec_track *tr = rec_track_of(o);
    if (tr) tr->flags |= COPY_WAITS;
    hw_fix(o, 0);   // the SDK records a CPU control entry here; GPU wait commands are emitted at submit
}
// Commands without an effect on this replay (hle_nvn.c g_nops). WaitEvent still relies on in-order replay; shader
// scratch memory and the tessellation patch size have no host counterpart the replay needs.
// void nvnCommandBufferWaitEvent(cmd, const NVNevent *event,
// NVNeventWaitMode mode, uint32_t value), SetShaderScratchMemory(cmd, const NVNmemoryPool *pool, ptrdiff_t offset,
// size_t size), SetPatchSize(cmd, int size)
NVN_API(nvnCommandBufferWaitEvent) { (void)c; }
NVN_API(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN_API(nvnCommandBufferSetPatchSize) { (void)c; }

// ---------- submit
// What the console reads at the submit call is read here, on the game thread: every handle must be a live recording,
// and deferred binds and pushes take the data the game's arrays hold now (the game fills its tables for the next frame
// as soon as the submit returns). The result is written into the recording copy.
static uint64_t def_reads, def_changed, n_clones;

// Only QueueFenceSync and window acquire currently produce syncs: both name work outside this submission that is
// already queued (or complete). Recorded FenceSync and imported/external producers remain unimplemented.
// Waiting for their maximum before the whole batch is conservative: it orders CPU completion callbacks as well
// as GPU work before *any* deferred read or backend snapshot, including those before a wait in another handle.
static uint64_t wait_target(const uint8_t *p, int depth) {
    uint64_t target = 0;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_)
            rt_fatal("nvn: corrupt recording at %p while preparing WaitSync (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_END) return target;
        if (h.op == OP_JUMP) {
            if (h.size < 16) rt_fatal("nvn: short WaitSync jump record at %p", (const void *)p);
            memcpy(&p, p + 8, 8);
            continue;
        }
        uint64_t value = 0;
        if (h.op == OP_CALL) {
            if (h.size < 16) rt_fatal("nvn: short WaitSync call record at %p", (const void *)p);
            uint64_t called;
            memcpy(&called, p + 8, 8);
            if (depth >= 16) rt_fatal("nvn: WaitSync preparation: call depth over 16");
            if (((const copy_prefix *)(uintptr_t)called - 1)->flags & COPY_WAITS)
                value = wait_target((const uint8_t *)(uintptr_t)called, depth + 1);
        } else if (h.op == OP_WAIT_SYNC) {
            if (h.size < 8 + sizeof(p_wait_sync)) rt_fatal("nvn: short WaitSync record at %p", (const void *)p);
            const sync_t *source = ((const p_wait_sync *)(const void *)(p + 8))->sync;
            if (!source) rt_fatal("nvn: CommandBufferWaitSync with a null sync");
            sync_t snapshot;
            memcpy(&snapshot, source, sizeof snapshot);   // one value-at-submit read; never reread while waiting
            if (snapshot.magic != MAGIC('S', 'Y', 'N', 'C') ||
                (snapshot.fenced != 0 && snapshot.fenced != 1 && snapshot.fenced != 3))
                rt_fatal("nvn: CommandBufferWaitSync with an invalid sync state");
            if (snapshot.fenced) value = snapshot.fence;   // an unfenced SDK sync emits no GPU wait
        }
        if (value > target) target = value;
        p += h.size;
    }
}

static void wait_submission(const uint64_t *handles, int n) {
    uint64_t target = 0;
    for (int i = 0; i < n; i++) {
        if (!(((const copy_prefix *)(uintptr_t)handles[i] - 1)->flags & COPY_WAITS)) continue;   // no WaitSync in it
        uint64_t value = wait_target((const uint8_t *)(uintptr_t)handles[i], 0);
        if (value > target) target = value;
    }
    if (!target) return;
    const GfxBackend *b = gfx_backend();
    // No copy/backend lock is held here. QueueSubmitCommands has given its virtual core up. The async front
    // submit lock does not stop an already-enqueued producer: replay/completion never acquire that lock.
    // fence_done/fence_wait include completion callbacks, unlike the raw Vulkan timeline.
    if (b->fence_done() < target && !b->fence_wait(target, UINT64_MAX))
        rt_fatal("nvn: CommandBufferWaitSync could not reach producer fence %llu", (unsigned long long)target);
}

static int resolve_deferred(const uint8_t *p, int depth) {   // returns the number of deferred binds and pushes
    int nd = 0;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_)
            rt_fatal("nvn: corrupt recording at %p while resolving deferred binds (op %u size %u)", (const void *)p,
                     h.op, h.size);
        if (h.op == OP_END) return nd;
        if (h.op == OP_JUMP) {
            memcpy(&p, p + 8, 8);
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            if (depth >= 16) rt_fatal("nvn: deferred bind resolution: call depth over 16");
            if (((const copy_prefix *)(uintptr_t)t - 1)->deferred != 1)   // a referenced copy holds none (inline_recording)
                nd += resolve_deferred((const uint8_t *)(uintptr_t)t, depth + 1);
        } else if (h.op == OP_PUSH_CONST) {
            if (h.size < 8 + offsetof(p_push_const, w)) rt_fatal("nvn: short push-constant record at %p", (const void *)p);
            p_push_const *b = (p_push_const *)(uintptr_t)(p + 8);
            if (b->src) {
                if (b->bytes <= 0 || b->bytes > (int)sizeof b->w || (b->bytes & 3) ||
                    h.size < 8 + offsetof(p_push_const, w) + (size_t)b->bytes)
                    rt_fatal("nvn: invalid deferred push-constant record at %p", (const void *)p);
                nd++;
                memcpy(b->w, (const void *)(uintptr_t)b->src, (size_t)b->bytes);
            }
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                nd++;
                uint64_t cur[32];
                memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
                __atomic_fetch_add(&def_reads, 1, __ATOMIC_RELAXED);
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    if (__atomic_fetch_add(&def_changed, 1, __ATOMIC_RELAXED) < 6)
                        NVN_LOG("deferred bind read new handles at submit: stage %d slot %d, %#llx at record -> %#llx",
                                b->stage, b->first, (unsigned long long)b->h[0], (unsigned long long)cur[0]);
                    memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
        p += h.size;
    }
}

static int resolve_deferred_list(uint8_t *base, const def_off_list *dl) {
    int nd = 0;
    for (uint32_t i = 0; i < dl->n; i++) {
        uint8_t *p = base + dl->offs[i];
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.op == OP_PUSH_CONST) {
            if (h.size < 8 + offsetof(p_push_const, w)) rt_fatal("nvn: short push-constant record at %p", (const void *)p);
            p_push_const *b = (p_push_const *)(uintptr_t)(p + 8);
            if (b->src) {
                if (b->bytes <= 0 || b->bytes > (int)sizeof b->w || (b->bytes & 3) ||
                    h.size < 8 + offsetof(p_push_const, w) + (size_t)b->bytes)
                    rt_fatal("nvn: invalid deferred push-constant record at %p", (const void *)p);
                nd++;
                memcpy(b->w, (const void *)(uintptr_t)b->src, (size_t)b->bytes);
            }
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                nd++;
                uint64_t cur[32];
                memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
                __atomic_fetch_add(&def_reads, 1, __ATOMIC_RELAXED);
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    if (__atomic_fetch_add(&def_changed, 1, __ATOMIC_RELAXED) < 6)
                        NVN_LOG("deferred bind read new handles at submit: stage %d slot %d, %#llx at record -> %#llx",
                                b->stage, b->first, (unsigned long long)b->h[0], (unsigned long long)cur[0]);
                    memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
    }
    return nd;
}

void nvn_submit_prepare(const uint64_t *handles, int n) {
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    nvn_mode_trace(NVN_MODE_PREPARE, 0, 0, (uint32_t)n, now);
    const int fast = cmd_rec_fast();
    if (fast && n > 0) {
        plat_mutex_lock_shared(&copy_lock);
        for (int i = 0; i < n; i++) {
            uint64_t handle = handles[i];
            copy_prefix *pf = copy_find(handle);
            if (__builtin_expect(!pf, 0)) {
                plat_mutex_unlock_shared(&copy_lock);
                rt_fatal("nvn: QueueSubmitCommands: command handle %#llx is not a live recording (released or never recorded)",
                         (unsigned long long)handle);
            }
            used_at_max(pf, now);
        }
        plat_mutex_unlock_shared(&copy_lock);
        for (int i = 0; i < n; i++) nvn_mode_trace(NVN_MODE_HANDLE, 0, handles[i], (uint32_t)i, now);
    } else {
        for (int i = 0; i < n; i++) {
            uint64_t handle = handles[i];
            copy_touch(handle, now, "QueueSubmitCommands");
            nvn_mode_trace(NVN_MODE_HANDLE, 0, handle, (uint32_t)i, now);
        }
    }
    wait_submission(handles, n);
    for (int i = 0; i < n; i++) {   // a copy known to hold no deferred record (take_recording, DKB_CALL_REF) has none to read
        const copy_prefix *pf = (const copy_prefix *)(uintptr_t)handles[i] - 1;
        if (pf->deferred == 1) continue;
        if (fast && pf->def_list) resolve_deferred_list((uint8_t *)(uintptr_t)handles[i], pf->def_list);
        else resolve_deferred((const uint8_t *)(uintptr_t)handles[i], 0);
    }
}

// Asynchronous replay. A copy a queued submission still reads keeps that submission's deferred binds: when it holds
// deferred binds (or is not known to hold none), this submission gets a clone, resolved here and freed after its replay.
void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n) {
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    nvn_mode_trace(NVN_MODE_PREPARE, 0, 0, (uint32_t)n, now);
    uint64_t replayed = gfx_replayed_submits();
    const int fast = cmd_rec_fast();
    uint32_t npf = 0;
    plat_mutex_lock(&copy_lock);
    if (nzombies) {
        if (fast) zombie_sweep_defer(replayed, &npf);
        else zombie_sweep(replayed);
    }
    for (int i = 0; i < n; i++) {
        uint64_t handle = handles[i];
        copy_prefix *pf = copy_find(handle);
        if (!pf) {
            plat_mutex_unlock(&copy_lock);
            if (npf) flush_deferred_frees(npf, 0);
            rt_fatal("nvn: QueueSubmitCommands: command handle %#llx is not a live recording (released or never recorded)",
                     (unsigned long long)handle);
        }
        nvn_mode_trace(NVN_MODE_HANDLE, 0, handle, (uint32_t)i, now);
        out[i] = handle;
        if (pf->used_at > replayed && pf->used_at != now && pf->deferred != 1) {
            size_t sz = sizeof *pf + pf->bytes;
            copy_prefix *cl = copy_block(sz);
            uint64_t block = cl->block;
            memcpy(cl, pf, sz);
            cl->block = block;
            cl->magic = 0;   // not a live handle: the game never sees it
            cl->refs = 0;
            if (cl->flags & COPY_CALLEES) {
                if (cl->ncallees > 4) {
                    copy_prefix **v = malloc(cl->ncallees * sizeof *v);
                    if (!v) rt_fatal("nvn: out of memory (clone callees)");
                    memcpy(v, pf->u.callees, cl->ncallees * sizeof *v);
                    cl->u.callees = v;
                }
                if (cl->flags & COPY_CALLS) {
                    copy_prefix *const *cs = copy_callees(cl);
                    for (uint32_t j = 0; j < cl->ncallees; j++)
                        __atomic_add_fetch(&cs[j]->refs, 1, __ATOMIC_RELAXED);
                }
            } else if (cl->flags & COPY_CALLS) {
                calls_adjust((const uint8_t *)(cl + 1), (const uint8_t *)(cl + 1) + cl->bytes, +1);
            }
            cl->ubo = NULL;   // the list stays the original's: the replay walks the clone's records
            cl->def_list = NULL;
            cl->used_at = now;
            zombie_add(cl);
            out[i] = (uintptr_t)(cl + 1);
            __atomic_fetch_add(&n_clones, 1, __ATOMIC_RELAXED);
        } else {
            used_at_max(pf, now);
        }
    }
    plat_mutex_unlock(&copy_lock);
    if (npf) flush_deferred_frees(npf, 0);
    wait_submission(out, n);
    for (int i = 0; i < n; i++) {
        copy_prefix *pf = (copy_prefix *)(uintptr_t)out[i] - 1;
        if (pf->deferred == 1) continue;   // known to hold no deferred record: nothing to read
        int nd = (fast && pf->def_list) ? resolve_deferred_list((uint8_t *)(uintptr_t)out[i], pf->def_list)
                                        : resolve_deferred((const uint8_t *)(uintptr_t)out[i], 0);
        if (out[i] == handles[i]) pf->deferred = nd ? 2 : 1;
    }
    if (now % 7200 == 0)
        rt_log("nvn: asynchronous replay at submit %llu: %llu recording clones so far, %u copies waiting for the replay",
               (unsigned long long)now, (unsigned long long)__atomic_load_n(&n_clones, __ATOMIC_RELAXED), nzombies);
}

uint64_t nvn_submit_count(void) { return __atomic_load_n(&nsubmits, __ATOMIC_RELAXED); }

const char *nvn_op_name(uint32_t op) {
    static const char *const names[] = {
        "END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB", "SET_RT",
        "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR",
        "BIND_VSTREAM", "BIND_VBUF", "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP", "STENCIL", "POLY_OFFSET", "DRAW",
        "DRAW_IDX", "DISPATCH", "BARRIER", "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR",
        "COPY_T2B", "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE",
        "RASTER_DISCARD", "RENDER_COND", "PRIM_RESTART", "DRAW_IDX_IND", "DISPATCH_IND", "CLEAR_BUF", "PUSH_CONST", "WAIT_SYNC",
    };
    _Static_assert(sizeof names / sizeof *names == OP_COUNT_, "one name per op");
    return op < OP_COUNT_ ? names[op] : "?";
}
