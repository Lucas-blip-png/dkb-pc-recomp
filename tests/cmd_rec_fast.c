// Unit and concurrency tests for DKB_CMD_REC_FAST in runtime/nvn_api_cmd.c:
// - Batched CallCommands/CopyCommands (>64 handles, mixed OP_CALL refs, deferred inline copies, CALL_REF_DEPTH)
// - Fast take_recording (0/1/2/many UBOs, single-callee bulk UBO list, consecutive callee dedup, thread-local seen
//   table across many recordings, offset-indexed deferred push constants and deferred texture/sampler/image binds)
// - Off-lock release_copies + nested zombie sweep cascade + copy_block_free_many + ubo_live accounting
// - Batched nvn_submit_prepare and nvn_submit_prepare_async (including async clone of deferred copies)
// - Multi-threaded stress exercising concurrent EndRecording, CallCommands, and AddControlMemory under real locks.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "nvn_api.h"
#include "platform/platform.h"

#define PROC(n) void nvnp_##n(Cpu *c)
PROC(nvnCommandBufferInitialize);
PROC(nvnCommandBufferFinalize);
PROC(nvnCommandBufferAddCommandMemory);
PROC(nvnCommandBufferAddControlMemory);
PROC(nvnCommandBufferBeginRecording);
PROC(nvnCommandBufferEndRecording);
PROC(nvnCommandBufferGetCommandMemoryUsed);
PROC(nvnCommandBufferBindUniformBuffer);
PROC(nvnCommandBufferBindTexture);
PROC(nvnCommandBufferBindTexturesDeferred);
PROC(nvnCommandBufferBindSeparateTexture);
PROC(nvnCommandBufferBindSeparateTexturesDeferred);
PROC(nvnCommandBufferBindSeparateSampler);
PROC(nvnCommandBufferBindSeparateSamplersDeferred);
PROC(nvnCommandBufferBindImage);
PROC(nvnCommandBufferBindImagesDeferred);
PROC(nvnCommandBufferBarrier);
PROC(nvnCommandBufferCallCommands);
PROC(nvnCommandBufferCopyCommands);
PROC(nvnCommandBufferPushGraphicsConstantsDeferred);
#undef PROC

#define UP(p) ((uint64_t)(uintptr_t)(p))
static uint64_t call(void (*fn)(Cpu *), const uint64_t args[8]) {
    Cpu c = {0};
    memcpy(c.x, args, sizeof c.x[0] * 8);
    fn(&c);
    return c.x[0];
}
#define CALL(n, ...) call(nvnp_##n, (const uint64_t[8]){__VA_ARGS__})

static int checks, failures;
#define CHECK(cond, text)                                                                                            \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(cond)) {                                                                                               \
            failures++;                                                                                              \
            fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, text);                                                 \
        }                                                                                                            \
    } while (0)

unsigned rt_trace;
static uint64_t g_replayed;

int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def) {
    if (!strcmp(name, "CMD_REC_FAST")) {
        const char *e = getenv("TEST_CMD_REC_FAST");
        return e ? atoi(e) != 0 : (!def || strcmp(def, "0") != 0);
    }
    return !def || strcmp(def, "0") != 0;
}

int cfg_bool(const char *name) {
    if (!strcmp(name, "DKB_CALL_REF")) return 1;
    fprintf(stderr, "unexpected cfg_bool: %s\n", name);
    abort();
}

void nvn_called(uint8_t *seen_, const char *name) { (void)seen_; (void)name; }
void nvn_mode_trace(unsigned kind, uint64_t cb, uint64_t handle, uint32_t value, uint64_t submit) {
    (void)kind; (void)cb; (void)handle; (void)value; (void)submit;
}
int hdr_trace_hit(const Cpu *c, const char *name) { (void)c; (void)name; return 0; }
void hdr_trace_dump(const char *name, const void *p) { (void)name; (void)p; }
uint64_t gfx_replayed_submits(void) { return __atomic_load_n(&g_replayed, __ATOMIC_ACQUIRE); }
const GfxBackend *gfx_backend(void) { fprintf(stderr, "unexpected gfx_backend\n"); abort(); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) { (void)c; (void)fn; (void)n; abort(); }

