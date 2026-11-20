"""Validated Bananza installs from ExeFS and RomFS folders or a .nup container. No NCA extraction or keys.

The existing runner owns step events and atomic checkpoints. Game-derived temporary files stay in a separately
marked task directory; only verified files enter the portable destination. No prebuilt TotK runtime is reused.

A .nup (the container Eglantine and Eden load) is read with tools/extract_nup.py: preflight reads only its table of
contents, which records every file's size and SHA-256, so the target is chosen and the ExeFS checked before anything
is written; the Data step then extracts the ExeFS and packs the RomFS straight from the container.
"""
import hashlib
import copy
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from dataclasses import replace
from pathlib import Path

from . import layout as layouts
from . import state as statefile
from . import ini
from . import dlss
from . import fsr4
from .events import LauncherReporter, Reporter
from .runner import Context, InstallError, Step, run_steps

STATE = 'dkb-install.json'
OWNER = '.dkb-setup-owner.json'
DEST_OWNER = '.dkb-install-owner.json'
NESTED = '.dkb-setup'   # the work folder's parent: beside the install folder, or inside it for the AppImage
GAME = 'dkb100'
TARGETS = (GAME, 'dkb458752')   # 1.0.0 and 3.2.0 (title version 458752); tools/data/expected-<target>.json
NUP_SELECT = ('latest', 'base')
EXE = '.exe' if os.name == 'nt' else ''
CHUNK = 8 << 20
WIDE_SIZES = ('2560x1080', '3440x1440', '3840x1600', '5120x2160', '3840x1080', '5120x1440')
# 16:10 screens: the UI keeps a centred 16:9 area and the overlay fills the bands above and below it; no edge HUD.
TALL_SIZES = ('1280x800', '1680x1050', '1920x1200', '2560x1600')
# The PlayStation prompt overlays and the face buttons each is made for (runtime/romfs.c ps_prompts_init): the two pad
# layouts, and each with A and B on the other layout's buttons (bind_ lines that swap X and Y over the Xbox table, or A
# and B over the Nintendo one; positions of A, B, X, Y in tools/dkb_ps_prompts.py).
PS_FOLDERS = {'romfs-patches-ps': 'xbox', 'romfs-patches-ps-nintendo': 'nintendo', 'romfs-patches-ps-senw': 'senw',
              'romfs-patches-ps-eswn': 'eswn'}
# The Switch-letter overlays for a pad whose face buttons are not where the game's letters say (runtime/romfs.c
# keys_prompts_init): the A, B, X, Y glyphs with their letters permuted, each folder named by what it shows
# (tools/dkb_ps_prompts.py --keys). ABYX swaps X and Y, BAXY A and B, BAYX both (pad_layout = xbox_prompts).
KEY_FOLDERS = {'romfs-patches-keys-ABYX': 'ABYX', 'romfs-patches-keys-BAXY': 'BAXY', 'romfs-patches-keys-BAYX': 'BAYX'}
PROMPT_ARGS = {**{name: ('--layout', layout) for name, layout in PS_FOLDERS.items()},
               **{name: ('--keys', keys) for name, keys in KEY_FOLDERS.items()}}
WIDE_FOLDERS = (tuple(f'romfs-patches-{size}{suffix}' for size in WIDE_SIZES for suffix in ('', '-hud-edges')) +
                tuple(f'romfs-patches-{size}' for size in TALL_SIZES))
COMMIT_WARN_GIB = 24   # the game commits about 14 GB; README asks for 16 GB of RAM plus a large page file


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(CHUNK):
            h.update(block)
    return h.hexdigest()


def digest(*parts):
    h = hashlib.sha256()
    for part in parts:
        h.update(json.dumps(part, sort_keys=True, ensure_ascii=False).encode() + b'\0')
    return h.hexdigest()


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def contains(parent, child):
    return child == parent or parent in child.parents


def ensure_plain(path):
    if any(linked(p) for p in (path, *path.parents)):
        raise InstallError('dkb.path_link', path=str(path))


def plain_tree(root):
    """Reject existing redirects without descending through them, before a tool can reuse the workspace."""
    ensure_plain(root)
    if not root.is_dir():
        return
    def failed(error):
        raise error
    for directory, dirs, files in os.walk(root, followlinks=False, onerror=failed):
        for name in dirs + files:
            p = Path(directory) / name
            if linked(p):
                raise InstallError('dkb.path_link', path=str(p))


def plain_destination(dest, dlls=()):
    # profile/ and caches belong to the player and are never traversed or written by setup.
    files = (STATE, STATE + '.tmp', DEST_OWNER, '.dkb-setup.lock', 'dkb.ini', 'dkb.ini.tmp', 'totk.ini',
             'totk.ini.tmp', 'romfs-index.bin', 'seed-nvn2.bin', 'dkb' + EXE, 'totk_xlate' + EXE, 'totk_xlate.id', *(p.name for p in dlls))
    for name in ('exefs', 'romfs', *WIDE_FOLDERS, *files, *(name + '.setup-part' for name in files)):
        plain_tree(dest / name)


def plain_outputs(ctx):
    plain_tree(ctx.temp)
    plain_destination(ctx.dest, ctx.layout.dlls)


def tree_files(root):
    """Same file selection as dkb_pack_romfs; refuse links instead of following data outside the selected folder."""
    result = []
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            p = Path(directory) / name
            if linked(p):
                raise InstallError('dkb.path_link', path=str(p))
        result += [Path(directory) / n for n in files if not n.startswith('.')]
    return sorted(result, key=lambda p: p.relative_to(root).as_posix().encode())


def inventory(root, reporter):
    files = tree_files(root)
    if not files:
        raise InstallError('dkb.romfs_empty', path=str(root))
    # The largest real RomFS file is about 55 MB; a file past 1 GiB is a RomFS image from a dumping tool.
    if any(p.stat().st_size > 1 << 30 for p in files):
        raise InstallError('dkb.romfs_image', path=str(root))
    result = []
    for i, p in enumerate(files):
        before = p.stat()
        value = sha256(p)
        after = p.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise InstallError('dkb.input_changed', path=str(p))
        result.append({'path': p.relative_to(root).as_posix(), 'size': before.st_size, 'sha256': value})
        reporter.progress('check', 'dkb_hash', i + 1, len(files), 'files')
    return result


def pack_manifest(folder):
    try:
        m = json.loads((folder / 'index.json').read_text(encoding='utf-8'))
        if m.get('status') != 'pass' or not isinstance(m['files'], list) or not m['files']:
            raise ValueError('missing verified file index')
        size, previous = m['romfs_size'], -1
        paths = set()
        for e in m['files']:
            p = Path(e['path'])
            if (not e['path'] or p.is_absolute() or p.drive or '..' in p.parts or '\\' in e['path'] or '\0' in e['path'] or
                    e['path'] in paths or e['offset'] < 0 or e['offset'] < previous or e['size'] < 0 or
                    e['offset'] + e['size'] > size):
                raise ValueError('invalid file range or path')
            paths.add(e['path'])
            previous = e['offset'] + e['size']
        image = folder / 'romfs.bin'
        if image.stat().st_size != size or sha256(image) != m['romfs_sha256']:
            raise ValueError('packed image checksum mismatch')
        return m
    except (OSError, KeyError, TypeError, ValueError) as e:
        raise InstallError('dkb.pack_invalid', path=str(folder), detail=str(e)) from None


def expected_exefs(layout, exefs, game=GAME):
    expected = json.loads((layout.tools / 'data' / f'expected-{game}.json').read_text(encoding='utf-8'))
    hashes = expected['exefs_sha256']
    for name, value in hashes.items():
        p = exefs / name
        ensure_plain(p)
        if not p.is_file() or sha256(p) != value:
            raise InstallError('dkb.exefs_invalid', path=str(p))
    if any(exefs.glob('subsdk*')):
        raise InstallError('dkb.update_unsupported')
    return hashes


def select_target(layout, exefs):
    main = exefs / 'main'
    ensure_plain(main)
    if main.is_file():
        value = sha256(main)
        for game in TARGETS:
            path = layout.tools / 'data' / f'expected-{game}.json'
            if path.is_file():
                expected = json.loads(path.read_text(encoding='utf-8'))
                if expected['exefs_sha256']['main'] == value:
                    return game
    raise InstallError('dkb.exefs_invalid', path=str(main))


