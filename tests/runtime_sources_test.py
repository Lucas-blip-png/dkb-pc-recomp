"""build_game.runtime_sources() honours runtime/sources.txt (a release unpacked over an older one) and ignores its absence.
Run with the repo's Python: tests/runtime_sources_test.py
"""
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402  # type: ignore

with tempfile.TemporaryDirectory() as d:
    rt = Path(d)
    (rt / 'a.c').write_text('')
    (rt / 'b.c').write_text('')
    build_game.RUNTIME = rt
    names = lambda: [p.name for p in build_game.runtime_sources()]  # noqa: E731
    assert names() == ['a.c', 'b.c'], names()
    (rt / 'sources.txt').write_text('a.c\n')
    assert names() == ['a.c'], names()
print('ok')
