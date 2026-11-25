"""Unit and concurrency tests for DKB_CMD_REC_FAST in runtime/nvn_api_cmd.c."""
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
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'tests' / 'cmd-rec-fast')
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with the project Clang executable')
    args.out.mkdir(parents=True, exist_ok=True)
    exe = args.out / ('cmd_rec_fast.exe' if os.name == 'nt' else 'cmd_rec_fast')
    extra = [] if os.name == 'nt' else ['-pthread']
    for opt in ('-O0', '-O2'):
        subprocess.run([str(args.cc), '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror', '-fno-strict-aliasing',
                        '-ffunction-sections', '-fdata-sections', '-I', str(ROOT / 'runtime'),
                        str(ROOT / 'tests' / 'cmd_rec_fast.c'), str(ROOT / 'runtime' / 'nvn_api_cmd.c'),
                        str(ROOT / 'runtime' / 'nvn_api_nvn2.c'), '-Wl,--gc-sections', *extra, '-o', str(exe)],
                       check=True, timeout=120)
        for mode in ('0', '1'):
            env = dict(os.environ, TEST_CMD_REC_FAST=mode)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env=env)
            print(opt, run.stdout.strip(), run.stderr.strip())
            if run.returncode:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