def tool_layout(layout, options):
    package = (options.package or layout.root).resolve()
    llvm = (options.toolchain or (package / 'llvm' if (package / 'llvm').is_dir() else layout.llvm)).resolve()
    deps = options.deps or (package / 'runtime' / 'deps' if (package / 'runtime' / 'deps').is_dir() else layout.deps)
    ninja = options.ninja or (package / 'bin' / ('ninja' + EXE))
    if not ninja.is_file():
        ninja = layout.ninja if layout.ninja.is_file() else Path(shutil.which('ninja') or ninja)
    dll_dir = options.dll_dir
    if dll_dir is None:
        # A tool package without the runtime DLLs (#433: a folder picked in the setup's tool package field) falls back
        # to the setup's own bin, which every release carries.
        tries = ([deps / 'bin'] if deps else []) + [package / 'bin', layout.root / 'bin']
        dll_dir = next((d for d in tries if (d / 'SDL3.dll').is_file()), package / 'bin')
    dlls = tuple(sorted(dll_dir.glob('*.dll'))) if os.name == 'nt' else layout.dlls
    return replace(layout, llvm=llvm, deps=deps.resolve() if deps else None, ninja=ninja.resolve(), dlls=dlls)


def source_key(layout, *folders):
    values = []
    for folder in folders:
        base = layout.root / folder
        for p in sorted(base.rglob('*')) if base.is_dir() else [base]:
            if p.is_file() and '__pycache__' not in p.parts:
                values.append((p.relative_to(layout.root).as_posix(), sha256(p)))
    return digest(values)


def tree_key(base):
    return digest([(p.relative_to(base).as_posix(), sha256(p)) for p in sorted(base.rglob('*'))
                   if p.is_file() and '__pycache__' not in p.parts])


def prebuilt_runtime(layout):
    """The release's runtime/libtotkrt.a, built with the folder's toolchain and sysroot, or None: a checkout compiles
    runtime/ from source, and so does a source package that carries no library."""
    return layout.runtime_lib if not layout.dev and layout.runtime_lib.is_file() else None


