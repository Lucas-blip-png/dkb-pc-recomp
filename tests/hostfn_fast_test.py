"""Fast host-function dispatch (DKB_HOSTFN_FAST): runtime/dispatch.c direct window check +
runtime/hostfn.c read-only hot_slots[], thread-local exact hostfn_calls(), DKB_TRACE=host
first-call logging, inline kern_host_track depth/preempt, blocking nn::fs core-away wrapper,
rt_original observer dispatch, setjmp/longjmp unwind, and concurrent A/B toggling. No game or GPU.
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
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'tests' / 'hostfn-fast')
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with the project Clang executable')
    args.out.mkdir(parents=True, exist_ok=True)
    exe = args.out / ('hostfn_fast.exe' if os.name == 'nt' else 'hostfn_fast')
    extra = [] if os.name == 'nt' else ['-lpthread']
    for opt in ('-O0', '-O2'):
        subprocess.run([str(args.cc), '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror', '-fno-strict-aliasing',
                        '-I', str(ROOT / 'runtime'),
                        str(ROOT / 'tests' / 'hostfn_fast.c'),
                        str(ROOT / 'runtime' / 'hostfn.c'),
                        str(ROOT / 'runtime' / 'dispatch.c'),
                        *extra, '-o', str(exe)], check=True, timeout=120)
        for mode in ('0', '1', '2'):
            env = dict(os.environ, HOSTFN_TEST_MODE=mode)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env=env)
            print(opt, run.stdout.strip(), run.stderr.strip())
            if run.returncode:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
