"""Assemble the release folder of docs/installer.md ("Release layout") from pinned downloads.

    .venv-py314\\Scripts\\python tools\\make_release.py --out build/release/TotK-Setup [--zip build/release/TotK-Setup.zip]
    .venv-py314\\Scripts\\python tools\\make_release.py --out build/release/TotK-Setup --prove

For Donkey Kong Bananza add --game dkb (needs generated/dkb100 and build/xlate): --out defaults to build/release/DKB-Setup and a
bare --zip writes DKB-Setup-<version>-windows.zip beside it, and with it DKB-Launcher.exe and dkb-release.txt, the
manifest the launcher updates from (write_launcher_release). The folder carries every runtime source instead of
libdkbrt.a, has no hactool, and --prove does not apply. The TotK description below is otherwise the same.

Needs Python 3.14 (tarfile reads the MSYS2 .zst packages), uv and CMake on PATH (CMake: or in MSYS2 UCRT64's bin,
C:\\msys64\\ucrt64\\bin or MSYS2_UCRT64_BIN) and network access on the first run. Downloads are
cached in toolchain/release-dl/ and checked against the sha256 in PINS; a mismatch stops the build. The folder holds:

    python/    python.org embeddable 3.14.9 + the packages of tools/requirements-installer.txt + tkinter (the Tcl/Tk 9.0
               files come out of the python.org installer's own component MSI, unpacked with msiexec /a and expand.exe;
               the installer is never run)
    llvm/      llvm-mingw, x86_64 only, the files clang and lld need to compile and link dkb.exe
    runtime/   libdkbrt.a (build_game.py --make-runtime-lib with llvm/), the runtime headers, romfs.c, patches.c and
               ov_zstd.c (compiled per install), deps/ (headers and import libraries of SDL3, Opus, libvpx, Vulkan and VMA)
    bin/       dkb_xlate.exe (+ .id), hactool.exe, SDL3.dll (built here with libusb, build_sdl), libusb-1.0.dll,
               libopus-0.dll, ninja.exe
    recomp/, tools/, data/, installer/, licenses/, THIRD_PARTY_NOTICES.md, LICENSE, LICENSE-MIT, dkb.ini.example
    VERSION    the repository's VERSION plus the commit, 0.1.0+973c626; the installer's release string
    TotK-Setup.exe  installer/launcher/launcher.c built with llvm/ (-mwindows): starts python/pythonw.exe -m installer.gui
                    with its own arguments (for Bananza it is also the launcher, docs/installer.md)

--prove compiles the generated C and links build/release-check/dkb.exe with nothing but the folder's python/, llvm/,
runtime/ and bin/ (PATH holds the Windows system directory and those), then runs the four probes there.

Both need one game version extracted and lifted first (tools/extract.py, recomp/gen.py): generated/v121 or, without
it, generated/v143, and for --prove its build/extracted/<version> (they are read, nothing is written to them). The
runtime library reads only what both versions' layout.h share (MOD_COUNT, MOD_MAIN_BASE), so either serves.

The folder contains nothing derived from the game: check_content runs over it before the script finishes. Then the
folder's own python runs a check-only installer pass (no game files: it must get through the release, toolchain and host
checks and stop at the missing game files) and the window's --selftest.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(1, str(Path(__file__).resolve().parent.parent))
import check_content  # noqa: E402
import shader_list  # noqa: E402
from installer import update  # noqa: E402  the manifest format the launcher reads

ROOT = Path(__file__).resolve().parent.parent
MAIN_ROOT = (ROOT / 'build').resolve().parent   # build/, generated/, toolchain/ and refs/ are junctions in a worktree
DL = MAIN_ROOT / 'toolchain' / 'release-dl'
WORK = MAIN_ROOT / 'toolchain' / 'release-work'   # unpacked downloads

MSYS2 = 'https://repo.msys2.org/mingw/ucrt64/'
SDL_VERSION = '3.4.18'
LLVM_VERSION = '20261020'
MNG_COMMIT = 'b75be8cfa704df3701e86130c8ced268e79d6b11'   # MacNativeGames, the shader list (refs/MacNativeGames)
# name: (url, sha256)
PINS = {
    'python-embed': ('https://www.python.org/ftp/python/3.14.9/python-3.14.9-embed-amd64.zip',
                     '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2'),
    'python-installer': ('https://www.python.org/ftp/python/3.14.9/python-3.14.9-amd64.exe',
                         'a247a0b612244a803f507b39fb7121e3602e4160d59c3b99d98e735289c53394'),
    'llvm-mingw': (f'https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_VERSION}/'
                   f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64.zip',
                   'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'),
    # SDL3 is built from its source release (build_sdl): the upstream Windows DLL has no libusb (#496)
    'sdl3-src': (f'https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz',
                 '9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3'),
    'libusb': (MSYS2 + 'mingw-w64-ucrt-x86_64-libusb-1.0.30-1-any.pkg.tar.zst',
               '20106e6ff31b4581bb111b28a68b6b79a50dc3e5fc3033944e1c4b3ff8cd812e'),
    # libusb is LGPL-2.1: its MSYS2 source package goes into licenses/ beside the DLL it built
    'libusb-src': ('https://repo.msys2.org/mingw/sources/mingw-w64-libusb-1.0.30-1.src.tar.zst',
                   'e1052bbff0fe7a7ce25b847ca5f69d7ecafe1c644a3d07f971e770c3a44724f6'),
    'ninja': ('https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
              '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'),
    'opus': (MSYS2 + 'mingw-w64-ucrt-x86_64-opus-1.6.1-2-any.pkg.tar.zst',
             'bb114bcf26225f50f874497ebfd6b8224fff2a20bda3f8ee45cd07ce5f02657b'),
    'libvpx': (MSYS2 + 'mingw-w64-ucrt-x86_64-libvpx-1.17.0-1-any.pkg.tar.zst',
               '8ff6919dbb88ea26ce28d22fd81d6f0d6e200cf56a4dd81ffc2d33124fe98024'),
    'libvpx-src': ('https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz',   # LICENSE and PATENTS only
                   '1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a'),
    'vulkan-headers': (MSYS2 + 'mingw-w64-ucrt-x86_64-vulkan-headers-1~1.4.363.0-1-any.pkg.tar.zst',
                       'be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a'),
    'vulkan-loader': (MSYS2 + 'mingw-w64-ucrt-x86_64-vulkan-loader-1~1.4.363.0-1-any.pkg.tar.zst',   # import library only
                      '065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe'),
    'vma': (MSYS2 + 'mingw-w64-ucrt-x86_64-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst',
            '44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781'),
    'zstd': (MSYS2 + 'mingw-w64-ucrt-x86_64-zstd-1.5.7-2-any.pkg.tar.zst',   # static libzstd.a (runtime/resolution.c)
             'dbdb8427280046a2b41697780aa4c52983b708082b0da4755951dc3bea96ca89'),
}
# The component MSI inside the python.org installer that carries Tcl/Tk and tkinter (Burn names its payloads a0..a27).
TCLTK_PAYLOAD = ('a12', 'ac87609db3f715404dd41a989c2f1706ea1512f3d86217f938d11c63af8eb505')
MSYS2_UCRT64_BIN = Path(os.environ.get('MSYS2_UCRT64_BIN', r'C:\msys64\ucrt64\bin'))   # CMake when it is not on PATH

# llvm-mingw: what clang and lld need to compile and link dkb.exe for x86_64-w64-mingw32 (dynamic LLVM, libc++, compiler-rt).
LLVM_BIN = {'clang.exe', 'clang++.exe', 'clang-23.exe', 'ld.lld.exe', 'llvm-ar.exe', 'llvm-ranlib.exe', 'llvm-nm.exe',
            'llvm-objdump.exe', 'llvm-objcopy.exe', 'llvm-strip.exe', 'llvm-readobj.exe', 'llvm-dlltool.exe',
            'llvm-rc.exe', 'llvm-cvtres.exe', 'llvm-windres.exe', 'llvm-wrapper.exe', 'mingw32-common.cfg',
            'x86_64-w64-windows-gnu.cfg', 'x86_64-pc-windows-gnu.cfg', 'x86_64-w64-mingw32-clang.exe',
            'x86_64-w64-mingw32-clang++.exe', 'libLLVM-23.dll', 'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll',
            'libwinpthread-1.dll', 'libffi-8.dll'}
LLVM_SKIP_DIRS = {'aarch64-w64-mingw32', 'armv7-w64-mingw32', 'i686-w64-mingw32', 'busybox', 'python', 'share'}
LLVM_SKIP_SUFFIXES = {'.idl', '.tlb'}

# tools/ files the installer steps run (and what they import), copied from the repo.
TOOLS_FILES = ['build_game.py', 'build_shaders.py', 'check_content.py', 'extract.py', 'hostmem.py', 'make_romfs_patches.py',
               'pack_shader_cache.py', 'prepare_shaders.py', 'romfs_index.py', 'shader_census.py', 'shader_list.py',
               'ultrawide_layout.py']
TOOLS_DIRS = ['vendor/macnativegames', 'data']   # data/: expected hashes
RUNTIME_SUFFIXES = {'.h', '.inc', '.ico'}   # .ico: the installer window icon

# --game dkb: Bananza builds the whole runtime from source at install time (installer/bananza.py, build_game.py
# --full-build), so the folder carries every runtime source and the translator's inputs instead of libdkbrt.a.
GAME = 'totk'
GAMES = {'totk': dict(lifted=('v121', 'v143'), name='TotK-Setup'), 'dkb': dict(lifted=('dkb100',), name='DKB-Setup')}
DKB_TOOLS_FILES = ['build_xlate.py', 'dkb_pack_romfs.py', 'dkb_shader_census.py', 'dkb_wide_layout.py',
                   'extract_nup.py', 'extract_nx.py', 'dkb_ps_prompts.py']   # extract_nup, extract_nx: .nup and .nx input
DKB_TOOLS_DIRS = ['vendor/ryujinx', 'xlate']   # translator identity hashes these

PROBES = ['--probe-kernel', '--probe-vkexec', '--probe-nvn-tex', '--probe-shader']


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(name):
    """The pinned download, from the cache or the network, checked against its sha256."""
    url, want = PINS[name]
    DL.mkdir(parents=True, exist_ok=True)
    path = DL / Path(url.split('?')[0]).name.replace('%7E', '~')
    if name in ('tcl-linux-src', 'tk-linux-src'):
        path = DL / f'{name}-9.0.4.tar.gz'   # upstream archives have identical basenames
    if name == 'libvpx-src':
        path = DL / 'libvpx-1.17.0.tar.gz'
    if path.is_file():
        if sha256_file(path) == want:
            return path
        print(f'{path}: not the pinned file (a cut download or an older pin); downloading it again')
        path.unlink()
    # into <name>.part, renamed once complete: a cut connection leaves a .part that the next run resumes, never a
    # short file under the final name
    part = path.with_name(path.name + '.part')
    have = part.stat().st_size if part.is_file() else 0
    print(f'download {url}' + (f' (resuming at {have} bytes)' if have else ''))
    req = urllib.request.Request(url, headers={'Range': f'bytes={have}-'} if have else {})
    try:
        with urllib.request.urlopen(req) as r:
            if have and r.status != 206:   # the server ignored the range: start over
                have = 0
            with open(part, 'ab' if have else 'wb') as f:
                shutil.copyfileobj(r, f)
    except urllib.error.HTTPError as e:
        if e.code != 416:   # 416: the .part already holds the whole file
            raise
    part.replace(path)
    got = sha256_file(path)
    if got != want:
        path.unlink()   # a .part resumed from another upload, or the URL now serves another file: start clean next run
        sys.exit(f'{path}: sha256 {got}, pinned {want}; deleted, run again to download it anew')
    return path


def unpack(name, member_filter=None):
    """Extract a pinned archive into toolchain/release-work/<name> (once) and return that directory."""
    dest = WORK / name
    stamp = dest / '.unpacked'
    if stamp.is_file():
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    path = fetch(name)
    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as z:
            z.extractall(dest)
    else:
        with tarfile.open(path) as t:
            t.extractall(dest, filter='data')
    stamp.write_text('')
    return dest


def copy_tree(src, dst, skip=lambda rel: False):
    """Copy src into dst file by file (sorted), leaving out what skip(relative posix path) names."""
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if p.is_dir() or skip(rel.as_posix()):
            continue
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst / rel)


def clean_env(*path_dirs):
    """A process environment without MSYS2 and the user's tools: the Windows system directory and path_dirs only."""
    system = os.environ.get('SystemRoot', r'C:\Windows')
    keep = ('SYSTEMROOT', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA', 'APPDATA', 'COMSPEC', 'PATHEXT',
            'NUMBER_OF_PROCESSORS')   # os.environ keys are upper case on Windows
    env = {k: v for k, v in os.environ.items() if k.upper() in keep}
    env['PATH'] = os.pathsep.join([*(str(Path(d)) for d in path_dirs), rf'{system}\System32', system])
    return env


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def mib(n):
    return f'{n / (1 << 20):.1f} MiB'


