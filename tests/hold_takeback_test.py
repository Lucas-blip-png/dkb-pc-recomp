"""Unit tests for runtime/vk_texture.c tex_same_desc, takeback_free, and epoch-view visibility (TOTK_HOLD_TAKEBACK).

Extracts agen_idx, vk_texture_takebacks, vk_texture_view, tex_is_released, tb_bucket_stale,
tex_same_desc, takeback_free, and vk_texture_slot_token_again verbatim from runtime/vk_texture.c
and compiles a C fixture at -O0 and -O2 with -Wall -Wextra -Werror (no Vulkan loader or GPU).
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


def build_source():
    tex_src = (ROOT / 'runtime' / 'vk_texture.c').read_text(encoding='utf-8')
    sigs = (
        'static uint32_t agen_idx(',
        'uint64_t vk_texture_takebacks(',
        'void vk_texture_view(',
        'static inline int tex_is_released(',
        'static inline int tb_bucket_stale(',
        'static int tex_same_desc(',
        'static int takeback_free(',
        'uint64_t vk_texture_slot_token_again(',
    )
    extracted = [extract_function(tex_src, s) for s in sigs]

    prelude = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_hmap.h"
#include "nvn_obj.h"
#include "vk.h"

#define AGEN_N 4096
#define RGEN_N 4096

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8];
} TexKey;

typedef struct Tex {
    VkTexImage pub;
    tex_t t;
    int released;
    uint64_t live_seq;
    struct Tex *next_at;
} Tex;

static uint32_t agen[AGEN_N];
static uint64_t agen_tb[AGEN_N];
static uint32_t rgen[RGEN_N];
static uint64_t tex_tb_seq;
static _Thread_local uint64_t view_upto;

static int mock_knob = 1;
static int mock_ab = 1;

int rt_knob(const char *name, const char *def) {
    (void)name; (void)def;
    return mock_knob;
}
int rt_ab(void) { return mock_ab; }

static uint64_t n_tb_not_target, n_tb_desc, n_tb_upload;
static int takeback_mode = -1;
static _Thread_local int target_lookup;
'''

    cases = r'''
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void init_tex(tex_t *t, uint8_t pad_fill) {
    memset(t, pad_fill, sizeof *t);
    t->magic = MAGIC('T', 'E', 'X', ' ');
    t->d.target = NVN_TEXTURE_TARGET_2D;
    t->d.format = NVN_FORMAT_RGBA8;
    t->d.flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE;
    t->d.samples = 1;
    t->d.width = 1280;
    t->d.height = 720;
    t->d.depth = 1;
    t->d.levels = 1;
    t->d.stride = 0;
    t->d.pkg_block_h = 4;
    t->d.swizzle_r = 2;
    t->d.swizzle_g = 3;
    t->d.swizzle_b = 4;
    t->d.swizzle_a = 5;
    t->d.swizzle_pad = pad_fill & 0xfffu;
    t->has_layout = 1;
    for (int i = 0; i < 8; i++) t->layout[i] = (uint8_t)(0x10 + i);
    t->L.size = 1280ull * 720ull * 4ull;
    t->L.layer_size = t->L.size;
    t->L.align = 512;
    t->L.layers = 1;
    t->L.bpb = 4;
    t->L.tileH = 4;
    t->L.tileD = 0;
    t->mem = (uint8_t *)(uintptr_t)0x200000000ull;
    t->pool = (pool_t *)(uintptr_t)0x100000000ull;
    t->off = 0x4000;
    t->serial = 1;
}

int main(void) {
    // 1. tex_same_desc ignores magic, serial, swizzle_pad, struct padding, and layout bytes when !has_layout.
    tex_t a, b;
    init_tex(&a, 0xaa);
    init_tex(&b, 0x55);
    b.magic = 0xdeadbeefu;
    b.serial = 999999ull;
    b.d.swizzle_pad = 0x777u;
    CHECK(tex_same_desc(&a, &b) == 1);

    a.has_layout = b.has_layout = 0;
    memset(a.layout, 0x11, sizeof a.layout);
    memset(b.layout, 0x22, sizeof b.layout);
    CHECK(tex_same_desc(&a, &b) == 1);

    // 2. tex_same_desc catches every semantic field change.
    init_tex(&a, 0x00);
    #define MUTATE_CHECK(stmt) do { b = a; stmt; CHECK(tex_same_desc(&a, &b) == 0); } while (0)
    MUTATE_CHECK(b.mem += 4096);
    MUTATE_CHECK(b.pool = (pool_t *)(uintptr_t)0x100001000ull);
    MUTATE_CHECK(b.off += 64);
    MUTATE_CHECK(b.has_layout = 0);
    MUTATE_CHECK(b.layout[7] ^= 1);
    MUTATE_CHECK(b.d.target = NVN_TEXTURE_TARGET_2D_ARRAY);
    MUTATE_CHECK(b.d.format = NVN_FORMAT_RGBA16F);
    MUTATE_CHECK(b.d.flags ^= 1);
    MUTATE_CHECK(b.d.samples = 4);
    MUTATE_CHECK(b.d.width += 1);
    MUTATE_CHECK(b.d.height += 1);
    MUTATE_CHECK(b.d.depth += 1);
    MUTATE_CHECK(b.d.levels += 1);
    MUTATE_CHECK(b.d.stride = 5120);
    MUTATE_CHECK(b.d.pkg_block_h = 2);
    MUTATE_CHECK(b.d.swizzle_r = 1);
    MUTATE_CHECK(b.d.swizzle_g = 1);
    MUTATE_CHECK(b.d.swizzle_b = 1);
    MUTATE_CHECK(b.d.swizzle_a = 1);
    MUTATE_CHECK(b.L.size += 512);
    MUTATE_CHECK(b.L.layer_size += 512);
    MUTATE_CHECK(b.L.align = 1024);
    MUTATE_CHECK(b.L.layers = 2);
    MUTATE_CHECK(b.L.bpb = 8);
    MUTATE_CHECK(b.L.tileH = 3);
    MUTATE_CHECK(b.L.tileD = 1);
    #undef MUTATE_CHECK

    // 3. takeback_free tests (including with by_addr siblings present: allowed via epoch-view).
    Tex e = {0}, sibling = {0};
    init_tex(&e.t, 0xaa);
    e.pub.mirrored = 1;
    e.pub.gpu_owned = 1;
    e.next_at = &sibling;   // sibling at same address must NOT block takeback_free anymore
    TexKey k = {0};
    k.addr = (uint64_t)(uintptr_t)e.t.mem;

    tex_t reinit;
    init_tex(&reinit, 0x55);
    reinit.serial = e.t.serial + 42;
    reinit.magic = 0x12345678u;

    mock_knob = 1, takeback_mode = -1, target_lookup = 1;
    CHECK(takeback_free(&e, &reinit, &k) == 1);
    CHECK(n_tb_not_target == 0 && n_tb_desc == 0 && n_tb_upload == 0);

    // Non-mirrored RT with gpu_owned=0 also passes.
    e.pub.mirrored = 0, e.pub.gpu_owned = 0;
    CHECK(takeback_free(&e, &reinit, &k) == 1);
    e.pub.mirrored = 1, e.pub.gpu_owned = 1;

    // Rejects !target_lookup and counts n_tb_not_target.
    target_lookup = 0;
    CHECK(takeback_free(&e, &reinit, &k) == 0);
    CHECK(n_tb_not_target == 1 && n_tb_desc == 0 && n_tb_upload == 0);
    target_lookup = 1;

    // Rejects descriptor mismatch and counts n_tb_desc.
    tex_t diff = reinit;
    diff.d.format = NVN_FORMAT_RGBA16F;
    CHECK(takeback_free(&e, &diff, &k) == 0);
    CHECK(n_tb_not_target == 1 && n_tb_desc == 1 && n_tb_upload == 0);

    // Rejects mirrored && !gpu_owned and counts n_tb_upload.
    e.pub.mirrored = 1, e.pub.gpu_owned = 0;
    CHECK(takeback_free(&e, &reinit, &k) == 0);
    CHECK(n_tb_not_target == 1 && n_tb_desc == 1 && n_tb_upload == 1);
    e.pub.gpu_owned = 1;

    // Knob OFF (0) and A/B block OFF (mode=2, rt_ab=0) decline early without bumping decline counters.
    uint64_t sum0 = n_tb_not_target + n_tb_desc + n_tb_upload;
    mock_knob = 0, takeback_mode = -1;
    CHECK(takeback_free(&e, &reinit, &k) == 0);
    mock_knob = 2, mock_ab = 0, takeback_mode = -1;
    CHECK(takeback_free(&e, &reinit, &k) == 0);
    CHECK(n_tb_not_target + n_tb_desc + n_tb_upload == sum0);
    mock_ab = 1;
    CHECK(takeback_free(&e, &reinit, &k) == 1);

    // 4. Epoch visibility (tex_is_released) across walk vs held slice vs newer pass.
    e.released = 1, e.live_seq = 0;
    vk_texture_view(0);
    CHECK(tex_is_released(&e) == 1);
    vk_texture_view(1);
    CHECK(tex_is_released(&e) == 1);

    // Taken back with hold_barrier (live_seq == 0): visible everywhere.
    e.released = 0, e.live_seq = 0;
    vk_texture_view(0);
    CHECK(tex_is_released(&e) == 0);
    vk_texture_view(1);
    CHECK(tex_is_released(&e) == 0);

    // Simulate pass A starting at tex_tb_seq == 4 -> view_upto = 5; then takeback bumps tex_tb_seq to 5.
    tex_tb_seq = 4;
    uint64_t pass_a_upto = vk_texture_takebacks() + 1;   // 5
    uint64_t seq = __atomic_add_fetch(&tex_tb_seq, 1, __ATOMIC_RELEASE);   // 5
    uint32_t aidx = agen_idx(k.addr);
    uint32_t other_aidx = (aidx + 1) & (AGEN_N - 1);
    e.released = 0, e.live_seq = seq;
    __atomic_store_n(&agen_tb[aidx], seq, __ATOMIC_RELEASE);
    uint64_t pass_b_upto = vk_texture_takebacks() + 1;   // 6

    // Walk (view_upto == 0) sees e as live and bucket not stale.
    vk_texture_view(0);
    CHECK(tex_is_released(&e) == 0);
    CHECK(tb_bucket_stale(aidx) == 0);

    // Held slice from pass A (started before takeback, view_upto == 5): e is hidden as released, bucket is stale.
    vk_texture_view(pass_a_upto);
    CHECK(tex_is_released(&e) == 1);
    CHECK(tb_bucket_stale(aidx) == 1);
    CHECK(tb_bucket_stale(other_aidx) == 0);

    // Token cache invalidation via vk_texture_slot_token_again on stale bucket vs unrelated bucket.
    agen[aidx] = 7, agen[other_aidx] = 9;
    rgen[1] = 3;
    VkSlotStamp st_hit = {.serial = 42, .ridx = 1, .rgen = 3, .aidx = aidx, .valid = 1};
    VkSlotStamp st_other = {.serial = 43, .ridx = 1, .rgen = 3, .aidx = other_aidx, .valid = 1};
    CHECK(vk_texture_slot_token_again(&st_hit) == 0);
    CHECK(vk_texture_slot_token_again(&st_other) == ((uint64_t)43 << 32 | 9));

    // Newer pass B (started after takeback, view_upto == 6): sees e as live and bucket not stale.
    vk_texture_view(pass_b_upto);
    CHECK(tex_is_released(&e) == 0);
    CHECK(tb_bucket_stale(aidx) == 0);
    CHECK(vk_texture_slot_token_again(&st_hit) == ((uint64_t)42 << 32 | 7));
    vk_texture_view(0);

    printf("hold_takeback: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
'''
    return '\n'.join([prelude, *extracted, cases])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--deps', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'tests' / 'hold-takeback')
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    src = args.out / 'hold_takeback.c'
    exe = args.out / 'hold_takeback.exe'
    src.write_text(build_source(), encoding='utf-8')
    includes = ['-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime' / 'third_party'),
                '-I', str(args.deps / 'include'), '-I', str(args.deps / 'include' / 'vma')]
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
