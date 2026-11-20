"""Detach a private Windows console and keep redirected standard I/O."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/tests/windows-console')
    args = parser.parse_args()
    out = args.out.resolve()
    out.relative_to((ROOT / 'build').resolve())
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'windows_console_test.exe'
    rt = ROOT / 'runtime'
    subprocess.run([
        args.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Wno-missing-field-initializers', '-Wno-sign-compare', '-Wno-unused-parameter', '-I', str(rt),
        str(ROOT / 'tests/windows_console_test.c'), str(rt / 'platform/win32.c'), str(rt / 'platform/common.c'),
        '-o', str(exe), '-lwinmm', '-lbcrypt', '-lpdh', '-lpsapi', '-lshell32', '-lrstrtmgr',
    ], check=True, timeout=120)

    startup = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=subprocess.SW_HIDE)
    run = subprocess.run([exe], input='redirected input\n', capture_output=True, text=True, startupinfo=startup,
                         creationflags=subprocess.CREATE_NEW_CONSOLE, timeout=10)
    if (run.returncode, run.stdout, run.stderr) != (0, 'redirected output\n', 'redirected error\n'):
        raise RuntimeError(f'redirected pipes: {run.returncode=}, {run.stdout=}, {run.stderr=}')

    input_file = out / 'input.txt'
    output_file = out / 'output.txt'
    error_file = out / 'error.txt'
    input_file.write_text('redirected input\n', encoding='ascii')
    with (input_file.open() as stdin, output_file.open('w') as stdout, error_file.open('w') as stderr):
        subprocess.run([exe], stdin=stdin, stdout=stdout, stderr=stderr, startupinfo=startup,
                       creationflags=subprocess.CREATE_NEW_CONSOLE, check=True, timeout=10)
    if (output_file.read_text(), error_file.read_text()) != ('redirected output\n', 'redirected error\n'):
        raise RuntimeError('redirected files did not receive the expected output')
    print('windows_console: private console detached and redirected streams preserved')


if __name__ == '__main__':
    main()