def dir_size(p):
    return sum(f.stat().st_size for f in Path(p).rglob('*') if f.is_file())


# ---- python/ -------------------------------------------------------------------------------------------------------

def tcltk_files():
    """Directory holding the python.org installer's tcltk payload, unpacked (not installed) into toolchain/release-work."""
    dest = WORK / 'tcltk'
    if (dest / 'DLLs' / '_tkinter.pyd').is_file():
        return dest
    exe = fetch('python-installer')
    data = exe.read_bytes()
    cabs = []
    pos = data.find(b'MSCF\0\0\0\0')
    while pos >= 0:
        cabs.append((struct.unpack_from('<I', data, pos + 8)[0], pos))
        pos = data.find(b'MSCF\0\0\0\0', pos + 1)
    size, pos = max(cabs)   # the attached container holds all component MSIs
    shutil.rmtree(WORK / 'tcltk-src', ignore_errors=True)
    src = WORK / 'tcltk-src'
    src.mkdir(parents=True)
    (src / 'container.cab').write_bytes(data[pos:pos + size])
    name, want = TCLTK_PAYLOAD
    expand = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'expand.exe'
    run([expand, '-F:' + name, src / 'container.cab', src], stdout=subprocess.DEVNULL)
    got = sha256_file(src / name)
    if got != want:
        sys.exit(f'{src / name}: sha256 {got}, pinned {want} (not the tcltk component of python 3.14.9)')
    msi = src / (name + '.msi')
    (src / name).rename(msi)
    shutil.rmtree(dest, ignore_errors=True)
    # Administrative image: copies the MSI's files to TARGETDIR, installs nothing and shows no window.
    run(['msiexec', '/a', msi, '/qn', f'TARGETDIR={dest}'])
    if not (dest / 'DLLs' / '_tkinter.pyd').is_file():
        sys.exit(f'{dest}: no _tkinter.pyd after msiexec /a')
    return dest


