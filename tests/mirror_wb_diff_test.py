"""Unit tests for Issue #485 mirror differential write-back, upload merge, and unsized SSBO bounds.

Usage: python tests/mirror_wb_diff_test.py --cc /path/to/clang --deps /path/to/deps [--spirv-val PATH] [--out DIR]
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
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


def check_shader_header(header_path, comp_path, spirv_val, out_dir):
    header_text = header_path.read_text(encoding='utf-8')
    comp_bytes = comp_path.read_bytes()
    want_sha = hashlib.sha256(comp_bytes).hexdigest()
    m = re.search(r'source sha256 ([0-9a-f]{64})', header_text)
    if not m or m.group(1) != want_sha:
        raise RuntimeError(f'{header_path.name}: source sha256 {m.group(1) if m else None} != {want_sha}')
    words = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', '\n'.join(header_text.splitlines()[2:]))]
    if not words or words[0] != 0x07230203:
        raise RuntimeError(f'{header_path.name}: invalid SPIR-V word array')
    spv_file = out_dir / (header_path.stem + '.spv')
    spv_file.write_bytes(struct.pack('<' + 'I' * len(words), *words))
    if spirv_val:
        subprocess.run([str(spirv_val), '--target-env', 'vulkan1.1', str(spv_file)], check=True, timeout=60)
    return len(words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang'))
    parser.add_argument('--deps', type=Path, default=ROOT / 'runtime/deps')
    parser.add_argument('--spirv-val', default=shutil.which('spirv-val'))
    parser.add_argument('--out', type=Path, default=ROOT / 'build/tests/mirror-wb-diff')
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with a C compiler')
    include = args.deps / 'include' if (args.deps / 'include').is_dir() else args.deps
    if not (include / 'vulkan/vulkan.h').is_file():
        parser.error('--deps must contain the Vulkan headers (directly or under include/)')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    diff_words = check_shader_header(
        ROOT / 'runtime/mirror_diff_spv.h',
        ROOT / 'runtime/shaders/mirror_diff.comp',
        args.spirv_val,
        args.out,
    )
    merge_words = check_shader_header(
        ROOT / 'runtime/mirror_merge_spv.h',
        ROOT / 'runtime/shaders/mirror_merge.comp',
        args.spirv_val,
        args.out,
    )
    snap_words = check_shader_header(
        ROOT / 'runtime/mirror_snap_spv.h',
        ROOT / 'runtime/shaders/mirror_snap.comp',
        args.spirv_val,
        args.out,
    )
    print(f'spirv: mirror_diff_spv ({diff_words} words), mirror_merge_spv ({merge_words} words), '
          f'mirror_snap_spv ({snap_words} words)'
          f'{" validated with spirv-val" if args.spirv_val else " (spirv-val not set)"}')

    mem = (ROOT / 'runtime/vk_memory.c').read_text(encoding='utf-8')
    draw = (ROOT / 'runtime/vk_exec_draw.c').read_text(encoding='utf-8')
    stg_body = definition(mem, 'void *vk_staging(')
    if 'VK_BUFFER_USAGE_STORAGE_BUFFER_BIT' not in stg_body:
        raise RuntimeError('vk_staging missing VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for merge_dispatch_slices')

    mem_types = '\n'.join([
        statement(mem, 'enum { POOL_IMPORT'),
        statement(mem, 'enum { MS_NONE'),
        typedef(mem, 'VPage'),
        typedef(mem, 'PoolRec'),
        definition(mem, 'static inline int ms_get('),
        statement(mem, 'static _Thread_local uint32_t wb_tag'),
        typedef(mem, 'MRange'),
        statement(mem, 'static MRange *pend'),
        statement(mem, 'static int npend'),
        typedef(mem, 'WbLocal'),
        statement(mem, 'static WbLocal *wb_locals'),
        statement(mem, 'static uint32_t pre_walk_writes'),
        statement(mem, 'static uint32_t pre_worker_seq'),
        statement(mem, 'static int pre_has_deferred'),
        statement(mem, 'static MRange *stw'),
        statement(mem, 'static int nstw'),
        typedef(mem, 'MirrorAcc'),
        typedef(mem, 'PreBuf'),
        typedef(mem, 'PreSeg'),
        typedef(mem, 'PreMap'),
        typedef(mem, 'PreCopy'),
        statement(mem, 'static PreMap wpre_map'),
        statement(mem, 'static PreCopy *pre_pend'),
        statement(mem, 'static int npre_pend'),
        statement(mem, 'static int pre_end_done'),
        statement(mem, 'static uint64_t pre_sealed_next'),
        statement(mem, 'static MRange *pre_wtmp'),
        statement(mem, 'static int pre_nwtmp'),
        statement(mem, 'static MRange *pre_wraw'),
        statement(mem, 'static int pre_nwraw'),
        statement(mem, 'static uint32_t pre_cache_upto'),
        typedef(mem, 'StRange'),
        typedef(mem, 'StJob'),
        typedef(mem, 'StMerge'),
        typedef(mem, 'CbPiece'),
    ])

    mem_funcs = '\n'.join([
        'static void grow(void **arr, int *cap, size_t elem);',
        definition(mem, 'static void grow('),
        definition(mem, 'uint32_t vk_mem_wb_tag('),
        definition(mem, 'static void wb_range_add('),
        definition(mem, 'static void range_add('),
        line(mem, '#define MRANGE_LESS('),
        line(mem, 'MSORT_DEFINE(sort_ranges,'),
        definition(mem, 'static int wb_diff_copy('),
        definition(mem, 'static int pre_find_first_seg('),
        line(mem, '#define STRANGE_LESS('),
        line(mem, 'MSORT_DEFINE(sort_st_ranges,'),
        definition(mem, 'static void st_sort_job('),
        line(mem, '#define ST_OBUF '),
        statement(mem, 'static StRange *st_obuf'),
        statement(mem, 'static int st_obuf_cap'),
        definition(mem, 'static void staged_split_own('),
        line(mem, '#define KEPT_LESS('),
        line(mem, 'MSORT_DEFINE(sort_kept,'),
        definition(mem, 'static void st_job_blocked('),
        definition(mem, 'static void st_blocked('),
        definition(mem, 'static void st_cut('),
        definition(mem, 'static void st_w_merge_scan('),
        definition(mem, 'static int staged_wb_record('),
        definition(mem, 'static int own_covered('),
        definition(mem, 'static void cw_note_range('),
        definition(mem, 'static void staged_copy_back('),
        definition(mem, 'static void pre_pend_add('),
        definition(mem, 'static PreBuf *pre_alloc('),
        definition(mem, 'static void mr_normalize('),
        definition(mem, 'static void pre_seg_push('),
        definition(mem, 'static void pre_cover_ranges('),
        definition(mem, 'static void pre_refresh_batch('),
        definition(mem, 'static void pre_refresh_staged('),
        definition(mem, 'static void pre_cut_copy('),
        definition(mem, 'static int pre_map_covers('),
        definition(mem, 'static void pre_scan_staged_range('),
        definition(mem, 'static void pre_map_rawify('),
        definition(mem, 'static void pre_raw_add('),
        definition(mem, 'static void pre_scan_staged_sealed('),
        definition(mem, 'static int pre_need_scan('),
        definition(mem, 'static void pre_collect_uncovered('),
        definition(mem, 'static void pre_prepare_part('),
        definition(mem, 'static void pre_reserve_end('),
        definition(mem, 'int vk_mem_mirror_pending('),
        definition(mem, 'int vk_mem_mirror_pending_for('),
        definition(mem, 'void vk_mem_mirror_part_done('),
    ])

    draw_funcs = '\n'.join([
        definition(draw, 'static int bind_range('),
        definition(draw, 'static VkDescriptorBufferInfo ssbo_info('),
    ])

    fixture = args.out / 'mirror_wb_diff.c'
    fixture.write_text(
        (ROOT / 'tests/mirror_wb_diff.c').read_text(encoding='utf-8')
        .replace('/* MEMORY_TYPES */', mem_types)
        .replace('/* MEMORY_FUNCTIONS */', mem_funcs)
        .replace('/* DRAW_FUNCTIONS */', draw_funcs),
        encoding='utf-8',
    )

    for opt in ('-O0', '-O2'):
        exe = args.out / ('mirror_wb_diff' + opt + ('.exe' if os.name == 'nt' else ''))
        subprocess.run(
            [args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Werror',
             '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'), '-I', str(include), '-I', str(include / 'vma'),
             str(fixture), '-o', str(exe)],
            check=True, timeout=120,
        )
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(opt, run.stdout.strip())
        if run.returncode:
            print(run.stderr, file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
