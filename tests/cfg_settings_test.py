"""Check the settings table (runtime/cfg.c) and, with --exe, every accepted value through the real dkb.exe.

Static: every metas[] key is in known[] and sections[], every default satisfies its own accepts list or range, no
duplicates, every key the menu uses has a row, and documented TOTK_/DKB_ names exist in known[] (warnings only).
Runtime (--exe): a scratch copy of the exe and its DLLs loads a dkb.ini through `dkb.exe --probe-cfg` with
DKB_PROBE_CFG_INI set; the probe prints "key = value" for every row. Run i sets each key to its i-th accepted value
(all keys in one ini), so the whole table takes about twenty launches.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG = (ROOT/'runtime/cfg.c').read_text(encoding='utf-8')
EXE = None
WARN = []


def block(start, end):
    return CFG[CFG.index(start):CFG.index(end, CFG.index(start))]


def parse():
    known = re.findall(r'"([A-Z0-9_]+)"', block('known[] = {', '};'))
    sections = re.findall(r'\{"([A-Z0-9_]+)", "[a-z ]+"\}', block('sections[] = {', '};'))
    body = block('metas[] = {', '};')
    metas = []
    for m in re.finditer(r'\{"([A-Z0-9_]+)", "([^"]*)", (NULL|"[^"]*"), (LIVE|RESTART)\}', body):
        metas.append((m.group(1), m.group(2), None if m.group(3) == 'NULL' else m.group(3)[1:-1], m.group(4)))
    return known, sections, metas


KNOWN, SECTIONS, METAS = parse()


def accepts(values, v):
    if values is None:
        return True
    if '..' in values:
        lo, hi = (float(x) for x in values.split('..'))
        try:
            return lo <= float(v) <= hi
        except ValueError:
            return False
    return v in values.split('|')


def samples(key, default, values):
    """The values to try for one row: every list entry, a range's min, middle and max, one sample for free text."""
    if values is None:
        return [{'WINDOW_SIZE': '1280x720', 'OUTPUT_RES': '1280x720', 'TRACE': 'perf', 'GPU_INDEX': '0',
                 'INPUT_RECORD': 'rec.bin'}.get(key, 'sample')]
    if '..' in values:
        lo, hi = values.split('..')
        mid = (float(lo)+float(hi))/2
        isint = '.' not in values
        mid = str(int(mid)) if isint else f'{mid:.10g}'
        return [lo, mid, hi]
    return values.split('|')


class Static(unittest.TestCase):
    def test_table_parsed(self):
        self.assertGreater(len(METAS), 80)
        self.assertGreater(len(KNOWN), 200)

    def test_no_duplicates(self):
        for name, lst in (('known', KNOWN), ('sections', SECTIONS), ('metas', [m[0] for m in METAS])):
            self.assertEqual([k for k in set(lst) if lst.count(k) > 1], [], f'duplicate in {name}[]')

    def test_metas_in_known_and_sections(self):
        # known[] is the ini-key allow-list; sections[] decides where the menu writes a key the file lacks.
        self.assertEqual([k for k, *_ in METAS if k not in KNOWN], [], 'metas[] key missing from known[]')
        self.assertEqual([k for k, *_ in METAS if k not in SECTIONS], [], 'metas[] key missing from sections[]')

    def test_defaults_satisfy_accepts(self):
        bad = [(k, d, v) for k, d, v, _ in METAS if d and not accepts(v, d)]
        self.assertEqual(bad, [])

    def test_menu_keys_have_rows(self):
        used = set()
        for f in (ROOT/'runtime/ui').glob('*.cpp'):
            used |= set(re.findall(r'"TOTK_([A-Z0-9_]+)"', f.read_text(encoding='utf-8')))
        used -= {'SAVE', 'MENU_TEST', 'MENU_TEST_DIR', 'MENU_TEST_SET'}   # test hooks, not rows
        rows = {m[0] for m in METAS}
        # BIND_* rows are the remapping page's, edited through binds_page.cpp, not metas[]
        self.assertEqual(sorted(k for k in used if k not in rows and not k.startswith('BIND_')), [])

    def test_documented_keys_exist(self):
        seen = {}
        for f in sorted((ROOT/'docs').glob('*.md')):
            for k in re.findall(r'\b(?:TOTK|DKB)_([A-Z][A-Z0-9_]+)', f.read_text(encoding='utf-8')):
                if k not in KNOWN:
                    seen.setdefault(k, set()).add(f.name)
        for k, fs in sorted(seen.items()):
            WARN.append(f'doc names {k} ({", ".join(sorted(fs))}) which is not in known[]')