def tool_module(layout, name):
    spec = importlib.util.spec_from_file_location('_dkb_' + name.removesuffix('.py'), layout.tools / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def translator_identity(layout):
    return tool_module(layout, 'build_xlate.py').identity()


def nup_source(layout, path, select):
    """What a .nup install takes from the container, from its table of contents alone: the target (by the recorded
    SHA-256 of main), the ExeFS hashes checked against the target's, and the RomFS file list (path, size, SHA-256) in
    the format and order inventory() gives for a loose folder. Nothing is decompressed or written."""
    tool = tool_module(layout, 'extract_nup.py')
    try:
        container = tool.open_container(path)
    except (tool.NupError, OSError, ValueError, UnicodeDecodeError) as e:
        raise InstallError('dkb.nup_invalid', path=str(path), detail=str(e)) from None
    try:
        plan = tool.install_plan(container, select)
        exefs = {'/'.join(parts): idx for parts, idx in tool.merged_entries(container, [plan['exefs']], 'exefs')}
        romfs = tool.packed_entries(tool.merged_entries(container, plan['romfs'], 'romfs'))
        package = plan['exefs']
        version = package.display_version or str(package.version)
        recorded = lambda name: container.files[exefs[name]][4].hex() if name in exefs else None
        game = next((g for g in TARGETS if (layout.tools / 'data' / f'expected-{g}.json').is_file() and
                     json.loads((layout.tools / 'data' / f'expected-{g}.json').read_text(encoding='utf-8'))
                     ['exefs_sha256']['main'] == recorded('main')), None)
        if game is None:
            raise InstallError('dkb.nup_version', path=str(path), version=version, title_version=package.version)
        hashes = json.loads((layout.tools / 'data' / f'expected-{game}.json').read_text(encoding='utf-8'))['exefs_sha256']
        for name, value in hashes.items():
            if recorded(name) != value:
                raise InstallError('dkb.exefs_invalid', path=f'{path} (exefs/{name})')
        if any(name.startswith('subsdk') for name in exefs):
            raise InstallError('dkb.update_unsupported')
        files = [{'path': '/'.join(parts), 'size': container.files[idx][1], 'sha256': container.files[idx][4].hex()}
                 for parts, idx in romfs]
        if not files:
            raise InstallError('dkb.romfs_empty', path=f'{path} (romfs)')
        compressed = any(container.files[i][5] for i in (*exefs.values(), *(idx for _, idx in romfs)))
    except tool.NupError as e:
        raise InstallError('dkb.nup_invalid', path=str(path), detail=str(e)) from None
    finally:
        container.close()
    if compressed:
        # The Data step decompresses in layout.python, which can differ from the Python running this check.
        probe = subprocess.run([str(layout.python), str(layout.tools / 'extract_nup.py'), '--check-zstd'],
                               capture_output=True, text=True)
        if probe.returncode:
            raise InstallError('dkb.nup_zstd', path=str(path))
    return game, hashes, files


def unsafe_temp_chars(path):
    # NativeAOT's generated link script puts the work path on a shell command line unquoted (issue 189).
    return ''.join(sorted({ch for ch in str(path) if ch in "()&;|<>$`'\"!*?[]{}#~"}))


def preflight(layout, options, dest, temp, rep):
    if options.base or options.update or options.keys:
        raise InstallError('dkb.update_unsupported')
    if options.jobs < 1:
        raise InstallError('dkb.jobs')
    if options.lang and ini.canonical_language(options.lang) not in ini.LANGUAGES:
        raise InstallError('ui.need_lang')
    ensure_plain(dest / STATE)
    old = statefile.load(dest, STATE)
    if old is not None and (old.get('game') not in TARGETS or old.get('destination') != str(dest)):
        raise InstallError('dkb.dest_unrelated', path=str(dest))
    nup_arg = getattr(options, 'nup', None)
    # A package choice only means something for a .nup, so it counts as asking for one next to folders too.
    if (nup_arg or getattr(options, 'nup_package', None)) and (options.exefs or options.romfs):
        raise InstallError('dkb.nup_and_folders')
    recorded = old['sources'] if old else {}
    if not nup_arg and not options.exefs and not options.romfs and recorded.get('nup'):
        nup_arg = Path(recorded['nup'])
    select = getattr(options, 'nup_package', None) or (recorded.get('nup_package') if nup_arg else None) or 'latest'
    if select not in NUP_SELECT:
        raise InstallError('dkb.nup_invalid', path=str(nup_arg), detail=f'unknown package choice {select}')
    dest_source = False
    if nup_arg:
        for p in (nup_arg, dest, temp):
            ensure_plain(p.absolute())
        nup = nup_arg.resolve()
        if not nup.is_file():
            raise InstallError('dkb.need_nup')
        rep.progress('check', 'dkb_base')
        game, hashes, files = nup_source(layout, nup, select)
        inputs, sources = (nup,), {'nup': str(nup), 'nup_package': select}
    else:
        exefs_arg = options.exefs or (Path(recorded['exefs']) if recorded.get('exefs') else None)
        romfs_arg = options.romfs or (Path(recorded['romfs']) if recorded.get('romfs') else None)
        # The recorded dump folders were moved or deleted: the verified copy in the install folder (ExeFS files and
        # the packed RomFS) is the source, used where it is. Only when nothing was given on the command line, the
        # last run packed the RomFS (not a loose in-place install) and its Data step left a record to verify against.
        dest_source = bool(
            old and not options.exefs and not options.romfs and not recorded.get('romfs_in_place') and
            'sha256' in old.get('steps', {}).get('dkb_data', {}) and
            not (exefs_arg and romfs_arg and exefs_arg.is_dir() and romfs_arg.is_dir()) and
            (dest / 'exefs').is_dir() and (dest / 'romfs' / 'romfs.bin').is_file() and
            (dest / 'romfs' / 'index.json').is_file())
        if dest_source:
            exefs_arg, romfs_arg = dest / 'exefs', dest / 'romfs'
        if not exefs_arg or not romfs_arg:
            raise InstallError('dkb.need_folders')
        for p in (exefs_arg, romfs_arg, dest, temp):
            ensure_plain(p.absolute())
        exefs, romfs = exefs_arg.resolve(), romfs_arg.resolve()
        if not exefs.is_dir() or not romfs.is_dir():
            raise InstallError('dkb.need_folders')
        game = select_target(layout, exefs)
        # The recorded sources stay as they were, so the Data step's key and a later run still work.
        inputs = () if dest_source else (exefs, romfs)   # the install folder is the source: no overlap to reject
        sources = dict(recorded) if dest_source else {'exefs': str(exefs), 'romfs': str(romfs)}
        # In place: the game reads the RomFS folder itself (romfs_dir, runtime/romfs.c loose mode) and no 9 GB image is
        # packed into the install. A resume without folders keeps the choice the last run recorded.
        if getattr(options, 'romfs_in_place', False) or (not options.romfs and recorded.get('romfs_in_place')):
            sources['romfs_in_place'] = True
    in_place = bool(sources.get('romfs_in_place'))
    if getattr(options, 'romfs_in_place', False) and not in_place:
        raise InstallError('dkb.in_place_folder', path=str(nup_arg))
    if old is not None and old['game'] != game:
        raise InstallError('dkb.dest_unrelated', path=str(dest))
    if dest.exists() and not dest.is_dir():
        raise InstallError('dkb.dest_unrelated', path=str(dest))
    nested = nested_work(dest, options.temp, layout.temp)
    for a, b in ((dest, temp), *((out, source) for out in (dest, temp) for source in inputs)):
        if (a, b) == (dest, temp) and nested:
            continue   # the AppImage's default work folder lives in the install folder, and nowhere else may
        if contains(a, b) or contains(b, a):
            raise InstallError('dkb.path_overlap', path=str(a), other=str(b))
    if dest.exists() and old is None and any(dest.iterdir()):
        try:
            ensure_plain(dest / DEST_OWNER)
            partial = json.loads((dest / DEST_OWNER).read_text(encoding='utf-8'))
            ours = partial == {'game': game, 'destination': str(dest)}
            ours &= {p.name for p in dest.iterdir()} <= {DEST_OWNER, '.dkb-setup.lock', NESTED}
        except (OSError, ValueError):
            ours = False
        if not ours:
            raise InstallError('dkb.dest_unrelated', path=str(dest))
    if nuget_needs_fallback(temp) and any(c.isspace() for c in tempfile.gettempdir()):
        raise InstallError('dkb.temp_space', path=str(temp))
    owner = {'game': game, 'destination': str(dest), 'temporary': str(temp)}
    for p in (temp / OWNER, dest / STATE, dest / (STATE + '.tmp'), dest / DEST_OWNER):
        ensure_plain(p)
    if temp.exists() and any(temp.iterdir()):
        try:
            actual = json.loads((temp / OWNER).read_text(encoding='utf-8'))
        except (ValueError, OSError):
            actual = None
        if actual != owner:
            raise InstallError('dkb.temp_unrelated', path=str(temp))
    plain_tree(temp)
    plain_destination(dest, layout.dlls)
    # Reject mixed-version inputs before any writer can replace a checkpoint or installed file.
    # A .nup's ExeFS was checked against its table of contents above, and its file list has the identity of a loose
    # folder holding the same files.
    packed = False
    if not nup_arg:
        rep.progress('check', 'dkb_base')
        hashes = expected_exefs(layout, exefs, game)
        packed = (romfs / 'index.json').is_file() and (romfs / 'romfs.bin').is_file()
    if packed and in_place:
        raise InstallError('dkb.in_place_folder', path=str(romfs))
    if packed:
        ensure_plain(romfs / 'index.json')
        ensure_plain(romfs / 'romfs.bin')
        manifest = pack_manifest(romfs)
        files = None
        romfs_id = digest(manifest)
        romfs_size = 0 if dest_source else manifest['romfs_size']   # verified where it is: nothing is copied
    else:
        files = files if nup_arg else inventory(romfs, rep)
        romfs_id = digest(files)
        romfs_size = 0 if in_place else sum(e['size'] + 15 for e in files)   # in place: nothing copied or packed
    missing = [p for p in (layout.clang, layout.ninja, layout.recomp / 'gen.py',
                           layout.recomp / f'patches-{game}.txt', layout.tools / 'dkb_pack_romfs.py',
                           *([layout.tools / 'extract_nup.py'] if nup_arg else []),
                           *([layout.tools / 'extract_nx.py'] if nup_arg and nup_arg.suffix.lower() == '.nx' else []),
                           layout.tools / 'dkb_shader_census.py',
                           layout.tools / 'dkb_wide_layout.py', layout.tools / 'ultrawide_layout.py',
                           layout.tools / 'dkb_ps_prompts.py', layout.tools / 'data' / f'expected-{game}.json')
               if not p.is_file()]
    # The release compiles nvn2_lower.cpp into libtotkrt.a, so it is not needed when the library is present.
    if prebuilt_runtime(layout) is None and not (layout.runtime / 'nvn2_lower.cpp').is_file():
        missing.append(layout.runtime / 'nvn2_lower.cpp')
    if layout.deps:
        missing += [p for p in (layout.deps / 'include', layout.deps / 'lib') if not p.is_dir()]
    if os.name == 'nt':
        names = {p.name for p in layout.dlls}
        lacking = {'SDL3.dll', 'libopus-0.dll'} - names
        if lacking:
            # Name the folder and the files, so a report (#433) shows whether the package lost them or the wrong
            # folder was searched.
            folder = layout.dlls[0].parent if layout.dlls else (Path(options.dll_dir) if options.dll_dir else
                                                                 layout.root / 'bin')
            raise InstallError('dkb.dlls_missing', path=str(folder), files=', '.join(sorted(lacking)))
    if missing:
        raise InstallError('dkb.tools_missing', paths='; '.join(map(str, missing)))
    identity = translator_identity(layout)
    candidates = [options.translator] if options.translator else [layout.xlate, dest / ('totk_xlate' + EXE)]
    translator = None
    for candidate in candidates:
        if candidate.is_file() and candidate.with_suffix('.id').is_file():
            if candidate.with_suffix('.id').read_text(encoding='ascii').strip() == identity:
                translator = candidate.resolve()
                break
    if options.translator and translator is None:
        raise InstallError('dkb.translator_mismatch')
    dotnet = options.dotnet or layout.root / 'toolchain' / 'dotnet' / ('dotnet' + EXE)
    if not dotnet.is_file():
        dotnet = Path(shutil.which('dotnet') or dotnet)
    if not translator:
        proc = subprocess.run([str(dotnet), '--list-sdks'], capture_output=True, text=True) if dotnet.is_file() else None
        if proc is None or proc.returncode or not any(line.startswith('10.') for line in proc.stdout.splitlines()):
            raise InstallError('dkb.dotnet_missing')
    return dict(old=old, game=game, sources=sources, hashes=hashes, files=files,
                romfs_id=romfs_id, romfs_size=romfs_size, owner=owner, translator=translator, identity=identity,
                dotnet=dotnet, packed=packed, in_place=in_place, dest_source=dest_source,
                pipeline=digest(*(sha256(Path(__file__).with_name(n)) for n in ('bananza.py', 'runner.py', 'state.py'))))


class BananzaContext(Context):
    def save(self):
        plain_outputs(self)
        ensure_plain(self.dest / STATE)
        ensure_plain(self.dest / (STATE + '.tmp'))
        self.dest.mkdir(parents=True, exist_ok=True)
        statefile.save(self.dest, self.state, STATE)


def owned_dir(ctx, path):
    ensure_plain(path)
    if not contains(ctx.temp, path) or path == ctx.temp:
        raise InstallError('dkb.path_overlap', path=str(path), other=str(ctx.temp))
    if json.loads((ctx.temp / OWNER).read_text(encoding='utf-8')) != ctx.info['owner']:
        raise InstallError('dkb.temp_unrelated', path=str(ctx.temp))
    if path.exists():
        plain_tree(path)
        shutil.rmtree(path)
    path.mkdir(parents=True)


def copy_file(src, dst):
    ensure_plain(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    temporary = dst.with_name(dst.name + '.setup-part')
    ensure_plain(temporary)
    try:
        shutil.copyfile(src, temporary)
        try:
            shutil.copymode(src, temporary)  # keeps the executable bit
        except OSError:
            pass
        os.replace(temporary, dst)
    except PermissionError:
        raise InstallError('compile.exe_in_use', path=str(dst)) from None


def nuget_needs_fallback(temp):
    """NativeAOT hands its native libraries to clang unquoted on POSIX, so NuGet must live where no path has a space."""
    return os.name != 'nt' and any(c.isspace() for c in str(temp / 'nuget'))


def lock_temp(temp):
    """An OS-held lock, released even after a crashed process; no PID guessing or stale-lock deletion."""
    path = temp / '.dkb-setup.lock'
    ensure_plain(path)
    stream = path.open('a+b')
    if path.stat().st_size == 0:
        stream.write(b'0')
        stream.flush()
    stream.seek(0)
    try:
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        stream.close()
        raise InstallError('dkb.busy', path=str(temp)) from None
    return stream


def run_tool(ctx, step, command, env=None, cwd=None, name=None):
    plain_outputs(ctx)
    log = ctx.temp / 'logs' / f'{name or step}.log'
    ensure_plain(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    ctx.reporter.progress(step, 'dkb_run', force=True)
    with log.open('w', encoding='utf-8') as out:
        try:
            proc = subprocess.Popen(list(map(str, command)), cwd=cwd or ctx.layout.root, stdin=subprocess.DEVNULL,
                                    stdout=out, stderr=subprocess.STDOUT,
                                    env=dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8', **(env or {})),
                                    start_new_session=os.name != 'nt')
        except OSError as e:
            if getattr(e, 'winerror', None) != 740:   # ERROR_ELEVATION_REQUIRED
                raise
            ctx.reporter.log(f'{step}: Windows requires elevation to start {command[0]}')
            raise InstallError('dkb.elevation_required', exe=Path(str(command[0])).name) from None
        try:
            code = proc.wait()
        except BaseException:
            if proc.poll() is None:
                if os.name == 'nt':
                    subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
                else:
                    os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            raise
    if code:
        raise InstallError('dkb.tool_failed', step=step, code=code, log=str(log))
    plain_outputs(ctx)


class Check(Step):
    id, always = 'check', True

    def run(self, ctx):
        disk = lambda path: next(p for p in (path, *path.parents) if p.exists())
        dest_disk, temp_disk = disk(ctx.dest), disk(ctx.temp)
        free, temp_free = shutil.disk_usage(dest_disk).free, shutil.disk_usage(temp_disk).free
        same_disk = dest_disk.stat().st_dev == temp_disk.stat().st_dev
        temp_need = ctx.info['romfs_size'] + (10 << 30)
        need = ctx.info['romfs_size'] + (3 << 30) + (temp_need if same_disk else 0)
        ctx.reporter.emit('space', dest=str(ctx.dest), need=need, free=free, temp=str(ctx.temp),
                          temp_need=temp_need, temp_free=temp_free, same_disk=same_disk)
        # Name the disk that is short: the temporary folder defaults to the setup folder (layout.temp), not the install
        # folder, so a player who only moves the install folder to a roomier disk still fails on the setup's disk.
        gib = lambda n: round(n / (1 << 30), 1)
        if free < need:
            raise InstallError('dkb.space', path=str(ctx.dest), need=gib(need), free=gib(free))
        if temp_free < temp_need:
            raise InstallError('dkb.temp_disk_space', path=str(ctx.temp), need=gib(temp_need), free=gib(temp_free))
        ctx.temp.mkdir(parents=True, exist_ok=True)
        self.host(ctx)
        ctx.verify_kept(self.id)

    def host(self, ctx):
        exe = ctx.temp / ('hostcheck' + EXE)
        command = [ctx.layout.clang, '-O1', ctx.layout.root / 'installer' / 'hostcheck.c', '-o', exe]
        if ctx.layout.deps:
            command += ['-I', ctx.layout.deps / 'include']
        if os.name != 'nt':
            command += ['-ldl']
            homebrew = Path('/opt/homebrew/include')
            if (homebrew / 'vulkan').is_dir():
                # Homebrew's prefix is not in clang's default search list: the Vulkan header lives there (tools/build_game.py:384)
                command += ['-I', homebrew]
        if ctx.layout.sysroot:
            # The release's clang has no system headers of its own: build against the sysroot, as the game does (#293)
            command += ['--target=x86_64-linux-gnu', f'--sysroot={ctx.layout.sysroot}']
        run_tool(ctx, self.id, command)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        try:
            host = json.loads(result.stdout)
        except ValueError:
            raise InstallError('check.host_probe_failed', code=result.returncode) from None
        # Without AVX2 the game builds for x86-64-v2 (tools/build_game.py then compiles runtime/ instead of the
        # release's v3 library)
        if not host['x86_64_v3'] and not host['x86_64_v2']:
            raise InstallError('check.cpu_no_avx2')
        if not host['loader']:
            raise InstallError('check.vulkan_missing')
        if not any(tuple(d['api'][:2]) >= (1, 3) for d in host['devices']):
            raise InstallError('check.vulkan_no_device')
        commit = host.get('commit_limit_gib')   # Windows only; the game commits about 14 GB, so leave room for the system
        if commit is not None and commit < COMMIT_WARN_GIB:
            ctx.reporter.warning('warn.low_commit_limit', commit=f'{commit:.0f}', need=COMMIT_WARN_GIB)


class Data(Step):
    id = 'dkb_data'

    def key(self, ctx):
        parts = [ctx.info['hashes'], ctx.info['romfs_id'], ctx.info['pipeline'],
                 sha256(ctx.layout.tools / 'dkb_pack_romfs.py')]
        if ctx.info['in_place']:
            parts.append(['romfs_in_place', ctx.info['sources']['romfs']])
        if 'nup' in ctx.info['sources']:   # folder installs keep the key they had before .nup input existed
            parts += [sha256(ctx.layout.tools / 'extract_nup.py'), ctx.info['sources']['nup_package']]
            if ctx.info['sources']['nup'].lower().endswith('.nx'):
                parts.append(sha256(ctx.layout.tools / 'extract_nx.py'))
        if ctx.info['dest_source']:
            # Sourced from the install folder itself: keep the key the copying run recorded, so the steps after this
            # one are not rebuilt because the source moved.
            return ctx.state['steps']['dkb_data']['key']
        return digest(*parts)

    def current(self, ctx, record):
        try:
            expected_exefs(ctx.layout, ctx.dest / 'exefs', ctx.info['game'])
            if 'loose' in record:
                return (Path(record['loose']).is_dir() and
                        sha256(ctx.dest / 'romfs' / 'index.json') == record['index_sha256'])
            image = ctx.dest / 'romfs' / 'romfs.bin'
            return (image.stat().st_size == record['size'] and image.stat().st_mtime_ns == record['mtime_ns'] and
                    sha256(ctx.dest / 'romfs' / 'index.json') == record['index_sha256'])
        except (OSError, InstallError, KeyError):
            return False

    def verify(self, ctx, record):
        if 'loose' in record:
            return self.current(ctx, record)
        try:
            return pack_manifest(ctx.dest / 'romfs')['romfs_sha256'] == record['sha256']
        except InstallError:
            return False

    def run(self, ctx):
        if ctx.info['dest_source']:
            return self.verify_in_place(ctx)
        stage = ctx.temp / 'data'
        owned_dir(ctx, stage)
        sources = ctx.info['sources']
        if 'nup' in sources:
            # Every extracted file is checked against the container's SHA-256 record; the packed image is read back
            # below against the preflight list taken from the same records.
            romfs = Path(sources['nup'])
            run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.tools / 'extract_nup.py', romfs,
                                     '--select', sources['nup_package'], '--pack', '--aoc', '--out', stage])
        else:
            exefs = Path(sources['exefs'])
            for name in ctx.info['hashes']:
                copy_file(exefs / name, stage / 'exefs' / name)
            romfs = Path(sources['romfs'])
        expected_exefs(ctx.layout, stage / 'exefs', ctx.info['game'])
        if ctx.info['in_place']:
            return self.in_place(ctx, stage, romfs)
        set_romfs_dir(ctx, None)
        if ctx.info['packed']:
            for name in ('romfs.bin', 'index.json'):
                copy_file(romfs / name, stage / 'romfs' / name)
        elif 'nup' not in sources:
            run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.tools / 'dkb_pack_romfs.py', '--dir', romfs,
                                     '--out', stage / 'romfs'])
        manifest = pack_manifest(stage / 'romfs')
        if ctx.info['packed'] and digest(manifest) != ctx.info['romfs_id']:
            raise InstallError('dkb.input_changed', path=str(romfs))
        if ctx.info['files'] is not None:
            # Compare every packed file with the preflight content snapshot; no NCA authenticity is claimed.
            files = ctx.info['files']
            if [(e['path'], e['size']) for e in manifest['files']] != [(e['path'], e['size']) for e in files]:
                raise InstallError('dkb.input_changed', path=str(romfs))
            with (stage / 'romfs' / 'romfs.bin').open('rb') as packed:
                for entry, source in zip(manifest['files'], files):
                    packed.seek(entry['offset'])
                    h, remaining = hashlib.sha256(), entry['size']
                    while remaining:
                        block = packed.read(min(CHUNK, remaining))
                        if not block:
                            raise InstallError('dkb.pack_invalid', path=str(romfs), detail='short file')
                        h.update(block)
                        remaining -= len(block)
                    if h.hexdigest() != source['sha256']:
                        raise InstallError('dkb.input_changed', path=str(romfs / source['path']))
        # Final names are replaced only after the complete stage has passed its checks.
        for name in ctx.info['hashes']:
            copy_file(stage / 'exefs' / name, ctx.dest / 'exefs' / name)
        for name in ('romfs.bin', 'index.json'):
            copy_file(stage / 'romfs' / name, ctx.dest / 'romfs' / name)
        # Add-on content found in the .nup: a failed pack was skipped by the tool, so only whole folders are here.
        for folder in sorted((stage / 'aoc').glob('aoc-*')):
            try:
                for name in ('romfs.bin', 'aoc-index.bin'):
                    copy_file(folder / name, ctx.dest / folder.name / name)
            except (InstallError, OSError) as e:
                ctx.reporter.log(f'{self.id}: add-on content {folder.name} not installed: {e}')
        image = ctx.dest / 'romfs' / 'romfs.bin'
        return {'sha256': manifest['romfs_sha256'], 'size': image.stat().st_size, 'mtime_ns': image.stat().st_mtime_ns,
                'index_sha256': sha256(ctx.dest / 'romfs' / 'index.json')}


    def verify_in_place(self, ctx):
        """Check the installed ExeFS and both parts of the packed RomFS before reusing them."""
        expected_exefs(ctx.layout, ctx.dest / 'exefs', ctx.info['game'])
        romfs = ctx.dest / 'romfs'
        recorded = ctx.state['steps']['dkb_data']
        if pack_manifest(romfs)['romfs_sha256'] != recorded['sha256']:
            raise InstallError('dkb.input_changed', path=str(romfs))
        index = romfs / 'index.json'
        index_sha256 = sha256(index)
        if index_sha256 != recorded.get('index_sha256'):
            raise InstallError('dkb.input_changed', path=str(index))
        image = romfs / 'romfs.bin'
        return {'sha256': recorded['sha256'], 'size': image.stat().st_size, 'mtime_ns': image.stat().st_mtime_ns,
                'index_sha256': index_sha256}

    def in_place(self, ctx, stage, romfs):
        """index.json of the folder as dkb_pack_romfs.py would pack it (the offsets only size the index), with each
        file's SHA-256 from preflight, and romfs_dir in dkb.ini; the installed romfs.bin, if any, is removed."""
        entries, off = [], 0
        for e in ctx.info['files']:
            off += -off % 16
            entries.append({'path': e['path'], 'offset': off, 'size': e['size'], 'sha256': e['sha256']})
            off += e['size']
        manifest = {'status': 'pass', 'source': 'loose RomFS folder read in place; each file hashed by the setup',
                    'loose': str(romfs), 'romfs_size': off, 'romfs_sha256': ctx.info['romfs_id'], 'files': entries}
        (stage / 'romfs').mkdir(parents=True, exist_ok=True)
        (stage / 'romfs' / 'index.json').write_text(json.dumps(manifest), encoding='utf-8')
        for name in ctx.info['hashes']:
            copy_file(stage / 'exefs' / name, ctx.dest / 'exefs' / name)
        copy_file(stage / 'romfs' / 'index.json', ctx.dest / 'romfs' / 'index.json')
        ensure_plain(ctx.dest / 'romfs' / 'romfs.bin')
        (ctx.dest / 'romfs' / 'romfs.bin').unlink(missing_ok=True)
        set_romfs_dir(ctx, str(romfs))
        return {'loose': str(romfs), 'index_sha256': sha256(ctx.dest / 'romfs' / 'index.json')}


