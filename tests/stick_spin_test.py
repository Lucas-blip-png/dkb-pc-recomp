"""Unit tests for stick radial calibration (s2_cal_apply), input_pad_stick sweeps, and spin_step cadence.

Extracts input_pad_stick from runtime/input_map.c and spin_step (+ scalar register helpers) verbatim from
runtime/frame_rate.c, compiles tests/stick_spin_test.c at -O0 and -O2 with -Wall -Wextra -Werror, and runs it.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def extract_range(source, start_marker, end_fn_sig):
    start = source.index(start_marker)
    fn = extract_function(source, end_fn_sig)
    end = source.index(fn, start) + len(fn)
    return source[start:end]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang') or 'clang')
    ap.add_argument('--source-root', type=Path, default=ROOT)
    args = ap.parse_args()

    im_src = (args.source_root / 'runtime' / 'input_map.c').read_text(encoding='utf-8')
    fr_src = (args.source_root / 'runtime' / 'frame_rate.c').read_text(encoding='utf-8')

    cut = '\n\n'.join([
        extract_function(im_src, 'void input_pad_stick('),
        extract_function(fr_src, 'static float reg_f32('),
        extract_function(fr_src, 'static void set_f32('),
        extract_function(fr_src, 'static float ld_f32('),
        extract_range(fr_src, '#define SPIN_ENTRIES', 'static void spin_step('),
    ])

    with tempfile.TemporaryDirectory(prefix='dkb-stick-spin-') as tmp:
        tmp_dir = Path(tmp)
        (tmp_dir / 'stick_spin_cut.c').write_text(cut + '\n', encoding='utf-8')
        for opt in ('-O0', '-O2'):
            exe = tmp_dir / ('stick_spin' + opt[2:] + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([
                args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror',
                '-I', str(tmp_dir),
                '-I', str(args.source_root / 'runtime'),
                str(args.source_root / 'tests' / 'stick_spin_test.c'),
                '-lm', '-o', str(exe),
            ], check=True)
            res = subprocess.run([str(exe)], capture_output=True, text=True)
            if res.returncode != 0:
                if res.stdout:
                    sys.stderr.write(res.stdout)
                if res.stderr:
                    sys.stderr.write(res.stderr)
                return res.returncode
            if res.stdout:
                print(opt, res.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
