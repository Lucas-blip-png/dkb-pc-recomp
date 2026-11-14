"""Synthetic layout/animation regression tests; no extracted game content."""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from dkb_wide_layout import BACKGROUNDS, HUD_ANCHORS, TITLE_EFFECT_PATHS, members, patch_archive, patch_title_effect, zstd


def archive(entries):
    names, nodes, payload = bytearray(), bytearray(), bytearray()
    for name, content in entries:
        offset = len(names) // 4
        names.extend(name.encode() + b'\0')
        names.extend(b'\0' * (-len(names) % 4))
        nodes.extend(struct.pack('<4I', 0, 0x1000000 | offset, len(payload), len(payload) + len(content)))
        payload.extend(content)
    start = 20 + 12 + len(nodes) + 8 + len(names)
    header = b'SARC' + struct.pack('<HHIII', 20, 0xfeff, start + len(payload), start, 0x100)
    raw = header + b'SFAT' + struct.pack('<HHI', 12, len(entries), 0x65) + nodes
    raw += b'SFNT' + struct.pack('<HH', 8, 0) + names + payload
    return zstd.compress(raw)


def layout_header(magic, sections):
    return magic + struct.pack('<HHIIHH', 0xfeff, 20, 0, 20 + sum(map(len, sections)), len(sections), 0) + b''.join(sections)


def pane(name, x):
    p = bytearray(84)
    p[:4] = b'pan1'
    struct.pack_into('<I', p, 4, len(p))
    p[12:12 + len(name)] = name.encode()
    struct.pack_into('<10f', p, 44, x, 472, 0, 0, 0, 0, 1, 1, 100, 100)
    return p


def animation(name):
    section = bytearray(24)
    section[:4] = b'pai1'
    struct.pack_into('<HII', section, 14, 1, 20, 24)
    entry = bytearray(36)
    entry[:len(name)] = name.encode()
    entry[28] = 1
    struct.pack_into('<I', entry, 32, 36)
    tag = bytearray(12)
    tag[:4], tag[4] = b'FLPA', 1
    struct.pack_into('<I', tag, 8, 12)
    curve = bytearray(12)
    curve[2] = 2  # Hermite X translation, two keys with nonzero tangents.
    struct.pack_into('<H', curve, 4, 2)
    struct.pack_into('<I', curve, 8, 12)
    section += entry + tag + curve + struct.pack('<6f', 0, -905, 2, 15, -885, -2)
    struct.pack_into('<I', section, 4, len(section))
    return layout_header(b'FLAN', [section])


def fixture(omit=None, wrong=False):
    panes = [pane('Unrelated', 123)]
    for name, (x, _) in HUD_ANCHORS['HudStatus'].items():
        if name != omit:
            panes.append(pane(name, x + (1 if wrong else 0)))
    return archive([('blyt/HudStatus.bflyt', layout_header(b'FLYT', panes)),
                    ('anim/Appear.bflan', animation('N_Hp_00')),
                    ('untouched.bin', b'opaque data stays identical')])


def unpack(raw):
    d = zstd.decompress(raw)
    return {name: d[lo:hi] for name, lo, hi in members(d)}