class Runtime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not EXE:
            raise unittest.SkipTest('needs --exe')
        cls.tmp = tempfile.mkdtemp(prefix='cfg-settings-')
        src = Path(EXE).resolve().parent
        shutil.copy2(EXE, Path(cls.tmp)/'dkb.exe')
        for dll in src.glob('*.dll'):
            shutil.copy2(dll, cls.tmp)
        cls.env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DKB_'))}
        cls.base = cls.dump({})

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(getattr(cls, 'tmp', ''), ignore_errors=True)

    @classmethod
    def dump(cls, ini):
        path = Path(cls.tmp)/'dkb.ini'
        path.write_text(''.join(f'{k.lower()} = {v}\n' for k, v in ini.items()), encoding='utf-8')
        env = dict(cls.env, DKB_PROBE_CFG_INI=str(path))
        r = subprocess.run([str(Path(cls.tmp)/'dkb.exe'), '--probe-cfg'], env=env, cwd=cls.tmp, capture_output=True,
                           text=True, timeout=120)
        out = r.stdout+r.stderr
        assert r.returncode == 0, f'exit {r.returncode}: {out}'
        for line in out.splitlines():
            if re.search(r"unknown key|rejected|invalid|not accepted", line, re.I):
                raise AssertionError(f'warning: {line}')
        return dict(re.findall(r'^([A-Z0-9_]+) = (.*)$', r.stdout, re.M))

    def test_baseline_has_every_row(self):
        self.assertEqual(sorted(self.base), sorted(m[0] for m in METAS))

    def test_every_accepted_value_echoes(self):
        plan = {k: samples(k, d, v) for k, d, v, _ in METAS}
        failures = []
        for i in range(max(map(len, plan.values()))):
            ini = {k: s[i % len(s)] for k, s in plan.items()}
            got = self.dump(ini)
            for k, v in ini.items():
                ok = float(got[k]) == float(v) if '..' in (next(m[2] for m in METAS if m[0] == k) or '') else got[k] == v
                if not ok:
                    failures.append(f'{k} = {v!r} read back as {got[k]!r}')
        self.assertEqual(failures, [])
        print(f'runtime: {len(plan)} keys, {sum(len(s) for s in plan.values())} values', file=sys.stderr)

    def test_dkb_prefix_in_file(self):
        # The issues ask for DKB_ names in dkb.ini; the file takes them as the environment does.
        k, d, v, _ = METAS[0]
        val = next(s for s in samples(k, d, v) if s != self.base[k])
        self.assertEqual(self.dump({'DKB_'+k: val})[k], val)

    def test_invalid_range_values_fall_back_to_default(self):
        # Lists are not validated by cfg.c (a bad entry reaches the consumer as is); a range row's number must parse.
        ranged = [k for k, _, v, _ in METAS if v and '..' in v]
        got = self.dump({k: 'banana' for k in ranged})
        self.assertEqual([k for k in ranged if got[k] != self.base[k]], [])


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', help='dkb.exe for the runtime checks (it is copied to a scratch folder)')
    ap.add_argument('--cc')   # accepted for the other tests' command line; no C helper is needed
    args, rest = ap.parse_known_args()
    EXE = args.exe
    res = unittest.main(argv=[sys.argv[0]]+rest, exit=False).result
    for w in WARN:
        print('warning:', w, file=sys.stderr)
    sys.exit(not res.wasSuccessful())