def build_python(out):
    py = out / 'python'
    with zipfile.ZipFile(fetch('python-embed')) as z:
        z.extractall(py)
    (py / 'python314._pth').write_text('python314.zip\n.\nLib/site-packages\n..\n../tools\n../recomp\n', encoding='utf-8')
    # The embeddable Python ignores the script directory and PYTHONPATH once a ._pth file exists, so the folder's own code
    # (installer/, and tools/*.py and recomp/*.py importing their siblings) has to be on the list.
    env = dict(os.environ, UV_CACHE_DIR=str(MAIN_ROOT / 'toolchain' / 'uv-cache'))
    run(['uv', 'pip', 'install', '--target', py / 'Lib' / 'site-packages', '--python', sys.executable,
         '--python-platform', 'windows', '--python-version', '3.14', '--only-binary', ':all:',
         '-r', ROOT / 'tools' / 'requirements-installer.txt'], env=env)
    shutil.rmtree(py / 'Lib' / 'site-packages' / 'bin', ignore_errors=True)   # numpy/cffi console scripts
    tk = tcltk_files()
    for dll in ('_tkinter.pyd', 'tcl90.dll', 'tcl9tk90.dll', 'zlib1.dll'):
        shutil.copy2(tk / 'DLLs' / dll, py / dll)
    copy_tree(tk / 'Lib' / 'tkinter', py / 'tkinter', lambda r: r.startswith('__pycache__'))
    copy_tree(tk / 'tcl', py / 'tcl', lambda r: r.endswith(('.lib', '.sh')) or r.startswith('nmake'))
    check = subprocess.run([str(py / 'python.exe'), '-I', '-c',
                            'import tkinter, numpy, lz4.block, cryptography, cffi, pycparser; '
                            'print("python ok: Tcl", tkinter.Tcl().eval("info patchlevel"), "numpy", numpy.__version__)'],
                           env=clean_env(py), capture_output=True, text=True)
    if check.returncode:
        sys.exit('bundled python check failed:\n' + check.stderr)
    print(check.stdout.strip())


# ---- llvm/ ---------------------------------------------------------------------------------------------------------