class LayoutTests(unittest.TestCase):
    def test_updated_dialog_accepts_only_the_version_specific_missing_pane(self):
        path = 'Layout/DialogConfirm.Nin_Ounce_NVN2.blarc.zs'
        raw = resource_files('dkb458752')[path]
        with self.assertRaisesRegex(ValueError, 'missing panes'):
            patch_archive(raw, 'DialogConfirm', 3440 / 2560)
        changed = unpack(patch_archive(raw, 'DialogConfirm', 3440 / 2560, version='dkb458752'))
        self.assertNotEqual(changed, unpack(raw))
        missing = archive([('blyt/DialogConfirm.bflyt', layout_header(b'FLYT', []))])
        with self.assertRaisesRegex(ValueError, 'missing panes'):
            patch_archive(missing, 'DialogConfirm', 3440 / 2560, version='dkb458752')

    def test_edges_translate_without_scaling_or_changing_animation_tangents(self):
        original = unpack(fixture())
        moved = unpack(patch_archive(fixture(), 'HudStatus', 3440 / 1440 / (16 / 9), True))
        before, after = original['blyt/HudStatus.bflyt'], moved['blyt/HudStatus.bflyt']
        self.assertEqual(before[:104], after[:104])  # Header and unrelated pane.
        for at in range(104, len(before), 84):
            self.assertAlmostEqual(struct.unpack_from('<f', after, at + 44)[0],
                                   struct.unpack_from('<f', before, at + 44)[0] - 330)
            self.assertEqual(before[at:at + 44], after[at:at + 44])
            self.assertEqual(before[at + 48:at + 84], after[at + 48:at + 84])
        self.assertEqual(moved['untouched.bin'], original['untouched.bin'])
        self.assertEqual(moved['anim/Appear.bflan'][:-24], original['anim/Appear.bflan'][:-24])
        self.assertEqual(struct.unpack('<6f', moved['anim/Appear.bflan'][-24:]), (0, -1235, 2, 15, -1215, -2))

    def test_centered_and_identity_preserve_every_byte(self):
        original = unpack(fixture())
        self.assertEqual(unpack(patch_archive(fixture(), 'HudStatus', 2, False)), original)
        self.assertEqual(unpack(patch_archive(fixture(), 'HudStatus', 1, True)), original)

    def test_unknown_layout_revision_fails_closed(self):
        with self.assertRaisesRegex(ValueError, 'baseline'):
            patch_archive(fixture(wrong=True), 'HudStatus', 2, True)
        with self.assertRaisesRegex(ValueError, 'missing panes'):
            patch_archive(fixture(omit='N_Hp_00'), 'HudStatus', 2, True)

    def test_status_indicators_move_only_final_anchors(self):
        for name in ('Saving', 'Loading'):
            panes = [pane('N_Capture_00', 0), pane('P_IcnSdw_00', 0)]
            panes += [pane(n, x) for n, (x, _) in HUD_ANCHORS[name].items()]
            file = f'blyt/{name}.bflyt'
            raw = archive([(file, layout_header(b'FLYT', panes))])
            before = unpack(raw)[file]
            for factor in (3440 / 2560, 2):
                with self.subTest(layout=name, factor=factor):
                    self.assertEqual(unpack(patch_archive(raw, name, factor, False))[file], before)
                    after = unpack(patch_archive(raw, name, factor, True))[file]
                    expected = bytearray(before)
                    for i, (_, (x, side)) in enumerate(HUD_ANCHORS[name].items()):
                        struct.pack_into('<f', expected, 20 + (i + 2) * 84 + 44,
                                         x + side * 960 * (factor - 1))
                    self.assertEqual(after, expected)


def title_effect():
    # Minimal synthetic container with the same bounded emitter links. Include
    # an unrelated particle and a string-table name to catch overly broad edits.
    data = bytearray(b'YB\x05\x00' + b'BootMenuEnv\0' + bytes(241))
    start = len(data)
    data.extend(bytes(0x80))
    data[start:start + 4] = b'ESET'
    data[start + 0x30:start + 0x3c] = b'BootMenuEnv\0'
    struct.pack_into('<I', data, start + 8, 0x80)
    for i, name in enumerate((b'Wall', b'Debris', b'Atmos')):
        pos = len(data)
        data.extend(bytes(0x1000))
        data[pos:pos + 4] = b'EMTR'
        struct.pack_into('<5I', data, pos + 4, 0x1000, 0xffffffff,
                         0x1000 if i < 2 else 0xffffffff, 0xffffffff, 0x20)
        data[pos + 0x30:pos + 0x30 + len(name)] = name
        struct.pack_into('<3f', data, pos + 0x20 + 0xfa0, 16, 9, 1)
    struct.pack_into('<I', data, start + 4, len(data) - start)
    return data, start


