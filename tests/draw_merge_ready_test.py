"""Unit tests for draw-merge readiness: variant pipeline-key records (runtime/vk_pipeline.c)
and c4-only cached host copies plus aligned lookups in the uniform snapshot (runtime/vk_usnap.c).

Part A extracts KeyRec, rec_fill, rec_hash, seen_add, keys_note, and prewarm_stage verbatim from
runtime/vk_pipeline.c and verifies version-3 ABI compatibility, distinct hashes for base / VS /
VS+FS instance_c4 variants, deduplication, GS exclusion, and prewarm stage resolution.

Part B includes runtime/vk_usnap.c directly with lightweight host-memory stubs (no Vulkan loader
or GPU) and verifies c4-only cpu_buf placement, per-submission toggling via vk_usnap_cpu_reads,
aligned and misaligned lookups, overlap/duplicate c4 upgrades, and drop invalidation.
"""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def extract_function(text, signature):
    i = text.index(signature)
    depth, k = 0, text.index('{', i)
    while True:
        depth += text[k] == '{'
        depth -= text[k] == '}'
        if text[k] == '}' and not depth:
            return text[i:k + 1]
        k += 1


def build_keys_source():
    source = (ROOT / 'runtime' / 'vk_pipeline.c').read_text(encoding='utf-8')
    fnv_fn = extract_function(source, 'static uint64_t fnv(')
    start = source.index('#define KEYS_VERSION 3')
    end = source.index('// Appends the pending records', start)
    keys_block = source[start:end]
    prewarm_fn = extract_function(source, 'static const GfxShader *prewarm_stage(')

    prelude = r'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "platform/platform.h"

#define FNV0 0xcbf29ce484222325ull

struct GfxShader {
    uint8_t iid[32];
    int is_var;
    const GfxShader *inst_c4;
};

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }
int gfx_shader_is_variant(const GfxShader *s) { return s && s->is_var; }
const GfxShader *gfx_shader_instance_c4(const GfxShader *s) { return s ? s->inst_c4 : NULL; }

