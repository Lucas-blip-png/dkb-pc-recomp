"""Generate Bananza 1.0.0/3.2.0 ultrawide background and HUD overlays from the user's RomFS.

The camera patches retain a centered UI. These named background panes extend to
the wider edges without stretching text, controls, logos, or guide movie panes.
The optional edge HUD moves named corner containers and their animation keys.
Output contains game data and stays under build/, or an installer's owned build staging directory.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

try:
    from compression import zstd
except ImportError:  # Python 3.12/3.13 development environments.
    import zstandard as zstd
from ultrawide_layout import anim_keys

# Stretch captures of the already-wide framebuffer horizontally. Flat colour
# and fade panes also extend horizontally. Decorative background art covers the
# frame with a uniform enlargement, preserving its proportions and cropping y.
BACKGROUNDS = {
    'Boot': {'P_Bs_00': 'wide'},
    'MenuContinue': {'P_BlackBg_00': 'wide', 'P_Bg_00': 'cover'},
    'MenuMain': {'P_CaptureBg_00': 'wide', 'P_PauseBgCrack_00': 'cover'},
    'MenuModeSelectFirst': {'P_BlackBg_00': 'wide'},
    'MenuHelp': {'P_Bg_00': 'cover', 'P_BgColor_00': 'wide'},
    'MenuSetting': {'P_Bg_00': 'cover', 'P_BgColor_00': 'wide'},
    'MenuSettingOperation': {'P_Bg_00': 'cover', 'P_BgColor_00': 'wide'},
    'MenuSave': {'P_PauseBg_00': 'cover', 'P_PauseBgColor_00': 'wide'},
    'MenuSkill': {'P_BgCapture_00': 'wide'},
    'MenuAmiibo': {'P_Bg_00': 'cover', 'P_BgColor_00': 'wide'},
    'MusicList': {'P_BgCapture_00': 'wide'},
    'MapStage': {'P_CaptureBg_00': 'wide', 'P_CircleBg_00': 'cover'},
    'MapElevator': {'P_BgBlack_00': 'wide', 'P_Bg_00': 'cover',
                    'P_SkyBg_00': 'cover', 'P_SkyBg_01': 'cover', 'P_Wipe_00': 'cover'},
    'Shop': {'P_Blur_00': 'wide', 'P_Blur_01': 'wide', 'P_Bg_00': 'cover'},
    'Wardrobe': {'P_BlackBlur_00': 'wide', 'P_GoldBlur_00': 'wide'},
    'Snapshot': {'P_CrackCapture_00': 'wide', 'P_Crack_00': 'cover',
                 'P_CaptureBlur_00': 'wide', 'P_BlackBlur_00': 'wide', 'P_BlackBlur_01': 'wide'},
    'DialogConfirm': {'P_Bg_00': 'cover', 'P_DarkBgColor_00': 'wide',
                      'P_LightBgColor_00': 'wide', 'P_LightBgColor_01': 'wide'},
    'DialogInformation': {'P_BgTop_00': 'cover', 'P_BgBottom_00': 'cover'},
    'TitleLogo': {'P_Fade_00': 'wide'},
    'TransitionFadeBlack': {'P_Fade_00': 'wide'},
    'TransitionFadeWhite': {'P_Fade_00': 'wide'},
    'TransitionMap': {'P_CaptureBg_00': 'wide'},
    'TransitionTitleLogo': {'P_Bg_00': 'wide'},
    'TransitionMiss': {'P_Bg_00': 'cover'},
    'TransitionTimeUp': {'P_Bg_00': 'cover'},
    'TransitionSkip': {'P_CrackIn_00': 'cover', 'P_CrackOut_00': 'cover'},
}

# Authored x and the edge (-1 left, +1 right). Move containers once, not their
# children; world-space markers, aiming, centered messages and menus are untouched.
HUD_ANCHORS = {
    'HudMain': {
        'N_Hint_00': (0, -1), 'N_Piece': (-890, -1),
        'N_Gold_00': (925, 1), 'N_Chip_00': (925, 1), 'N_Item_00': (925, 1),
        'N_Reward_00': (760, 1), 'N_ReturnBalloon_00': (-870, -1),
        'N_BottleLife_00': (-785, -1), 'N_BottleAdrenaline_00': (-692, -1),
        'N_BottleAdrenaline_01': (-692, -1), 'P_ExplanationPopUp_00': (893, 1),
    },
    'HudStatus': {'N_Adrenaline_00': (-781, -1), 'N_Hp_00': (-905, -1),
                  'N_Style_00': (-801, -1), 'N_KeyGuide_00': (-850, -1)},
    'HudActionGuide': {'L_Movie_00': (680, 1)},
    'HudDepth': {'N_All': (61, 1)},
    'HudRace': {'N_Tutorial_00': (890, 1)},
    'HudAdrenalineTimer': {'N_Gauge_00': (-790, -1), 'N_BottleAdrenaline_00': (-692, -1)},
    # Saving draws its shadow into a local capture before compositing it beside
    # the banana. Move only the final icon/shadow; keep the capture at its origin.
    'Saving': {'P_IcnSdw_01': (-910, -1), 'P_Icn_00': (-910, -1)},
    'Loading': {'T_Text_00': (813, 1), 'N_Down_00': (810, 1),
                'N_EvUp_00': (810, 1), 'N_EvDown_00': (810, 1),
                'N_Common_00': (810, 1), 'N_Common_01': (810, 1), 'N_Common_02': (810, 1)},
}

TITLE_EFFECT_PATH = 'Effect/static.Nin_Ounce_NVN2.esetb.byml.zs'
TITLE_EFFECT_PATHS = {'dkb100': TITLE_EFFECT_PATH,
                      'dkb458752': 'Effect/static.Product.200.Nin_Ounce_NVN2.esetb.byml.zs'}


def patch_title_effect(raw, factor):
    """Cover the title with its existing wall and atmosphere, preserving proportions.

    Bananza 1.0.0's BootMenuEnv has two camera-facing 16 by 9 particles.
    Enlarge both axes together so neither the wall nor the haze ends inside a
    wide viewport. The logo, debris, timing, materials and textures are untouched.
    This uses the Ounce EMTR layout verified against the local 1.0.0 resources;
    older NintendoWare particle layouts have different field offsets.
    """
    if not math.isfinite(factor) or factor < 1:
        raise ValueError('title background factor must be finite and at least one')
    data = bytearray(zstd.decompress(raw))
    if data[:4] != b'YB\x05\x00':
        raise ValueError('title effects: expected little-endian BYML version 5')
    # The name also occurs in BYML's string table. Accept only the ESET header
    # occurrence, then follow bounded child/sibling offsets inside that set.
    sets = []
    pos = 0
    while (pos := data.find(b'BootMenuEnv\0', pos)) >= 0:
        start = pos - 0x30
        if start >= 0 and data[start:start + 4] == b'ESET':
            sets.append(start)
        pos += 1
    if len(sets) != 1:
        raise ValueError('title effects: expected one BootMenuEnv emitter set')
    start = sets[0]
    size, child = struct.unpack_from('<2I', data, start + 4)
    end = start + size
    if not 0x40 <= child < size or end > len(data):
        raise ValueError('title effects: invalid emitter set bounds')
    pos = start + child
    found = set()
    while True:
        if pos + 0x18 > end or data[pos:pos + 4] != b'EMTR':
            raise ValueError('title effects: invalid emitter header')
        size, _, sibling, _, offset = struct.unpack_from('<5I', data, pos + 4)
        body = pos + offset
        if size < 0x18 or pos + size > end or offset < 0x18 or body + 0xfac > pos + size:
            raise ValueError('title effects: invalid emitter bounds')
        name = bytes(data[body + 0x10:body + 0x50]).split(b'\0', 1)[0]
        if name in (b'Wall', b'Atmos'):
            if name in found or struct.unpack_from('<3f', data, body + 0xfa0) != (16, 9, 1):
                raise ValueError('title effects: unexpected background scale')
            found.add(name)
            struct.pack_into('<2f', data, body + 0xfa0, 16 * factor, 9 * factor)
        if sibling == 0xffffffff:
            break
        if sibling < size or pos + sibling >= end:
            raise ValueError('title effects: invalid emitter sibling')
        pos += sibling
    if found != {b'Wall', b'Atmos'}:
        raise ValueError('title effects: missing wall or atmosphere')
    return zstd.compress(data, level=9)


def members(data):
    if data[:4] != b'SARC' or data[6:8] != b'\xff\xfe':
        raise ValueError('expected a little-endian SARC')
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    if size != len(data) or data[header:header + 4] != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    if data[names:names + 4] != b'SFNT':
        raise ValueError('missing SARC names')
    names += 8
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        name = data[names + (attr & 0xffffff) * 4:].split(b'\0', 1)[0].decode()
        if not 0 <= lo <= hi <= len(data) - start:
            raise ValueError('SARC member outside archive')
        yield name, start + lo, start + hi


def patch_archive(raw, layout, factor, hud_edges=False, version='dkb100'):
    data = bytearray(zstd.decompress(raw))
    entries = list(members(data))
    backgrounds = BACKGROUNDS.get(layout, {})
    # 3.2.0's DialogConfirm removed this duplicate light background. Keep the other panes and their geometry
    # mandatory; accepting a missing pane globally would hide an unsupported 1.0.0 resource revision.
    if version == 'dkb458752' and layout == 'DialogConfirm':
        backgrounds = {name: mode for name, mode in backgrounds.items() if name != 'P_LightBgColor_01'}
    anchors = HUD_ANCHORS.get(layout, {}) if hud_edges else {}
    dx = 960 * (factor - 1)
    found = set()
    for name, lo, hi in entries:
        if name != f'blyt/{layout}.bflyt':
            continue
        if data[lo:lo + 4] != b'FLYT':
            raise ValueError(f'{layout}: not BFLYT')
        pos = lo + struct.unpack_from('<H', data, lo + 6)[0]
        while pos + 8 <= hi:
            tag = data[pos:pos + 4]
            size = struct.unpack_from('<I', data, pos + 4)[0]
            if size < 8 or pos + size > hi:
                raise ValueError(f'{layout}: invalid BFLYT section')
            if tag in (b'pan1', b'pic1', b'txt1', b'prt1', b'wnd1', b'bnd1'):
                pane = data[pos + 12:pos + 36].split(b'\0', 1)[0].decode()
                if pane in anchors:
                    base, side = anchors[pane]
                    x = struct.unpack_from('<f', data, pos + 44)[0]
                    if x != base or pane in found:
                        raise ValueError(f'{layout}/{pane}: unexpected HUD baseline {x}')
                    found.add(pane)
                    struct.pack_into('<f', data, pos + 44, x + side * dx)
                if pane in backgrounds:
                    x, y = struct.unpack_from('<2f', data, pos + 44)
                    w, h = struct.unpack_from('<2f', data, pos + 76)
                    if (x, y, w, h) != (0, 0, 1920, 1080) or pane in found:
                        raise ValueError(f'{layout}/{pane}: unexpected baseline geometry')
                    found.add(pane)
                    struct.pack_into('<f', data, pos + 76, w * factor)
                    if backgrounds[pane] == 'cover':
                        struct.pack_into('<f', data, pos + 80, h * factor)
            pos += size
    expected = set(backgrounds) | set(anchors)
    if found != expected:
        raise ValueError(f'{layout}: missing panes {expected - found}')
    for name, lo, hi in entries:
        if not name.endswith('.bflan'):
            continue
        for pane, (_, side) in anchors.items():
            for keys in anim_keys(data, lo, pane, b'FLPA', 0):
                for key in keys:
                    value = struct.unpack_from('<f', data, key + 4)[0]
                    struct.pack_into('<f', data, key + 4, value + side * dx)
        for pane, mode in backgrounds.items():
            for target in ((8, 9) if mode == 'cover' else (8,)):
                for keys in anim_keys(data, lo, pane, b'FLPA', target):
                    for key in keys:
                        value, slope = struct.unpack_from('<2f', data, key + 4)
                        struct.pack_into('<2f', data, key + 4, value * factor, slope * factor)
    return zstd.compress(data, level=9)


def generate(romfs, out, w, h, hud, version='dkb100'):
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        raise ValueError('RomFS index has not passed extraction verification')
    files = {e['path']: e for e in index['files']}
    outputs = []
    with (romfs / 'romfs.bin').open('rb') as source:
        def read(path):
            entry = files[path]
            if entry['offset'] < 0 or entry['size'] < 0 or entry['offset'] + entry['size'] > index['romfs_size']:
                raise ValueError(f'{path}: packed resource outside image')
            source.seek(entry['offset'])
            raw = source.read(entry['size'])
            # Loose-folder packs record the whole image hash; some extractors also supply individual hashes.
            if (len(raw) != entry['size'] or
                    ('sha256' in entry and hashlib.sha256(raw).hexdigest() != entry['sha256'])):
                raise ValueError(f'{path}: packed resource checksum mismatch')
            return raw

        layouts = dict.fromkeys((*BACKGROUNDS, *(HUD_ANCHORS if hud == 'edges' else ())))
        for layout in layouts:
            path = f'Layout/{layout}.Nin_Ounce_NVN2.blarc.zs'
            raw = read(path)
            patched = patch_archive(raw, layout, (w / h) / (16 / 9), hud == 'edges', version)
            outputs.append((path, raw, patched))
        # In 3.2.0 BootMenuEnv lives in the Product.200 effect archive, not the general static archive.
        title_path = TITLE_EFFECT_PATHS[version]
        raw = read(title_path)
        outputs.append((title_path, raw, patch_title_effect(raw, (w / h) / (16 / 9))))
    # Validate every requested pane before publishing any replacement files.
    lines = ['# path\tsize\tsha256\toriginal_sha256 (tools/dkb_wide_layout.py)']
    for path, raw, patched in outputs:
        dest = out / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(patched)
        lines.append('\t'.join((path, str(len(patched)), hashlib.sha256(patched).hexdigest(),
                                hashlib.sha256(raw).hexdigest())))
    manifest = out / 'manifest.tsv'
    temporary = out / 'manifest.tsv.tmp'
    temporary.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    temporary.replace(manifest)
    print(f'Validated {len(outputs)} UI/background resources for {w}x{h}, HUD {hud}: {out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--version', choices=tuple(TITLE_EFFECT_PATHS), default='dkb100')
    ap.add_argument('--out', type=Path, required=True)
    sizes = ap.add_mutually_exclusive_group(required=True)
    sizes.add_argument('--aspect', help='e.g. 3440x1440')
    sizes.add_argument('--presets', nargs='+', help='prepare both HUD variants in named subfolders for these sizes')
    ap.add_argument('--hud', choices=('centered', 'edges'), default='centered')
    ap.add_argument('--installer-workspace', type=Path, help='owned installer workspace containing build staging')
    args = ap.parse_args()
    build = (Path(__file__).resolve().parents[1] / 'build').resolve()
    if args.installer_workspace:
        workspace = args.installer_workspace.resolve()
        owner = json.loads((workspace / '.dkb-setup-owner.json').read_text(encoding='utf-8'))
        if (Path(owner['temporary']).resolve() != workspace or
                owner['game'] not in ('dkb100', 'dkb458752')):
            ap.error('invalid installer workspace owner')
        build = workspace / 'build'
    out = args.out.resolve()
    if not out.is_relative_to(build) or out == build:
        ap.error('game-derived overlays must be in a dedicated directory under build/')
    parsed = []
    for size in args.presets or [args.aspect]:
        try:
            w, h = map(int, size.split('x'))
        except ValueError:
            ap.error('aspect must be WxH')
        if not (1280 <= w <= 7680 and 720 <= h <= 4320 and w % 2 == h % 2 == 0 and w * 9 > h * 16):
            ap.error('aspect must be an even output resolution wider than 16:9')
        parsed.append((w, h))
    for w, h in parsed:
        for hud in ('centered', 'edges') if args.presets else (args.hud,):
            folder = f'romfs-patches-{w}x{h}' + ('-hud-edges' if hud == 'edges' else '')
            generate(args.romfs, out / folder if args.presets else out, w, h, hud, args.version)


if __name__ == '__main__':
    main()
