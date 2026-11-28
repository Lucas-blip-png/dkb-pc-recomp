"""Update tests: the notice, the release manifest, the check, the download, the unpack, the switch of versions and the
launcher's own replacement. Every lookup goes through an injected opener; nothing reaches the network.

    .venv\\Scripts\\python tests\\installer_update_test.py
"""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from installer import update  # noqa: E402
from installer.update import UpdateError  # noqa: E402


def release(tag, **extra):
    return lambda: {'tag_name': tag, 'html_url': f'https://example.invalid/{tag}', **extra}


def broken():
    raise OSError('offline')


class NoticeTest(unittest.TestCase):
    def test_newer(self):
        self.assertEqual(update.newer_release('0.2.0+33f8bbc', release('v0.3.0')), ('v0.3.0', 'https://example.invalid/v0.3.0'))
        self.assertIsNotNone(update.newer_release('0.2.0-dev+abc', release('v0.2.1')))
        self.assertIsNotNone(update.newer_release('0.9.0', release('v0.10.0')))   # numeric, not text, order

    def test_equal_and_older(self):
        self.assertIsNone(update.newer_release('0.2.0+33f8bbc', release('v0.2.0')))
        self.assertIsNone(update.newer_release('0.2.0', release('v0.1.9')))

    def test_failures_return_none(self):
        self.assertIsNone(update.newer_release('0.2.0', broken))
        self.assertIsNone(update.newer_release('0.2.0', lambda: None))
        self.assertIsNone(update.newer_release('0.2.0', lambda: {'oops': 1}))
        self.assertIsNone(update.newer_release('unknown', release('v0.3.0')))
        with patch.object(update.OPENER, 'open', side_effect=OSError) as opened:
            self.assertIsNone(update.fetch_latest())
        self.assertEqual(opened.call_args[0][0].full_url, update.API)
        self.assertEqual(opened.call_args[0][0].get_header('User-agent'), update.USER_AGENT)   # generic, nothing else

    def test_no_clear_text(self):
        with self.assertRaises(UpdateError):
            update.http_open('http://github.com/x')
        handler = update.HttpsRedirects()
        with self.assertRaises(UpdateError):
            handler.redirect_request(None, None, 302, 'Found', {}, 'http://objects.example.invalid/zip')

    def test_prerelease_ignored(self):
        self.assertIsNone(update.newer_release('0.2.0', release('v0.3.0-rc1')))
        self.assertIsNone(update.newer_release('0.2.0', release('v0.3.0', prerelease=True)))

    def test_switch(self):
        with patch.dict(os.environ, {'DKB_NO_UPDATE_CHECK': '1'}):
            self.assertTrue(update.disabled())
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('DKB_NO_UPDATE_CHECK', None)
            self.assertFalse(update.disabled())
            self.assertTrue(update.disabled(True))

    def test_text_in_both_languages(self):
        for lang in ('en', 'ru'):
            text = json.loads((ROOT / 'installer' / 'text' / f'{lang}.json').read_text(encoding='utf-8'))
            self.assertIn('{tag}', text['ui.update_available'])
            self.assertIn('{url}', text['warn.update_available'])
        en = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        ru = json.loads((ROOT / 'installer' / 'text' / 'ru.json').read_text(encoding='utf-8'))
        self.assertNotEqual(en['ui.update_available'], ru['ui.update_available'])

    def test_every_update_error_has_text(self):
        source = (ROOT / 'installer' / 'update.py').read_text(encoding='utf-8')
        ids = set(__import__('re').findall(r"UpdateError\('(update\.\w+)'", source))
        for lang in ('en', 'ru'):
            text = json.loads((ROOT / 'installer' / 'text' / f'{lang}.json').read_text(encoding='utf-8'))
            self.assertEqual(sorted(ids - set(text)), [], lang)