static struct { int on; } A;
'''

    cases = r'''
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    GfxShader vs_base = {.is_var = 0}, vs_c4 = {.is_var = 1};
    GfxShader fs_base = {.is_var = 0}, fs_c4 = {.is_var = 1};
    GfxShader gs_base = {.is_var = 0}, vs_no_var = {.is_var = 0};
    memset(vs_base.iid, 0x11, 32);
    memset(vs_c4.iid, 0x11, 32);   // variant shares source's input id
    memset(fs_base.iid, 0x22, 32);
    memset(fs_c4.iid, 0x22, 32);
    memset(gs_base.iid, 0x33, 32);
    memset(vs_no_var.iid, 0x44, 32);
    vs_base.inst_c4 = &vs_c4;
    fs_base.inst_c4 = &fs_c4;
    vs_no_var.inst_c4 = NULL;

    // Version-3 binary layout compatibility: pad became flags without changing size or offsets.
    CHECK(KEYS_VERSION == 3);
    CHECK(sizeof(KeyRec) == 424);
    CHECK(offsetof(KeyRec, has_fs) == 64);
    CHECK(offsetof(KeyRec, flags) == 68);
    CHECK(sizeof(((KeyRec *)0)->state) == offsetof(VkPipeState, gs));

    VkPipeState st;
    memset(&st, 0x5a, sizeof st);
    st.vs = &vs_base;
    st.fs = &fs_base;
    st.gs = NULL;
    st.prim_restart = 1;

    KeyRec r_base, r_vs, r_both, r_fs, r_nofs;
    rec_fill(&r_base, &st);
    CHECK(r_base.has_fs == 1);
    CHECK(r_base.flags == 0);
    CHECK(memcmp(r_base.vs, vs_base.iid, 32) == 0);
    CHECK(memcmp(r_base.fs, fs_base.iid, 32) == 0);

    // prim_restart is dynamic state and must zero out identically to v3 records.
    VkPipeState st_no_restart = st;
    st_no_restart.prim_restart = 0;
    KeyRec r_base2;
    rec_fill(&r_base2, &st_no_restart);
    CHECK(memcmp(&r_base, &r_base2, sizeof r_base) == 0);

    st.vs = &vs_c4;
    st.fs = &fs_base;
    rec_fill(&r_vs, &st);
    CHECK(r_vs.has_fs == 1 && r_vs.flags == KEY_VS_INST_C4);
    CHECK(memcmp(r_vs.vs, vs_base.iid, 32) == 0);
    CHECK(memcmp(r_vs.fs, fs_base.iid, 32) == 0);

    st.vs = &vs_c4;
    st.fs = &fs_c4;
    rec_fill(&r_both, &st);
    CHECK(r_both.has_fs == 1 && r_both.flags == (KEY_VS_INST_C4 | KEY_FS_INST_C4));

    st.vs = &vs_base;
    st.fs = &fs_c4;
    rec_fill(&r_fs, &st);
    CHECK(r_fs.has_fs == 1 && r_fs.flags == KEY_FS_INST_C4);

    st.vs = &vs_c4;
    st.fs = NULL;
    rec_fill(&r_nofs, &st);
    CHECK(r_nofs.has_fs == 0 && r_nofs.flags == KEY_VS_INST_C4);
    uint8_t zero32[32] = {0};
    CHECK(memcmp(r_nofs.fs, zero32, 32) == 0);

    // Distinct hashes and bytes for base, VS-variant, VS+FS-variant, FS-variant.
    uint64_t h_base = rec_hash(&r_base);
    uint64_t h_vs = rec_hash(&r_vs);
    uint64_t h_both = rec_hash(&r_both);
    uint64_t h_fs = rec_hash(&r_fs);
    CHECK(h_base != h_vs && h_base != h_both && h_vs != h_both && h_fs != h_base && h_fs != h_vs && h_fs != h_both);
    CHECK(memcmp(&r_base, &r_vs, sizeof r_base) != 0);
    CHECK(memcmp(&r_vs, &r_both, sizeof r_vs) != 0);

    // keys_note accepts base, VS-variant, and VS+FS-variant as distinct entries, deduplicates repeats,
    // and continues skipping geometry-shader pipelines.
    A.on = 1;
    snprintf(K.path, sizeof K.path, "test-pipeline-keys.bin");
    st.vs = &vs_base; st.fs = &fs_base; st.gs = NULL;
    keys_note(&st);
    CHECK(K.pend_n == 1 && K.seen_n == 1 && K.pend[0].flags == 0);
    keys_note(&st);
    CHECK(K.pend_n == 1 && K.seen_n == 1);

    st.vs = &vs_c4; st.fs = &fs_base; st.gs = NULL;
    keys_note(&st);
    CHECK(K.pend_n == 2 && K.seen_n == 2 && K.pend[1].flags == KEY_VS_INST_C4);
    keys_note(&st);
    CHECK(K.pend_n == 2 && K.seen_n == 2);

    st.vs = &vs_c4; st.fs = &fs_c4; st.gs = NULL;
    keys_note(&st);
    CHECK(K.pend_n == 3 && K.seen_n == 3 && K.pend[2].flags == (KEY_VS_INST_C4 | KEY_FS_INST_C4));
    keys_note(&st);
    CHECK(K.pend_n == 3 && K.seen_n == 3);

    // GS pipelines must remain excluded even with variant VS/FS.
    st.vs = &vs_c4; st.fs = &fs_c4; st.gs = &gs_base;
    keys_note(&st);
    CHECK(K.pend_n == 3 && K.seen_n == 3);

    // prewarm_stage resolves base vs variant and handles NULL / unrewritable stages.
    CHECK(prewarm_stage(&vs_base, 0) == &vs_base);
    CHECK(prewarm_stage(&vs_base, KEY_VS_INST_C4) == &vs_c4);
    CHECK(prewarm_stage(&fs_base, 0) == &fs_base);
    CHECK(prewarm_stage(&fs_base, KEY_FS_INST_C4) == &fs_c4);
    CHECK(prewarm_stage(&vs_no_var, KEY_VS_INST_C4) == NULL);
    CHECK(prewarm_stage(NULL, KEY_FS_INST_C4) == NULL);
    CHECK(prewarm_stage(NULL, 0) == NULL);

    free(K.pend);
    free(K.seen);
    printf("pipeline keys variants: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
'''
    return '\n'.join([prelude, fnv_fn, keys_block, prewarm_fn, cases])


def build_usnap_source():
    return r'''#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_spec.h"
#include "platform/platform.h"

// Minimal host-only stubs for vk_usnap.c (no Vulkan loader or GPU).
static VkCtx mock_ctx;
static int mock_fast = 1;
static uint64_t mock_done_fence;
static void (*last_comp_fn)(void *);
static void *last_comp_arg;

VkCtx *vk_ctx(void) { return &mock_ctx; }
void rt_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}
int rt_knob(const char *name, const char *def) {
    if (!strcmp(name, "USNAP_FAST")) return mock_fast;
    return def && !strcmp(def, "1") ? 1 : 0;
}
int rt_ab(void) { return 1; }
uint64_t kern_now_ns(void) { return 1000; }
uint64_t perf_ph_begin(void) { return 0; }
void perf_ph_end(int ph, uint64_t t0) { (void)ph; (void)t0; }
_Thread_local int vk_spec_here;
uint64_t vk_sync_fence_done(void) { return mock_done_fence; }
int vk_mem_kept_overlaps(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    (void)fence;
    last_comp_fn = fn;
    last_comp_arg = arg;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }

