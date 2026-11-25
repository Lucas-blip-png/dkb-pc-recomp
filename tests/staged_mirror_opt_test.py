"""CPU checks for staged-mirror cpu_written tracking, decommit_hook clearing, staged_runs, and staged_split_own.

Usage: python tests/staged_mirror_opt_test.py --cc /path/to/clang --deps /path/to/runtime/deps [--out DIR]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from nvn2_compute_c0_test import function
from descriptor_pool_registry_test import typedef, statement

ROOT = Path(__file__).resolve().parents[1]


def definition(source, start):
    """The function whose signature begins with start, skipping forward declarations."""
    at = 0
    while True:
        at = source.index(start, at)
        brace, semi = source.find('{', at), source.find(';', at)
        if brace >= 0 and (semi < 0 or brace < semi):
            return function(source[at:], start)
        at = semi


def line(source, start):
    """The whole line of source that starts with start."""
    begin = source.index('\n' + start) + 1
    return source[begin:source.index('\n', begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang'))
    parser.add_argument('--deps', type=Path, default=ROOT / 'runtime/deps')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/tests/staged-mirror-opt')
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with a C compiler')
    include = args.deps / 'include' if (args.deps / 'include').is_dir() else args.deps
    if not (include / 'vulkan/vulkan.h').is_file():
        parser.error('--deps must contain the Vulkan headers (directly or under include/)')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    src = (ROOT / 'runtime/vk_memory.c').read_text(encoding='utf-8')

    types = '\n'.join([
        statement(src, 'enum { POOL_IMPORT'),
        statement(src, 'enum { MS_NONE'),
        typedef(src, 'VPage'),
        typedef(src, 'PoolRec'),
        definition(src, 'static inline int ms_get('),
        typedef(src, 'MRange'),
        typedef(src, 'StRange'),
        typedef(src, 'StJob'),
        typedef(src, 'MirrorAcc'),
        line(src, '#define GEN_CHUNK_SHIFT '),
        line(src, '#define GEN_PAGE_SHIFT '),
        line(src, '#define GEN_CHUNKS '),
        line(src, '#define GEN_PER_CHUNK '),
        statement(src, 'static uint32_t *gen_chunk'),
        statement(src, 'static uint32_t gen_now'),
        statement(src, 'static PlatMutex gen_lock'),
        definition(src, 'static void gen_lock_x('),
        statement(src, 'static uint64_t gen_pages'),
        line(src, '#define REG_SHIFT '),
        statement(src, 'static uint32_t reg_max'),
        statement(src, 'static uint32_t *own_chunk'),
        line(src, '#define CPU_WRITTEN_WORDS '),
        statement(src, 'static uint64_t *cpu_written_chunk'),
        statement(src, 'static int freeze_lazy'),
    ])

    code = '\n'.join([
        'static void grow(void **arr, int *cap, size_t elem);',
        'static void stamp_page(uint64_t a, uint32_t g);',
        definition(src, 'static void grow('),
        definition(src, 'static void range_add('),
        definition(src, 'static void cpu_written_set_page('),
        definition(src, 'static void cpu_written_set_range('),
        definition(src, 'static void cpu_written_mark_range('),
        definition(src, 'static void mem_decommit_cb('),
        definition(src, 'static int ww_stamp('),
        definition(src, 'static uint32_t *gen_slot('),
        definition(src, 'static void stamp_page('),
        definition(src, 'static void stamp_page_gpu_only('),
        definition(src, 'static void stamp_range('),
        definition(src, 'void vk_guest_written('),
        definition(src, 'void vk_gpu_written_into('),
        definition(src, 'static void written_batch('),
        definition(src, 'static uint64_t staged_runs('),
        line(src, '#define STRANGE_LESS('),
        line(src, 'MSORT_DEFINE(sort_st_ranges,'),
        definition(src, 'static void st_sort_job('),
        line(src, '#define ST_OBUF '),
        statement(src, 'static StRange *st_obuf'),
        statement(src, 'static int st_obuf_cap'),
        definition(src, 'static void staged_split_own('),
    ])

    fixture = args.out / 'staged_mirror_opt.c'
    fixture.write_text(
        (ROOT / 'tests/staged_mirror_opt.c').read_text(encoding='utf-8')
        .replace('/* PRODUCTION_TYPES */', types)
        .replace('/* PRODUCTION_FUNCTIONS */', code),
        encoding='utf-8',
    )

    for opt in ('-O0', '-O2'):
        exe = args.out / ('staged_mirror_opt' + opt + ('.exe' if os.name == 'nt' else ''))
        subprocess.run(
            [args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Werror',
             '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'), '-I', str(include), '-I', str(include / 'vma'),
             str(fixture), '-o', str(exe)],
            check=True, timeout=120,
        )
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(opt, run.stdout.strip())
        if run.returncode:
            print(run.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
