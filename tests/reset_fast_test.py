"""runtime/vk_exec_draw.c reset_sort_fast and note_written_many against reset_sort and note_written.

reset_sort_fast uses an O(n) sorted check and an 11-bit LSD radix sort on (addr - lo) >> 2, falling back to sort_u64
above 32 GiB spans or below 64 items. Every pass-count boundary (2047, 2048, 2^22 - 1, 2^22, 2^33 - 1, 2^33 word
steps), already-sorted, reverse-sorted, null-filled, duplicate and overlapping inputs must match reset_sort in both
sorted output and overlap answer.

note_written_many pre-sizes written and written_slot once for a batch of disjoint runs; starting from both empty and
populated tables (including pre-existing VK_WB_LAZY ranges and conflicting VkBuffer handles), it must leave the exact
same entries, hash lookups and shadow-pending notifications as calling note_written for each run.

Both functions are cut out of runtime/vk_exec_draw.c and compiled at -O0 and -O2. No GPU or game data is needed.
"""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msort.h"
#include "nvn_indirect_reset.h"

static void rt_fatal(const char *m) { puts(m); fflush(stdout); abort(); }
#define VK_WB_LAZY (1ull << 63)
#define VK_NULL_HANDLE ((VkBuffer)0)
typedef uintptr_t VkBuffer;
typedef struct { uint64_t addr, size; VkBuffer buf; } Range;
typedef struct {
    Range *written;
    int nwritten, cwritten;
    uint32_t *written_slot;
    uint32_t written_slots;
} CbRes;
typedef struct { CbRes *cur; } DrawCtx;
typedef struct { DrawCtx *draw; } VkExec;
#define DC_OF(ex) ((ex)->draw)
#define IS_HELPER(dc) ((void)(dc), 0)
#define vk_spec_fail(x) 0
static CbRes *cb_res(VkExec *ex) { return ex->draw->cur; }
static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 64;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("out of memory");
    *cap = n;
    return q;
}
static int shadow_mode;
static int vk_mem_shadow_on(void) { return shadow_mode; }
static uint64_t pending_count, pending_xor;
static void vk_mem_gpu_pending(uint64_t addr, uint64_t size) {
    if (!shadow_mode) return;
    pending_count++;
    pending_xor ^= addr * 0x9e3779b97f4a7c15ull ^ size;
}
#define U64_LESS(x, y) (*(x) < *(y))
MSORT_DEFINE(sort_u64, uint64_t, U64_LESS)
'''

CASES = r'''
static uint64_t rnd(void) { static uint64_t x = 918273645564738291ull; x ^= x << 13, x ^= x >> 7, x ^= x << 17; return x; }
static int checks;

static int sort_cases(void) {
    static const uint64_t word_spans[] = {
        2047ull, 2048ull, (1ull << 22) - 1, 1ull << 22, (1ull << 33) - 1, 1ull << 33
    };
    static uint64_t in[10000];
    int fails = 0;
    for (int s = 0; s < 6; s++) {
        for (int round = 0; round < 6; round++) {
            uint32_t count = 64u + (uint32_t)(rnd() % 8000u);
            uint64_t base = 0x100000000ull + (rnd() % 4096u) * 4u;
            uint64_t span = word_spans[s];
            for (uint32_t i = 0; i < count; i++) {
                if (i > 2 && rnd() % 11 == 0) { in[i] = 0; continue; }
                uint64_t k = i == 0 ? 0 : i == 1 ? span : rnd() % (span + 1);
                in[i] = base + k * 4u;
            }
            uint64_t *o0 = NULL, *o1 = NULL;
            uint32_t n0 = 0, n1 = 0;
            int ov0 = reset_sort(in, count, &o0, &n0);
            int ov1 = reset_sort_fast(in, count, &o1, &n1);
            checks += 2;
            if (ov0 != ov1 || n0 != n1 || (n0 && memcmp(o0, o1, (size_t)n0 * sizeof *o0))) {
                fails++;
                printf("FAIL: reset_sort_fast span %llu round %d (ov %d/%d n %u/%u)\n",
                       (unsigned long long)span, round, ov0, ov1, n0, n1);
            }
        }
    }
    // Already-sorted, reverse-sorted, all-null, small (<64), exact-20-byte touching, and 16-byte overlap edge cases.
    uint32_t edge_n = 512;
    for (uint32_t i = 0; i < edge_n; i++) in[i] = 0x200000000ull + (uint64_t)i * 20u;
    uint64_t *o0 = NULL, *o1 = NULL;
    uint32_t n0 = 0, n1 = 0;
    int ov0 = reset_sort(in, edge_n, &o0, &n0), ov1 = reset_sort_fast(in, edge_n, &o1, &n1);
    checks++;
    if (ov0 != 0 || ov1 != 0 || n0 != n1 || memcmp(o0, o1, (size_t)n0 * sizeof *o0))
        fails++, puts("FAIL: already-sorted touching records");
    for (uint32_t i = 0; i < edge_n; i++) in[i] = 0x200000000ull + (uint64_t)(edge_n - 1 - i) * 20u;
    ov0 = reset_sort(in, edge_n, &o0, &n0), ov1 = reset_sort_fast(in, edge_n, &o1, &n1);
    checks++;
    if (ov0 != 0 || ov1 != 0 || n0 != n1 || memcmp(o0, o1, (size_t)n0 * sizeof *o0))
        fails++, puts("FAIL: reverse-sorted touching records");
    in[250] = in[251] + 16u;   // overlaps by 4 bytes
    ov0 = reset_sort(in, edge_n, &o0, &n0), ov1 = reset_sort_fast(in, edge_n, &o1, &n1);
    checks++;
    if (ov0 != 1 || ov1 != 1 || n0 != n1 || memcmp(o0, o1, (size_t)n0 * sizeof *o0))
        fails++, puts("FAIL: overlapping target detection");
    memset(in, 0, 128 * sizeof *in);
    ov0 = reset_sort(in, 128, &o0, &n0), ov1 = reset_sort_fast(in, 128, &o1, &n1);
    checks++;
    if (ov0 != 0 || ov1 != 0 || n0 != 0 || n1 != 0) fails++, puts("FAIL: all-null list");
    return fails;
}

