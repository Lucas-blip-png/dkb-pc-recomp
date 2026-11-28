"""installer/amiibo.py: the copy of the player's amiibo tag files into the game folder, on pretended files (no tag dump
is in the repository). Only tag-sized .bin files and .nfc files are copied, a folder keeps its subfolders four deep,
and the amiibo folder is never copied into itself."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from installer import amiibo  # noqa: E402


class AmiiboCopy(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.game = self.root / 'game'
        self.game.mkdir()
        self.src = self.root / 'Collection'
        (self.src / 'Series').mkdir(parents=True)
        (self.src / 'Series' / 'a.bin').write_bytes(bytes(540))
        (self.src / 'Series' / 'b.NFC').write_text('Page 0: 00 00 00 00\n')
        (self.src / 'Series' / 'c.bin').write_bytes(bytes(100))      # no tag's size
        (self.src / 'Series' / 'notes.txt').write_text('x')
        (self.src / 'Series' / '.hidden.bin').write_bytes(bytes(540))
        deep = self.src / '1' / '2' / '3' / '4'
        deep.mkdir(parents=True)
        (self.src / '1' / '2' / '3' / 'd.bin').write_bytes(bytes(572))   # four parts: listed
        (deep / 'e.bin').write_bytes(bytes(532))                        # five parts: deeper than the menu looks

    def tearDown(self):
        self.tmp.cleanup()

    def files(self):
        d = amiibo.folder(self.game)
        return sorted(p.relative_to(d).as_posix() for p in d.rglob('*') if p.is_file())

    def test_files(self):
        got = amiibo.add_files(self.game, [self.src / 'Series' / n for n in ('a.bin', 'b.NFC', 'c.bin', 'notes.txt')])
        self.assertEqual(got, (2, 2))
        self.assertEqual(self.files(), ['a.bin', 'b.NFC'])
        self.assertEqual(amiibo.count(self.game), 2)
        self.assertEqual(amiibo.add_files(self.game, [amiibo.folder(self.game) / 'a.bin']), (1, 0))   # onto itself

    def test_folder(self):
        self.assertEqual(amiibo.add_folder(self.game, self.src), (3, 1))   # c.bin is counted as skipped
        self.assertEqual(self.files(), ['Collection/1/2/3/d.bin', 'Collection/Series/a.bin', 'Collection/Series/b.NFC'])
        self.assertEqual(amiibo.add_folder(self.game, amiibo.folder(self.game)), (0, 0))
        self.assertEqual(amiibo.add_folder(self.game, amiibo.folder(self.game) / 'Collection'), (0, 0))
        self.assertEqual(amiibo.count(self.game), 3)

    def test_empty(self):
        self.assertEqual(amiibo.count(self.game), 0)
        self.assertEqual(amiibo.add_files(self.game, []), (0, 0))
        self.assertFalse(amiibo.folder(self.game).exists())


class AmiiboRow(unittest.TestCase):
    def test_methods(self):
        """Every App method installer/gui.py calls on self exists: the amiibo row's Open button once called a helper
        that an edit had removed, which only showed when the button was clicked."""
        import re
        source = (ROOT / 'installer' / 'gui.py').read_text(encoding='utf-8')
        app = source[source.index('class App'):]
        app = app[:app.index('\n# ---- self test')]
        defined = set(re.findall(r'^    def (\w+)\(self', app, re.M))
        called = set(re.findall(r'self\.(\w+)\(', app))
        attributes = set(re.findall(r'self\.(\w+) = ', app))
        self.assertEqual(sorted(called - defined - attributes), [])


if __name__ == '__main__':
    unittest.main()
