"""Multithreaded stress test for sharded command-copy maps (DKB_COPY_SHARDS) in runtime/nvn_api_cmd.c."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang'))
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'tests' / 'copy-shards')
    args = ap.parse_args()
    if not args.cc:
        ap.error('pass --cc with the project Clang executable')
    args.out.mkdir(parents=True, exist_ok=True)
    exe = args.out / ('copy_shards.exe' if os.name == 'nt' else 'copy_shards')
    for opt in ('-O0', '-O2'):
        subprocess.run([str(args.cc), '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                        '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                        '-I', str(ROOT / 'runtime'),
                        str(ROOT / 'tests' / 'copy_shards.c'),
                        str(ROOT / 'runtime' / 'nvn_api_cmd.c'),
                        '-Wl,--gc-sections', *([] if os.name == 'nt' else ['-pthread']),
                        '-o', str(exe)], check=True)
        for mode in ('1', '0'):
            env = dict(os.environ, DKB_COPY_SHARDS=mode)
            r = subprocess.run([str(exe)], env=env, timeout=30, capture_output=True, text=True)
            print(f'{opt} DKB_COPY_SHARDS={mode}: {r.stdout.strip()}', end='')
            if r.stderr:
                print(' ' + r.stderr.strip(), end='')
            print()
            if r.returncode != 0:
                return r.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