def set_romfs_dir(ctx, folder):
    """romfs_dir in dkb.ini: the folder the game reads in place, or None to remove the key (the packed image)."""
    for name in ('dkb.ini', 'dkb.ini.tmp', 'totk.ini'):
        ensure_plain(ctx.dest / name)
    if folder or (ctx.dest / 'dkb.ini').is_file() or (ctx.dest / 'totk.ini').is_file():
        ctx.dest.mkdir(parents=True, exist_ok=True)
        ini.write(ctx.dest, None, ctx.layout.ini_example, 'dkb.ini', fallback='totk.ini',
                  change=lambda text: ini.set_value(text, 'romfs_dir', folder, 'files'))


class Translator(Step):
    id = 'dkb_translator'

    def key(self, ctx):
        return digest(ctx.info['identity'], sha256(ctx.info['translator']) if ctx.info['translator'] else 'build')

    def current(self, ctx, record):
        exe = ctx.dest / ('totk_xlate' + EXE)
        return (exe.is_file() and sha256(exe) == record.get('sha256') and exe.with_suffix('.id').is_file() and
                exe.with_suffix('.id').read_text(encoding='ascii').strip() == ctx.info['identity'])

    def run(self, ctx):
        source = ctx.info['translator']
        if source is None:
            out = ctx.temp / 'xlate'
            nuget = Path(tempfile.mkdtemp(prefix='dkb-nuget-')) if nuget_needs_fallback(ctx.temp) else ctx.temp / 'nuget'
            env = {'PATH': str(ctx.info['dotnet'].parent) + os.pathsep + os.environ.get('PATH', ''),
                   'DOTNET_ROOT': str(ctx.info['dotnet'].parent), 'DOTNET_CLI_HOME': str(ctx.temp / 'dotnet-home'),
                   'NUGET_PACKAGES': str(nuget),
                   'NUGET_HTTP_CACHE_PATH': str(nuget / 'http-cache'),   # NuGet's download cache: in the work folder too
                   'DOTNET_PROCESSOR_COUNT': str(ctx.options.jobs),
                   # Reusable MSBuild nodes retain NuGet task DLLs after publish and block Windows cleanup.
                   # These settings affect only this build, not the owner's other .NET work.
                   'MSBUILDDISABLENODEREUSE': '1', 'DOTNET_CLI_USE_MSBUILD_SERVER': '0',
                   'UseSharedCompilation': 'false',
                   # A fresh DOTNET_CLI_HOME is a first run: without these, dotnet adds that home's tools folder to
                   # the user's PATH for good, one dead entry per install, until cmd can no longer find System32.
                   'DOTNET_ADD_GLOBAL_TOOLS_TO_PATH': 'false', 'DOTNET_SKIP_FIRST_TIME_EXPERIENCE': '1',
                   'DOTNET_NOLOGO': '1', 'DOTNET_CLI_TELEMETRY_OPTOUT': '1'}
            command = [ctx.layout.python, ctx.layout.tools / 'build_xlate.py', '--out', out]
            if os.name != 'nt':
                command += ['--llvm', ctx.layout.llvm]
                if ctx.layout.sysroot:
                    command += ['--sysroot', ctx.layout.sysroot]
            try:
                run_tool(ctx, self.id, command, env)
            finally:
                if nuget.parent != ctx.temp:
                    shutil.rmtree(nuget, ignore_errors=True)
            source = out / ('totk_xlate' + EXE)
        if source.with_suffix('.id').read_text(encoding='ascii').strip() != ctx.info['identity']:
            raise InstallError('dkb.translator_mismatch')
        target = ctx.dest / ('totk_xlate' + EXE)
        copy_file(source, target)
        copy_file(source.with_suffix('.id'), target.with_suffix('.id'))
        return {'sha256': sha256(target)}