// Minimal UBO range builder matching vk_usnap.c nvn_ubo_list_build.
int nvn_ubo_lists_on(void) { return 1; }
static int by_lo(const void *a, const void *b) {
    uint64_t al = ((const nvn_range *)a)->lo & ~1ull, bl = ((const nvn_range *)b)->lo & ~1ull;
    return al < bl ? -1 : al > bl ? 1 : 0;
}
size_t nvn_ubo_list_build(nvn_range *r, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (r[i].hi > (r[i].lo & ~1ull)) r[k++] = r[i];
    if (k <= 1) return k;
    qsort(r, k, sizeof *r, by_lo);
    size_t m = 0;
    for (size_t i = 0; i < k; i++) {
        if (m && (r[i].lo & ~1ull) <= r[m - 1].hi) {
            r[m - 1].lo |= r[i].lo & 1ull;
            if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi;
        } else {
            r[m++] = r[i];
        }
    }
    return m;
}

#ifdef _WIN32
_Static_assert(sizeof(PlatMutex) == sizeof(SRWLOCK), "SRWLOCK fits in PlatMutex");
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }
void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)m); }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }
typedef HANDLE Thread;
#define THREAD_RET DWORD WINAPI
#define THREAD_OK 0
static Thread t_start(LPTHREAD_START_ROUTINE fn, void *arg) {
    Thread t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!t) abort();
    return t;
}
static void t_join(Thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static pthread_rwlock_t *lazy_rw(void *volatile *slot) {
    void *p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (p) return p;
    pthread_rwlock_t *fresh = malloc(sizeof *fresh);
    if (!fresh || pthread_rwlock_init(fresh, NULL)) abort();
    if (__atomic_compare_exchange_n(slot, &p, fresh, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) return fresh;
    pthread_rwlock_destroy(fresh);
    free(fresh);
    return p;
}
void plat_mutex_lock(PlatMutex *m) { pthread_rwlock_wrlock(lazy_rw(&m->p)); }
void plat_mutex_unlock(PlatMutex *m) { pthread_rwlock_unlock(m->p); }
void plat_mutex_lock_shared(PlatMutex *m) { pthread_rwlock_rdlock(lazy_rw(&m->p)); }
void plat_mutex_unlock_shared(PlatMutex *m) { pthread_rwlock_unlock(m->p); }
typedef pthread_t Thread;
#define THREAD_RET void *
#define THREAD_OK NULL
static Thread t_start(void *(*fn)(void *), void *arg) {
    Thread t;
    if (pthread_create(&t, NULL, fn, arg)) abort();
    return t;
}
static void t_join(Thread t) { pthread_join(t, NULL); }
#endif

static void init_cmd(NVNcommandBuffer *cmd, void *ctl, size_t ctl_sz) {
    CALL(nvnCommandBufferInitialize, UP(cmd), 0);
    CALL(nvnCommandBufferAddCommandMemory, UP(cmd), 0, 0, 0x100000);
    CALL(nvnCommandBufferAddControlMemory, UP(cmd), UP(ctl), ctl_sz);
}

static void test_batched_call_and_deferred(void) {
    NVNcommandBuffer cmd = {0}, sub = {0};
    _Alignas(16) uint8_t ctl_main[0x10000], ctl_sub[0x10000];
    init_cmd(&cmd, ctl_main, sizeof ctl_main);
    init_cmd(&sub, ctl_sub, sizeof ctl_sub);

    // Build 80 leaf recordings (exceeds the 64-handle chunk boundary in call_commands_fast) plus 1 deferred leaf.
    uint64_t leaves[80];
    for (int i = 0; i < 80; i++) {
        CALL(nvnCommandBufferBeginRecording, UP(&sub));
        CALL(nvnCommandBufferBindUniformBuffer, UP(&sub), 0, 1, 0x10000000ull + (uint64_t)i * 0x1000ull, 128);
        leaves[i] = CALL(nvnCommandBufferEndRecording, UP(&sub));
        const nvn_range_list *l = nvn_handle_ubo_list(leaves[i]);
        CHECK(l && l->n == 1 && l->r[0].lo == ((0x10000000ull + (uint64_t)i * 0x1000ull) | 1ull),
              "1-range leaf UBO list built inline");
    }

    uint32_t push_src[2] = {0x11223344u, 0x55667788u};
    uint64_t tex_src[2] = {0x100001ull, 0x200002ull};
    uint64_t img_src[1] = {0x300003ull};
    CALL(nvnCommandBufferBeginRecording, UP(&sub));
    CALL(nvnCommandBufferBindUniformBuffer, UP(&sub), 0, 2, 0x20000200ull, 0x100);
    CALL(nvnCommandBufferBindUniformBuffer, UP(&sub), 0, 3, 0x20000100ull, 0x100);   // touches previous -> merges to 1
    CALL(nvnCommandBufferPushGraphicsConstantsDeferred, UP(&sub), 0, 4, 8, UP(push_src));
    CALL(nvnCommandBufferBindTexturesDeferred, UP(&sub), 4, 0, 2, UP(tex_src));
    CALL(nvnCommandBufferBindImagesDeferred, UP(&sub), 4, 0, 1, UP(img_src));
    uint64_t def_leaf = CALL(nvnCommandBufferEndRecording, UP(&sub));
    const nvn_range_list *dl = nvn_handle_ubo_list(def_leaf);
    CHECK(dl && dl->n == 1 && dl->r[0].lo == 0x20000100ull && dl->r[0].hi == 0x20000300ull,
          "2-range touching UBO list merged inline");

    // Single-callee wrapper: UBO list should equal leaves[0]'s list without calling nvn_ubo_list_build.
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    CALL(nvnCommandBufferCallCommands, UP(&cmd), 1, UP(&leaves[0]));
    CALL(nvnCommandBufferCallCommands, UP(&cmd), 1, UP(&leaves[0]));   // consecutive identical callee
    uint64_t wrap = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    const nvn_range_list *wl = nvn_handle_ubo_list(wrap);
    CHECK(wl && wl->n == 1 && wl->r[0].lo == (0x10000000ull | 1ull), "single-callee wrapper keeps callee UBO list");
    CHECK(nvn_copy_refs(leaves[0]) == 2, "two OP_CALL refs counted on leaves[0]");

    // Caller batching 80 OP_CALL leaves + 1 deferred leaf (inlined) via CallCommands and CopyCommands.
    uint64_t batch[81];
    memcpy(batch, leaves, sizeof leaves);
    batch[80] = def_leaf;
    CALL(nvnCommandBufferBeginRecording, UP(&cmd));
    CALL(nvnCommandBufferCallCommands, UP(&cmd), 45, UP(batch));
    CALL(nvnCommandBufferCopyCommands, UP(&cmd), 36, UP(batch + 45));
    uint64_t parent = CALL(nvnCommandBufferEndRecording, UP(&cmd));
    CHECK(nvn_copy_refs(leaves[0]) == 3 && nvn_copy_refs(leaves[79]) == 1, "batched Call/CopyCommands count refs");
    CHECK(nvn_copy_refs(def_leaf) == 0, "deferred leaf is inlined, not referenced");
    const nvn_range_list *pl = nvn_handle_ubo_list(parent);
    CHECK(pl && pl->n == 81, "parent composed all 80 callee UBOs plus inlined deferred leaf UBO");

    // Mutate deferred sources and run synchronous nvn_submit_prepare.
    push_src[0] = 0xaabbccddu, push_src[1] = 0xdeadbeefu;
    tex_src[0] = 0x400004ull, tex_src[1] = 0x500005ull;
    img_src[0] = 0x600006ull;
    uint64_t sub_handles[2] = {wrap, parent};
    nvn_submit_prepare(sub_handles, 2);

    int found_push = 0, found_tex = 0, found_img = 0;
    for (const op_hdr *op = nvn_op_first(parent); op->op != OP_END; op = nvn_op_next(op)) {
        if (op->op == OP_PUSH_CONST) {
            const p_push_const *p = nvn_op_payload(op);
            found_push = p->w[0] == 0xaabbccddu && p->w[1] == 0xdeadbeefu;
        } else if (op->op == OP_BIND_TEX) {
            const p_bind_handles *b = nvn_op_payload(op);
            found_tex = b->n == 2 && b->h[0] == 0x400004ull && b->h[1] == 0x500005ull;
        } else if (op->op == OP_BIND_IMG) {
            const p_bind_handles *b = nvn_op_payload(op);
            found_img = b->n == 1 && b->h[0] == 0x600006ull;
        }
    }
    CHECK(found_push && found_tex && found_img, "offset-indexed resolve_deferred refreshed push, tex, and img records");

    // Async resubmit while still queued must clone parent without double-freeing def_list.
    push_src[0] = 0x13579bdfu;
    uint64_t out[2] = {0};
    nvn_submit_prepare_async(sub_handles, out, 2);
    CHECK(out[0] == wrap && out[1] != parent, "async resubmit clones pending deferred parent");
    int found_clone_push = 0;
    for (const op_hdr *op = nvn_op_first(out[1]); op->op != OP_END; op = nvn_op_next(op)) {
        if (op->op == OP_PUSH_CONST) {
            const p_push_const *p = nvn_op_payload(op);
            found_clone_push = p->w[0] == 0x13579bdfu;
        }
    }
    CHECK(found_clone_push, "async clone resolved updated deferred push constants");

    // Retire leaves first (they stay as zombies while wrap/parent/clone hold refs), then retire main + advance replay.
    uint64_t live_before = nvn_ubo_lists_live();
    CALL(nvnCommandBufferAddControlMemory, UP(&sub), UP(ctl_sub), sizeof ctl_sub);
    CHECK(nvn_copy_zombies() >= 80, "retired referenced leaves become zombies");
    __atomic_store_n(&g_replayed, nvn_submit_count(), __ATOMIC_RELEASE);
    CALL(nvnCommandBufferAddControlMemory, UP(&cmd), UP(ctl_main), sizeof ctl_main);
    // One more sweep drains leaves whose refs dropped to 0 when parent/wrap/clone freed.
    CALL(nvnCommandBufferAddControlMemory, UP(&sub), UP(ctl_sub), sizeof ctl_sub);
    CHECK(nvn_copy_zombies() == 0, "nested zombie sweep drains all retired copies and clones");
    CHECK(nvn_ubo_lists_live() < live_before, "off-lock release decrements ubo_live");

    CALL(nvnCommandBufferFinalize, UP(&cmd));
    CALL(nvnCommandBufferFinalize, UP(&sub));
}

typedef struct { int id; } WorkerArg;
static THREAD_RET stress_worker(void *arg) {
    int id = ((WorkerArg *)arg)->id;
    NVNcommandBuffer leaf_cmd = {0}, par_cmd = {0};
    uint8_t *ctl_leaf = malloc(0x10000), *ctl_par = malloc(0x10000);
    if (!ctl_leaf || !ctl_par) abort();
    init_cmd(&leaf_cmd, ctl_leaf, 0x10000);
    init_cmd(&par_cmd, ctl_par, 0x10000);
    uint64_t base = 0x40000000ull + (uint64_t)id * 0x1000000ull;

    for (int iter = 0; iter < 40; iter++) {
        uint64_t hs[32];
        for (int i = 0; i < 32; i++) {
            CALL(nvnCommandBufferBeginRecording, UP(&leaf_cmd));
            for (int u = 0; u < (i % 20) + 1; u++)
                CALL(nvnCommandBufferBindUniformBuffer, UP(&leaf_cmd), 0, (uint64_t)(u % 8),
                     base + (uint64_t)((u * 3 + i) % 16) * 0x200ull, 256);
            hs[i] = CALL(nvnCommandBufferEndRecording, UP(&leaf_cmd));
        }
        CALL(nvnCommandBufferBeginRecording, UP(&par_cmd));
        CALL(nvnCommandBufferCallCommands, UP(&par_cmd), 32, UP(hs));
        uint64_t ph = CALL(nvnCommandBufferEndRecording, UP(&par_cmd));
        const nvn_range_list *l = nvn_handle_ubo_list(ph);
        if (!l || !l->n) abort();
        // Release parent then leaves concurrently across threads.
        CALL(nvnCommandBufferAddControlMemory, UP(&par_cmd), UP(ctl_par), 0x10000);
        CALL(nvnCommandBufferAddControlMemory, UP(&leaf_cmd), UP(ctl_leaf), 0x10000);
    }
    CALL(nvnCommandBufferFinalize, UP(&par_cmd));
    CALL(nvnCommandBufferFinalize, UP(&leaf_cmd));
    free(ctl_par);
    free(ctl_leaf);
    return THREAD_OK;
}

static void test_concurrent_stress(void) {
    __atomic_store_n(&g_replayed, nvn_submit_count() + 1000, __ATOMIC_RELEASE);
    Thread ts[4];
    WorkerArg args[4];
    for (int i = 0; i < 4; i++) args[i].id = i, ts[i] = t_start(stress_worker, &args[i]);
    for (int i = 0; i < 4; i++) t_join(ts[i]);
    // Final sweep on a dummy buffer to drain any last zombies from cross-thread timing.
    NVNcommandBuffer tmp = {0};
    _Alignas(16) uint8_t ctl[0x1000];
    init_cmd(&tmp, ctl, sizeof ctl);
    CALL(nvnCommandBufferAddControlMemory, UP(&tmp), UP(ctl), sizeof ctl);
    CALL(nvnCommandBufferFinalize, UP(&tmp));
    CHECK(nvn_copy_zombies() == 0, "concurrent stress leaves zero leaked zombies");
    CHECK(nvn_ubo_lists_live() == 0, "concurrent stress leaves zero leaked heap UBO lists");
}

int main(void) {
    test_batched_call_and_deferred();
    test_concurrent_stress();
    printf("cmd_rec_fast (mode=%s): %d checks, %d failures\n",
           getenv("TEST_CMD_REC_FAST") ? getenv("TEST_CMD_REC_FAST") : "default", checks, failures);
    return failures != 0;
}