def build_llvm(out):
    src = unpack('llvm-mingw') / f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64'
    dst = out / 'llvm'

    def skip(rel):
        top = rel.split('/')[0]
        if top in LLVM_SKIP_DIRS:
            return True
        if top == 'bin':
            return rel not in {f'bin/{n}' for n in LLVM_BIN}
        if top == 'lib':   # lib/clang/<major>/include and the x86_64 Windows builtins; no sanitizers, no other targets
            if rel.startswith('lib/clang/') and '/include/' in rel:
                return False
            return not rel.endswith('/lib/windows/libclang_rt.builtins-x86_64.a')
        return Path(rel).suffix.lower() in LLVM_SKIP_SUFFIXES or rel.startswith('include/ddk/')

    copy_tree(src, dst, skip)
    return dst


# ---- bin/ and deps ---------------------------------------------------------------------------------------------------

def cmake_program():
    """CMake from PATH, else from MSYS2 UCRT64 (mingw-w64-ucrt-x86_64-cmake); the GitHub Windows runner has it on PATH."""
    found = shutil.which('cmake') or shutil.which('cmake', path=str(MSYS2_UCRT64_BIN))
    if not found:
        sys.exit(f'cmake not found on PATH or in {MSYS2_UCRT64_BIN} (MSYS2_UCRT64_BIN): SDL3 is built with it (build_sdl)')
    return Path(found)


def pe_imports(objdump, dll):
    """The DLL names a PE image imports, as llvm-objdump -p lists them."""
    r = subprocess.run([str(objdump), '-p', str(dll)], check=True, capture_output=True, text=True)
    return [line.split(':', 1)[1].strip() for line in r.stdout.splitlines() if line.strip().startswith('DLL Name:')]


def build_sdl(llvm_bin, ninja, exe='.exe'):
    """SDL3 built from its pinned source release with llvm-mingw (llvm_bin: its bin/) and libusb, installed into
    toolchain/release-work/sdl3-libusb/install (bin/SDL3.dll, lib/libSDL3.dll.a, include/SDL3) and returned; kept
    there while the source, libusb and llvm-mingw pins stay the same.

    Why: SDL starts the Switch 2 controllers over USB (SDL_hidapi_switch2.c) only through libusb: it sends their
    start-up commands on the controller's second, non-HID interface. The upstream Windows SDL3.dll and MSYS2's are built
    without libusb, so a Switch 2 GameCube or Pro Controller on USB showed up as a gamepad that never sent input unless
    Steam Input woke it (#496). With SDL_HIDAPI_LIBUSB_SHARED, SDL3.dll loads libusb-1.0.dll (MSYS2, LGPL-2.1, shipped
    unmodified beside it, build_bin) when it is present and does without it otherwise; libusb is not linked in.
    The configuration follows mkw-recomp's tools/build/windeps.py build_sdl (MIT)."""
    dest = WORK / 'sdl3-libusb'
    key = f'{PINS["sdl3-src"][1]} {PINS["libusb"][1]} {LLVM_VERSION}\n'
    stamp, prefix = dest / '.built', dest / 'install'
    if stamp.is_file() and stamp.read_text() == key:
        return prefix
    shutil.rmtree(dest, ignore_errors=True)
    src = unpack('sdl3-src') / f'SDL3-{SDL_VERSION}'
    usb = unpack('libusb') / 'ucrt64'
    cc = (Path(llvm_bin) / 'x86_64-w64-mingw32-').as_posix()   # forward slashes: CMake reads backslashes as escapes
    build = dest / 'build'
    cmake = cmake_program()
    run([cmake, '-S', src, '-B', build, '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=x86_64',
         f'-DCMAKE_C_COMPILER={cc}clang{exe}', f'-DCMAKE_CXX_COMPILER={cc}clang++{exe}',
         f'-DCMAKE_RC_COMPILER={cc}windres{exe}', f'-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}',
         '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF',
         '-DSDL_HIDAPI_LIBUSB=ON', '-DSDL_HIDAPI_LIBUSB_SHARED=ON',
         f'-DLibUSB_LIBRARY={(usb / "lib" / "libusb-1.0.dll.a").as_posix()}',
         f'-DLibUSB_INCLUDE_PATH={(usb / "include" / "libusb-1.0").as_posix()}'], stdout=subprocess.DEVNULL)
    run([cmake, '--build', build, '--target', 'install'], stdout=subprocess.DEVNULL)
    config = (build / 'include-config-release' / 'build_config' / 'SDL_build_config.h').read_text(encoding='utf-8')
    if '#define SDL_LIBUSB_DYNAMIC "libusb-1.0.dll"' not in config:
        sys.exit('SDL3 was built without libusb: the Switch 2 controllers would not start over USB (#496)')
    # Only Windows' own DLLs: a libc++ or libunwind import from llvm-mingw would fail to load on the player's PC
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    for dll in (prefix / 'bin' / 'SDL3.dll', usb / 'bin' / 'libusb-1.0.dll'):
        names = pe_imports(Path(llvm_bin) / f'llvm-objdump{exe}', dll)
        foreign = [n for n in names if not n.lower().startswith('api-ms-win-') and
                   (os.name != 'nt' or not (system / n).is_file())]
        if os.name != 'nt':
            print(f'{dll.name} imports {", ".join(names)}')   # a cross test build: no System32 to look in
        elif foreign:
            sys.exit(f'{dll}: imports {", ".join(foreign)}, which Windows does not carry and the release does not ship')
    stamp.write_text(key)
    return prefix


def build_bin(out, sdl):
    b = out / 'bin'
    b.mkdir(parents=True, exist_ok=True)
    for f in ('dkb_xlate.exe', 'dkb_xlate.id'):
        src = MAIN_ROOT / 'build' / 'xlate' / f
        if not src.is_file():
            sys.exit(f'{src}: missing; build the shader translator with: .venv\\Scripts\\python tools\\build_xlate.py')
        shutil.copy2(src, b / f)
    if GAME == 'totk':   # Bananza's installer extracts with its own tools, not hactool
        shutil.copy2(MAIN_ROOT / 'toolchain' / 'hactool' / 'hactool.exe', b / 'hactool.exe')
    shutil.copy2(sdl / 'bin' / 'SDL3.dll', b / 'SDL3.dll')
    shutil.copy2(unpack('libusb') / 'ucrt64' / 'bin' / 'libusb-1.0.dll', b / 'libusb-1.0.dll')   # SDL3.dll loads it
    shutil.copy2(unpack('opus') / 'ucrt64' / 'bin' / 'libopus-0.dll', b / 'libopus-0.dll')
    shutil.copy2(unpack('ninja') / 'ninja.exe', b / 'ninja.exe')