// Guest memory pool backing all test UBO ranges.
static uint8_t *guest_pool;
static uint64_t guest_size = 64u << 10;

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) {
    uint64_t base = (uint64_t)(uintptr_t)guest_pool;
    if (addr < base || addr + size > base + guest_size) return 0;
    memset(out, 0, sizeof *out);
    out->pool = (void *)1;
    out->avail = base + guest_size - addr;
    out->cls = VK_MCLS_HOST_CACHED;
    return 1;
}

// Host-allocated mock VMA arena buffer.
VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo *pBufferCreateInfo,
                         const VmaAllocationCreateInfo *pAllocationCreateInfo, VkBuffer *pBuffer,
                         VmaAllocation *pAllocation, VmaAllocationInfo *pAllocationInfo) {
    (void)allocator; (void)pAllocationCreateInfo;
    uint8_t *mem = calloc(1, (size_t)pBufferCreateInfo->size);
    if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pBuffer = (VkBuffer)(uintptr_t)mem;
    *pAllocation = (VmaAllocation)(uintptr_t)mem;
    if (pAllocationInfo) {
        memset(pAllocationInfo, 0, sizeof *pAllocationInfo);
        pAllocationInfo->pMappedData = mem;
    }
    return VK_SUCCESS;
}
VkResult vmaFlushAllocation(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size) {
    (void)allocator; (void)allocation; (void)offset; (void)size;
    return VK_SUCCESS;
}
void vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) {
    (void)allocator; (void)buffer;
    free((void *)(uintptr_t)allocation);
}

#include "vk_usnap.c"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// Builds an NVN command record stream in buf[] and returns its handle (pointer to first op_hdr).
typedef struct { uint8_t data[4096]; size_t pos; } CmdStream;

static void cmd_bind_ubo(CmdStream *s, int stage, int index, uint64_t addr, uint64_t size) {
    size_t sz = (sizeof(op_hdr) + sizeof(p_bind_buf) + 7u) & ~(size_t)7u;
    op_hdr *h = (op_hdr *)(s->data + s->pos);
    h->op = OP_BIND_UBO;
    h->size = (uint16_t)sz;
    p_bind_buf *p = (p_bind_buf *)nvn_op_payload(h);
    p->stage = stage;
    p->index = index;
    p->addr = addr;
    p->size = size;
    s->pos += sz;
}

static void cmd_end(CmdStream *s) {
    size_t sz = (sizeof(op_hdr) + 7u) & ~(size_t)7u;
    op_hdr *h = (op_hdr *)(s->data + s->pos);
    h->op = OP_END;
    h->size = (uint16_t)sz;
    s->pos += sz;
}

static const Reg *find_reg(uint64_t addr) {
    for (int i = 0; i < U.nreg; i++)
        if (U.reg[i].lo <= addr && addr < U.reg[i].hi) return &U.reg[i];
    return NULL;
}

