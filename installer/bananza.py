"""Validated Bananza folder installs. No container extraction, keys or Python 3.14 dependencies.

The existing runner owns step events and atomic checkpoints. Game-derived temporary files stay in a separately
marked task directory; only verified files enter the portable destination. No prebuilt TotK runtime is reused.
"""
import hashlib
import copy
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import time
from dataclasses import replace
from pathlib import Path

from . import layout as layouts
from . import state as statefile
from . import ini
from .events import Reporter
from .runner import Context, InstallError, Step, run_steps

STATE = 'dkb-install.json'
OWNER = '.dkb-setup-owner.json'
DEST_OWNER = '.dkb-install-owner.json'
GAME = 'dkb100'
TARGETS = (GAME, 'dkb458752')
EXE = '.exe' if os.name == 'nt' else ''
CHUNK = 8 << 20


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
    for name in ('exefs', 'romfs', *files, *(name + '.setup-part' for name in files)):
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
        dll_dir = deps / 'bin' if deps and (deps / 'bin' / 'SDL3.dll').is_file() else package / 'bin'
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


def translator_identity(layout):
    spec = importlib.util.spec_from_file_location('_dkb_build_xlate', layout.tools / 'build_xlate.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.identity()


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
    exefs_arg = options.exefs or (Path(old['sources']['exefs']) if old else None)
    romfs_arg = options.romfs or (Path(old['sources']['romfs']) if old else None)
    if not exefs_arg or not romfs_arg:
        raise InstallError('dkb.need_folders')
    for p in (exefs_arg, romfs_arg, dest, temp):
        ensure_plain(p.absolute())
    exefs, romfs = exefs_arg.resolve(), romfs_arg.resolve()
    if not exefs.is_dir() or not romfs.is_dir():
        raise InstallError('dkb.need_folders')
    game = select_target(layout, exefs)
    if old is not None and old['game'] != game:
        raise InstallError('dkb.dest_unrelated', path=str(dest))
    if dest.exists() and not dest.is_dir():
        raise InstallError('dkb.dest_unrelated', path=str(dest))
    for a, b in ((dest, temp), (dest, exefs), (dest, romfs), (temp, exefs), (temp, romfs)):
        if contains(a, b) or contains(b, a):
            raise InstallError('dkb.path_overlap', path=str(a), other=str(b))
    if dest.exists() and old is None and any(dest.iterdir()):
        try:
            ensure_plain(dest / DEST_OWNER)
            partial = json.loads((dest / DEST_OWNER).read_text(encoding='utf-8'))
            ours = partial == {'game': game, 'destination': str(dest)}
            ours &= {p.name for p in dest.iterdir()} <= {DEST_OWNER, '.dkb-setup.lock'}
        except (OSError, ValueError):
            ours = False
        if not ours:
            raise InstallError('dkb.dest_unrelated', path=str(dest))
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
    rep.progress('check', 'dkb_base')
    hashes = expected_exefs(layout, exefs, game)
    packed = (romfs / 'index.json').is_file() and (romfs / 'romfs.bin').is_file()
    if packed:
        ensure_plain(romfs / 'index.json')
        ensure_plain(romfs / 'romfs.bin')
        manifest = pack_manifest(romfs)
        files = None
        romfs_id = digest(manifest)
        romfs_size = manifest['romfs_size']
    else:
        files = inventory(romfs, rep)
        romfs_id = digest(files)
        romfs_size = sum(e['size'] + 15 for e in files)
    missing = [p for p in (layout.clang, layout.ninja, layout.recomp / 'gen.py',
                           layout.recomp / f'patches-{game}.txt', layout.tools / 'dkb_pack_romfs.py',
                           layout.tools / 'dkb_shader_census.py',
                           layout.runtime / 'nvn2_lower.cpp', layout.tools / 'data' / f'expected-{game}.json')
               if not p.is_file()]
    if layout.deps:
        missing += [p for p in (layout.deps / 'include', layout.deps / 'lib') if not p.is_dir()]
    if os.name == 'nt':
        names = {p.name for p in layout.dlls}
        if not {'SDL3.dll', 'libopus-0.dll'} <= names:
            raise InstallError('dkb.dlls_missing')
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
    return dict(old=old, game=game, sources={'exefs': str(exefs), 'romfs': str(romfs)}, hashes=hashes, files=files,
                romfs_id=romfs_id, romfs_size=romfs_size, owner=owner, translator=translator, identity=identity,
                dotnet=dotnet, packed=packed,
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
        os.replace(temporary, dst)
    except PermissionError:
        raise InstallError('compile.exe_in_use', path=str(dst)) from None


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
        proc = subprocess.Popen(list(map(str, command)), cwd=cwd or ctx.layout.root, stdin=subprocess.DEVNULL,
                                stdout=out, stderr=subprocess.STDOUT,
                                env=dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8', **(env or {})),
                                start_new_session=os.name != 'nt')
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
        if free < need or temp_free < temp_need:
            raise InstallError('dkb.space', need=round(need / (1 << 30), 1))
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
        run_tool(ctx, self.id, command)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        try:
            host = json.loads(result.stdout)
        except ValueError:
            raise InstallError('check.host_probe_failed', code=result.returncode) from None
        if not host['x86_64_v3']:
            raise InstallError('check.cpu_no_avx2')
        if not host['loader']:
            raise InstallError('check.vulkan_missing')
        if not any(tuple(d['api'][:2]) >= (1, 3) for d in host['devices']):
            raise InstallError('check.vulkan_no_device')


class Data(Step):
    id = 'dkb_data'

    def key(self, ctx):
        return digest(ctx.info['hashes'], ctx.info['romfs_id'], ctx.info['pipeline'],
                      sha256(ctx.layout.tools / 'dkb_pack_romfs.py'))

    def current(self, ctx, record):
        try:
            expected_exefs(ctx.layout, ctx.dest / 'exefs', ctx.info['game'])
            image = ctx.dest / 'romfs' / 'romfs.bin'
            return (image.stat().st_size == record['size'] and image.stat().st_mtime_ns == record['mtime_ns'] and
                    sha256(ctx.dest / 'romfs' / 'index.json') == record['index_sha256'])
        except (OSError, InstallError, KeyError):
            return False

    def verify(self, ctx, record):
        try:
            return pack_manifest(ctx.dest / 'romfs')['romfs_sha256'] == record['sha256']
        except InstallError:
            return False

    def run(self, ctx):
        stage = ctx.temp / 'data'
        owned_dir(ctx, stage)
        exefs = Path(ctx.info['sources']['exefs'])
        for name in ctx.info['hashes']:
            copy_file(exefs / name, stage / 'exefs' / name)
        expected_exefs(ctx.layout, stage / 'exefs', ctx.info['game'])
        romfs = Path(ctx.info['sources']['romfs'])
        if ctx.info['packed']:
            for name in ('romfs.bin', 'index.json'):
                copy_file(romfs / name, stage / 'romfs' / name)
        else:
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
        image = ctx.dest / 'romfs' / 'romfs.bin'
        return {'sha256': manifest['romfs_sha256'], 'size': image.stat().st_size, 'mtime_ns': image.stat().st_mtime_ns,
                'index_sha256': sha256(ctx.dest / 'romfs' / 'index.json')}


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
            env = {'PATH': str(ctx.info['dotnet'].parent) + os.pathsep + os.environ.get('PATH', ''),
                   'DOTNET_ROOT': str(ctx.info['dotnet'].parent), 'DOTNET_CLI_HOME': str(ctx.temp / 'dotnet-home'),
                   'NUGET_PACKAGES': str(ctx.temp / 'nuget'),
                   'DOTNET_PROCESSOR_COUNT': str(ctx.options.jobs),
                   # Reusable MSBuild nodes retain NuGet task DLLs after publish and block Windows cleanup.
                   # These settings affect only this build, not the owner's other .NET work.
                   'MSBUILDDISABLENODEREUSE': '1', 'DOTNET_CLI_USE_MSBUILD_SERVER': '0',
                   'UseSharedCompilation': 'false'}
            command = [ctx.layout.python, ctx.layout.tools / 'build_xlate.py', '--out', out]
            if os.name != 'nt':
                command += ['--llvm', ctx.layout.llvm]
                if ctx.layout.sysroot:
                    command += ['--sysroot', ctx.layout.sysroot]
            run_tool(ctx, self.id, command, env)
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
        return digest(ctx.keys['lift'], source_key(ctx.layout, 'runtime', 'tools/build_game.py', 'tools/romfs_index.py'),
                      str(ctx.layout.llvm), str(ctx.layout.deps),
                      sha256(ctx.layout.clang), sha256(ctx.layout.ninja),
                      tree_key(ctx.layout.deps) if ctx.layout.deps else 'system-libraries',
                      [(str(p), sha256(p)) for p in ctx.layout.dlls])

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
    translates them. The cache belongs to the player and survives later steps; the 187 MiB pack is removed again."""
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
        return {'selftest': True}


class Cleanup(Step):
    id, always = 'cleanup', True

    def run(self, ctx):
        for name in ('data', 'xlate', 'dotnet-home', 'nuget'):
            p = ctx.temp / name
            if p.exists():
                owned_dir(ctx, p)
                p.rmdir()


def all_steps():
    return [Check(), Data(), Translator(), Lift(), Compile(), Shaders(), Prepare(), Cleanup()]


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


def main(options):
    rep = Reporter()
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
        if previous and previous.get('game') in TARGETS and previous.get('destination') == str(dest):
            for name, value in previous.get('build_tools', {}).items():
                if name in ('toolchain', 'deps', 'ninja', 'dll_dir', 'dotnet') and getattr(options, name) is None and value:
                    setattr(options, name, Path(value))
        layout = tool_layout(layouts.find(options.root), options)
        raw_base = (options.temp or layout.temp or dest.parent / '.dkb-setup').absolute()
        ensure_plain(raw_base)
        base = raw_base.resolve()
        tag = hashlib.sha256(str(dest).lower().encode()).hexdigest()[:12]
        temp = base / f'dkb-{dest.name}-{tag}'
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
