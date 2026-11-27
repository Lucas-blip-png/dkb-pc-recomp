"""The game folder's amiibo files: tag dumps of the player's own (.bin, or a Flipper Zero .nfc), copied into
<game folder>/amiibo, where the game's menu lists them (runtime/ov_nfp.c). Nothing here reads what a tag holds, and
no tag file belongs in the repository."""
import shutil
from pathlib import Path

FOLDER = 'amiibo'
ENDINGS = ('.bin', '.nfc')
BIN_SIZES = (540, 532, 572)   # an NTAG215 dump as the usual tools write it
NFC_LIMIT = 65536              # a Flipper file is a few kilobytes of text


def is_tag_file(path):
    """A file the game's menu would list: a .nfc text file, or a .bin of a tag dump's size."""
    p = Path(path)
    if not p.is_file() or p.name.startswith('.') or p.suffix.lower() not in ENDINGS:
        return False
    size = p.stat().st_size
    return size in BIN_SIZES if p.suffix.lower() == '.bin' else 0 < size <= NFC_LIMIT


def folder(dest):
    return Path(dest) / FOLDER


def count(dest):
    d = folder(dest)
    return sum(1 for p in d.rglob('*') if is_tag_file(p)) if d.is_dir() else 0


def add_files(dest, paths):
    """Copies the tag files among paths into the amiibo folder. Returns (copied, skipped): a file that is no tag dump
    is skipped; one of the same name is replaced."""
    d = folder(dest)
    copied = skipped = 0
    for p in map(Path, paths):
        if not is_tag_file(p):
            skipped += 1
            continue
        d.mkdir(parents=True, exist_ok=True)
        if (d / p.name).resolve() != p.resolve():
            shutil.copyfile(p, d / p.name)
        copied += 1
    return copied, skipped


def add_folder(dest, source):
    """Copies every tag file under source, four folders deep (as far as the menu looks), into a folder of source's
    name below the amiibo folder, keeping the subfolders. Returns (copied, skipped)."""
    src = Path(source)
    d = folder(dest) / src.name
    copied = skipped = 0
    if d.resolve() == src.resolve() or folder(dest).resolve() in [src.resolve(), *src.resolve().parents]:
        return 0, 0   # the amiibo folder itself, or a folder inside it: already there
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if p.is_dir() or len(rel.parts) > 4 or any(part.startswith('.') for part in rel.parts):
            continue
        if not is_tag_file(p):
            skipped += p.suffix.lower() in ENDINGS
            continue
        (d / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, d / rel)
        copied += 1
    return copied, skipped
