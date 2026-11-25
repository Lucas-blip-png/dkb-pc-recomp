// Multithreaded stress and correctness test for DKB_COPY_SHARDS in runtime/nvn_api_cmd.c.
// Exercises concurrent EndRecording, CallCommands, CopyCommands, AddControlMemory (empty and populated),
// nvn_submit_prepare, nvn_submit_prepare_async, nested OP_CALL reference trees, and zombie sweeping across
// worker threads under both DKB_COPY_SHARDS=1 and DKB_COPY_SHARDS=0.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "cpu.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

unsigned rt_trace;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}
void rt_call_guest(Cpu *c, uint64_t addr) { (void)c, (void)addr; }
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) { (void)c, (void)fn, (void)n; return 0; }
static GfxBackend g_dummy_backend;
const GfxBackend *gfx_backend(void) { return &g_dummy_backend; }
int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def) {
    if (!strcmp(name, "COPY_SHARDS")) {
        const char *e = getenv("DKB_COPY_SHARDS");
        if (e) return strcmp(e, "0") != 0;
    }
    return !def || strcmp(def, "0") != 0;
}
int cfg_bool(const char *name) {
    if (!strcmp(name, "DKB_CALL_REF")) return 1;
    rt_fatal("unexpected cfg_bool(%s)", name);
    return 0;
}
int cfg_int(const char *name) { (void)name; return 0; }
void nvn_mode_trace(uint32_t k, uint64_t a, uint64_t b, uint32_t c, uint64_t d) {
    (void)k, (void)a, (void)b, (void)c, (void)d;
}
void nvn_called(uint8_t *seen, const char *name) { (void)seen, (void)name; }
#define NVN_PROC(n) void nvnp_##n(Cpu *c);
#include "nvn_procs.inc"
#undef NVN_PROC
void nvn_obj_drop(const void *obj) { (void)obj; }
void nvn_obj_track(const void *obj, size_t size, const char *kind) { (void)obj, (void)size, (void)kind; }
void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) { (void)p, (void)off, (void)size; }
uint64_t nvn_pool_gpu(const pool_t *p) { return p ? (uintptr_t)p->mem : 0; }

static uint64_t g_replayed;
uint64_t gfx_replayed_submits(void) { return __atomic_load_n(&g_replayed, __ATOMIC_ACQUIRE); }
int gfx_fence_wait(uint64_t v, uint64_t ns) { (void)v, (void)ns; return 1; }
int gfx_active_is_vulkan(void) { return 0; }
int vk_mem_gpu_writable(uint64_t a, uint64_t s) { (void)a, (void)s; return 0; }

#ifdef _WIN32
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }
void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)m); }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }
typedef HANDLE ThreadH;
typedef DWORD (WINAPI *ThreadFn)(LPVOID);
static ThreadH start_thread(ThreadFn fn, void *arg) { return CreateThread(NULL, 0, fn, arg, 0, NULL); }
static void join_thread(ThreadH h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
#else
static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t *rw_of(PlatMutex *m) {
    pthread_rwlock_t *rw = __atomic_load_n(&m->p, __ATOMIC_ACQUIRE);
    if (!rw) {
        pthread_mutex_lock(&g_init_mu);
        rw = __atomic_load_n(&m->p, __ATOMIC_RELAXED);
        if (!rw) {
            rw = calloc(1, sizeof *rw);
            pthread_rwlock_init(rw, NULL);
            __atomic_store_n(&m->p, rw, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_init_mu);
    }
    return rw;
}
void plat_mutex_lock(PlatMutex *m) { pthread_rwlock_wrlock(rw_of(m)); }
void plat_mutex_unlock(PlatMutex *m) { pthread_rwlock_unlock(rw_of(m)); }
void plat_mutex_lock_shared(PlatMutex *m) { pthread_rwlock_rdlock(rw_of(m)); }
void plat_mutex_unlock_shared(PlatMutex *m) { pthread_rwlock_unlock(rw_of(m)); }
typedef pthread_t ThreadH;
typedef void *(*ThreadFn)(void *);
static ThreadH start_thread(ThreadFn fn, void *arg) {
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
}
static void join_thread(ThreadH h) { pthread_join(h, NULL); }
#endif

static int checks, failures;
#define CHECK(cond, msg) do {                                                        \
    __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED);                                \
    if (!(cond)) {                                                                   \
        __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);                          \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);                    \
    }                                                                                \
} while (0)