class Lift(Step):
    id, transient = 'lift', True

    def key(self, ctx):
        return digest(ctx.keys['dkb_data'], source_key(ctx.layout, 'recomp'))

    def current(self, ctx, record):
        gen = ctx.temp / 'gen'
        try:
            for name in ('main', 'sdk'):
                m = json.loads((gen / name / 'manifest.json').read_text(encoding='utf-8'))
                if not m['chunks'] or not all((gen / name / chunk).is_file() for chunk in m['chunks']):
                    return False
            return ((gen / 'layout.json').is_file() and (gen / 'patches.h').is_file() and
                    record.get('outputs') == tree_key(gen))
        except (OSError, KeyError, ValueError):
            return False

    def run(self, ctx):
        gen = ctx.temp / 'gen'
        plain_outputs(ctx)
        gen.mkdir(parents=True, exist_ok=True)
        run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.recomp / 'gen.py', '--exefs', ctx.dest / 'exefs',
                                 '--out', gen, '--modules', 'main,sdk', '--jobs', ctx.options.jobs,
                                 '--patches', ctx.layout.recomp / f"patches-{ctx.info['game']}.txt"])
        manifest = json.loads((gen / 'main' / 'manifest.json').read_text(encoding='utf-8'))
        if ctx.info['game'] == GAME and manifest.get('stats', {}).get('patched_sites', 0) < 1:
            raise InstallError('dkb.patches_missing')
        return {'outputs': tree_key(gen)}


