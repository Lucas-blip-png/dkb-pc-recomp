"""Native nn::codec ABI regressions against the production overrides; no game or GPU."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--deps', type=Path, required=True)
    args = ap.parse_args()
    out = ROOT / 'build/tests/codec-abi'
    out.mkdir(parents=True, exist_ok=True)
    for opt in ('-O0', '-O2'):
        exe = out / ('codec-' + opt[1:] + '.exe')
        subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'), '-I', str(args.deps / 'include'),
                        str(ROOT / 'tests/codec_abi.c'), '-L', str(args.deps / 'lib'),
                        '-lvpx', '-Wl,-Bstatic', '-lpthread', '-Wl,-Bdynamic', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