#define U(p) ((uint64_t)(uintptr_t)(p))
static uint64_t call6(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    Cpu cpu = {0};
    cpu.x[0] = a0; cpu.x[1] = a1; cpu.x[2] = a2; cpu.x[3] = a3; cpu.x[4] = a4; cpu.x[5] = a5;
    fn(&cpu);
    return cpu.x[0];
}
#define CALL(fn, ...) call6(nvnp_##fn, __VA_ARGS__)

static uint32_t g_push_src[8] = {1, 2, 3, 4, 5, 6, 7, 8};

enum { NTHREADS = 6, SHARED_LEAVES = 32, ROUNDS = 80 };
static uint64_t g_shared[SHARED_LEAVES];
static uint8_t *g_shared_ctl;
static NVNcommandBuffer g_shared_cmd;
static uint8_t g_cmd_mem_buf[1u << 20];
static pool_t g_cmd_pool = {.mem = g_cmd_mem_buf, .size = sizeof g_cmd_mem_buf};

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID arg)
#else
static void *worker_main(void *arg)
#endif
{
    int tid = (int)(uintptr_t)arg;
    NVNcommandBuffer cmd_leaf = {0}, cmd_parent = {0}, cmd_root = {0};
    size_t ctl_sz = 256u << 10;
    uint8_t *ctl_leaf = malloc(ctl_sz);
    uint8_t *ctl_parent = malloc(ctl_sz);
    uint8_t *ctl_root = malloc(ctl_sz);
    uint8_t *ctl_empty = malloc(2u << 20);
    if (!ctl_leaf || !ctl_parent || !ctl_root || !ctl_empty) rt_fatal("oom");

    CALL(nvnCommandBufferInitialize, U(&cmd_leaf), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferInitialize, U(&cmd_parent), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferInitialize, U(&cmd_root), 0, 0, 0, 0, 0);

    for (int r = 0; r < ROUNDS; r++) {
        // Release an empty 2 MiB control range to exercise empty PageShard fast skip
        CALL(nvnCommandBufferAddControlMemory, U(&cmd_leaf), U(ctl_empty), 2u << 20, 0, 0, 0);
        // Reset leaf control memory (retires previous round's leaves)
        CALL(nvnCommandBufferAddControlMemory, U(&cmd_leaf), U(ctl_leaf), ctl_sz, 0, 0, 0);
        CALL(nvnCommandBufferAddCommandMemory, U(&cmd_leaf), U(&g_cmd_pool), 0, sizeof g_cmd_mem_buf, 0, 0);

        uint64_t local_leaves[8];
        for (int k = 0; k < 8; k++) {
            CALL(nvnCommandBufferBeginRecording, U(&cmd_leaf), 0, 0, 0, 0, 0);
            CALL(nvnCommandBufferBarrier, U(&cmd_leaf), 1, 0, 0, 0, 0);
            if ((k & 3) == 3)
                CALL(nvnCommandBufferPushGraphicsConstantsDeferred, U(&cmd_leaf), 0, 0, 4, U(&g_push_src[tid & 7]), 0);
            local_leaves[k] = CALL(nvnCommandBufferEndRecording, U(&cmd_leaf), 0, 0, 0, 0, 0);
            CHECK(nvn_handle_live(local_leaves[k]), "fresh leaf handle is live");
            CHECK(nvn_handle_bytes(local_leaves[k]) > 16, "fresh leaf has non-empty body");
        }

        // Parent calls both shared handles (across threads) and local leaves (single & batched)
        CALL(nvnCommandBufferAddControlMemory, U(&cmd_parent), U(ctl_parent), ctl_sz, 0, 0, 0);
        CALL(nvnCommandBufferAddCommandMemory, U(&cmd_parent), U(&g_cmd_pool), 0, sizeof g_cmd_mem_buf, 0, 0);
        CALL(nvnCommandBufferBeginRecording, U(&cmd_parent), 0, 0, 0, 0, 0);
        CALL(nvnCommandBufferCallCommands, U(&cmd_parent), 8, U(local_leaves), 0, 0, 0);
        for (int s = 0; s < 4; s++) {
            uint64_t sh = g_shared[(tid * 5 + r + s) % SHARED_LEAVES];
            CALL(nvnCommandBufferCallCommands, U(&cmd_parent), 1, U(&sh), 0, 0, 0);
        }
        uint64_t parent_h = CALL(nvnCommandBufferEndRecording, U(&cmd_parent), 0, 0, 0, 0, 0);
        CHECK(nvn_handle_live(parent_h), "parent handle is live");

        // Root copies parent and calls shared batch
        CALL(nvnCommandBufferAddControlMemory, U(&cmd_root), U(ctl_root), ctl_sz, 0, 0, 0);
        CALL(nvnCommandBufferAddCommandMemory, U(&cmd_root), U(&g_cmd_pool), 0, sizeof g_cmd_mem_buf, 0, 0);
        CALL(nvnCommandBufferBeginRecording, U(&cmd_root), 0, 0, 0, 0, 0);
        CALL(nvnCommandBufferCopyCommands, U(&cmd_root), 1, U(&parent_h), 0, 0, 0);
        CALL(nvnCommandBufferCallCommands, U(&cmd_root), 4, U(&g_shared[(r * 3) % (SHARED_LEAVES - 4)]), 0, 0, 0);
        uint64_t root_h = CALL(nvnCommandBufferEndRecording, U(&cmd_root), 0, 0, 0, 0, 0);
        CHECK(nvn_handle_live(root_h), "root handle is live");

        // Prepare both sync and async submits
        if ((r & 1) == 0) {
            nvn_submit_prepare(&root_h, 1);
        } else {
            uint64_t out_h = 0;
            nvn_submit_prepare_async(&root_h, &out_h, 1);
            CHECK(out_h != 0, "async prepare returned valid handle");
        }
        if ((r & 7) == 7) __atomic_store_n(&g_replayed, nvn_submit_count(), __ATOMIC_RELEASE);
    }

    // Final release of worker's control memory
    CALL(nvnCommandBufferAddControlMemory, U(&cmd_root), U(ctl_root), ctl_sz, 0, 0, 0);
    CALL(nvnCommandBufferAddControlMemory, U(&cmd_parent), U(ctl_parent), ctl_sz, 0, 0, 0);
    CALL(nvnCommandBufferAddControlMemory, U(&cmd_leaf), U(ctl_leaf), ctl_sz, 0, 0, 0);
    CALL(nvnCommandBufferFinalize, U(&cmd_root), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferFinalize, U(&cmd_parent), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferFinalize, U(&cmd_leaf), 0, 0, 0, 0, 0);

    free(ctl_leaf); free(ctl_parent); free(ctl_root); free(ctl_empty);
    return 0;
}

