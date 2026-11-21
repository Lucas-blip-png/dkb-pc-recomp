"""Uniform range lists of TOTK_USNAP_LIST: runtime/nvn_api_cmd.c take_recording gathers each recording's normalized
uniform bindings (composing OP_CALL callees' merged lists through an L1 dedup cache under TOTK_CALL_REF) and
runtime/vk_usnap.c nvn_ubo_list_build sorts (11-bit LSD radix sort / merge sort fallback) and merges them.
tests/usnap_list.c records through the real recorder and checks leaf recordings, nested OP_CALL trees up to
CALL_REF_DEPTH, repeated callees, >64 KiB merged callee spans, radix vs merge-sort parity, large spans (>33 bits),
async clones, list_check, and submission union equivalence. No game or GPU.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''// Cut from runtime/vk_usnap.c by tests/usnap_list_test.py.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "msort.h"
typedef nvn_range Rng;
static struct { int fast; Rng *col; int ncol, ccol; uint64_t cols, repeats, list_bad; } U;
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


def line(text, start):
    i = text.index('\n' + start) + 1
    return text[i:text.index('\n', i)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang'))
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'tests' / 'usnap-list')
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with the project Clang executable')
    source = (ROOT / 'runtime' / 'vk_usnap.c').read_text(encoding='utf-8')
    parts = [PRELUDE]
    parts += [line(source, start) for start in ('#define ALIGN ', '#define MAX_RANGE ', '#define CALL_DEPTH ',
                                                '#define RADIX_BITS ', '#define SEEN_BITS ', 'static Rng seen[',
                                                '#define RNG_LESS', 'MSORT_DEFINE(sort_rng')]
    parts += [function(source, start) for start in
              ('static void *grow(', 'static void add_range(', 'static void col_append(', 'static void collect(',
               'static int by_lo(', 'static int radix_passes(', 'static size_t rng_merge(',
               'size_t nvn_ubo_list_build(', 'static void sort_col(', 'static void list_check(')]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'usnap_list_cut.c').write_text('\n'.join(parts) + '\n', encoding='utf-8')
    exe = args.out / ('usnap_list.exe' if os.name == 'nt' else 'usnap_list')
    for opt in ('-O0', '-O2'):
        subprocess.run([str(args.cc), '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror', '-fno-strict-aliasing',
                        '-ffunction-sections', '-fdata-sections', '-I', str(args.out), '-I', str(ROOT / 'runtime'),
                        str(ROOT / 'tests' / 'usnap_list.c'), str(ROOT / 'runtime' / 'nvn_api_cmd.c'),
                        '-Wl,--gc-sections', '-o', str(exe)], check=True, timeout=120)
        for call_ref in ('0', '1'):   # TOTK_CALL_REF off: callees inlined; on: OP_CALL records compose callee lists
            env = dict(os.environ, USNAP_LIST_CALL_REF=call_ref)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300, env=env)
            print(opt, run.stdout.strip(), run.stderr.strip())
            if run.returncode:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