class Compile(Step):
    id = 'compile'

    def key(self, ctx):
        lib = prebuilt_runtime(ctx.layout)
        return digest(ctx.keys['lift'], source_key(ctx.layout, 'runtime', 'tools/build_game.py', 'tools/romfs_index.py'),
                      str(ctx.layout.llvm), str(ctx.layout.deps),
                      sha256(ctx.layout.clang), sha256(ctx.layout.ninja),
                      tree_key(ctx.layout.deps) if ctx.layout.deps else 'system-libraries',
                      [(str(p), sha256(p)) for p in ctx.layout.dlls],
                      sha256(lib) if lib else 'runtime-from-source')

    def current(self, ctx, record):
        exe = ctx.dest / ('dkb' + EXE)
        try:
            return (exe.is_file() and sha256(exe) == record.get('sha256') and
                    sha256(ctx.dest / 'romfs-index.bin') == record.get('index_sha256') and
                    all(sha256(ctx.dest / p.name) == sha256(p) for p in ctx.layout.dlls))
        except OSError:
            return False

    def run(self, ctx):
        out = ctx.temp / 'build'
        game = ctx.info['game']
        args = [ctx.layout.python, ctx.layout.tools / 'build_game.py', '--version', game, '--expected',
                ctx.layout.tools / 'data' / f'expected-{game}.json', '--gen', ctx.temp / 'gen', '--out', out,
                '--modules', 'main,sdk', '--exefs', ctx.dest / 'exefs', '--romfs', ctx.dest / 'romfs',
                '--profile', ctx.dest / 'profile', '--runtime', ctx.layout.runtime, '--toolchain', ctx.layout.llvm,
                '--ninja', ctx.layout.ninja, '--jobs', ctx.options.jobs, '--full-build']
        lib = prebuilt_runtime(ctx.layout)
        if lib:
            args += ['--runtime-lib', lib]
        if ctx.layout.deps:
            args += ['--deps', ctx.layout.deps]
        if ctx.layout.sysroot:
            args += ['--sysroot', ctx.layout.sysroot]
        run_tool(ctx, self.id, args)
        for p in (out / ('dkb' + EXE), out / 'romfs-index.bin', *ctx.layout.dlls):
            copy_file(p, ctx.dest / p.name)
        return {'sha256': sha256(ctx.dest / ('dkb' + EXE)), 'index_sha256': sha256(ctx.dest / 'romfs-index.bin')}


class Shaders(Step):
    """Translate every shader stage in the game's files into the shader cache, so no draw waits for a first translation.

    tools/dkb_shader_census.py lists the stages (RomFS) into seed-nvn2.bin and dkb.exe --translate-shaders lowers and
    translates them. The cache belongs to the player and survives later steps (the verify step folds it into
    shader-cache.pack); the 187 MiB seed pack is removed again."""
    id = 'dkb_shaders'

    def key(self, ctx):
        return digest(ctx.keys['compile'], ctx.keys['dkb_translator'], ctx.keys['dkb_data'],
                      sha256(ctx.layout.tools / 'dkb_shader_census.py'))

    def current(self, ctx, record):
        return record.get('prepared') is True and (ctx.dest / 'shader-cache').exists()

    def run(self, ctx):
        pack = ctx.dest / 'seed-nvn2.bin'
        run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.tools / 'dkb_shader_census.py', '--version', ctx.info['game'],
                                '--romfs', ctx.dest / 'romfs', '--out', ctx.dest], name=f'{self.id}-census')
        try:
            run_tool(ctx, self.id, [ctx.dest / ('dkb' + EXE), '--translate-shaders'], cwd=ctx.dest)
        finally:
            pack.unlink(missing_ok=True)
        # The driver pipeline libraries are an optimisation: a driver that crashes building them (nvgpucomp64.dll on
        # NVIDIA 621.01) must not fail the step, or every retry would crash again.
        libraries = True
        try:
            run_tool(ctx, self.id, [ctx.dest / ('dkb' + EXE), '--warm-shaders'], cwd=ctx.dest,
                     name=f'{self.id}-warm')
        except InstallError as error:
            if error.id != 'dkb.tool_failed':   # a path-safety rejection still stops the install
                raise
            # On Windows Ctrl+C the child can exit 0xC000013A before the parent handler raises; that is recorded as
            # libraries:false here, which is harmless.
            libraries = False
            ctx.reporter.warning('warn.dkb_libraries_failed', code=error.params.get('code'),
                                 log=str(error.params.get('log')))
        return {'prepared': True, 'libraries': libraries}


class WideLayouts(Step):
    """Validate both HUD variants for every wide menu preset before publishing any overlay."""
    id = 'dkb_wide'

    def key(self, ctx):
        return digest(ctx.keys['dkb_data'], ctx.info['game'], WIDE_SIZES, TALL_SIZES,
                      source_key(ctx.layout, 'tools/dkb_wide_layout.py', 'tools/ultrawide_layout.py'))

    def current(self, ctx, record):
        try:
            return (set(record.get('outputs', {})) == set(WIDE_FOLDERS) and
                    all((ctx.dest / name / 'manifest.tsv').is_file() and
                        tree_key(ctx.dest / name) == record['outputs'][name] for name in WIDE_FOLDERS))
        except OSError:
            return False

    def verify(self, ctx, record):
        return self.current(ctx, record)

    def run(self, ctx):
        plain_outputs(ctx)
        staged = ctx.temp / 'build' / 'wide-overlays'
        owned_dir(ctx, staged)
        try:
            run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.tools / 'dkb_wide_layout.py',
                                    '--version', ctx.info['game'],
                                    '--romfs', ctx.dest / 'romfs', '--out', staged,
                                    '--installer-workspace', ctx.temp, '--presets', *WIDE_SIZES, *TALL_SIZES])
        except InstallError as error:
            if error.id != 'dkb.tool_failed':   # path/ownership failures still stop setup
                raise
            ctx.reporter.warning('warn.dkb_wide_failed', code=error.params.get('code'),
                                 log=str(error.params.get('log')))
            # These assets are optional for 16:9 play. Publish nothing on failure, retain any installed overlays,
            # and leave outputs unrecorded so the next setup run can retry generation.
            return {'prepared': False}
        plain_outputs(ctx)
        # Every preset has been validated. Publish each manifest last, so an interrupted copy is detected by
        # the runtime's resource hashes and the next install repairs it without recompiling the game.
        for name in WIDE_FOLDERS:
            folder = staged / name
            for source in sorted(folder.rglob('*')):
                if source.is_file() and source.name != 'manifest.tsv':
                    copy_file(source, ctx.dest / name / source.relative_to(folder))
            copy_file(folder / 'manifest.tsv', ctx.dest / name / 'manifest.tsv')
        return {'outputs': {name: tree_key(ctx.dest / name) for name in WIDE_FOLDERS}}


class PadPrompts(Step):
    """The PlayStation button prompts (tools/dkb_ps_prompts.py): one overlay per face-button layout, and the
    Switch-letter overlays (--keys), from the user's font."""
    id = 'dkb_prompts'

    def key(self, ctx):
        return digest(ctx.keys['dkb_data'], ctx.info['game'], PROMPT_ARGS, source_key(ctx.layout, 'tools/dkb_ps_prompts.py'))

    def current(self, ctx, record):
        try:
            return (set(record.get('outputs', {})) == set(PROMPT_ARGS) and
                    all((ctx.dest / name / 'manifest.tsv').is_file() and
                        tree_key(ctx.dest / name) == record['outputs'][name] for name in PROMPT_ARGS))
        except OSError:
            return False

    def verify(self, ctx, record):
        return self.current(ctx, record)

    def run(self, ctx):
        plain_outputs(ctx)
        staged = ctx.temp / 'build' / 'ps-prompts'
        owned_dir(ctx, staged)
        try:
            for name, (option, value) in PROMPT_ARGS.items():
                run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.tools / 'dkb_ps_prompts.py',
                                        '--romfs', ctx.dest / 'romfs', '--out', staged / name, option, value,
                                        '--installer-workspace', ctx.temp], name=f'{self.id}-{value}')
        except InstallError as error:
            if error.id != 'dkb.tool_failed':   # path/ownership failures still stop setup
                raise
            ctx.reporter.warning('warn.dkb_prompts_failed', code=error.params.get('code'),
                                 log=str(error.params.get('log')))
            # Optional: the Switch prompts stay. Nothing is published and nothing recorded, so the next setup retries.
            return {'prepared': False}
        plain_outputs(ctx)
        for name in PROMPT_ARGS:   # each manifest last, as WideLayouts
            folder = staged / name
            for source in sorted(folder.rglob('*')):
                if source.is_file() and source.name != 'manifest.tsv':
                    copy_file(source, ctx.dest / name / source.relative_to(folder))
            copy_file(folder / 'manifest.tsv', ctx.dest / name / 'manifest.tsv')
        return {'outputs': {name: tree_key(ctx.dest / name) for name in PROMPT_ARGS}}