static void test_sharded_track_and_clone(void) {
    // Regression for #539 + #540: >4 OP_CALL callees via call_commands_sharded, depth/ref tracking,
    // async cloning of a deferred >4-callee parent (duplicating heap u.callees without double-free),
    // and cascading zombie sweep via copy_calls_adjust + zombie_dirty.
    NVNcommandBuffer leaf_cb = {0}, par_cb = {0};
    size_t ctl_sz = 64u << 10;
    uint8_t *ctl_leaf = malloc(ctl_sz), *ctl_par = malloc(ctl_sz);
    if (!ctl_leaf || !ctl_par) rt_fatal("oom");
    CALL(nvnCommandBufferInitialize, U(&leaf_cb), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferInitialize, U(&par_cb), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferAddControlMemory, U(&leaf_cb), U(ctl_leaf), ctl_sz, 0, 0, 0);
    CALL(nvnCommandBufferAddCommandMemory, U(&leaf_cb), U(&g_cmd_pool), 0, sizeof g_cmd_mem_buf, 0, 0);
    CALL(nvnCommandBufferAddControlMemory, U(&par_cb), U(ctl_par), ctl_sz, 0, 0, 0);
    CALL(nvnCommandBufferAddCommandMemory, U(&par_cb), U(&g_cmd_pool), 0, sizeof g_cmd_mem_buf, 0, 0);

    uint64_t leaves[8];
    for (int i = 0; i < 8; i++) {
        CALL(nvnCommandBufferBeginRecording, U(&leaf_cb), 0, 0, 0, 0, 0);
        CALL(nvnCommandBufferBarrier, U(&leaf_cb), 1, 0, 0, 0, 0);
        leaves[i] = CALL(nvnCommandBufferEndRecording, U(&leaf_cb), 0, 0, 0, 0, 0);
    }

    // Parent calls 8 leaves (>4 -> heap u.callees) and has a deferred push constant so async resubmit clones it.
    CALL(nvnCommandBufferBeginRecording, U(&par_cb), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferCallCommands, U(&par_cb), 8, U(leaves), 0, 0, 0);
    CALL(nvnCommandBufferPushGraphicsConstantsDeferred, U(&par_cb), 0, 0, 4, U(&g_push_src[0]), 0);
    uint64_t parent = CALL(nvnCommandBufferEndRecording, U(&par_cb), 0, 0, 0, 0, 0);
    CHECK(nvn_copy_depth(parent) == 1, "sharded >4-callee parent tracks depth == 1");
    for (int i = 0; i < 8; i++) CHECK(nvn_copy_refs(leaves[i]) == 1, "sharded CallCommands increments leaf ref");

    // Submit once, then async-submit again while still in-flight to trigger copy_clone with heap u.callees.
    nvn_submit_prepare(&parent, 1);
    uint64_t out = 0;
    nvn_submit_prepare_async(&parent, &out, 1);
    CHECK(out != 0 && out != parent, "async resubmit clones in-flight deferred >4-callee parent");
    for (int i = 0; i < 8; i++) CHECK(nvn_copy_refs(leaves[i]) == 2, "async clone increments >4-callee refs");

    // Retire leaves first (stay zombies while parent + clone hold refs), then retire parent + advance replay.
    CALL(nvnCommandBufferAddControlMemory, U(&leaf_cb), U(ctl_leaf), ctl_sz, 0, 0, 0);
    CHECK(nvn_copy_zombies() >= 8, "retired referenced leaves stay zombies");
    __atomic_store_n(&g_replayed, nvn_submit_count(), __ATOMIC_RELEASE);
    CALL(nvnCommandBufferAddControlMemory, U(&par_cb), U(ctl_par), ctl_sz, 0, 0, 0);
    // Single follow-up sweep must drain both clone and all 8 leaves without waiting for replayed to advance.
    CALL(nvnCommandBufferAddControlMemory, U(&leaf_cb), U(ctl_leaf), ctl_sz, 0, 0, 0);
    CHECK(nvn_copy_zombies() == 0, "cascading sweep drains clone and >4 callees via zombie_dirty");

    CALL(nvnCommandBufferFinalize, U(&par_cb), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferFinalize, U(&leaf_cb), 0, 0, 0, 0, 0);
    free(ctl_par);
    free(ctl_leaf);
}

