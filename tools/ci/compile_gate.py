"""Compile every runtime translation unit without game files (docs/building.md, "Checks").

    python tools/ci/compile_gate.py --cc clang --sysroot DIR --deps DIR [--undefined]

The file set, flags and per-OS exclusions come from build_game.py so this gate cannot drift from the real build. It
writes a stub layout.h (tools/ci/stub/dkb100/layout.h) holding the four symbols the runtime reads, so the generated C
is not needed. --undefined fails when a plat_* or os_* symbol is used and no object defines it.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402

STUB = ROOT / 'tools' / 'ci' / 'stub' / 'dkb100'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', default='clang', help='C/C++ compiler')
    ap.add_argument('--sysroot', type=Path, help='sysroot (--sysroot=...)')
    ap.add_argument('--deps', type=Path, help='deps dir with include/ and lib/')
    ap.add_argument('--undefined', action='store_true', help='fail on undefined plat_*/os_* symbols')
    ap.add_argument('--out', type=Path, default=Path('/tmp/compile-gate'), help='object output dir')
    args = ap.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stub_include = STUB

    # The runtime sources build_game.py compiles, with Windows-only files excluded on Linux.
    sources = build_game.runtime_sources()
    if os.name != 'nt':
        sources = [s for s in sources if not s.name.endswith('_win.c') and not s.name.endswith('_win.cpp')]
    # Skip translation units that read generated headers (romfs_config.h): those need a real game.
    sources = [s for s in sources if s.name != 'romfs.c']

    cc = args.cc
    flags = list(build_game.GEN_FLAGS) + list(build_game.RT_FLAGS)
    include = [str(ROOT / 'runtime'), str(stub_include),
               str(ROOT / 'runtime' / 'third_party' / 'imgui'),
               str(ROOT / 'runtime' / 'third_party' / 'eglantine')]
    if args.deps:
        include.append(str(args.deps / 'include'))
        if (args.deps / 'include' / 'vma').is_dir():
            include.append(str(args.deps / 'include' / 'vma'))
    if args.sysroot:
        include += [f'--sysroot={args.sysroot}']

    cmd_base = [cc, '-c', *flags, *[f'-I{d}' for d in include]]
    if os.name != 'nt':
        cmd_base += ['-D__linux__']

    failed = 0
    for src in sources:
        obj = out / (src.stem + '.o')
        cmd = list(cmd_base)
        if src.suffix == '.cpp':
            cmd += ['-std=c++20', '-I', str(ROOT / 'runtime' / 'third_party' / 'eglantine')]
        cmd += [str(src), '-o', str(obj)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            failed += 1
            print(f'FAIL {src.name}:\n{r.stderr[-1500:]}')
    print(f'compile gate: {len(sources) - failed}/{len(sources)} files compiled, {failed} failed')

    if failed:
        sys.exit(1)

    if args.undefined:
        # Collect undefined plat_/os_ symbols, then remove those defined by another object.
        undef = set()
        defined = set()
        for src in sources:
            obj = out / (src.stem + '.o')
            r = subprocess.run(['nm', str(obj)], capture_output=True, text=True)
            for line in r.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1] not in ('U', 'w', 'v'):
                    defined.add(parts[2])
            r = subprocess.run(['nm', '-u', str(obj)], capture_output=True, text=True)
            for line in r.stdout.splitlines():
                sym = line.split()[-1] if line.split() else ''
                if sym.startswith('plat_') or sym.startswith('os_'):
                    undef.add(sym)
        missing = undef - defined
        if missing:
            print('undefined plat_/os_ symbols:')
            for s in sorted(missing):
                print('  ' + s)
            sys.exit(1)
        print('undefined check: every plat_/os_ symbol used is defined')

    print('compile gate: OK')


if __name__ == '__main__':
    main()