def build_deps(out, sdl):
    """runtime/deps: the headers and import libraries build_game.py --deps expects (include/, lib/)."""
    d = out / 'runtime' / 'deps'
    (d / 'include').mkdir(parents=True, exist_ok=True)
    (d / 'lib').mkdir(parents=True, exist_ok=True)
    copy_tree(sdl / 'include' / 'SDL3', d / 'include' / 'SDL3')
    shutil.copy2(sdl / 'lib' / 'libSDL3.dll.a', d / 'lib')
    for pkg, dirs, libs in (('opus', ['opus'], ['libopus.dll.a']), ('libvpx', ['vpx'], ['libvpx.a']),
                            ('vulkan-headers', ['vulkan', 'vk_video'], []), ('vulkan-loader', [], ['libvulkan-1.dll.a']),
                            ('vma', ['vma'], [])):
        root = unpack(pkg) / 'ucrt64'
        for inc in dirs:
            copy_tree(root / 'include' / inc, d / 'include' / inc)
        for lib in libs:
            shutil.copy2(root / 'lib' / lib, d / 'lib')
    zstd = unpack('zstd') / 'ucrt64'
    for name in ('zstd.h', 'zstd_errors.h'):
        shutil.copy2(zstd / 'include' / name, d / 'include')
    shutil.copy2(zstd / 'lib' / 'libzstd.a', d / 'lib')


def collect_licenses(out):
    lic = out / 'licenses'
    lic.mkdir(parents=True, exist_ok=True)
    pairs = [
        (unpack('opus') / 'ucrt64' / 'share' / 'licenses' / 'opus' / 'COPYING', 'opus-COPYING.txt'),
        (unpack('vulkan-headers') / 'ucrt64' / 'share' / 'licenses' / 'vulkan-headers' / 'LICENSE', 'vulkan-headers-LICENSE.txt'),
        (unpack('vulkan-loader') / 'ucrt64' / 'share' / 'licenses' / 'vulkan-loader' / 'LICENSE', 'vulkan-loader-LICENSE.txt'),
        (unpack('vma') / 'ucrt64' / 'share' / 'licenses' / 'VulkanMemoryAllocator' / 'LICENSE', 'vulkan-memory-allocator-LICENSE.txt'),
        (unpack('sdl3-src') / f'SDL3-{SDL_VERSION}' / 'LICENSE.txt', 'SDL3-LICENSE.txt'),
        (unpack('libusb') / 'ucrt64' / 'share' / 'licenses' / 'libusb' / 'COPYING', 'libusb-COPYING.txt'),
        (fetch('libusb-src'), 'mingw-w64-libusb-1.0.30-1.src.tar.zst'),
        (unpack('zstd') / 'ucrt64' / 'share' / 'licenses' / 'zstd' / 'LICENSE', 'zstd-LICENSE.txt'),
        (ROOT / 'runtime' / 'shaders' / 'ffx' / 'LICENSE.txt', 'FidelityFX-FSR-LICENSE.txt'),
    ]
    if GAME == 'totk':
        pairs.append((MAIN_ROOT / 'refs' / 'MacNativeGames' / 'LICENSE', 'MacNativeGames-LICENSE.txt'))
        pairs.append((MAIN_ROOT / 'toolchain' / 'hactool' / 'LICENSE', 'hactool-LICENSE.txt'))
    for src, name in pairs:
        shutil.copy2(src, lic / name)
    with tarfile.open(fetch('libvpx-src')) as t:   # the MSYS2 package carries no licence text; the notice needs both
        for member, name in (('libvpx-1.17.0/LICENSE', 'libvpx-LICENSE.txt'), ('libvpx-1.17.0/PATENTS', 'libvpx-PATENTS.txt')):
            (lic / name).write_bytes(t.extractfile(member).read())
    for name in ('LICENSE.TXT',):
        shutil.copy2(out / 'llvm' / name, lic / 'llvm-mingw-LICENSE.txt')


# ---- runtime/, recomp/, tools/, data/ ---------------------------------------------------------------------------------

def lifted_version():
    """The game version whose generated C the runtime library and --prove build against: v121, else v143 (dkb100)."""
    for version in GAMES[GAME]['lifted']:
        if (MAIN_ROOT / 'generated' / version / 'layout.json').is_file():
            return version
    sys.exit(f'none of generated/{", generated/".join(GAMES[GAME]["lifted"])} exists: extract a game version and lift it '
             'first (tools/extract.py, recomp/gen.py; docs/building.md)')