static int compare_cb(const CbRes *a, const CbRes *b) {
    if (a->nwritten != b->nwritten) return 1;
    if (a->nwritten && memcmp(a->written, b->written, (size_t)a->nwritten * sizeof *a->written)) return 1;
    // Every entry in b must be findable through b->written_slot.
    uint32_t mask = b->written_slots - 1;
    for (int i = 0; i < b->nwritten; i++) {
        uint32_t at = written_hash(b->written[i].addr, b->written[i].size) & mask;
        int found = 0;
        for (; b->written_slot[at]; at = (at + 1) & mask)
            if (b->written_slot[at] == (uint32_t)i + 1) { found = 1; break; }
        if (!found) return 1;
    }
    return 0;
}

static void free_cb(CbRes *r) {
    free(r->written), free(r->written_slot);
    memset(r, 0, sizeof *r);
}

static int note_written_cases(void) {
    static uint64_t runs[6000][2];
    int fails = 0;
    for (int sh = 0; sh < 2; sh++) {
        shadow_mode = sh;
        for (int pre = 0; pre < 2; pre++) {
            CbRes r0 = {0}, r1 = {0};
            DrawCtx d0 = {&r0}, d1 = {&r1};
            VkExec e0 = {&d0}, e1 = {&d1};
            uint64_t base = 0x300000000ull;
            uint32_t nr = 5000;
            for (uint32_t i = 0; i < nr; i++) {
                uint64_t lo = base + (uint64_t)i * 64u;
                runs[i][0] = lo, runs[i][1] = lo + 20u * (1u + (i % 3u));
            }
            if (pre) {
                // Pre-populate with a mix of distinct ranges, lazy duplicates (same buf) and different-buf duplicates.
                for (uint32_t i = 0; i < 400; i++) {
                    uint64_t addr = (i % 2 == 0) ? runs[i * 5][0] : (0x400000000ull + (uint64_t)i * 64u);
                    uint64_t sz = (i % 2 == 0) ? (runs[i * 5][1] - runs[i * 5][0]) : 32u;
                    if (i % 3 == 0) sz |= VK_WB_LAZY;
                    VkBuffer buf = (i % 4 == 0) ? (VkBuffer)0x2222 : (VkBuffer)0x1111;
                    note_written(&e0, addr, sz, buf);
                    note_written(&e1, addr, sz, buf);
                }
            }
            pending_count = 0, pending_xor = 0;
            for (uint32_t i = 0; i < nr; i++)
                note_written(&e0, runs[i][0], runs[i][1] - runs[i][0], (VkBuffer)0x1111);
            uint64_t pc0 = pending_count, px0 = pending_xor;
            pending_count = 0, pending_xor = 0;
            note_written_many(&e1, (const uint64_t (*)[2])runs, nr, (VkBuffer)0x1111);
            checks += 2;
            if (compare_cb(&r0, &r1) || pc0 != pending_count || px0 != pending_xor) {
                fails++;
                printf("FAIL: note_written_many shadow=%d pre=%d (n %d/%d pend %llu/%llu)\n",
                       sh, pre, r0.nwritten, r1.nwritten, (unsigned long long)pc0, (unsigned long long)pending_count);
            }
            free_cb(&r0), free_cb(&r1);
        }
    }
    return fails;
}

int main(void) {
    int fails = 0;
    fails += sort_cases();
    fails += note_written_cases();
    printf("reset fast: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
'''


def function(text, start):
    i = text.index(start)
    depth, k = 0, text.index('{', i)
    while True:
        depth += text[k] == '{'
        depth -= text[k] == '}'
        if text[k] == '}' and not depth:
            return text[i:k + 1]
        k += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'test' / 'reset-fast')
    args = parser.parse_args()
    source = (ROOT / 'runtime' / 'vk_exec_draw.c').read_text(encoding='utf-8')
    parts = [PRELUDE] + [function(source, start) for start in
                         ('static uint32_t written_hash(', 'static void note_written(',
                          'static void note_written_many(', 'static int reset_sort(',
                          'static int reset_sort_fast(')] + [CASES]
    args.out.mkdir(parents=True, exist_ok=True)
    src, exe = args.out / 'reset_fast.c', args.out / 'reset_fast.exe'
    src.write_text('\n'.join(parts), encoding='utf-8')
    for opt in ('-O0', '-O2'):
        subprocess.run([str(args.cc), '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                        str(src), '-o', str(exe)], check=True, timeout=120)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(opt, run.stdout.strip())
        if run.returncode:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
