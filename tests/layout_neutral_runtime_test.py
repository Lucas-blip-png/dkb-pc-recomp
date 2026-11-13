"""The runtime library must not know the guest layout it will run with.

The library ships inside the installer image and is linked at install time against the modules.c that
tools/build_game.py generates for the user's own game (tools/build_game.py, ninja_file: --runtime-lib). Only that
generated file may name a module base or size: rt_layout is the layout the process actually has.

A compile-time MOD_<NAME>_BASE or MOD_<NAME>_SIZE in runtime/ is therefore a lie as soon as the library is built
ahead of the layout, which is what tools/ci/stub does for the CI image (no game dump there). It cost a shipped
release: memory.c's mods_lo came from the stub layout's 0x10000000 while the install's rt_layout ended at
0x70039f0000, so mem_init reserved 448 GiB instead of 58 MiB and every start died with "the module window is not
free after 100 attempts".

Run: python tests/layout_neutral_runtime_test.py
"""
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
SOURCE_SUFFIXES = ('.c', '.h', '.cpp', '.hpp')
BAKED = re.compile(r'\bMOD_[A-Z0-9_]+_(?:BASE|SIZE)\b')


def sources():
    return [p for p in RUNTIME.rglob('*')
            if p.suffix in SOURCE_SUFFIXES and 'third_party' not in p.parts and p.is_file()]


def baked(paths):
    """Every `path:line: MOD_..._BASE` in the given files, as report lines."""
    out = []
    for p in paths:
        for n, line in enumerate(p.read_text(encoding='utf-8').splitlines(), 1):
            for m in BAKED.finditer(line):
                try:
                    where = p.relative_to(ROOT)
                except ValueError:
                    where = p
                out.append(f'{where}:{n}: {m.group(0)}')
    return out


class LayoutNeutralRuntime(unittest.TestCase):
    def test_runtime_takes_the_layout_from_rt_layout(self):
        hits = baked(sources())
        self.assertEqual(hits, [], 'the prebuilt runtime library must not name a module base or size; it is compiled '
                                   'before the modules.c of the install it runs with. Use rt_layout (dispatch.h):\n'
                                   + '\n'.join(hits))

    def test_the_scanner_finds_a_baked_base(self):
        # The check above is only worth a build step if it sees the shape it forbids.
        output = ROOT / 'build/test/layout-neutral'
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as tmp:
            bad = Path(tmp) / 'memory.c'
            bad.write_text('    mods_lo = MOD_MAIN_BASE;\n    size = MOD_SDK_SIZE;\n', encoding='utf-8')
            self.assertEqual([line.rsplit(': ', 1)[-1] for line in baked([bad])],
                             ['MOD_MAIN_BASE', 'MOD_SDK_SIZE'])


if __name__ == '__main__':
    unittest.main()