def runtime_lib(out, llvm):
    """Build libdkbrt.a with the folder's llvm/ and copy it, the headers and the install-side sources into runtime/."""
    if GAME == 'dkb':   # no prebuilt library: the installer compiles the full runtime sources
        copy_tree(ROOT / 'runtime', out / 'runtime', lambda r: '__pycache__' in r)
        # build_game.runtime_sources() keeps only these files, so a release unpacked over an older one cannot
        # compile the sources the older one shipped and this one dropped.
        src = ROOT / 'runtime'
        names = sorted(p.relative_to(src).as_posix() for p in src.rglob('*')
                       if p.is_file() and '__pycache__' not in p.parts)
        (out / 'runtime' / 'sources.txt').write_text(''.join(f'{n}\n' for n in names), encoding='utf-8',
                                                      newline='\n')
        return
    build = MAIN_ROOT / 'build' / 'release-check' / 'rtlib'
    version = lifted_version()
    gen = MAIN_ROOT / 'generated' / version
    run([sys.executable, ROOT / 'tools' / 'build_game.py', '--make-runtime-lib', '--toolchain', llvm, '--version', version,
         '--deps', out / 'runtime' / 'deps', '--ninja', out / 'bin' / 'ninja.exe', '--gen', gen, '--out', build],
        env=clean_env(llvm / 'bin', out / 'bin'))
    rt = out / 'runtime'
    shutil.copy2(build / 'libdkbrt.a', rt / 'libdkbrt.a')
    for p in sorted((ROOT / 'runtime').rglob('*')):
        if p.suffix in RUNTIME_SUFFIXES and p.is_file():
            (rt / p.relative_to(ROOT / 'runtime')).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, rt / p.relative_to(ROOT / 'runtime'))
    for name in ('romfs.c', 'patches.c', 'ov_zstd.c'):   # build_game.py INSTALL_SIDE
        shutil.copy2(ROOT / 'runtime' / name, rt / name)


def build_sources(out, *, setup_program=None):
    copy_tree(ROOT / 'recomp', out / 'recomp', lambda r: not r.endswith(('.py', '.txt')) or '__pycache__' in r)
    for f in TOOLS_FILES:
        (out / 'tools').mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'tools' / f, out / 'tools' / f)
    for f in (DKB_TOOLS_FILES if GAME == 'dkb' else []):
        shutil.copy2(ROOT / 'tools' / f, out / 'tools' / f)
    for d in TOOLS_DIRS + (DKB_TOOLS_DIRS if GAME == 'dkb' else []):
        # The translator identity (build_xlate.py identity) hashes every file under vendor/ryujinx, README.md included.
        copy_tree(ROOT / 'tools' / d, out / 'tools' / d,
                  lambda r, d=d: '__pycache__' in r or (r.endswith('.md') and d != 'vendor/ryujinx'))
    (out / 'data').mkdir(exist_ok=True)
    if GAME == 'totk':   # the TotK installer reads the shared shader list; the Bananza one does not
        text = shader_list.history(MAIN_ROOT / 'refs' / 'MacNativeGames', MNG_COMMIT)
        (out / 'data' / 'shader_list.tsv').write_text(text, encoding='utf-8', newline='\n')
    copy_tree(ROOT / 'installer', out / 'installer', lambda r: '__pycache__' in r or r.startswith('launcher/'))
    shutil.copy2(ROOT / 'dkb.ini.example', out / 'dkb.ini.example')
    if GAME == 'dkb':
        profile = json.loads((ROOT / 'tools' / 'release' / 'recomplauncher.json').read_text(encoding='utf-8'))
        if setup_program:   # The bundled installers can both prepare and play. Source packages use play.sh.
            profile['prepare']['program'] = [setup_program]
            profile['start'] = {'program': [setup_program], 'args': ['--play', '{install}/game']}
            if setup_program.endswith('.exe'):   # the game the build leaves, as prepare.done names it
                profile['prepare']['done'] = ['{install}/game/dkb.exe']
        (out / 'recomplauncher.json').write_text(json.dumps(profile, indent=2) + '\n', encoding='utf-8')
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', out / 'THIRD_PARTY_NOTICES.md')
    for lic in sorted(ROOT.glob('LICENSE*')):   # GPL-3.0-or-later, and MIT for the files from TotK Recomp
        shutil.copy2(lic, out / lic.name)
    (out / 'VERSION').write_text(version() + '\n', encoding='utf-8')


def version():
    """The port version from VERSION with the commit as build metadata: 0.1.0+973c626. The commit keeps two releases of
    one version apart, so the installer reruns the steps an earlier release made."""
    r = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        sys.exit('no git version: not a git checkout')
    return (ROOT / 'VERSION').read_text(encoding='utf-8').strip() + '+' + r.stdout.strip()


# ---- TotK-Setup.exe --------------------------------------------------------------------------------------------------

def c_wide(text):
    """A C wide string literal of text: ASCII as is, everything else as a universal character name."""
    bs = chr(92)
    out = []
    for ch in text:
        if ch in (bs, '"'):
            out.append(bs + ch)
        elif ch == '\n':   # C allows no universal character name for a basic character such as \u000a
            out.append(bs + 'n')
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        elif ord(ch) < 0x10000:
            out.append(f'{bs}u{ord(ch):04x}')
        else:
            sys.exit(f'launcher text: {ch!r} is outside the BMP')
    return 'L"' + ''.join(out) + '"'


LAUNCHER_TEXT = ('title', 'python_missing', 'confirm', 'checking', 'downloading', 'unpacking', 'no_release', 'network',
                 'checksum', 'unpack_failed', 'write_failed', 'start_failed')
LAUNCHER = 'DKB-Launcher.exe'   # the standalone download: the same file as the folder's DKB-Setup.exe
DEPENDENT_LOAD_FLAG = '0x800'   # LOAD_LIBRARY_SEARCH_SYSTEM32 for the launcher's static imports (build_launcher)