class TitleEffectTests(unittest.TestCase):
    def test_only_wall_and_haze_scale_uniformly(self):
        original, start = title_effect()
        raw = zstd.compress(original)
        for factor in (1, 3440 / 2560, 2):
            changed = zstd.decompress(patch_title_effect(raw, factor))
            expected = original.copy()
            for i in (0, 2):
                struct.pack_into('<2f', expected, start + 0x80 + i * 0x1000 + 0x20 + 0xfa0,
                                 16 * factor, 9 * factor)
            self.assertEqual(changed, expected)

    def test_unknown_and_malformed_emitters_fail_closed(self):
        for fault in ('scale', 'bounds', 'sibling', 'missing'):
            data, start = title_effect()
            first = start + 0x80
            if fault == 'scale':
                struct.pack_into('<f', data, first + 0x20 + 0xfa0, 17)
            elif fault == 'bounds':
                struct.pack_into('<I', data, first + 0x14, 0xfffffff0)
            elif fault == 'sibling':
                struct.pack_into('<I', data, first + 0xc, 0)
            else:
                data[first + 0x2000 + 0x30] = ord('X')
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                patch_title_effect(zstd.compress(data), 2)


def resource_files(version='dkb100'):
    """Complete synthetic RomFS inputs for the installer/generator integration test."""
    files = {}
    for name in dict.fromkeys((*BACKGROUNDS, *HUD_ANCHORS)):
        panes = []
        for key in dict.fromkeys((*BACKGROUNDS.get(name, {}), *HUD_ANCHORS.get(name, {}))):
            if version == 'dkb458752' and name == 'DialogConfirm' and key == 'P_LightBgColor_01':
                continue
            p = pane(key, HUD_ANCHORS.get(name, {}).get(key, (0, 0))[0])
            if key in BACKGROUNDS.get(name, {}):
                struct.pack_into('<2f', p, 44, 0, 0)
                struct.pack_into('<2f', p, 76, 1920, 1080)
            panes.append(p)
        files[f'Layout/{name}.Nin_Ounce_NVN2.blarc.zs'] = archive([
            (f'blyt/{name}.bflyt', layout_header(b'FLYT', panes))])
    files[TITLE_EFFECT_PATHS[version]] = zstd.compress(title_effect()[0])
    return files


class GeneratorTests(unittest.TestCase):
    def command(self, romfs, out, *args):
        return subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'tools/dkb_wide_layout.py'),
                               '--romfs', str(romfs), '--out', str(out), *map(str, args)],
                              capture_output=True, text=True)

    def test_installer_workspace_does_not_allow_unowned_or_outside_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            marker = work / '.dkb-setup-owner.json'
            marker.write_text(json.dumps({'game': 'dkb100', 'temporary': str(work / 'other')}))
            staged = work / 'build' / 'overlays'
            result = self.command(work / 'romfs', staged, '--installer-workspace', work, '--presets', '3440x1440')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('invalid installer workspace owner', result.stderr)
            self.assertFalse(staged.exists())
            marker.write_text(json.dumps({'game': 'dkb100', 'temporary': str(work)}))
            outside = work / 'outside'
            result = self.command(work / 'romfs', outside, '--installer-workspace', work, '--presets', '3440x1440')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('dedicated directory under build/', result.stderr)
            self.assertFalse(outside.exists())

    def test_manual_single_aspect_and_recorded_resource_hash_validation(self):
        root = Path(__file__).resolve().parents[1]
        build = root / 'build'
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build) as temporary:
            work = Path(temporary)
            loose, packed = work / 'loose', work / 'packed'
            for name, raw in resource_files().items():
                path = loose / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
            result = subprocess.run([sys.executable, str(root / 'tools/dkb_pack_romfs.py'), '--dir', str(loose),
                                     '--out', str(packed)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.command(packed, work / 'centered', '--aspect', '3440x1440')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((work / 'centered' / 'manifest.tsv').is_file())
            index = json.loads((packed / 'index.json').read_text())
            index['files'][0]['sha256'] = '0' * 64
            (packed / 'index.json').write_text(json.dumps(index))
            result = self.command(packed, work / 'bad', '--aspect', '3440x1440')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('checksum mismatch', result.stderr)
            self.assertFalse((work / 'bad').exists())


if __name__ == '__main__':
    unittest.main()
