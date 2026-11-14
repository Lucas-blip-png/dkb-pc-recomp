"""Compile the real fullscreen hold gate at O0/O2 without SDL or game files.

Usage: python tests/controller_hold_test.py --cc <clang>
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    args = ap.parse_args()
    out = ROOT / 'build/tests/controller-hold'
    out.mkdir(parents=True, exist_ok=True)
    for opt in ('-O0', '-O2'):
        exe = out / f'controller-hold-{opt[1:]}.exe'
        subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'),
                        str(ROOT / 'tests/controller_hold_test.c'), str(ROOT / 'runtime/input_map.c'),
                        '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