def fsr4_platform():
    """AMD publishes the FSR 4 libraries for Windows only (docs/dkb-dlss.md)."""
    return os.name == 'nt'


class Fsr4(Step):
    """AMD FSR 4's Vulkan libraries next to dkb.exe (installer/fsr4.py), only when the user asked for them and accepted
    AMD's license of the pinned release: --fsr4 accept (the window asks first), recorded as state['fsr4'] = that release
    so a later setup keeps them. --fsr4 off stops fetching them; files already there stay, and upscaler = fsr4 is still
    the user's choice either way. A failed download warns and setup goes on with FSR 3.1; the next setup tries again."""
    id = 'dkb_fsr4'

    def key(self, ctx):
        return digest('fsr4', ctx.state.get('fsr4') or 'off')

    def current(self, ctx, record):
        if not ctx.state.get('fsr4') or not fsr4_platform():
            return True
        try:   # sizes only here; verify hashes them
            return record.get('files') is True and all(
                (ctx.dest / name).stat().st_size == size for _, name, size, _ in (fsr4.LICENSE, *fsr4.FILES))
        except OSError:
            return False

    def verify(self, ctx, record):
        return not ctx.state.get('fsr4') or not fsr4_platform() or fsr4.installed(ctx.dest)

    def run(self, ctx):
        if not ctx.state.get('fsr4'):
            return {'files': False}
        if not fsr4_platform():
            ctx.reporter.log('fsr4: AMD publishes no library for this system; nothing fetched')
            return {'files': False}
        for _, name, _, _ in (fsr4.LICENSE, *fsr4.FILES):
            ensure_plain(ctx.dest / name)
            ensure_plain(ctx.dest / (name + '.part'))
        total = fsr4.total_bytes()
        ctx.reporter.progress(self.id, 'dkb_fetch', 0, total, 'bytes', force=True)
        try:
            fsr4.fetch(ctx.dest, lambda done: ctx.reporter.progress(self.id, 'dkb_fetch', done, total, 'bytes'))
        except fsr4.FetchError as error:
            ctx.reporter.warning('warn.dkb_fsr4_failed', file=error.args[0], detail=error.args[1])
            return {'files': False}   # not current: the next setup tries again
        return {'files': True, 'tag': fsr4.TAG}


class Dlss(Step):
    """NVIDIA's nvngx_dlss.dll next to dkb.exe (installer/dlss.py), only when the user asked for it and accepted NVIDIA's
    license of the pinned release: --dlss accept (the window asks first), recorded as state['dlss'] = that release so a
    later setup keeps it. --dlss off stops fetching it; files already there stay. A failed download warns and setup
    goes on; the next setup tries again. DLSS needs an NVIDIA RTX GPU; setup does not check for one."""
    id = 'dkb_dlss'

    def key(self, ctx):
        return digest('dlss', ctx.state.get('dlss') or 'off')

    def current(self, ctx, record):
        if not ctx.state.get('dlss') or not fsr4_platform():
            return True
        try:   # sizes only here; verify hashes them
            return record.get('files') is True and all(
                (ctx.dest / name).stat().st_size == size for _, name, size, _ in (dlss.LICENSE, *dlss.FILES))
        except OSError:
            return False

    def verify(self, ctx, record):
        return not ctx.state.get('dlss') or not fsr4_platform() or dlss.installed(ctx.dest)

    def run(self, ctx):
        if not ctx.state.get('dlss'):
            return {'files': False}
        if not fsr4_platform():
            ctx.reporter.log('dlss: DLSS is for Windows only here; nothing fetched')
            return {'files': False}
        for _, name, _, _ in (dlss.LICENSE, *dlss.FILES):
            ensure_plain(ctx.dest / name)
            ensure_plain(ctx.dest / (name + '.part'))
        total = dlss.total_bytes()
        ctx.reporter.progress(self.id, 'dkb_fetch', 0, total, 'bytes', force=True)
        try:
            dlss.fetch(ctx.dest, lambda done: ctx.reporter.progress(self.id, 'dkb_fetch', done, total, 'bytes'))
        except dlss.FetchError as error:
            ctx.reporter.warning('warn.dkb_dlss_failed', file=error.args[0], detail=error.args[1])
            return {'files': False}   # not current: the next setup tries again
        return {'files': True, 'tag': dlss.TAG}


class Prepare(Step):
    id = 'dkb_verify'

    def key(self, ctx):
        return digest(ctx.keys['compile'], ctx.keys['dkb_translator'], ctx.state['lang'])

    def current(self, ctx, record):
        return record.get('selftest') is True

    def run(self, ctx):
        for name in ('dkb.ini', 'dkb.ini.tmp', 'totk.ini'):
            ensure_plain(ctx.dest / name)
        ini.write(ctx.dest, ctx.state['lang'], ctx.layout.ini_example, 'dkb.ini', fallback='totk.ini')
        run_tool(ctx, self.id, [ctx.dest / ('dkb' + EXE), '--selftest'])
        # One shader-cache.pack instead of four loose files per shader (about 118,000 files on 3.2.0, which take half an
        # hour to delete on Windows). The runtime reads the pack first and the loose files after it (gfx_shader.c, cache
        # pack). Only a speed-up for copying and deleting, so a failure here is a warning.
        try:
            run_tool(ctx, self.id, [ctx.layout.python, ctx.layout.tools / 'pack_shader_cache.py', ctx.dest / 'shader-cache'],
                     name=f'{self.id}-pack')
        except InstallError as error:
            if error.id != 'dkb.tool_failed':
                raise
            ctx.reporter.warning('warn.dkb_pack_failed', code=error.params.get('code'), log=str(error.params.get('log')))
        return {'selftest': True}


class Cleanup(Step):
    id, always = 'cleanup', True

    def run(self, ctx):
        # Update 458752 keeps generated C and objects so a second install does not relift or recompile.
        keep = ctx.info['game'] != GAME
        for name in ('data', 'build/wide-overlays', 'xlate', 'dotnet-home', 'nuget') if keep else ('data', 'gen', 'build', 'xlate', 'dotnet-home', 'nuget'):
            p = ctx.temp / name
            if p.exists():
                owned_dir(ctx, p)
                p.rmdir()


def all_steps():
    return [Check(), Data(), Translator(), Lift(), Compile(), Shaders(), WideLayouts(), PadPrompts(), Fsr4(), Dlss(), Prepare(),
            Cleanup()]


def cancel(*_):
    raise KeyboardInterrupt


def write_owner(path, value):
    ensure_plain(path)
    try:
        with path.open('x', encoding='utf-8') as stream:
            json.dump(value, stream)
    except FileExistsError:
        if json.loads(path.read_text(encoding='utf-8')) != value:
            raise InstallError('dkb.temp_unrelated', path=str(path.parent)) from None


def nested_work(dest, temp, layout_temp):
    """True when the work folder goes inside dest: the AppImage's mount is read-only and its folder may sit on a
    full disk, so with no --temp the install folder (on the player's chosen drive) holds it."""
    return bool(os.environ.get('APPIMAGE')) and not temp and not layout_temp


def work_folder(dest, temp, layout_temp):
    """The folder that holds this install's generated C, objects and logs, for a resolved dest."""
    raw_base = (temp or layout_temp or (dest if nested_work(dest, temp, layout_temp) else dest.parent) / NESTED).absolute()
    ensure_plain(raw_base)
    tag = hashlib.sha256(str(dest).lower().encode()).hexdigest()[:12]
    # The work folder's name goes onto compiler and MSBuild command lines that split on spaces (issue 134): keep
    # only characters that need no quoting. The tag already tells two destinations apart; a name that was safe
    # keeps its folder, so an interrupted setup still resumes.
    safe = ''.join(ch if ch.isascii() and (ch.isalnum() or ch in '._-') else '_' for ch in dest.name)
    return raw_base.resolve() / f'dkb-{safe}-{tag}'


def folder_size(folder):
    return sum(p.stat().st_size for p in folder.rglob('*') if p.is_file())