int main(void) {
    // 256-byte aligned guest pool so offsets are exact multiples of ALIGN.
    uint8_t *raw = malloc(guest_size + ALIGN);
    if (!raw) return 2;
    guest_pool = (uint8_t *)(((uintptr_t)raw + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1));
    for (uint64_t i = 0; i < guest_size; i++) guest_pool[i] = (uint8_t)((i * 37u + 13u) & 0xffu);

    uint64_t base = (uint64_t)(uintptr_t)guest_pool;
    // Distinct well-separated addresses inside guest_pool:
    uint64_t a_non_slot0 = base + 0x0400;   // VS index 0, 1024 B -> non-c4
    uint64_t a_non_big   = base + 0x1000;   // VS index 1, 512 B  -> non-c4 (>256)
    uint64_t a_non_cs    = base + 0x1800;   // CS index 1, 256 B  -> non-c4 (compute stage)
    uint64_t a_c4_vs     = base + 0x2000;   // VS index 1, 256 B  -> c4
    uint64_t a_c4_fs     = base + 0x2800;   // FS index 1, 128 B  -> c4
    uint64_t a_merge     = base + 0x3000;   // VS index 0 [0..512) + VS index 1 [256..512) -> merged c4
    uint64_t a_upgrade   = base + 0x3800;   // VS index 0 [0..256) then FS index 1 [0..256) -> upgraded c4

    CmdStream cs = {0};
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_VERTEX,   0, a_non_slot0, 1024);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_VERTEX,   1, a_non_big,   512);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_COMPUTE,  1, a_non_cs,    256);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_VERTEX,   1, a_c4_vs,     256);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_FRAGMENT, 1, a_c4_fs,     128);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_VERTEX,   0, a_merge,     512);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_VERTEX,   1, a_merge + 256, 256);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_VERTEX,   0, a_upgrade,   256);
    cmd_bind_ubo(&cs, NVN_SHADER_STAGE_FRAGMENT, 1, a_upgrade,   256);
    cmd_end(&cs);

    uint64_t handle = (uint64_t)(uintptr_t)cs.data;

    // 1. cpu_reads ON: only c4 regions land in cpu_buf; cpu_cap covers only c4 bytes.
    vk_usnap_cpu_reads(1);
    vk_usnap_begin(&handle, 1, 1);
    CHECK(U.on == 1);
    CHECK(U.nreg == 7);
    CHECK(U.cpu != NULL && U.cpu == cpu_buf);

    const Reg *r_slot0 = find_reg(a_non_slot0);
    const Reg *r_big   = find_reg(a_non_big);
    const Reg *r_cs    = find_reg(a_non_cs);
    const Reg *r_vs    = find_reg(a_c4_vs);
    const Reg *r_fs    = find_reg(a_c4_fs);
    const Reg *r_mrg   = find_reg(a_merge);
    const Reg *r_upg   = find_reg(a_upgrade);
    CHECK(r_slot0 && r_slot0->cpu_off == REG_NO_CPU);
    CHECK(r_big   && r_big->cpu_off   == REG_NO_CPU);
    CHECK(r_cs    && r_cs->cpu_off    == REG_NO_CPU);
    CHECK(r_vs    && r_vs->cpu_off    != REG_NO_CPU);
    CHECK(r_fs    && r_fs->cpu_off    != REG_NO_CPU);
    CHECK(r_mrg   && r_mrg->cpu_off   != REG_NO_CPU);
    CHECK(r_upg   && r_upg->cpu_off   != REG_NO_CPU);

    uint64_t expected_c4_bytes = 256 + 128 + 512 + 256;
    CHECK(cpu_cap == expected_c4_bytes);

    // Mutate guest memory after snapshot to prove lookups return copied bytes.
    uint8_t saved_vs[256], saved_slot0[256];
    memcpy(saved_vs, guest_pool + 0x2000, 256);
    memcpy(saved_slot0, guest_pool + 0x0400, 256);
    memset(guest_pool + 0x2000, 0xaa, 256);
    memset(guest_pool + 0x0400, 0xbb, 256);

    uint64_t hits0 = U.hits;
    const uint8_t *p_vs = vk_usnap_bytes_aligned(a_c4_vs, 256, 256);
    CHECK(p_vs != NULL);
    CHECK(p_vs >= U.cpu && p_vs + 256 <= U.cpu + cpu_cap);
    CHECK(memcmp(p_vs, saved_vs, 256) == 0);
    CHECK(U.hits == hits0 + 1);

    const uint8_t *p_fs = vk_usnap_bytes_aligned(a_c4_fs, 128, 256);
    CHECK(p_fs != NULL && p_fs >= U.cpu && p_fs + 128 <= U.cpu + cpu_cap);
    CHECK(memcmp(p_fs, guest_pool + 0x2800, 128) == 0);

    const uint8_t *p_mrg = vk_usnap_bytes_aligned(a_merge + 256, 256, 256);
    CHECK(p_mrg != NULL && p_mrg >= U.cpu && p_mrg + 256 <= U.cpu + cpu_cap);
    CHECK(memcmp(p_mrg, guest_pool + 0x3100, 256) == 0);

    const uint8_t *p_upg = vk_usnap_bytes_aligned(a_upgrade, 256, 256);
    CHECK(p_upg != NULL && p_upg >= U.cpu && p_upg + 256 <= U.cpu + cpu_cap);
    CHECK(memcmp(p_upg, guest_pool + 0x3800, 256) == 0);

    // Non-c4 region must read from arena->map, not cpu_buf.
    const uint8_t *p_non = vk_usnap_bytes_aligned(a_non_slot0, 256, 256);
    CHECK(p_non != NULL);
    CHECK(p_non >= U.arena->map && p_non + 256 <= U.arena->map + U.arena->size);
    CHECK(memcmp(p_non, saved_slot0, 256) == 0);

    // Misaligned lookup returns NULL; matching alignment and unaligned vk_usnap_bytes succeed.
    CHECK(vk_usnap_bytes_aligned(a_c4_vs + 64, 128, 256) == NULL);
    const uint8_t *p_sub = vk_usnap_bytes_aligned(a_c4_vs + 64, 128, 64);
    CHECK(p_sub == p_vs + 64);
    CHECK(vk_usnap_bytes(a_c4_vs + 64, 128) == p_vs + 64);

    // Out-of-bounds length returns NULL.
    CHECK(vk_usnap_bytes_aligned(a_c4_vs, 257, 256) == NULL);

    // Drop invalidates the region for both aligned and unaligned lookups, leaving others intact.
    vk_usnap_drop(a_c4_vs, 256);
    CHECK(vk_usnap_bytes_aligned(a_c4_vs, 256, 256) == NULL);
    CHECK(vk_usnap_bytes(a_c4_vs, 256) == NULL);
    CHECK(vk_usnap_bytes_aligned(a_c4_fs, 128, 256) == p_fs);

    vk_usnap_end(1);
    if (last_comp_fn) last_comp_fn(last_comp_arg), last_comp_fn = NULL;
    mock_done_fence = 1;

    // Restore guest memory before submission 2.
    memcpy(guest_pool + 0x2000, saved_vs, 256);
    memcpy(guest_pool + 0x0400, saved_slot0, 256);

    // 2. cpu_reads OFF: U.cpu is NULL, every region has REG_NO_CPU, lookups read arena->map.
    vk_usnap_cpu_reads(0);
    vk_usnap_begin(&handle, 1, 2);
    CHECK(U.on == 1 && U.cpu == NULL);
    for (int i = 0; i < U.nreg; i++) CHECK(U.reg[i].cpu_off == REG_NO_CPU);
    const uint8_t *p_vs_off = vk_usnap_bytes_aligned(a_c4_vs, 256, 256);
    CHECK(p_vs_off != NULL && p_vs_off >= U.arena->map && p_vs_off + 256 <= U.arena->map + U.arena->size);
    CHECK(memcmp(p_vs_off, saved_vs, 256) == 0);
    vk_usnap_end(2);
    if (last_comp_fn) last_comp_fn(last_comp_arg), last_comp_fn = NULL;
    mock_done_fence = 2;

    // 3. cpu_reads ON with only non-c4 bindings: U.cpu stays NULL.
    CmdStream cs_non = {0};
    cmd_bind_ubo(&cs_non, NVN_SHADER_STAGE_VERTEX, 0, a_non_slot0, 1024);
    cmd_bind_ubo(&cs_non, NVN_SHADER_STAGE_COMPUTE, 1, a_non_cs, 256);
    cmd_end(&cs_non);
    uint64_t handle_non = (uint64_t)(uintptr_t)cs_non.data;
    vk_usnap_cpu_reads(1);
    vk_usnap_begin(&handle_non, 1, 3);
    CHECK(U.on == 1 && U.nreg == 2 && U.cpu == NULL);
    for (int i = 0; i < U.nreg; i++) CHECK(U.reg[i].cpu_off == REG_NO_CPU);
    vk_usnap_end(3);
    if (last_comp_fn) last_comp_fn(last_comp_arg), last_comp_fn = NULL;

    vk_usnap_shutdown();
    free(raw);
    printf("usnap c4 host copy: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--deps', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'tests' / 'draw-merge-ready')
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    includes = ['-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime' / 'third_party'),
                '-I', str(args.deps / 'include'), '-I', str(args.deps / 'include' / 'vma')]

    targets = [
        ('keys_test', build_keys_source()),
        ('usnap_c4_test', build_usnap_source()),
    ]
    for name, code in targets:
        src = args.out / f'{name}.c'
        exe = args.out / f'{name}.exe'
        src.write_text(code, encoding='utf-8')
        for opt in ('-O0', '-O2'):
            subprocess.run([str(args.cc), '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                            *includes, str(src), '-o', str(exe)], check=True, timeout=120)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            print(opt, run.stdout.strip())
            if run.stderr:
                print(run.stderr.strip(), file=sys.stderr)
            if run.returncode:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
