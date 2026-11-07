"""Compile and run the captured local last-use load regression."""
import argparse, os, shutil, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    parser.add_argument('--out', type=Path, default=ROOT / 'build/test/native-local-lu')
    args = parser.parse_args()
    if not args.cxx:
        parser.error('pass --cxx or put clang++ on PATH')
    args.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get('PATH', '')
    for opt in ('0', '2'):
        exe = args.out.resolve() / ('local-lu-O' + opt + ('.exe' if os.name == 'nt' else ''))
        cmd = [args.cxx, '-std=c++20', '-O' + opt, '-Wall', '-Wextra', '-Werror',
               '-I', str(ROOT / 'runtime/third_party/eglantine'),
               str(ROOT / 'tests/native_local_lu_test.cpp'), '-o', str(exe)]
        subprocess.run(cmd, env=env, check=True, timeout=120)
        subprocess.run([str(exe)], env=env, check=True, timeout=30)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