def clean(dest, temp=None, layout_temp=None):
    """Delete the work folder of the install at dest, once its owner file says it is ours; returns (folder, bytes
    freed). dest itself is never touched."""
    ensure_plain(dest.absolute())
    dest = dest.resolve()
    folder = work_folder(dest, temp, layout_temp)
    if not folder.is_dir():
        raise InstallError('dkb.clean_nothing', path=str(folder))
    try:
        ensure_plain(folder / OWNER)
        owner = json.loads((folder / OWNER).read_text(encoding='utf-8'))
        ours = (isinstance(owner, dict) and owner.get('destination') == str(dest) and owner.get('temporary') == str(folder)
                and owner.get('game') in TARGETS)
    except (OSError, ValueError):
        ours = False
    if not ours:
        raise InstallError('dkb.temp_unrelated', path=str(folder))
    plain_tree(folder)
    # A setup running on this folder holds its lock: dkb.busy then. The lock is released before the delete, since
    # Windows cannot delete a file this process holds open; a setup starting in between is the window that remains.
    lock_temp(folder).close()
    size = folder_size(folder)
    shutil.rmtree(folder)
    return folder, size


def clean_main(options):
    rep = Reporter()
    try:
        folder, size = clean(options.dest, options.temp, layouts.find(options.root).temp)
    except InstallError as e:
        rep.error(e.id, e.params)
        return 1
    rep.emit('cleaned', folder=str(folder), bytes=size)
    return 0


KEPT_SAVES = ('profile', 'dkb.ini', 'mods')   # the player's own files (saves, settings, loose mods), kept unless asked


def uninstall(dest, temp=None, layout_temp=None, keep_saves=True):
    """Remove the install at dest and its work folder; returns (removed paths, bytes freed). Needs our state or owner
    record for this destination. With keep_saves, profile/ and dkb.ini stay and dest itself stays; links are refused
    before anything is deleted and never followed."""
    ensure_plain(dest.absolute())
    dest = dest.resolve()
    ensure_plain(dest / STATE)
    ensure_plain(dest / DEST_OWNER)
    try:
        old = statefile.load(dest, STATE)
        ours = old is not None and old.get('game') in TARGETS and old.get('destination') == str(dest)
        if not ours and old is None:
            owner = json.loads((dest / DEST_OWNER).read_text(encoding='utf-8'))
            ours = isinstance(owner, dict) and owner.get('destination') == str(dest) and owner.get('game') in TARGETS
    except (OSError, ValueError, statefile.StateError):
        ours = False
    if not ours:
        raise InstallError('dkb.uninstall_unowned', path=str(dest))
    # dkb.busy while a setup runs. Taken before the listing, since it creates the lock file the listing must include;
    # released before the deletes, since Windows cannot delete a file this process holds open.
    lock_temp(dest).close()
    entries = [p for p in dest.iterdir() if not (keep_saves and p.name.lower() in KEPT_SAVES)]
    for p in entries:
        plain_tree(p)
        if linked(p):
            raise InstallError('dkb.path_link', path=str(p))
    folder = work_folder(dest, temp, layout_temp)
    plain_tree(folder)
    removed, freed = [], 0
    if folder.is_dir():
        try:
            folder, size = clean(dest, temp, layout_temp)
            removed.append(str(folder))
            freed += size
        except InstallError as e:
            if e.id != 'dkb.clean_nothing':
                raise
    for p in entries:
        if p.is_dir():
            freed += folder_size(p)
            shutil.rmtree(p)
        else:
            freed += p.stat().st_size
            p.unlink()
        removed.append(str(p))
    if not keep_saves:
        dest.rmdir()
    return removed, freed


def uninstall_main(options):
    rep = Reporter()
    try:
        removed, freed = uninstall(options.dest, options.temp, layouts.find(options.root).temp,
                                   keep_saves=not options.delete_saves)
    except InstallError as e:
        rep.error(e.id, e.params)
        return 1
    rep.emit('uninstalled', removed=removed, bytes=freed, saves_kept=not options.delete_saves)
    return 0


def main(options):
    rep = LauncherReporter() if getattr(options, 'prepare', False) else Reporter()
    locks = []
    started = time.monotonic()
    try:
        for name in ('SIGBREAK', 'SIGTERM'):
            if hasattr(signal, name):
                signal.signal(getattr(signal, name), cancel)
        ensure_plain(options.dest.absolute())
        dest = options.dest.resolve()
        ensure_plain(dest / STATE)
        previous = statefile.load(dest, STATE)
        options = copy.copy(options)
        found = layouts.find(options.root)
        # A resume reuses the tool paths the last run recorded, for a checkout run with its own --toolchain or --deps.
        # A release carries its tools, and the folder of the release that made the install may be gone or older (a new
        # release unpacked elsewhere): its own tools win there, and so does an explicit --package. A recorded path
        # that no longer exists is never reused.
        if (previous and previous.get('game') in TARGETS and previous.get('destination') == str(dest) and found.dev and
                options.package is None):
            for name, value in previous.get('build_tools', {}).items():
                if (name in ('toolchain', 'deps', 'ninja', 'dll_dir', 'dotnet') and getattr(options, name) is None and
                        value and Path(value).exists()):
                    setattr(options, name, Path(value))
        layout = tool_layout(found, options)
        temp = work_folder(dest, options.temp, layout.temp)
        if os.name != 'nt' and unsafe_temp_chars(temp.parent):
            raise InstallError('dkb.temp_unsafe_path', path=str(temp.parent))
        info = preflight(layout, options, dest, temp, rep)
        # All read-only validation succeeded. Logs and owned output may now be created.
        temp.mkdir(parents=True, exist_ok=True)
        write_owner(temp / OWNER, info['owner'])
        locks.append(lock_temp(temp))
        dest.mkdir(parents=True, exist_ok=True)
        write_owner(dest / DEST_OWNER, {'game': info['game'], 'destination': str(dest)})
        locks.append(lock_temp(dest))
        plain_tree(temp)
        plain_destination(dest, layout.dlls)
        log = temp / 'logs' / time.strftime('install-%Y%m%d-%H%M%S.log')
        ensure_plain(log)
        log.parent.mkdir(parents=True, exist_ok=True)
        rep.log_file = log.open('a', encoding='utf-8', buffering=1)
        rep.log_path = log
        ctx = BananzaContext(layout, dest, temp, rep, options, state=info['old'], verify=options.verify)
        ctx.info = info
        ctx.state.update(game=info['game'], destination=str(dest), sources=info['sources'])
        ctx.state['build_tools'] = {'toolchain': str(layout.llvm), 'deps': str(layout.deps) if layout.deps else None,
                                    'ninja': str(layout.ninja),
                                    'dll_dir': str(layout.dlls[0].parent) if layout.dlls else None,
                                    'dotnet': str(info['dotnet'])}
        ctx.state['lang'] = ini.canonical_language(options.lang or ctx.state.get('lang') or 'en-US')
        choice = getattr(options, 'fsr4', None)   # accept: the user accepted AMD's license of fsr4.TAG
        if choice == 'accept':
            ctx.state['fsr4'] = fsr4.TAG
        elif choice == 'off':
            ctx.state.pop('fsr4', None)
        elif ctx.state.get('fsr4') not in (None, fsr4.TAG):
            ctx.state.pop('fsr4', None)   # accepted for another release: its license is to be accepted again
            rep.warning('warn.dkb_fsr4_new_release', release=fsr4.TAG)
        choice = getattr(options, 'dlss', None)   # accept: the user accepted NVIDIA's license of dlss.TAG
        if choice == 'accept':
            ctx.state['dlss'] = dlss.TAG
        elif choice == 'off':
            ctx.state.pop('dlss', None)
        elif ctx.state.get('dlss') not in (None, dlss.TAG):
            ctx.state.pop('dlss', None)   # accepted for another release: its license is to be accepted again
            rep.warning('warn.dkb_dlss_new_release', release=dlss.TAG)
        steps = all_steps()
        rep.emit('start', release=layout.release, mode=ctx.mode, dev=layout.dev, dest=str(dest), log=str(log),
                 game=info['game'], game_exe='dkb' + EXE, steps=[s.id for s in steps])
        result = run_steps(steps, ctx)
    except InstallError as e:
        rep.error(e.id, e.params)
        result = 'failed'
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)})
        result = 'failed'
    except KeyboardInterrupt:
        result = 'cancelled'
    except (OSError, ValueError, KeyError, TypeError, ImportError) as e:
        rep.error('internal', {'exception': type(e).__name__, 'detail': str(e)})
        result = 'failed'
    rep.emit('end', result=result, seconds=round(time.monotonic() - started, 1))
    rep.close()
    for lock in locks:
        lock.close()
    return {'ok': 0, 'failed': 1, 'cancelled': 3}[result]
