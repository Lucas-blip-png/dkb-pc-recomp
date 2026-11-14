"""Test input.c's private identity scanner against an instrumented SDL substitute, without a game or USB devices.

Usage: python tests/controller_devices_test.py --cc <clang>
The block between device_hash and input_pad_devices has no window/game dependencies. Compile that exact source
with the fixture so USB open counts, failed opens and reconnect identities can be checked deterministically.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    args = ap.parse_args()
    out = ROOT / 'build/tests/controller-devices'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'runtime/input.c').read_text(encoding='utf-8')
    block = source[source.index('static uint64_t device_hash('):source.index('int input_pad_devices(')]
    (out / 'controller_devices_under_test.inc').write_text(block, encoding='utf-8')
    for opt in ('-O0', '-O2'):
        exe = out / f'controller-devices-{opt[1:]}.exe'
        subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'), '-I', str(out),
                        str(ROOT / 'tests/controller_devices_test.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