def launcher_header(path, game=None):
    """launcher_text.h: the launcher's messages in English, Russian and Chinese, taken from the installer's catalog
    (a dkb. id replaces the plain one for Bananza), and LAUNCHER_REPO, the repository whose releases the launcher
    installs from (empty for TotK: no launcher releases, the exe is only the setup)."""
    game = game or GAME
    lines = ['/* generated by tools/make_release.py from installer/text/{en,ru,zh}.json */']
    for lang in ('en', 'ru', 'zh'):
        texts = json.loads((ROOT / 'installer' / 'text' / f'{lang}.json').read_text(encoding='utf-8'))
        for key in LAUNCHER_TEXT:
            text = texts.get('dkb.launcher.' + key) if game == 'dkb' else None
            lines.append(f'#define LAUNCHER_{key.upper()}_{lang.upper()} {c_wide(text or texts["launcher." + key])}')
    lines.append(f'#define LAUNCHER_REPO {c_wide(update.REPO if game == "dkb" else "")}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='ascii')


def build_launcher(out, llvm):
    work = MAIN_ROOT / 'build' / 'release-check' / 'launcher'
    launcher_header(work / 'launcher_text.h')
    env = clean_env(llvm / 'bin')
    res = work / 'launcher_rc.o'
    run([llvm / 'bin' / 'llvm-windres.exe', '-I', ROOT / 'runtime', '-O', 'coff',
         ROOT / 'installer' / 'launcher' / 'launcher.rc', '-o', res], env=env)
    # winhttp.dll and bcrypt.dll are not KnownDLLs, so without the flag Windows looks for them first in the exe's own
    # folder: a DLL of that name left in Downloads beside DKB-Launcher.exe would run inside it. 0x800 is
    # LOAD_LIBRARY_SEARCH_SYSTEM32, which the loader then uses for every static import of the exe.
    exe = out / (GAMES[GAME]['name'] + '.exe')
    run([llvm / 'bin' / 'clang.exe', '-O2', '-mwindows', '-municode', '-Wall', '-Wl,-s',
         f'-Wl,--dependent-load-flag={DEPENDENT_LOAD_FLAG}', '-I', work, *launcher_sources(GAME), res,
         '-o', exe], env=env)
    check_launcher(exe)


def check_launcher(exe):
    """Stop the build when the linker did not write the flag into the exe (a linker that ignores the option)."""
    flags = dependent_load_flags(exe)
    if flags != int(DEPENDENT_LOAD_FLAG, 16):
        sys.exit(f'{exe}: DependentLoadFlags {flags:#x}, not {DEPENDENT_LOAD_FLAG}')
    print(f'{exe.name}: DependentLoadFlags {flags:#x}')


def dependent_load_flags(path):
    """DependentLoadFlags of a PE32+ image: the field of its load configuration directory (IMAGE_LOAD_CONFIG_DIRECTORY64
    at offset 0x4e) the loader uses as the search flags of every static import; 0 when the image has none."""
    data = Path(path).read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', data, pe + 24)[0] != 0x20b:
        raise ValueError(f'{path}: not a PE32+ image')
    sections, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from('<I', data, opt + 108)[0] <= 10:   # NumberOfRvaAndSizes: no load configuration entry
        return 0
    rva = struct.unpack_from('<I', data, opt + 112 + 10 * 8)[0]   # data directory 10, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG
    if not rva:
        return 0
    for i in range(sections):
        virtual_size, virtual, raw_size, raw = struct.unpack_from('<IIII', data, opt + optional_size + 40 * i + 8)
        if virtual <= rva < virtual + max(virtual_size, raw_size):
            at = raw + rva - virtual
            return struct.unpack_from('<H', data, at + 0x4e)[0] if struct.unpack_from('<I', data, at)[0] >= 0x50 else 0
    raise ValueError(f'{path}: the load configuration is in no section')


def launcher_sources(game):
    """The launcher's sources and libraries. TotK's is only the setup: launcher.c, MIT, nothing else. Bananza's is also
    the launcher that installs a release, which adds manifest.c (MIT) and fetch.c, adapted from Longshot and Project
    ReLink under the GPL-3.0-or-later (THIRD_PARTY_NOTICES.md), with the network and hash libraries."""
    folder = ROOT / 'installer' / 'launcher'
    if game != 'dkb':
        return [folder / 'launcher.c']
    return ['-DDKB_LAUNCHER', folder / 'launcher.c', folder / 'manifest.c', folder / 'fetch.c',
            '-lwinhttp', '-lbcrypt', '-lcomctl32']


def write_launcher_release(out, zip_path):
    """Beside the zip: DKB-Launcher.exe (the folder's DKB-Setup.exe) and dkb-release.txt, the manifest the launcher
    reads (installer/update.py parse_manifest): the version, its tag and the size and SHA-256 of both files. The
    workflow publishes the three with the tag's release."""
    launcher = zip_path.parent / LAUNCHER
    shutil.copy2(out / (GAMES[GAME]['name'] + '.exe'), launcher)
    semver = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    manifest = zip_path.parent / update.MANIFEST
    manifest.write_text(update.write_manifest(version(), 'v' + semver, zip_path, out.name, launcher), encoding='ascii',
                        newline='\n')
    print(f'{launcher}: {mib(launcher.stat().st_size)}\n{manifest}:\n{manifest.read_text(encoding="ascii")}')


# ---- checks ----------------------------------------------------------------------------------------------------------

def check_folder(out):
    """check_content over every file of the release folder; a hit stops the build."""
    bad = []
    for p in sorted(out.rglob('*')):
        if p.is_file():
            bad += [f'{p.relative_to(out)}: {problem}' for problem in check_content.problems_for(p, allow_large=True)]
    if bad:
        sys.exit('game-derived content in the release folder:\n' + '\n'.join(bad))
    print(f'check_content: {sum(1 for p in out.rglob("*") if p.is_file())} files, no hit')


def check_bundle(out):
    """The folder's own python runs the installer without game files and the window's selftest."""
    chk = MAIN_ROOT / 'build' / 'release-check' / 'bundle'
    shutil.rmtree(chk, ignore_errors=True)
    env = clean_env(out / 'python')
    py = out / 'python' / 'python.exe'
    if GAME == 'dkb':   # the no-input CLI pass is the TotK path; Bananza gets only the window selftest
        selftest(out, py, env)
        return
    r = subprocess.run([py, '-m', 'installer', '--lang', 'en-US', '--dest', chk / 'install', '--temp', chk / 'temp'],
                       cwd=out, env=env, capture_output=True, text=True, timeout=600)
    events = [json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
    errors = [e['id'] for e in events if e['type'] == 'error']
    started = any(e['type'] == 'start' and e.get('release') == (out / 'VERSION').read_text().strip() for e in events)
    steps = [e['phase'] for e in events if e['type'] == 'progress']
    if errors != ['check.inputs_needed'] or not started or 'inputs' not in steps:
        sys.exit(f'bundle check: exit {r.returncode}, errors {errors}, release read {started}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    print(f'bundle check: the installer ran the host check from the folder and stopped at the game files (exit {r.returncode})')
    selftest(out, py, env)


def selftest(out, py, env):
    r = subprocess.run([py, '-m', 'installer.gui', '--selftest'], cwd=out, env=env, capture_output=True, text=True, timeout=300)
    if r.returncode:
        sys.exit(f'bundle check: window selftest exit {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    print('bundle check: the window built every screen in both languages and closed')


def sizes(out):
    rows = [(p.name if p.is_dir() else p.name, dir_size(p) if p.is_dir() else p.stat().st_size) for p in sorted(out.iterdir())]
    for name, n in rows:
        print(f'  {name:26} {mib(n):>12}')
    print(f'  {"total":26} {mib(sum(n for _, n in rows)):>12}')


def commit_time():
    """The commit's time (Unix seconds): every file of a release folder is stamped with it. A fixed date for all releases
    let a folder unpacked over an older one keep the older Python byte code: the interpreter takes a __pycache__ entry
    whose recorded source time and size match, and a source edited without changing its size (build_xlate.py's patch
    count, 23 to 24) matched. The installer then ran the old code and stopped at its first step with exit code 1."""
    r = subprocess.run(['git', '-C', ROOT, 'log', '-1', '--format=%ct', 'HEAD'], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip().isdigit():
        sys.exit('no commit time: not a git checkout')
    return int(r.stdout.strip())


def zip_date():
    """commit_time() as a zip entry's date (UTC; zip stores no zone, and two seconds is its resolution)."""
    return time.gmtime(commit_time())[:6]


def write_zip(out, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    date = zip_date()
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file():
                info = zipfile.ZipInfo((Path(out.name) / p.relative_to(out)).as_posix(), date)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                z.writestr(info, p.read_bytes(), compresslevel=6)
    print(f'{path}: {mib(path.stat().st_size)}')


def prove(out):
    """Compile the generated C and link dkb.exe with the folder's tools only, then run the probes next to it."""
    out = out.resolve()
    version = lifted_version()
    gen = MAIN_ROOT / 'generated' / version
    ext = MAIN_ROOT / 'build' / 'extracted' / version
    chk = MAIN_ROOT / 'build' / 'release-check'
    env = clean_env(out / 'python', out / 'llvm' / 'bin', out / 'bin')
    t0 = time.time()
    run([out / 'python' / 'python.exe', out / 'tools' / 'build_game.py', '--toolchain', out / 'llvm',
         '--deps', out / 'runtime' / 'deps', '--runtime', out / 'runtime', '--runtime-lib', out / 'runtime' / 'libdkbrt.a',
         '--ninja', out / 'bin' / 'ninja.exe', '--version', version, '--gen', gen, '--out', chk, '--exefs', ext / 'exefs',
         '--romfs', ext / 'romfs', '--profile', chk / 'profile'], env=env)
    print(f'prove: build {time.time() - t0:.0f} s')
    for dll in ('SDL3.dll', 'libusb-1.0.dll', 'libopus-0.dll'):
        shutil.copy2(out / 'bin' / dll, chk / dll)
    failed = []
    for probe in PROBES:
        r = subprocess.run([str(chk / 'dkb.exe'), probe], cwd=chk, env=clean_env(chk), capture_output=True, text=True,
                           timeout=900)
        tail = (r.stdout.strip().splitlines() or [''])[-1]
        print(f'{probe}: exit {r.returncode}: {tail}')
        if r.returncode:
            failed.append(probe)
    if failed:
        sys.exit(f'probes failed: {failed}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', choices=sorted(GAMES), default='totk',
                    help='totk (default) or dkb: generated/dkb100, DKB-Setup.exe, runtime built from source at install')
    ap.add_argument('--out', type=Path, help='release folder (replaced); dkb default build/release/DKB-Setup')
    ap.add_argument('--zip', type=Path, nargs='?', const=True,
                    help='also write the folder as this zip; a bare --zip means DKB-Setup-<version>-windows.zip beside it (dkb)')
    ap.add_argument('--prove', action='store_true', help='only run the compile-and-probe check on an existing folder')
    args = ap.parse_args()
    global GAME
    GAME = args.game
    if args.out is None:
        if GAME == 'totk':
            ap.error('--out is required')
        args.out = MAIN_ROOT / 'build' / 'release' / GAMES[GAME]['name']
    out = args.out.resolve()
    if args.zip is True:
        args.zip = out.parent / f'{GAMES[GAME]["name"]}-{version()}-windows.zip'
    if args.prove:
        if GAME == 'dkb':
            sys.exit('--prove needs libdkbrt.a, which the dkb folder does not carry (it compiles the runtime at install)')
        prove(out)
        return
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    build_python(out)
    llvm = build_llvm(out)
    sdl = build_sdl(unpack('llvm-mingw') / f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64' / 'bin', unpack('ninja') / 'ninja.exe')
    build_bin(out, sdl)
    build_deps(out, sdl)
    runtime_lib(out, llvm)
    build_sources(out, setup_program='DKB-Setup.exe')
    build_launcher(out, llvm)
    collect_licenses(out)
    check_folder(out)
    check_bundle(out)
    sizes(out)
    if args.zip:
        write_zip(out, args.zip.resolve())
        if GAME == 'dkb':
            write_launcher_release(out, args.zip.resolve())


if __name__ == '__main__':
    main()
