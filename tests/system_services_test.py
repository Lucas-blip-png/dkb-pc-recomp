"""Native system-service ABI checks against production overrides; no game or GPU."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--deps', type=Path, required=True)
    args = ap.parse_args()
    out = ROOT / 'build/tests/system-services'
    out.mkdir(parents=True, exist_ok=True)
    for source in ('system_services', 'display_abi'):
        for opt in ('-O0', '-O2'):
            exe = out / (source + '-' + opt[1:] + '.exe')
            subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-sign-compare',
                            '-I', str(ROOT / 'runtime'), '-I', str(args.deps / 'include'),
                            str(ROOT / f'tests/{source}.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