int main(void) {
    size_t sctl_sz = 128u << 10;
    g_shared_ctl = malloc(sctl_sz);
    if (!g_shared_ctl) rt_fatal("oom");
    CALL(nvnCommandBufferInitialize, U(&g_shared_cmd), 0, 0, 0, 0, 0);
    CALL(nvnCommandBufferAddControlMemory, U(&g_shared_cmd), U(g_shared_ctl), sctl_sz, 0, 0, 0);
    CALL(nvnCommandBufferAddCommandMemory, U(&g_shared_cmd), U(&g_cmd_pool), 0, sizeof g_cmd_mem_buf, 0, 0);
    for (int i = 0; i < SHARED_LEAVES; i++) {
        CALL(nvnCommandBufferBeginRecording, U(&g_shared_cmd), 0, 0, 0, 0, 0);
        CALL(nvnCommandBufferBarrier, U(&g_shared_cmd), 1, 0, 0, 0, 0);
        CALL(nvnCommandBufferBarrier, U(&g_shared_cmd), 2, 0, 0, 0, 0);
        g_shared[i] = CALL(nvnCommandBufferEndRecording, U(&g_shared_cmd), 0, 0, 0, 0, 0);
        CHECK(nvn_handle_live(g_shared[i]), "shared leaf is live");
    }

    ThreadH th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) th[i] = start_thread(worker_main, (void *)(uintptr_t)i);
    for (int i = 0; i < NTHREADS; i++) join_thread(th[i]);

    // Advance replayed past all submissions and sweep remaining zombies
    __atomic_store_n(&g_replayed, nvn_submit_count(), __ATOMIC_RELEASE);
    CALL(nvnCommandBufferAddControlMemory, U(&g_shared_cmd), U(g_shared_ctl), sctl_sz, 0, 0, 0);
    // One more sweep in case nested zombies cleared in the call above
    CALL(nvnCommandBufferAddControlMemory, U(&g_shared_cmd), U(g_shared_ctl), sctl_sz, 0, 0, 0);
    for (int i = 0; i < SHARED_LEAVES; i++) {
        CHECK(!nvn_handle_live(g_shared[i]), "shared leaf retired after control memory reuse");
    }
    CHECK(nvn_copy_zombies() == 0, "all zombies swept once submissions replayed and roots released");
    CALL(nvnCommandBufferFinalize, U(&g_shared_cmd), 0, 0, 0, 0, 0);
    free(g_shared_ctl);

    test_sharded_track_and_clone();

    printf("copy_shards: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
