"""Concurrency and decommit-hook checks for runtime/memory.c (cl_thread shared lock, mem_query, SetHeapSize).

Usage: python tests/commit_later_lock_test.py --cc /path/to/clang [--out DIR]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang'))
    parser.add_argument('--out', type=Path, default=ROOT / 'build/tests/commit-later-lock')
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with a C compiler')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    # Minimal layout.h providing MOD_COUNT for runtime/memory.c without game files.
    (args.out / 'layout.h').write_text('#pragma once\n#define MOD_COUNT 1\n', encoding='utf-8')

    for opt in ('-O0', '-O2'):
        exe = args.out / ('commit_later_lock' + opt + ('.exe' if os.name == 'nt' else ''))
        subprocess.run(
            [args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Werror',
             '-fno-strict-aliasing', '-D_WIN32_WINNT=0x0A00', '-I', str(args.out), '-I', str(ROOT / 'runtime'),
             str(ROOT / 'tests/commit_later_lock.c'), '-o', str(exe)],
            check=True, timeout=120,
        )
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        print(opt, run.stdout.strip())
        if run.returncode:
            print(run.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