class VersionTest(unittest.TestCase):
    def test_compare(self):
        self.assertEqual(update.compare('0.4.1+ac46642', 'v0.4.1'), 0)   # the commit does not count
        self.assertEqual(update.compare('0.4', '0.4.0'), 0)
        self.assertEqual(update.compare('v0.10.0', '0.9.9'), 1)
        self.assertEqual(update.compare('0.4.1', '0.4.2+abc'), -1)
        self.assertEqual(update.compare('0.4.1-dev+abc', '0.4.1'), 0)
        self.assertIsNone(update.compare('unknown', '0.4.1'))
        self.assertIsNone(update.compare('v0.5.0-rc1', '0.4.1'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Files:
    """A release in a temporary folder: the tools zip with DKB-Setup/ inside, the launcher exe, the manifest."""

    def __init__(self, folder, version='0.5.0+abc1234', tag='v0.5.0', extra_members=(), version_text=None):
        self.folder = Path(folder)
        self.version, self.tag = version, tag
        self.launcher_bytes = b'MZ launcher ' + version.encode()
        self.zip = self.folder / f'DKB-Setup-{version}-windows.zip'
        python = update.version_python(Path('DKB-Setup')).as_posix()
        with zipfile.ZipFile(self.zip, 'w') as z:
            z.writestr('DKB-Setup/VERSION', (version_text or version) + '\n')
            z.writestr(python, b'python')
            z.writestr('DKB-Setup/installer/gui.py', b'# window')
            z.writestr(f'DKB-Setup/{update.LAUNCHER_EXE}', self.launcher_bytes)
            z.writestr('DKB-Setup/tools/x/y.txt', b'deep')
            for name, data in extra_members:
                z.writestr(name, data)
        self.launcher = self.folder / 'DKB-Launcher.exe'
        self.launcher.write_bytes(self.launcher_bytes)
        self.text = update.write_manifest(version, tag, self.zip, 'DKB-Setup', self.launcher)
        self.manifest = update.parse_manifest(self.text)
        self.zip_bytes = self.zip.read_bytes()

    def api(self, **change):
        info = {'tag_name': self.tag, 'html_url': f'https://github.com/{update.REPO}/releases/tag/{self.tag}',
                'body': '- Faster\n- Fixed a crash', 'draft': False, 'prerelease': False,
                'assets': [{'name': update.MANIFEST, 'size': len(self.text)},
                           {'name': self.zip.name, 'size': len(self.zip_bytes), 'digest': 'sha256:' + sha(self.zip_bytes)},
                           {'name': self.launcher.name, 'size': len(self.launcher_bytes)}]}
        info.update(change)
        return json.dumps(info).encode()


class Opener:
    """URL -> bytes, an exception, or a list of answers taken one per request. Records every request."""

    def __init__(self, answers):
        self.answers, self.requests = answers, []

    def __call__(self, url, accept=None, timeout=None):
        self.requests.append(url)
        answer = self.answers.get(url, OSError(f'no route to {url}'))
        if isinstance(answer, list):
            answer = answer.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer if hasattr(answer, 'read') else io.BytesIO(answer)


class Interrupted(io.BytesIO):
    """A response that ends with a reset connection after `cut` bytes."""

    def __init__(self, data, cut):
        super().__init__(data[:cut])

    def read(self, n=-1):
        block = super().read(n)
        if not block:
            raise ConnectionResetError('connection reset')
        return block


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.files = Files(self.work.name)

    def tearDown(self):
        self.work.cleanup()

    def test_written_manifest_reads_back(self):
        m = self.files.manifest
        self.assertEqual((m.version, m.tag, m.tools.top), ('0.5.0+abc1234', 'v0.5.0', 'DKB-Setup'))
        self.assertEqual(m.tools.sha256, sha(self.files.zip_bytes))
        self.assertEqual(m.launcher.size, len(self.files.launcher_bytes))
        self.assertTrue(m.tools.url('v0.5.0').endswith('/v0.5.0/DKB-Setup-0.5.0%2Babc1234-windows.zip'))
        self.assertEqual(update.parse_manifest(self.files.text.encode() + b'\nlater-field x\n'), m)   # newer fields

    def test_tampered_manifests_are_refused(self):
        good = self.files.text
        tools = next(line for line in good.splitlines() if line.startswith('tools '))
        cases = {
            'header': good.replace('dkb-release 1', 'dkb-release 2'),
            'checksum': good.replace(tools.split()[1], 'Z' * 64),
            'size': good.replace(tools, tools.replace(tools.split()[2], '0')),
            'path': good.replace(tools.split()[3], '../evil.zip'),
            'top': good.replace(' DKB-Setup\n', ' ..\n'),
            'version': good.replace('tag v0.5.0', 'tag v0.6.0'),
            'twice': good + 'tag v0.5.0\n',
            'binary': good.encode() + b'\xff',
        }
        for name, text in cases.items():
            with self.subTest(name), self.assertRaises(UpdateError) as e:
                update.parse_manifest(text)
            self.assertEqual(e.exception.id, 'update.manifest_bad')

    def test_truncated_manifests_are_refused(self):
        good = self.files.text
        for cut in (0, 10, good.index('tools'), good.index('launcher') + 12, len(good) - 30):
            with self.subTest(cut=cut), self.assertRaises(UpdateError):
                update.parse_manifest(good[:cut])


def find_clang():
    """MSYS2 UCRT64's clang (docs/building.md) or one on PATH; None skips the tests that compile the launcher's C."""
    for candidate in (r'C:\msys64\ucrt64\bin\clang.exe', shutil.which('clang')):
        if candidate and Path(candidate).is_file():
            return candidate
    return None


CLANG = find_clang() if os.name == 'nt' else None
LAUNCHER_SRC = ROOT / 'installer' / 'launcher'


@unittest.skipUnless(CLANG, 'needs clang (C:\\msys64\\ucrt64\\bin) on Windows')
class ManifestParsersTest(unittest.TestCase):
    """installer/launcher/manifest.c, the first install's reader, must accept exactly what parse_manifest accepts, and
    read the same fields: both get the same good and bad manifests."""

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.driver = Path(cls.work.name) / 'manifest_driver.exe'
        subprocess.run([CLANG, '-O1', '-Wall', '-Werror', '-o', str(cls.driver), str(ROOT / 'tests' / 'launcher_manifest_driver.c'),
                        str(LAUNCHER_SRC / 'manifest.c')], check=True, capture_output=True,
                       env={**os.environ, 'PATH': str(Path(CLANG).parent) + os.pathsep + os.environ.get('PATH', '')})
        cls.files = Files(cls.work.name)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def c_parse(self, texts):
        paths = []
        for i, data in enumerate(texts):
            path = Path(self.work.name) / f'm{i}.txt'
            path.write_bytes(data)
            paths.append(str(path))
        out = subprocess.run([str(self.driver), *paths], capture_output=True, text=True, check=True).stdout
        return out.splitlines()

    @staticmethod
    def py_parse(data):
        try:
            m = update.parse_manifest(data)
        except UpdateError:
            return 'bad'
        return (f'ok {m.version} {m.tag} {m.tools.sha256} {m.tools.size} {m.tools.name} {m.tools.top} '
                f'{m.launcher.sha256} {m.launcher.size} {m.launcher.name}')

    def corpus(self):
        good = self.files.text
        tools = next(line for line in good.splitlines() if line.startswith('tools '))
        launcher = next(line for line in good.splitlines() if line.startswith('launcher '))
        sha, size, name, top = tools.split()[1:]
        lines = good.splitlines()
        cases = {
            'good': good, 'crlf': good.replace('\n', '\r\n'), 'no final newline': good.rstrip('\n'),
            'blank lines after the header': good.replace('\n', '\n\n\n', 2), 'trailing blank lines': good + '\n \n\t\n',
            'shuffled fields': '\n'.join([lines[0], *reversed(lines[1:])]) + '\n',
            'tabs and odd spaces': good.replace(' ', '\t').replace('tag\t', 'tag\x1c\x0b'),
            'spaces around the header': '  dkb-release   1 \n' + good.split('\n', 1)[1],
            'lone CR inside a line': good.replace('tag v', 'tag\rv'),
            'unknown field': good + 'later-field x y z\n', 'unknown field with NUL': good + 'x\x00y 1\n',
            'upper-case suffix': good.replace(name, name[:-4] + '.ZIP'),
            'size with leading zeros': good.replace(f' {size} ', f' 000{size} '),
            'version 0.5 for tag v0.5.0': good.replace('version 0.5.0+abc1234', 'version 0.5+abc1234'),
            'version 00.5.0': good.replace('version 0.5.0+abc1234', 'version 00.5.0+abc1234'),
            'tag v0.5.0.0': good.replace('tag v0.5.0', 'tag v0.5.0.0'),
            'blank line before the header': '\n' + good, 'space line before the header': ' \n' + good,
            'empty': '', 'header only': 'dkb-release 1\n', 'header 2': good.replace('dkb-release 1', 'dkb-release 2'),
            'header 01': good.replace('dkb-release 1', 'dkb-release 01'),
            'header with more': good.replace('dkb-release 1', 'dkb-release 1 x'),
            'header not first': good.replace('dkb-release 1\n', '') + 'dkb-release 1\n',
            'no launcher line': good.replace(launcher + '\n', ''), 'no tools line': good.replace(tools + '\n', ''),
            'launcher with a fourth word': good.replace(launcher, launcher + ' extra'),
            'tools with a fifth word': good.replace(tools, tools + ' extra'),
            'version with two words': good.replace('version 0.5.0+abc1234', 'version 0.5.0 +abc1234'),
            'hash too long': good.replace(sha, sha + 'a'), 'hash digit run-on': good.replace(f'{sha} {size}', f'{sha}5 {size}'),
            'hash too short': good.replace(sha, sha[:-1]), 'hash upper case': good.replace(sha, sha.upper()),
            'size zero': good.replace(f' {size} ', ' 0 '), 'size signed': good.replace(f' {size} ', f' +{size} '),
            'size negative': good.replace(f' {size} ', ' -5 '),
            'size over the limit': good.replace(f' {size} ', f' {(8 << 30) + 1} '),
            'size at the limit': good.replace(f' {size} ', f' {8 << 30} '),
            'size huge': good.replace(f' {size} ', ' 99999999999999999999999999 '),
            'launcher over 16 MiB': good.replace(launcher.split()[2], str((16 << 20) + 1)),
            'name with a folder': good.replace(name, 'x/' + name), 'name starting with _': good.replace(name, '_' + name),
            'name starting with +': good.replace(name, '+' + name), 'name not a zip': good.replace(name, name + '.exe'),
            'name of 129 characters': good.replace(name, 'a' * 125 + '.zip'),
            'name of 128 characters': good.replace(name, 'a' * 124 + '.zip'),
            'top ..': good.replace(f' {top}\n', ' ..\n'), 'top with a colon': good.replace(f' {top}\n', ' C:x\n'),
            'tag of another version': good.replace('tag v0.5.0', 'tag v0.6.0'),
            'tag pre-release': good.replace('tag v0.5.0', 'tag v0.5.0-rc1'),
            'tag of five numbers': good.replace('tag v0.5.0', 'tag v0.5.0.0.0'),
            'tag 32 characters': good.replace('tag v0.5.0', 'tag v0.5.0.' + '0' * 26).replace(
                'version 0.5.0+abc1234', 'version 0.5.0.' + '0' * 26),
            'version 64 characters': good.replace('version 0.5.0+abc1234', 'version 0.5.0+' + 'a' * 50),
            'version 63 characters': good.replace('version 0.5.0+abc1234', 'version 0.5.0+' + 'a' * 49),
            'version plus nothing': good.replace('version 0.5.0+abc1234', 'version 0.5.0+'),
            'version plus dash': good.replace('version 0.5.0+abc1234', 'version 0.5.0+abc-1'),
            'version dev': good.replace('version 0.5.0+abc1234', 'version 0.5.0-dev'),
            'version NUL': good.replace('version 0.5.0+abc1234', 'version 0.5.0\x00'),
            'tag twice': good + 'tag v0.5.0\n', 'unknown field twice': good + 'x 1\nx 2\n',
        }
        data = {k: v.encode('latin-1') for k, v in cases.items()}
        data['byte above 127'] = good.encode() + b'\xff\n'
        data['byte above 127 in an unknown field'] = good.encode() + b'note caf\xc3\xa9\n'
        return data

    def test_same_verdict_and_fields(self):
        corpus = self.corpus()
        c = dict(zip(corpus, self.c_parse(list(corpus.values()))))
        accepted = []
        for name, data in corpus.items():
            with self.subTest(name):
                self.assertEqual(c[name], self.py_parse(data))
            if c[name] != 'bad':
                accepted.append(name)
        # the corpus holds both kinds, so the agreement is not one of refusing everything
        self.assertIn('good', accepted)
        self.assertNotIn('no launcher line', accepted)
        self.assertGreater(len(accepted), 10)
        self.assertGreater(len(corpus) - len(accepted), 30)

    def test_same_tags(self):
        tags = ['v1', 'v0.4.1', 'v1.2.3.4', 'v1.2.3.4.5', 'v', 'v01', 'v1.', 'v.1', 'V1', 'v1-rc1', 'v1..2', 'v1 ',
                '1.2', 'v' + '1' * 30, 'v' + '1' * 31, '..\\..\\evil', '']
        out = subprocess.run([str(self.driver), '--tag', *tags], capture_output=True, text=True, check=True)
        self.assertEqual(out.stdout.split('\n')[:len(tags)], ['1' if update.TAG_RE.fullmatch(t) else '0' for t in tags])


@unittest.skipUnless(CLANG, 'needs clang (C:\\msys64\\ucrt64\\bin) on Windows')
class LauncherBuildTest(unittest.TestCase):
    """The launcher builds tools/make_release.py makes: the release has no test overrides, the TotK setup none of the
    download code."""

    def build(self, *flags_and_sources, name, game='dkb'):
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        work = Path(self.work.name) / game
        make_release.launcher_header(work / 'launcher_text.h', game)
        out = work / name
        subprocess.run([CLANG, '-O1', '-mwindows', '-municode', '-Wall', '-Werror', '-Wno-missing-field-initializers',
                        '-fuse-ld=lld', '-I', str(work), *map(str, flags_and_sources), '-o', str(out)], check=True,
                       capture_output=True,
                       env={**os.environ, 'PATH': str(Path(CLANG).parent) + os.pathsep + os.environ.get('PATH', '')})
        self.exe = out
        return out.read_bytes()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)

    def test_release_build_has_no_test_overrides(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        sources = make_release.launcher_sources('dkb')
        release = self.build(*sources, name='release.exe')
        test = self.build('-DDKB_LAUNCHER_TEST', *sources, name='test.exe')
        marker = 'DKB_LAUNCHER_'.encode('utf-16-le')
        self.assertEqual(release.count(marker), 0)   # DKB_LAUNCHER_BASE, _MANIFEST, _DOWNLOAD, _LOG are compiled out
        self.assertGreater(test.count(marker), 3)
        self.assertIn(b'WINHTTP.DLL', release.upper())

    def test_totk_setup_has_no_download_code(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        exe = self.build(*make_release.launcher_sources('totk'), name='dkb.exe', game='totk').upper()
        self.assertNotIn(b'WINHTTP.DLL', exe)
        self.assertNotIn(b'BCRYPT.DLL', exe)
        self.assertNotIn('dkb-release.txt'.upper().encode('utf-16-le'), exe)

    def test_dependent_load_flags_are_read_back(self):
        """make_release.check_launcher reads the flag out of the linked exe, so a linker that dropped the option
        stops the build instead of publishing a launcher that loads a winhttp.dll from its own folder."""
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        sources = make_release.launcher_sources('dkb')
        self.build('-Wl,--dependent-load-flag=0x800', *sources, name='flag.exe')
        self.assertEqual(make_release.dependent_load_flags(self.exe), 0x800)
        make_release.check_launcher(self.exe)
        self.build(*sources, name='plain.exe')
        self.assertEqual(make_release.dependent_load_flags(self.exe), 0)
        with self.assertRaises(SystemExit):
            make_release.check_launcher(self.exe)


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.files = Files(self.work.name)
        f = self.files
        self.answers = {update.API: f.api(), update.release_url(f.tag, update.MANIFEST): f.text.encode()}

    def tearDown(self):
        self.work.cleanup()

    def test_newer_release_is_offered_with_its_notes(self):
        opener = Opener(self.answers)
        offer = update.check('0.4.1+ac46642', opener)
        self.assertEqual((offer.tag, offer.version, offer.direction), ('v0.5.0', '0.5.0+abc1234', 1))
        self.assertIn('Fixed a crash', offer.notes)
        self.assertTrue(update.may_install(offer))
        self.assertTrue(all(u.startswith('https://') for u in opener.requests))
        self.assertEqual(len(opener.requests), 2)   # the API and the manifest: nothing is downloaded before Update

    def test_same_and_older(self):
        self.assertEqual(update.check('0.5.0+abc1234', Opener(self.answers)).direction, 0)
        self.assertFalse(update.may_install(update.check('0.5.0+other', Opener(self.answers))))
        older = update.check('0.6.0', Opener(self.answers))
        self.assertEqual(older.direction, -1)
        self.assertFalse(update.may_install(older))                         # no downgrade
        self.assertTrue(update.may_install(older, allow_downgrade=True))    # unless asked

    def test_digest_or_size_disagreeing_with_the_manifest(self):
        f = self.files
        for assets in ([{'name': update.MANIFEST, 'size': 1},
                        {'name': f.zip.name, 'size': len(f.zip_bytes), 'digest': 'sha256:' + '0' * 64},
                        {'name': f.launcher.name, 'size': len(f.launcher_bytes)}],
                       [{'name': update.MANIFEST, 'size': 1}, {'name': f.zip.name, 'size': 5},
                        {'name': f.launcher.name, 'size': len(f.launcher_bytes)}],
                       [{'name': update.MANIFEST, 'size': 1}, {'name': f.zip.name, 'size': len(f.zip_bytes)}]):
            self.answers[update.API] = f.api(assets=assets)
            with self.subTest(assets=len(assets)), self.assertRaises(UpdateError) as e:
                update.check('0.4.1', Opener(self.answers))
            self.assertEqual(e.exception.id, 'update.manifest_mismatch')

    def test_manifest_of_another_tag(self):
        self.answers[update.API] = self.files.api(tag_name='v0.5.1')
        self.answers[update.release_url('v0.5.1', update.MANIFEST)] = self.files.text.encode()
        with self.assertRaises(UpdateError) as e:
            update.check('0.4.1', Opener(self.answers))
        self.assertEqual(e.exception.id, 'update.manifest_mismatch')

    def test_api_down_falls_back_to_the_download_link(self):
        self.answers[update.API] = OSError('HTTP 403 rate limit')
        self.answers[update.LATEST_MANIFEST] = self.files.text.encode()
        offer = update.check('0.4.1', Opener(self.answers))
        self.assertEqual((offer.tag, offer.direction, offer.notes), ('v0.5.0', 1, ''))
        self.assertTrue(update.may_install(offer))

    def test_offline(self):
        with self.assertRaises(UpdateError) as e:
            update.check('0.4.1', Opener({}))
        self.assertEqual(e.exception.id, 'update.offline')

    def test_release_without_manifest_offers_only_its_page(self):
        self.answers[update.API] = self.files.api(assets=[])
        offer = update.check('0.4.1', Opener(self.answers))
        self.assertIsNone(offer.manifest)
        self.assertFalse(update.may_install(offer))
        self.assertEqual(offer.direction, 1)

    def test_prerelease_is_not_offered(self):
        self.answers[update.API] = self.files.api(prerelease=True)
        with self.assertRaises(UpdateError):
            update.check('0.4.1', Opener(self.answers))

    def test_release_candidate_tag_is_not_offered(self):
        """A v*-rc* tag the workflow publishes (as a pre-release) is never an update, even marked as the latest"""
        for flags in ({'prerelease': True}, {}):
            self.answers[update.API] = self.files.api(tag_name='v0.5.0-rc1', **flags)
            self.answers[update.release_url('v0.5.0-rc1', update.MANIFEST)] = self.files.text.encode()
            with self.subTest(flags=flags), self.assertRaises(UpdateError) as e:
                update.check('0.4.1', Opener(self.answers))
            self.assertEqual(e.exception.id, 'update.offline')
            self.assertIsNone(update.newer_release('0.4.1', lambda: json.loads(self.files.api(tag_name='v0.5.0-rc1'))))


class DownloadTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.files = Files(self.work.name)
        self.url = self.files.manifest.tools.url(self.files.tag)
        self.out = Path(self.work.name) / 'downloads'

    def tearDown(self):
        self.work.cleanup()

    def get(self, answer, **kw):
        opener = Opener({self.url: answer})
        path = update.download(self.files.manifest.tools, self.files.tag, self.out, opener, sleep=lambda s: None, **kw)
        return path, opener

    def leftovers(self):
        return sorted(p.name for p in self.out.iterdir()) if self.out.exists() else []

    def test_good_download_is_kept_under_its_checksum(self):
        seen = []
        path, _ = self.get(self.files.zip_bytes, progress=lambda done, total: seen.append((done, total)))
        self.assertEqual(path.name, self.files.manifest.tools.sha256 + '.zip')
        self.assertEqual(path.read_bytes(), self.files.zip_bytes)
        self.assertEqual(seen[-1], (len(self.files.zip_bytes),) * 2)
        self.assertEqual(self.leftovers(), [path.name])
        again, opener = self.get(OSError('must not be asked'))   # a checked file is used again
        self.assertEqual((again, opener.requests), (path, []))

    def test_tampered_download_is_deleted(self):
        bad = bytearray(self.files.zip_bytes)
        bad[100] ^= 1
        with self.assertRaises(UpdateError) as e:
            self.get(bytes(bad))
        self.assertEqual(e.exception.id, 'update.checksum')
        self.assertEqual(self.leftovers(), [])

    def test_truncated_download_fails_after_the_retries(self):
        cut = self.files.zip_bytes[:len(self.files.zip_bytes) // 2]
        with self.assertRaises(UpdateError) as e:
            self.get([io.BytesIO(cut), io.BytesIO(cut), io.BytesIO(cut)])
        self.assertEqual(e.exception.id, 'update.download_failed')
        self.assertEqual(self.leftovers(), [])

    def test_interrupted_download_is_tried_again(self):
        data = self.files.zip_bytes
        path, opener = self.get([Interrupted(data, len(data) // 3), data])
        self.assertEqual(len(opener.requests), 2)
        self.assertEqual(sha(path.read_bytes()), self.files.manifest.tools.sha256)

    def test_longer_than_the_manifest_says(self):
        with self.assertRaises(UpdateError) as e:
            self.get(self.files.zip_bytes + b'more')
        self.assertEqual(e.exception.id, 'update.download_size')
        self.assertEqual(self.leftovers(), [])

    def test_cancel(self):
        with self.assertRaises(UpdateError) as e:
            self.get(self.files.zip_bytes, cancelled=lambda: True)
        self.assertEqual(e.exception.id, 'update.cancelled')
        self.assertEqual(self.leftovers(), [])


class StageTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.base = Path(self.work.name) / 'base with space и юникод'
        (self.base / 'versions').mkdir(parents=True)
        update.write_pointer(self.base, 'current', 'v0.4.1')
        (self.base / 'versions' / 'v0.4.1').mkdir()

    def tearDown(self):
        self.work.cleanup()

    def stage(self, files):
        return update.stage(files.zip, self.base, files.manifest)

    def assert_untouched(self):
        names = sorted(p.name for p in (self.base / 'versions').iterdir())
        self.assertEqual(names, ['current.txt', 'v0.4.1'])
        self.assertEqual(update.read_pointer(self.base, 'current'), 'v0.4.1')

    def test_good_zip_becomes_a_version_folder(self):
        files = Files(self.work.name)
        folder = self.stage(files)
        self.assertEqual(folder, self.base / 'versions' / 'v0.5.0')
        self.assertTrue(update.complete(folder, files.version))
        self.assertEqual((folder / 'tools' / 'x' / 'y.txt').read_bytes(), b'deep')
        self.assertFalse((self.base / 'versions' / 'v0.5.0.part').exists())
        self.assertEqual(update.read_pointer(self.base, 'current'), 'v0.4.1')   # only the new window's start switches
        self.assertEqual(self.stage(files), folder)   # a complete folder is used again

    def test_member_leaving_the_folder_is_refused(self):
        for name in ('DKB-Setup/../../evil.txt', 'Other/file', '/abs/file', 'DKB-Setup/a\\..\\b'):
            files = Files(self.work.name, extra_members=[(name, b'x')])
            with self.subTest(name), self.assertRaises(UpdateError) as e:
                self.stage(files)
            self.assertEqual(e.exception.id, 'update.zip_bad')
            self.assert_untouched()
            self.assertFalse((Path(self.work.name) / 'evil.txt').exists())

    def test_names_windows_would_change_are_refused(self):
        """Windows drops a trailing dot or space, opens a device for CON or NUL.txt and forbids <>:"|?*: such a member
        would be written under another name than the zip gives, or not at all."""
        for name in ('DKB-Setup/tools/x.', 'DKB-Setup/tools/x ', 'DKB-Setup/tools/.. /y', 'DKB-Setup/CON',
                     'DKB-Setup/tools/nul.txt', 'DKB-Setup/Com1.log', 'DKB-Setup/lpt¹', 'DKB-Setup/a<b',
                     'DKB-Setup/a|b', 'DKB-Setup/a?b', 'DKB-Setup/a*b', 'DKB-Setup/a"b', 'DKB-Setup/a\x01b',
                     'DKB-Setup/conin$'):
            files = Files(self.work.name, extra_members=[(name, b'x')])
            with self.subTest(name), self.assertRaises(UpdateError) as e:
                self.stage(files)
            self.assertEqual(e.exception.id, 'update.zip_bad')
            self.assert_untouched()
        for fine in ('DKB-Setup/console.txt', 'DKB-Setup/CONFIG', 'DKB-Setup/com10', 'DKB-Setup/.hidden', 'DKB-Setup/a b'):
            self.assertIsNotNone(update.member_path(fine, 'DKB-Setup'), fine)

    def test_zip_that_unpacks_past_the_limit_is_refused(self):
        files = Files(self.work.name, extra_members=[('DKB-Setup/big.bin', b'\0' * (1 << 20))])
        with patch.object(update, 'UNPACK_FLOOR', 1 << 19), patch.object(update, 'UNPACK_RATIO', 0):   # a 512 KiB cap
            with self.assertRaises(UpdateError) as e:
                self.stage(files)
        self.assertEqual(e.exception.id, 'update.zip_large')
        self.assert_untouched()
        self.assertEqual(update.unpack_limit(update.Asset('0' * 64, 1, 'a.zip')), update.UNPACK_FLOOR)
        self.assertEqual(update.unpack_limit(update.Asset('0' * 64, 8 << 30, 'a.zip')), update.UNPACK_LIMIT)
        self.assertEqual(update.unpack_limit(update.Asset('0' * 64, 700 << 20, 'a.zip')), 20 * (700 << 20))

    def test_zip_of_another_version_is_rolled_back(self):
        files = Files(self.work.name, version_text='0.4.9+0000000')
        with self.assertRaises(UpdateError) as e:
            self.stage(files)
        self.assertEqual(e.exception.id, 'update.stage_failed')
        self.assert_untouched()

    def test_damaged_zip_is_rolled_back(self):
        files = Files(self.work.name)
        files.zip.write_bytes(files.zip_bytes[:len(files.zip_bytes) // 2])
        with self.assertRaises(UpdateError):
            self.stage(files)
        self.assert_untouched()

    def test_incomplete_folder_of_the_same_tag_is_replaced(self):
        files = Files(self.work.name)
        stale = self.base / 'versions' / 'v0.5.0'
        stale.mkdir()
        (stale / 'leftover').write_text('x')
        folder = self.stage(files)
        self.assertTrue(update.complete(folder, files.version))
        self.assertFalse((folder / 'leftover').exists())


class FakeProcess:
    def __init__(self, on_poll):
        self.on_poll, self.polls = on_poll, 0

    def poll(self):
        self.polls += 1
        return self.on_poll(self.polls)


class SwitchTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.base = Path(self.work.name)
        for tag in ('v0.3.0', 'v0.4.1', 'v0.5.0'):
            (self.base / 'versions' / tag).mkdir(parents=True)
        update.write_pointer(self.base, 'current', 'v0.4.1')
        update.write_pointer(self.base, 'previous', 'v0.3.0')

    def tearDown(self):
        self.work.cleanup()

    def test_confirmed_start_switches_and_prunes(self):
        self.assertTrue(update.confirm_start(self.base, 'v0.5.0'))
        self.assertEqual(update.read_pointer(self.base, 'current'), 'v0.5.0')
        self.assertEqual(update.read_pointer(self.base, 'previous'), 'v0.4.1')
        self.assertEqual(sorted(p.name for p in (self.base / 'versions').iterdir() if p.is_dir()), ['v0.4.1', 'v0.5.0'])
        self.assertFalse(update.confirm_start(self.base, 'v0.5.0'))   # the next start of the same version changes nothing

    def test_switch_clears_cut_off_downloads(self):
        downloads = self.base / 'downloads'
        downloads.mkdir()
        (downloads / ('a' * 64 + '.zip.part')).write_bytes(b'half of an older release')   # its window was closed
        (downloads / ('b' * 64 + '.zip')).write_bytes(b'checked, never unpacked')
        busy = downloads / ('c' * 64 + '.zip.part')
        with open(busy, 'wb'):   # another window still writing: Windows refuses the delete, which is no error
            self.assertTrue(update.confirm_start(self.base, 'v0.5.0'))
        left = sorted(p.name for p in downloads.iterdir())
        self.assertEqual(left, [busy.name] if os.name == 'nt' else [])
        self.assertTrue((self.base / 'versions' / 'v0.4.1').is_dir())

    @unittest.skipUnless(os.name == 'nt', 'junctions exist on Windows only')
    def test_prune_removes_junctions_without_entering_them(self):
        """A junction is a folder to is_dir() but not a link to is_symlink(): remove_tree removes it as a link, at the
        top (a version folder that is a junction) and inside a version, and never what it points to."""
        import _winapi
        outside = self.base / 'outside'
        outside.mkdir()
        (outside / 'save.txt').write_text('keep')
        _winapi.CreateJunction(str(outside), str(self.base / 'versions' / 'v0.2.0'))
        _winapi.CreateJunction(str(outside), str(self.base / 'versions' / 'v0.3.0' / 'link'))
        self.assertTrue(update.is_link(self.base / 'versions' / 'v0.2.0'))
        self.assertTrue(update.confirm_start(self.base, 'v0.5.0'))   # keeps v0.5.0 and v0.4.1
        self.assertEqual(sorted(p.name for p in (self.base / 'versions').iterdir()),
                         ['current.txt', 'previous.txt', 'v0.4.1', 'v0.5.0'])   # no .trash left either
        self.assertEqual((outside / 'save.txt').read_text(), 'keep')

    def spawn(self, on_poll, seen):
        def spawn(cmd, cwd):
            seen.append((cmd, cwd))
            return FakeProcess(on_poll)
        return spawn

    def test_new_version_that_confirms(self):
        seen = []

        def confirm(polls):
            if polls == 2:
                update.confirm_start(self.base, 'v0.5.0')
            return None
        result = update.start_version(self.base, 'v0.5.0', self.base / 'DKB-Launcher.exe', ('--lang', 'en'),
                                      spawn=self.spawn(confirm, seen), sleep=lambda s: None)
        self.assertEqual(result, 'started')
        cmd, cwd = seen[0]
        self.assertEqual(cmd[1:3], ['-m', 'installer.gui'])
        self.assertIn('--after-update', cmd)
        self.assertEqual(cmd[cmd.index('--launcher') + 1], str(self.base / 'DKB-Launcher.exe'))
        self.assertEqual(cwd, str(self.base / 'versions' / 'v0.5.0'))

    def test_new_version_that_fails_leaves_the_old_one_current(self):
        result = update.start_version(self.base, 'v0.5.0', None, spawn=self.spawn(lambda polls: 1, []),
                                      sleep=lambda s: None)
        self.assertEqual(result, 'failed')
        self.assertEqual(update.read_pointer(self.base, 'current'), 'v0.4.1')
        self.assertTrue((self.base / 'versions' / 'v0.4.1').is_dir())

    def test_slow_start(self):
        result = update.start_version(self.base, 'v0.5.0', None, spawn=self.spawn(lambda polls: None, []),
                                      timeout=1, sleep=lambda s: None)
        self.assertEqual(result, 'slow')
        self.assertEqual(update.read_pointer(self.base, 'current'), 'v0.4.1')

    def test_going_back_is_a_confirmed_start_of_the_previous_version(self):
        """The home screen's Go back starts previous.txt's version with --after-update; its confirm_start swaps the
        two pointers and keeps both folders, so the newer one is offered (and staged from its folder) again."""
        from installer import guilogic
        update.confirm_start(self.base, 'v0.5.0')   # current v0.5.0, previous v0.4.1
        for tag in ('v0.4.1', 'v0.5.0'):
            python = update.version_python(self.base / 'versions' / tag)
            python.parent.mkdir(parents=True, exist_ok=True)
            python.write_bytes(b'')
        self.assertEqual(guilogic.previous_version(self.base, 'v0.5.0'), 'v0.4.1')
        self.assertIsNone(guilogic.previous_version(self.base, 'v0.4.1'))   # that one runs: nothing to go back to
        seen = []

        def spawn(cmd, cwd):   # the previous version's window: confirms its own start, as gui.start_launcher does
            seen.append(cmd)
            update.confirm_start(self.base, Path(cwd).name)
            return FakeProcess(lambda polls: None)
        self.assertEqual(update.start_version(self.base, 'v0.4.1', None, spawn=spawn, sleep=lambda s: None), 'started')
        self.assertIn('--after-update', seen[0])
        self.assertEqual((update.read_pointer(self.base, 'current'), update.read_pointer(self.base, 'previous')),
                         ('v0.4.1', 'v0.5.0'))
        self.assertTrue((self.base / 'versions' / 'v0.5.0').is_dir())
        self.assertEqual(guilogic.previous_version(self.base, 'v0.4.1'), 'v0.5.0')
        (update.version_python(self.base / 'versions' / 'v0.5.0')).unlink()   # its Python gone: not offered
        self.assertIsNone(guilogic.previous_version(self.base, 'v0.4.1'))

    def test_release_pages_only_on_github(self):
        from installer.guilogic import release_page
        self.assertEqual(release_page('https://github.com/x/y/releases/tag/v1'), 'https://github.com/x/y/releases/tag/v1')
        for url in ('http://github.com/x', 'https://github.com.evil.example/x', 'file:///C:/Windows/notepad.exe',
                    'C:\\Windows\\notepad.exe', 'https://github.com/..\\..\\x', None, 5, ''):
            self.assertIsNone(release_page(url), url)

    def test_pointers_refuse_junk(self):
        (self.base / 'versions' / 'current.txt').write_text('..\\..\\evil\n')
        self.assertIsNone(update.read_pointer(self.base, 'current'))
        self.assertIsNone(update.read_pointer(self.base, 'missing'))

    def test_own_tag(self):
        self.assertEqual(update.own_tag(self.base, self.base / 'versions' / 'v0.5.0'), 'v0.5.0')
        self.assertIsNone(update.own_tag(self.base, ROOT))


class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.dir = Path(self.work.name) / 'Игры DKB'
        self.dir.mkdir()
        self.launcher = self.dir / 'DKB-Launcher.exe'
        self.launcher.write_bytes(b'MZ old')
        self.new = self.dir / 'new-DKB-Setup.exe'
        self.new.write_bytes(b'MZ new')

    def tearDown(self):
        self.work.cleanup()

    def names(self):
        return sorted(p.name for p in self.dir.iterdir())

    def failing_replace(self, *fail):
        """os.replace that raises PermissionError on the calls numbered in fail (1 is the first)."""
        real, calls = os.replace, []

        def replace(a, b):
            calls.append((Path(a).name, Path(b).name))
            if len(calls) in fail:
                raise PermissionError('denied')
            return real(a, b)
        return patch.object(update.os, 'replace', side_effect=replace), calls

    def test_replaced_in_one_rename_when_not_running(self):
        """launcher.c has exited when the window replaces it: the copy takes its name at once, with no moment in which
        no DKB-Launcher.exe exists."""
        patcher, calls = self.failing_replace()
        with patcher:
            self.assertTrue(update.replace_launcher(self.launcher, self.new))
        self.assertEqual(calls, [('DKB-Launcher.exe.new', 'DKB-Launcher.exe')])
        self.assertEqual(self.launcher.read_bytes(), b'MZ new')
        self.assertEqual(self.names(), ['DKB-Launcher.exe', 'new-DKB-Setup.exe'])
        self.assertFalse(update.replace_launcher(self.launcher, self.new))   # already the same

    def test_running_launcher_is_renamed_away(self):
        patcher, calls = self.failing_replace(1)   # a running exe cannot be replaced, only renamed
        with patcher:
            self.assertTrue(update.replace_launcher(self.launcher, self.new))
        self.assertEqual(calls[1:], [('DKB-Launcher.exe', 'DKB-Launcher.exe.old'),
                                     ('DKB-Launcher.exe.new', 'DKB-Launcher.exe')])
        self.assertEqual(self.launcher.read_bytes(), b'MZ new')
        self.assertEqual((self.dir / 'DKB-Launcher.exe.old').read_bytes(), b'MZ old')
        self.assertFalse((self.dir / 'DKB-Launcher.exe.new').exists())
        update.clean_old_launcher(self.launcher)
        self.assertFalse((self.dir / 'DKB-Launcher.exe.old').exists())

    def test_failed_rename_puts_the_old_launcher_back(self):
        patcher, _ = self.failing_replace(1, 3)
        with patcher, self.assertRaises(PermissionError):
            update.replace_launcher(self.launcher, self.new)
        self.assertEqual(self.launcher.read_bytes(), b'MZ old')
        self.assertEqual(self.names(), ['DKB-Launcher.exe', 'new-DKB-Setup.exe'])

    def test_launcher_that_cannot_be_renamed_leaves_no_copy(self):
        patcher, _ = self.failing_replace(1, 2)   # a folder where renaming is refused
        with patcher, self.assertRaises(PermissionError):
            update.replace_launcher(self.launcher, self.new)
        self.assertEqual(self.launcher.read_bytes(), b'MZ old')
        self.assertEqual(self.names(), ['DKB-Launcher.exe', 'new-DKB-Setup.exe'])

    def test_only_an_exe_is_replaced(self):
        other = self.dir / 'notes.txt'
        other.write_bytes(b'x')
        self.assertFalse(update.replace_launcher(other, self.new))
        self.assertFalse(update.replace_launcher(self.dir / 'missing.exe', self.new))


class WindowLogicTest(unittest.TestCase):
    """installer/guilogic.py: what the launcher's window remembers and shows (gui.py only draws it)."""

    def setUp(self):
        from installer import guilogic
        self.g = guilogic
        self.work = tempfile.TemporaryDirectory()
        self.base = Path(self.work.name) / 'DKB-Recomp'

    def tearDown(self):
        self.work.cleanup()

    def test_settings_round_trip_and_opt_out(self):
        self.assertEqual(self.g.Settings.load(self.base), self.g.Settings())   # nothing saved: check at start
        self.g.Settings(dest='D:\\Игры\\DKB Bananza', check_updates=False).save(self.base)
        loaded = self.g.Settings.load(self.base)
        self.assertEqual((loaded.dest, loaded.check_updates), ('D:\\Игры\\DKB Bananza', False))
        (self.base / self.g.SETTINGS).write_text('{broken', encoding='utf-8')
        self.assertEqual(self.g.Settings.load(self.base), self.g.Settings())
        (self.base / self.g.SETTINGS).write_text('{"dest": 5, "check_updates": "no"}', encoding='utf-8')
        self.assertEqual(self.g.Settings.load(self.base), self.g.Settings(dest='', check_updates=True))

    def test_default_install_folder_is_beside_the_tools(self):
        dest = Path(self.g.default_dest(self.base))
        self.assertEqual(dest.parent, self.base.parent)
        self.assertNotEqual(dest, self.base)

    def test_install_state(self):
        dest = Path(self.work.name) / 'game'
        self.assertIsNone(self.g.install_state(str(dest)))
        self.assertIsNone(self.g.install_state(''))
        dest.mkdir()
        (dest / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': {'check': {}}}), encoding='utf-8')
        self.assertEqual(self.g.install_state(str(dest)), 'unfinished')
        (dest / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': {'dkb_verify': {}}}), encoding='utf-8')
        self.assertEqual(self.g.install_state(str(dest)), 'unfinished')   # no game exe yet
        (dest / self.g.GAME_EXE).write_bytes(b'')
        self.assertEqual(self.g.install_state(str(dest)), 'ready')
        steps = {'compile': {'finished': '2026-11-18T10:00:00Z'}, 'dkb_verify': {'finished': '2026-11-18T10:05:00Z'}}
        (dest / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': steps}), encoding='utf-8')
        self.assertEqual(self.g.install_state(str(dest)), 'ready')
        # A later setup run that reran only steps the check does not depend on: the runner skips the check, the run is
        # finished and Play must stay (it once stayed off for good, since every later run skipped the check again)
        for later in ('dkb_shaders', 'dkb_wide', 'dkb_prompts', 'dkb_data'):
            steps[later] = {'finished': '2026-11-19T08:00:00Z'}
        (dest / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': steps}), encoding='utf-8')
        self.assertEqual(self.g.install_state(str(dest)), 'ready')
        steps['compile']['finished'] = '2026-11-19T09:00:00Z'   # an update recompiled; its check did not finish
        (dest / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': steps}), encoding='utf-8')
        self.assertEqual(self.g.install_state(str(dest)), 'unfinished')
        steps['compile']['finished'] = '2026-11-18T10:00:00Z'
        steps['dkb_translator'] = {'finished': '2026-11-19T09:00:00Z'}   # so did a new translator
        (dest / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': steps}), encoding='utf-8')
        self.assertEqual(self.g.install_state(str(dest)), 'unfinished')

    def test_verify_inputs_are_the_check_steps_key(self):
        """install_state counts exactly the steps whose keys the runner puts into dkb_verify's key: those after which
        the runner runs the check again."""
        from installer import bananza

        class Seen(dict):
            def __getitem__(self, name):
                self.setdefault('seen', set()).add(name)
                return name
        keys = Seen()
        bananza.Prepare().key(type('Ctx', (), {'keys': keys, 'state': {'lang': 'en-US'}})())
        self.assertEqual(dict.get(keys, 'seen'), set(self.g.VERIFY_INPUTS))

    def test_update_line(self):
        files = Files(self.work.name)
        newer = update.Offer('v0.5.0', files.version, 1, 'notes', 'page', files.manifest)
        page_only = update.Offer('v0.5.0', 'v0.5.0', 1, 'notes', 'page', None)
        older = update.Offer('v0.3.0', files.version, -1, '', 'page', files.manifest)
        same = update.Offer('v0.5.0', files.version, 0, '', 'page', files.manifest)
        status = self.g.update_status
        self.assertEqual(status(newer, None), (('ui.update_offer', {'tag': 'v0.5.0'}), True))
        self.assertEqual(status(page_only, None), (('ui.update_page_only', {'tag': 'v0.5.0'}), False))
        self.assertEqual(status(older, None), (('ui.update_older', {'tag': 'v0.3.0'}), False))
        self.assertEqual(status(older, None, allow_downgrade=True)[1], True)
        self.assertEqual(status(same, None), (('ui.update_none', {}), False))
        self.assertEqual(status(None, 'no answer'), (('ui.update_offline', {'detail': 'no answer'}), False))
        self.assertEqual(status(None, None), (('ui.update_off', {}), False))

    def test_update_log(self):
        launcher = self.g.Launcher(base=self.base)
        launcher.note('check: latest v0.5.0')
        self.assertIn('check: latest v0.5.0', launcher.log.read_text(encoding='utf-8'))
        self.assertEqual(launcher.temp, self.base / 'temp')


class CheckReleaseTest(unittest.TestCase):
    """tools/check_release.py, the release workflow's check before it publishes."""

    def setUp(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import check_release
        self.c = check_release
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.files = Files(self.work.name)
        (Path(self.work.name) / update.MANIFEST).write_text(self.files.text, encoding='ascii', newline='\n')
        self.dist = Path(self.work.name)

    def test_release_and_pre_release_tags(self):
        self.assertEqual(self.c.check(self.dist, 'v0.5.0'), 'latest')
        for tag in ('v0.5.0-rc1', 'v0.5.0-rc.2', 'v0.5.0-beta1'):   # the tags the workflow published before
            self.assertEqual(self.c.check(self.dist, tag), 'prerelease', tag)
        for tag in ('v0.5.1', 'v0.5.1-rc1', 'v0.5.0-', 'v0.5.0-rc 1', 'release-0.5.0', '0.5.0'):
            with self.subTest(tag), self.assertRaises(self.c.ReleaseError):
                self.c.check(self.dist, tag)

    def test_files_that_are_not_the_manifests(self):
        zip_path = self.files.zip
        zip_path.write_bytes(self.files.zip_bytes[:-1] + bytes([self.files.zip_bytes[-1] ^ 1]))   # same size
        with self.assertRaisesRegex(self.c.ReleaseError, 'SHA-256'):
            self.c.check(self.dist, 'v0.5.0')
        zip_path.write_bytes(self.files.zip_bytes + b'x')
        with self.assertRaisesRegex(self.c.ReleaseError, 'bytes'):
            self.c.check(self.dist, 'v0.5.0')
        zip_path.unlink()
        with self.assertRaisesRegex(self.c.ReleaseError, 'not among'):
            self.c.check(self.dist, 'v0.5.0')
        (self.dist / update.MANIFEST).write_text('dkb-release 2\n')
        with self.assertRaisesRegex(self.c.ReleaseError, 'not one the launcher reads'):
            self.c.check(self.dist, 'v0.5.0')

    def test_command_line(self):
        with patch('builtins.print') as printed:
            self.assertEqual(self.c.main([str(self.dist), 'v0.5.0-rc1']), 0)
        printed.assert_called_with('prerelease')
        with patch('builtins.print') as printed:
            self.assertEqual(self.c.main([str(self.dist), 'v0.6.0']), 1)
        self.assertTrue(printed.call_args[0][0].startswith('::error::'))


class ReleaseToolTest(unittest.TestCase):
    """tools/make_release.py: the launcher's strings and the files published beside the zip."""

    def setUp(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        self.m = make_release
        self.work = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.work.cleanup()

    def test_launcher_header(self):
        out = Path(self.work.name) / 'launcher_text.h'
        self.m.launcher_header(out, 'dkb')
        text = out.read_text(encoding='ascii')
        for key in self.m.LAUNCHER_TEXT:
            for lang in ('EN', 'RU', 'ZH'):
                self.assertIn(f'#define LAUNCHER_{key.upper()}_{lang} L"', text)
        self.assertIn(f'#define LAUNCHER_REPO L"{update.REPO}"', text)
        self.assertNotIn('\\u000a', text)   # C allows no universal character name for a newline
        self.assertIn('\\n', text)
        self.m.launcher_header(out, 'totk')
        self.assertIn('#define LAUNCHER_REPO L""', out.read_text(encoding='ascii'))   # TotK: only the setup

    def test_launcher_loads_its_dlls_from_system32_only(self):
        """The launcher is run from Downloads: its static imports (winhttp.dll, bcrypt.dll) must not be looked for
        beside it, where a downloaded DLL of that name would be loaded instead of Windows' own."""
        for game in ('dkb', 'totk'):
            commands = []
            out = Path(self.work.name) / 'out'
            with patch.object(self.m, 'run', side_effect=lambda cmd, **kw: commands.append([str(c) for c in cmd])), \
                    patch.object(self.m, 'launcher_header'), patch.object(self.m, 'clean_env', return_value={}), \
                    patch.object(self.m, 'check_launcher') as checked, patch.object(self.m, 'GAME', game):
                self.m.build_launcher(out, Path(self.work.name) / 'llvm')
            link = next(c for c in commands if Path(c[0]).name == 'clang.exe')
            self.assertIn('-Wl,--dependent-load-flag=0x800', link, game)
            self.assertEqual(checked.call_args[0][0], Path(link[link.index('-o') + 1]))   # the flag is read back
            sources = {Path(c).name for c in link if c.endswith('.c')}
            if game == 'dkb':
                self.assertEqual(sources, {'launcher.c', 'manifest.c', 'fetch.c'})
                self.assertIn('-DDKB_LAUNCHER', link)
                self.assertIn('-lwinhttp', link)
            else:   # the TotK setup stays MIT: fetch.c (GPL-3.0-or-later) is neither compiled nor linked
                self.assertEqual(sources, {'launcher.c'})
                self.assertFalse([c for c in link if c.startswith('-D') or c.startswith('-l')])

    def test_manifest_and_launcher_beside_the_zip(self):
        folder = Path(self.work.name) / 'DKB-Setup'
        folder.mkdir()
        (folder / 'DKB-Setup.exe').write_bytes(b'MZ launcher')
        semver = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
        zip_path = Path(self.work.name) / f'DKB-Setup-{semver}+abc1234-windows.zip'
        zip_path.write_bytes(b'PK zip')
        with patch.object(self.m, 'GAME', 'dkb'), patch.object(self.m, 'version', return_value=semver + '+abc1234'), \
                patch('builtins.print'):
            self.m.write_launcher_release(folder, zip_path)
        manifest = update.parse_manifest((zip_path.parent / update.MANIFEST).read_bytes())
        self.assertEqual((manifest.tag, manifest.version), ('v' + semver, semver + '+abc1234'))
        self.assertEqual(manifest.tools.sha256, sha(b'PK zip'))
        self.assertEqual(manifest.tools.top, 'DKB-Setup')
        self.assertEqual((zip_path.parent / self.m.LAUNCHER).read_bytes(), b'MZ launcher')
        self.assertEqual(manifest.launcher.sha256, sha(b'MZ launcher'))


if __name__ == '__main__':
    unittest.main()
