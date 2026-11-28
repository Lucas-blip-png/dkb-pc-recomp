"""NVIDIA DLSS for upscaler = dlss (docs/dkb-dlss.md): nvngx_dlss.dll, the DLSS model, and nvngx_dlssg.dll, DLSS
Frame Generation's, fetched from NVIDIA's own repository (NVIDIA/DLSS) when the user asks for them and accepts
NVIDIA's license, never shipped with this project.

runtime/vk_dlss.c loads nvngx_dlss.dll from the folder of dkb.exe (Windows only); NGX looks for nvngx_dlssg.dll in
the same folder when frame generation is asked for (runtime/vk_dlssg.cpp). The release is pinned (TAG) and so is each
file's SHA-256, the license among them: the setup shows that text and asks the user to accept it before anything is
fetched, and a new release means new hashes here and a new acceptance. The files are read from
raw.githubusercontent.com at the tag, a public download that needs no login. The license is saved next to the DLLs.
The download and hash check are installer/fsr4.py's.
"""
import hashlib
import urllib.request
from pathlib import Path

from .fsr4 import FetchError, NET_ERRORS, TIMEOUT, _get, _sha256

TAG = 'v310.9.16'
REPO = 'https://raw.githubusercontent.com/NVIDIA/DLSS/' + TAG + '/'
PAGE = 'https://github.com/NVIDIA/DLSS/blob/' + TAG + '/LICENSE.txt'
LICENSE = ('LICENSE.txt', 'NVIDIA-DLSS-license.txt', 26620,
           'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0')
FILES = (
    ('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'nvngx_dlss.dll', 58956912,
     'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'),
    ('lib/Windows_x86_64/rel/nvngx_dlssg.dll', 'nvngx_dlssg.dll', 7506544,
     '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7'),
)

def license_text(repo=REPO):
    """NVIDIA's license of the pinned release as text, fetched and checked against its pinned SHA-256."""
    path, _, size, sha = LICENSE
    try:
        with urllib.request.urlopen(repo + path, timeout=TIMEOUT) as r:
            data = r.read(size + 1)
    except NET_ERRORS as e:
        raise FetchError('license', str(getattr(e, 'reason', e)) or type(e).__name__) from e
    if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
        raise FetchError('license', f'{len(data)} bytes with SHA-256 {hashlib.sha256(data).hexdigest()}, expected {sha}')
    return data.decode('utf-8')


def total_bytes():
    return sum(size for _, _, size, _ in (LICENSE, *FILES))


def installed(folder):
    """True when the DLL and the license are in folder with their pinned size and SHA-256."""
    try:
        return all((Path(folder) / name).stat().st_size == size and _sha256(Path(folder) / name) == sha
                   for _, name, size, sha in (LICENSE, *FILES))
    except OSError:
        return False


def fetch(folder, progress=None, repo=REPO):
    """Fetches the license and the DLL of TAG into folder, each checked before it takes its name. A file already
    there with the pinned hash is kept. progress(done_bytes) reports the bytes fetched so far."""
    folder = Path(folder)
    try:
        folder.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise FetchError(str(folder), str(e)) from e
    done = 0
    for path, name, size, sha in (LICENSE, *FILES):
        out = folder / name
        try:
            keep = out.stat().st_size == size and _sha256(out) == sha
        except OSError:
            keep = False
        if not keep:
            _get(repo + path, size, sha, out, progress, done)
        done += size
        if progress:
            progress(done)
