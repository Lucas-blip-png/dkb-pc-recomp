# Installer

## Bananza 1.0.0 and 3.2.0

The installer takes the game as one `.nup` file, or as an already dumped ExeFS (`main`, `sdk`, `rtld`, `main.npdm`)
and either a loose RomFS folder or a folder containing `romfs.bin` and `index.json`. No keys are requested. Two builds
are supported, each pinned by the SHA-256 of its ExeFS: base 1.0.0 (`dkb100`, `tools/data/expected-dkb100.json`) and
update 3.2.0 (`dkb458752`, title version 458752, `tools/data/expected-dkb458752.json`). The `main` hash picks the
target; any other ExeFS, or a mix of the two, fails these checks before the installer creates output or logs.

### .nup input

A `.nup` is the container Eglantine and Eden load: loose ExeFS and RomFS trees for the base, its updates and add-on
content, every file raw or zstd-compressed and listed with its size and SHA-256 in a table of contents whose own
SHA-256 is in the superblock. `tools/extract_nup.py` reads it (an independent reader of Eglantine's
`core/file_sys/nup_container.cpp` format).

- What is installed follows Eglantine's loader (`core/loader/nup.cpp`, `AppLoader_NUP`): with `--nup-package latest`,
  the default, the ExeFS of the newest update package (the base's when the update has none) and the base RomFS with
  the update's RomFS laid over it, a file of the update replacing the one at the same path and folders merging
  (`NupContainer::MergeShadowDirs`). A standalone update package without a base gives its own trees.
  `--nup-package base` installs 1.0.0 from a container that also holds an update. Add-on content is ignored.
- Preflight reads only the table of contents: the target by the recorded SHA-256 of `main`, every pinned ExeFS hash
  against its record, and the RomFS file list (path, size, SHA-256) in the form a loose folder gives, so the identity
  of the data step is the same as for the same files in a folder. An update that is neither target stops with
  `dkb.nup_version`, which names its version. When a selected file is zstd-compressed, the Python that runs the
  tools must decompress zstd (`compression.zstd` of Python 3.14, which the Windows release ships, the `zstandard`
  module or a `zstd` program), or setup stops with `dkb.nup_zstd`.
- The data step runs `extract_nup.py --select <latest|base> --pack`, which writes the ExeFS and packs the RomFS straight
  from the container into `romfs.bin` and `index.json` in `tools/dkb_pack_romfs.py`'s layout (no loose 19 GB copy),
  checking every file against its SHA-256 record. The image is then read back against the preflight list as for loose
  input. The container is not hashed as a whole; a change after preflight shows up as a failed record check.

### .nx input

`--nup` (and the window's picker) also takes a `.nx` archive, the [NXFormat](https://github.com/fab918/nxformat) container
(MIT, `docs/SPEC.md`). `tools/extract_nx.py`, written from the specification, presents it to `extract_nup.py` like a
`.nup`: each snapshot of the first application is a package (base, update, or add-on content that carries its title id),
read from the `exefs/` and `romfs/` folders of the snapshot's tree. The file is told apart by its `NXAR` magic, so the
choices, checks, output folders and `--nup-package` behave as in the `.nup` section above. Blocks are RAW or zstd (the
same `dkb.nup_zstd` rule); there is no encryption. The header CRC-32C and the catalog, manifest and block-index SHA-256
are checked at open and each file's SHA-256 when it is read; the per-16-KiB block CRCs are not. Source resources of
NX 0.3 are ignored. An update snapshot is a complete tree, so laying it over the base also keeps files the update
deleted, as for a `.nup`.

Run from this checkout with Python 3.12 or later, with the generator's dependencies (`tools/requirements.txt`). The
window also needs tkinter. The source checkout must contain the full runtime; the older TotK prebuilt runtime
library cannot build Bananza. A Windows tool package supplies:

```
<tools>/llvm/bin/clang.exe, llvm-ar.exe, llvm-windres.exe
<tools>/runtime/deps/include/, lib/       Vulkan, VMA, SDL3, Opus, libvpx and zstd dependencies
<tools>/bin/ninja.exe, SDL3.dll, libopus-0.dll
```

The package contains tools only, and may be kept separately from this checkout. No machine-specific path is built
into the installer. `--toolchain`, `--deps`, `--ninja` and `--dll-dir` override individual paths. Without `--package`,
the installer checks this checkout's existing toolchain layout. It never installs tools globally.

```
python -m installer --game bananza --nup "<dump>/game.nup" --dest "<install folder>" --package "<tools>" --jobs 6
python -m installer --game bananza --exefs "<dump>/base/exefs" --romfs "<dump>/base/romfs" --dest "<install folder>" --package "<tools>" --jobs 6
```

`--romfs-in-place` (the window's "Use the RomFS where it is") skips the 9 GB packed copy: the install gets only
`romfs/index.json`, and `romfs_dir` in `dkb.ini` points the game at the extracted RomFS folder, which it reads file by
file (runtime/romfs.c, loose mode). The folder must then stay where it is; if it moves, the game stops at start or at
the first missing file and names the path. It needs a folder of loose files, not a `.nup` or a packed image. A resume
without folders keeps the choice; giving the folders again without the option packs the copy and removes the key.

The shader translator is reused when its `.id` matches the current translator sources. Pass `--translator
<path>/totk_xlate.exe` to choose a prebuilt translator, keeping `totk_xlate.id` beside it. Otherwise the installer
builds it with `tools/build_xlate.py` in its owned temporary folder. That path needs the .NET 10 SDK and, on Windows,
the Visual Studio C++ build tools and Windows SDK for NativeAOT. `--dotnet <portable-sdk>/dotnet.exe` selects a local
SDK; the checkout's `toolchain/dotnet` and PATH are also checked. These settings affect child processes only.

The native window uses the same CLI, steps and cancellation mechanism:

```
python -m installer.gui --package "<tools>"
```

Its pickers select a `.nup` file or the ExeFS and RomFS folders, the optional tool package, language and destination.
The window always installs the newest supported content of a `.nup`; `--nup-package base` is a command-line option. The individual tool overrides,
including `--translator` and `--dotnet`, also work on this command. `--game totk` selects the inherited TotK window.

The Bananza steps are:

1. Hash the supported ExeFS and all loose RomFS input files (for a `.nup`, read their records from its table of
   contents), reject overlapping or linked input/output paths,
   validate the tool paths and translator identity, then check disk space, AVX2 and Vulkan 1.3.
2. Stage ExeFS and run `tools/dkb_pack_romfs.py` for loose input, or `tools/extract_nup.py --pack` for a `.nup`. Read the packed image back, check its image checksum
   and every file against the initial content snapshot, then copy verified files into the destination. A prepacked
   input's checksum and bounds are checked too. This verifies packing integrity; a loose dump has no NCA hash tree
   to authenticate its original contents.
3. Prepare the matching shader translator.
4. Generate `main,sdk` into the temporary folder with the target's patch table: `recomp/patches-dkb100.txt`, whose
   missing applied patches stop the build, or the deliberately empty `recomp/patches-dkb458752.txt`. `subsdk0` is not
   generated.
5. Build the target (`dkb100` or `dkb458752`) from the current runtime sources with `tools/build_game.py`, without `--runtime-lib`, then copy the
   executable, runtime libraries and RomFS index into place. The default worker limit is six; `--jobs` changes it.
6. Translate the game's shaders: `tools/dkb_shader_census.py` writes the list of shader stages in the packed RomFS into
   `seed-nvn2.bin`, `dkb.exe --translate-shaders` lowers and translates them into the shader cache beside the executable
   (a few minutes, about 3 GB), and the pack is deleted. Last, `tools/pack_shader_cache.py` folds the cache's loose files (four per shader, about 118,000 on 3.2.0) into `shader-cache/shader-cache.pack`, so the folder copies and deletes in seconds instead of half an hour on Windows; a failure there is a warning. Without this step the first visit to each area waits for its
   shaders. Then `dkb.exe --warm-shaders` builds the driver's pipeline libraries; if that fails or the driver crashes
   (seen in `nvgpucomp64.dll` on NVIDIA 621.01) the installer warns and goes on, records `libraries: false` and does not
   retry, since the libraries only speed up the first draws. A translation failure fails the step.
7. Generate backgrounds and both centered/edge HUD variants for all six ultrawide presets, including 3440x1440.
   `tools/dkb_wide_layout.py` checks packed resource bounds and expected layout geometry in the owned temporary
   `build/` folder before copying overlays beside `romfs-index.bin`. Each overlay's manifest is published last.
   Missing or changed overlays are repaired on resume without relifting or compiling the game; changes to either
   layout tool also invalidate this step. Python 3.14+ uses `compression.zstd`; older development Python needs
   `zstandard` from `tools/requirements-installer.txt`. If generation fails (including an unrecognized resource layout),
   setup warns with the generator log path and continues without publishing new overlays; existing overlays remain
   unchanged. The next setup run retries generation. Installer path/ownership failures and cancellation still stop setup.
   The verified game version selects the layout rules: 3.2.0 removes one confirmation
   dialog background pane and stores its title effects in `static.Product.200`. Overlay preparation preserves the
   player's resolution and HUD settings.
8. Write the selected language while preserving existing settings, run the installed executable's `--selftest`, and
   remove only owned staging/build directories. Logs remain in the temporary folder. The setup itself does not start
   the game; the window's finish screen has a Play button that does, once the folder checks out as finished.

The portable output contains `dkb.exe`, its runtime DLLs, `totk_xlate.exe` and `.id`, `exefs/`, packed `romfs/`,
`romfs-index.bin`, `dkb.ini`, `dkb-install.json`, the `shader-cache/` folder of step 6, and twelve
`romfs-patches-<width>x<height>[-hud-edges]/` folders. An existing `totk.ini` is migrated to `dkb.ini`.
Saves and further shader caches are created beside the executable when it runs. Source dumps stay untouched.

Setup owns those twelve preset overlay folders and overwrites matching hand-made overlays when generation succeeds.
Keep custom overlays in a separate folder and select it with `DKB_ROMFS_PATCHES` to avoid that overwrite.

Only an empty destination or a Bananza install with matching ownership metadata is accepted. Temporary work has its
own ownership marker; an unrelated nonempty temporary directory is refused. OS-held locks prevent simultaneous
setups from writing the same destination or workspace and are released after process exit, including a crash.
Cancellation stops the installer's child process tree, leaves completed steps recorded atomically, and never deletes
source files, saves or other user settings. Partial files are not recorded as completed steps.

Resume with the same command, or `python -m installer --game bananza --dest "<install folder>"`. Recorded source and
tool paths are reused, input contents are fingerprinted again, and intact completed steps are skipped. Keep the source
folders available until setup completes. `--verify` also hashes kept output. Changed input, generator, required patch
table, runtime, dependencies or translator identity invalidates the corresponding steps. `--temp` selects a workspace
base; it is kept separate from the destination and sources. Re-running setup preserves `profile/`.

A finished Bananza install keeps its generated C and objects in the work folder so a later run does not lift or compile
again. `python -m installer --dest "<install folder>" --clean` (add `--temp` if setup used it) deletes that folder, only
when its owner file says it belongs to that destination, and prints its path and the freed size; it exits 1 when there
is nothing to delete or the folder is not ours. The install folder is never touched. The finish screen shows the same
folder and size with a Delete build files button.

`python -m installer --dest "<install folder>" --uninstall` removes the whole install: the work folder and everything in the
install folder except `profile/` (saves), `dkb.ini` and `mods/`, which stay. A command line cannot ask, so `--delete-saves` with it
removes those too and the install folder itself. It refuses a folder without this setup's record and any link inside it,
before deleting anything, and exits 1 while a setup runs. The finish screen has an Uninstall button that asks for
confirmation, then asks separately whether to delete the saves (default No).

To reinstall after keeping player files, choose the same destination and provide a `.nup` or dump folders for the same
game version. Setup recognizes the record left by uninstall and reuses `profile/`, `dkb.ini` and `mods/`. For a folder
left by an older uninstaller without that record, install in an empty folder and copy the player files there afterward.

CLI exit codes and JSON events are the same as below. Lightweight checks, with no game files:

```
python tests/installer_bananza_test.py
python -m installer.gui --game bananza --selftest
```

A full generation/build and a title-screen run from the finished folder are separate end-to-end checks. Synthetic
installer tests do not establish game compatibility. No completed install, game-derived output or cache belongs in a
release or pull request. Release assembly in `tools/make_release*.py` still describes the inherited TotK package; a
Bananza release must include the complete runtime source/dependencies rather than its old runtime archive.

## Launcher and updates (Windows)

`DKB-Launcher.exe` on the release page is the one file a player downloads (about 100 KB). It is the same program as
the zip's `DKB-Setup.exe`: beside a `python\` folder it starts the setup window as before and passes its own arguments
on, anywhere else it is the launcher. It is built from three files in `installer/launcher/`:

- `launcher.c` (MIT, from TotK Recomp): the setup, the start of the installed version (`current.txt`, else
  `previous.txt`), the arguments and the catalog strings. `TotK-Setup.exe` is this file alone.
- `manifest.c` (MIT): reads `dkb-release.txt`, accepting exactly what `installer/update.py parse_manifest` accepts
  (`ManifestParsersTest` feeds both the same files).
- `fetch.c` (GPL-3.0-or-later): the first install, that is the download, the checks, the unpack and the progress
  window. It is adapted from Longshot's and Project ReLink's setup programs (`THIRD_PARTY_NOTICES.md`), and only the
  Bananza build compiles it (`tools/make_release.py launcher_sources`).

What the player does:

1. Puts `DKB-Launcher.exe` in a folder of its own (for example `C:\Games\DKB\`) and starts it.
2. The first start asks before it downloads anything, then reads `dkb-release.txt` of the latest release, downloads
   the tools zip it names (the same `DKB-Setup-<version>-windows.zip`), checks its size and SHA-256 and unpacks it
   with Windows' own `tar.exe`. A file that does not match is deleted and nothing is unpacked or run. Closing the
   progress window cancels: during the download at once, during the unpack once `tar.exe` is done; either way nothing
   is kept or started. A second launcher started meanwhile waits for the first one's install and then starts it.
3. The setup window opens with the install folder proposed beside the launcher's folder (`DKB-Bananza`); the player
   picks the `.nup` or the ExeFS and RomFS folders, as with the zip, and the setup builds the game.
4. From then on the launcher opens on its home screen: Play (starts `dkb.exe` from the install folder and closes the
   launcher), Open folder, Install or rebuild (the form), and the release check.

Everything the launcher keeps is in `DKB-Recomp` beside it (`installer/update.py` describes each file):

```
DKB-Recomp/
  launcher.json            the install folder and the "look for a new release at start" setting
  versions/current.txt     the version that starts; previous.txt the one before it
  versions/<tag>/          an unpacked release zip (its own Python, LLVM, sources and installer)
  downloads/               the zip while it downloads; deleted once unpacked
  temp/                    the setup's work folder, shared by every version (--temp)
  logs/launcher.log        what each check, download, verification and update did
```

The game folder stays as described above: saves in `profile/`, `dkb.ini`, the shader cache, all beside `dkb.exe`.

**The check.** At each start the window asks GitHub's API for the latest release of this repository (when the API
does not answer, for instance past its 60 requests an hour, it reads `dkb-release.txt` from the release's download
link instead). That is the only request; it carries a generic User-Agent and no personal data, and nothing is
downloaded until the player clicks Update. The home screen then says one of: a newer release is available, with its
notes as GitHub has them and an Update button; this is the latest release; the latest release is older (no update
offered, unless the window was started with `--allow-downgrade`); or GitHub could not be asked (offline: Play still
works). A release made before the launcher (no `dkb-release.txt`) is offered with its page only. Versions compare by
their numbers (0.10.0 is newer than 0.9.9, the commit after `+` does not count); pre-releases and drafts are never
offered. The tick box under it turns the check at start off (`launcher.json` `"check_updates": false`); Check now still
asks once. `--no-update-check` and `DKB_NO_UPDATE_CHECK=1` turn it off as well.

**The update**, on one click:

1. The window checks that the release's `dkb-release.txt` names the release's own tag and that the API lists both
   files with the manifest's sizes (and, where GitHub gives one, the same SHA-256 digest).
2. It downloads the tools zip to `downloads/<sha256>.zip.part`, three tries, counting the bytes and hashing as they
   come; a file longer than the manifest says, cut short, or with another SHA-256 is deleted. Only a match is renamed
   to `<sha256>.zip`.
3. It unpacks the zip into `versions/<tag>.part`. Every member must lie under the zip's one top folder (no absolute
   path, no `..`, no link) and have a name Windows keeps as it is (no `<>:"|?*` or control character, no trailing dot
   or space, no device name such as `CON` or `NUL.txt`). The members together may unpack to at most 20 times the zip's
   size, never less than 1 GiB and never more than 64 GiB (`update.unpack_limit`). It then checks that the result holds
   the manifest's `VERSION`, its Python and the installer, and renames it to `versions/<tag>`. Any failure removes the
   `.part` folder; the installed version is untouched. Cancel stops the download at its next block and deletes its
   `.part` file; a Cancel that comes as the last block arrives deletes the checked zip without unpacking it, one
   during the unpack keeps the unpacked folder for the next try; nothing starts in either case. Closing the window
   during the update asks first, then does the same and closes once the worker is done.
4. It starts the new version's window with `--after-update` and waits until that window confirms its start: it writes
   its own tag to `current.txt` and the old one to `previous.txt`. If the new window exits first, `current.txt` still
   names the old version, the old window says so and the next start of the launcher runs the old version.
5. The new window replaces the launcher exe with its own `DKB-Setup.exe` when they differ (the file came in the checked
   zip): a copy, `DKB-Launcher.exe.new`, is checked again and renamed over the launcher in one step, which works because
   the launcher has exited by then. A launcher that still runs (started a second time) cannot be overwritten but can be
   renamed: it becomes `DKB-Launcher.exe.old`, deleted at a later start, and the copy takes its name. No `.new` copy is
   left behind.
6. It runs the setup on the recorded install folder (`--dest` only, as a resume). The step keys decide what runs
   again: a release that changes the runtime recompiles, one that changes the lifter relifts, one that changes nothing
   game-derived skips the build. For 3.2.0 the generated code and objects in the shared `temp/` are reused. The dump
   recorded at install is read again only when the data step must run again, so keep it where it was. Saves
   (`profile/`) and the `dkb.ini` settings are kept, as on every setup run.

A failure in the tools part (steps 1 to 4) changes nothing that runs: the installed version and the game folder are
as they were. A failure or cancel of the setup run in step 6 leaves what the setup's resume handles. Before a rerun
step finishes (the runner records a step only once it is done, and compile copies `dkb.exe` in at its end) the old
game still plays and Play stays. After a rerun compile or translator step, until the final check (`dkb_verify`) has
run again, the home screen shows the game as not built (`guilogic.install_state`) and Install or rebuild finishes it
from the steps already done. Steps the check does not depend on (shaders, wide layouts, prompts) never hold Play back. The previous version folder stays until the next update and is the one the launcher starts when the current one
has lost its Python (an antivirus removing files). Older version folders are removed when a new version confirms its
start; a folder still in use stays and goes at a later start.

**Going back.** While `previous.txt` names a version whose folder still holds its Python, the home screen offers
"Go back to <tag>". After the player confirms, the window starts that version with `--after-update`, exactly as an
update does with a new one. Its start (`update.confirm_start`) makes it `current.txt` and the running version
`previous.txt`, and its setup run brings the game folder back to it. Nothing is downloaded and both folders stay, so
the next check offers the newer version again, unpacked already.

**Uninstall.** Delete the install folder (it holds the saves), `DKB-Recomp` and `DKB-Launcher.exe`. The launcher
writes nothing to the registry, the Start menu or AppData; the GPU driver's own shader cache stays in the driver's
folder.

**Publishing.** `tools/make_release.py --game dkb --zip` writes `DKB-Launcher.exe` and `dkb-release.txt` beside the
zip (`write_launcher_release`). The manifest is plain text, so the C launcher reads it without a JSON parser:

```
dkb-release 1
version 0.4.2+abc1234
tag v0.4.2
tools <sha256> <bytes> DKB-Setup-0.4.2+abc1234-windows.zip DKB-Setup
launcher <sha256> <bytes> DKB-Launcher.exe
```

The release workflow (`.github/workflows/dkb-release.yml`) uploads all three as the Windows artifact. On a `v*` tag,
`tools/check_release.py` then checks that the manifest reads as the launcher reads it, names the tag and gives the size
and SHA-256 of the files about to be published, and the job publishes them. A tag `v` plus the repository's `VERSION`
becomes the latest release. A pre-release tag (`v0.4.2-rc1`; its manifest names `v0.4.2`) becomes a pre-release,
which neither the launcher nor the window ever offers. Only that publish job may write to the repository; the build
jobs only read it. Older releases have no manifest: the launcher's first start then says so and points to the release
page.

**Testing without a release.** A launcher built with `-DDKB_LAUNCHER_TEST` (never a release: the release build has no
such variables) reads `DKB_LAUNCHER_BASE` in place of the `DKB-Recomp` folder, `DKB_LAUNCHER_MANIFEST` and
`DKB_LAUNCHER_DOWNLOAD` in place of the manifest link and the download prefix (http is accepted only for 127.0.0.1),
and with `DKB_LAUNCHER_LOG` set it shows no window: its messages go to that file and its questions are answered yes.
`tests/installer_update_test.py` covers the manifest (good, tampered, truncated), the check (newer, same,
older, downgrade, offline, API fallback, mismatching digests), the download (good, tampered, truncated, interrupted,
too long, cancelled), the unpack (good, members leaving the folder, wrong version, damaged zip), the switch of versions
(confirmed, failed, slow start), the launcher's replacement and the window's settings, all with an injected opener and
no network. `python -m installer.gui --selftest --shots DIR` draws the home and update screens in both languages.
`tests/launcher_e2e.py` (`launcher-e2e` in `tools/test.py`, tier 1: Windows, MSYS2 clang, under a minute) runs
against a fake release server on 127.0.0.1. It does the first install of a release A with a `-DDKB_LAUNCHER_TEST`
launcher for real, with a stub in place of Python and a junk `winhttp.dll` and `bcrypt.dll` beside the launcher,
then the update to a release B in A's window: an `installer.gui.App` on a hidden Tk root whose check, Update and Go
back run as a click runs them, its requests going through `update.http_open`, and B's start code
(`gui.start_launcher`) in B's own folder. Both include the failure paths: a tampered zip, a cancel, two launchers at
once, a window that never confirms, a kill between the two pointer files, and going back. GitHub's TLS and redirects
are the part it does not cover.

## Inherited TotK installer

The remainder documents the preserved TotK container workflow (`--game totk`).

A public release that turns the player's own game files into a playable TotK folder. The release carries no game
data and no code derived from the game: the installer builds the game on the player's machine. The installed folder
holds only the game; the installer and its tools stay outside it.

## What the player does

1. Downloads `TotK-Setup-<version>-windows.zip` from GitHub Releases (none is published yet), unpacks it anywhere and
   starts `TotK-Setup.exe`.
2. Picks:
   - the base game (XCI or NSP) and the 1.2.1 or 1.4.3 update (NSP);
   - their `prod.keys`;
   - the game language and the install folder.
3. Waits for the setup, one time. The window shows each step and its progress.
4. Plays `totk.exe` from the install folder. The installer and the source NSP or XCI can be deleted.

The install folder (`main.c` `portable_paths()` reads this layout; it stays portable):

```
<install folder>/
  totk.exe, SDL3.dll, libopus-0.dll, totk_xlate.exe
  totk.ini                       language; the other keys as commented defaults (totk.ini.example)
  exefs/, romfs/                 extracted from the player's files
  profile/                       saves
  logs/                          totk.log of the last run, totk.1.log to totk.4.log of the runs before it
  shader and pipeline caches     prepared by the setup; shader-cache/shader-cache.pack holds the translations
  totk-install.json              what built this folder: release version, input hashes, finished steps
```

## Updating the port

The player runs a newer installer on the same folder. It reads `totk-install.json`, keeps `romfs/`, `exefs/`,
`profile/` and `totk.ini` after checking their hashes, rebuilds `totk.exe` and re-prepares the shaders. A newer
release that changes nothing game-derived skips the rebuild.

Given an update file other than the recorded one (1.2.1, then 1.4.3), the installer extracts and builds again from it and
keeps `profile/`. Saves carry over both ways: 1.4.3 converts a 1.2.1 save on load, and 1.2.1 loads the saves 1.4.3 wrote.

## Setup steps

1. **Check** before writing anything: the files open with the keys, the update is 1.2.1 or 1.4.3 (by hash, as
   `tools/extract.py`), the keys are new enough for the update, enough space is free on the target disk (about 36 GiB at the peak of a
   first install, 20 GiB after it), the CPU has
   AVX2 (`-march=x86-64-v3`), a Vulkan 1.3 GPU exists. Each failure is one clear message. On Windows the Bananza
   installer also warns, without stopping, when the commit limit (RAM plus page file, from `hostcheck.c`) is under 24 GiB.
2. **Extract** ExeFS and RomFS into the install folder and verify the hashes.
3. **Lift** the ExeFS to C (`recomp/gen.py`, `recomp/patches-v121.txt` or `patches-v143.txt`) into a temp folder next
   to the installer.
4. **Compile** the generated C, `modules.c`, `romfs.c`, `patches.c` and `ov_zstd.c` (they bake the per-install layout, RomFS
   hashes and the game version's patch page) and link
   them with the prebuilt runtime library into `totk.exe` (`build_game.py --runtime-lib`; about 3 min on 24 cores).
5. **Prepare** the RomFS patches and the shaders (`tools/make_romfs_patches.py`, `tools/prepare_shaders.py`,
   `totk.exe --prepare-shaders`) in the install folder, then fold the shader cache's loose files (four per shader, about
   117,000) into `shader-cache/shader-cache.pack` (`tools/pack_shader_cache.py`). totk.exe reads the pack first and
   writes the translations it makes in play as loose files; running the tool again with the game closed folds those in.
   The patches are written twice: `romfs-patches` and `romfs-patches-xbox` (`--xbox-prompts`, the A/B and X/Y prompts
   swapped for `pad_layout = xbox_prompts`).
6. **Clean up** the temp folder (generated C and objects, about 1.3 GB).

A cancelled or failed setup resumes from the last finished step (`totk-install.json`).

## Command line

`installer/` (stdlib only; `installer/layout.py` maps the release folder and, in a repository, the dev toolchain):

```
python -m installer --base FILE --update FILE --keys FILE --lang CODE --dest DIR [--root DIR] [--temp DIR] [--verify]
```

- On a folder with `totk-install.json` only `--dest` is needed; the game files only when the extraction must run again.
- stdout: one JSON event per line, types `start`, `plan`, `step` (started, done, skipped), `progress` (step, phase,
  done/total/unit), `space` (bytes needed and free on the install disk and the temp disk), `warning`, `error`, `end` (`installer/events.py`). Messages are ids with parameters;
  `installer/text/en.json` holds their English text. The human-readable log goes to `<temp>/logs/`.
- Exit codes: 0 done, 1 failed, 2 bad command line, 3 cancelled. The window cancels with Ctrl+Break to the
  installer's process group.
- Each step records a key over its inputs (runner.py). A step with the recorded key and intact output is skipped, so
  resume and update are one rule; output kept from another release is re-hashed first.
- Peak space on the install disk is about 36 GiB, not 22: the extraction copies both Program NCAs (16.3 GiB) next to
  the merged RomFS (15.9 GiB) before it deletes them. The check computes the exact figure.
- Window (`python -m installer.gui`, `installer/gui.py` draws, `installer/guilogic.py` decides): form, step list with
  progress, space line, collapsible log, Cancel (Ctrl+Break to the CLI's process group, then it waits for the exit),
  finish screen with Open folder. English or Russian, chosen from the Windows display language, switchable in the
  window; every sentence is an id in `installer/text/<en|ru>.json`. A folder with `totk-install.json` needs only the
  folder. `--selftest [--shots DIR]` builds every screen in both languages and closes itself.
  The look follows the in-game menu (bark browns, banana yellow, cream text, red and green accents) as a ttk theme on
  clam with a drawn banana-bunch header; no game art or fonts. Text colours keep at least 4.5:1 on their background.
- `tests/installer_gui_test.py`: event model, form, catalogs, cancel, the self-closing window and one end-to-end run
  of the CLI in `--verify` mode (needs `build/install-test` and `build/extracted/v121`; see its docstring).
- `tests/installer_test.py`: bookkeeping with fake steps. `tests/installer_e2e.py`: a dev-mode run with the files of
  `config.local.toml`.

## Release layout

The unpacked release; `tools/make_release.py` assembles it and `--zip` packs it:

```
TotK-Setup.exe        starts python/pythonw.exe -m installer.gui (installer/launcher/launcher.c, no console)
installer/            the installer: steps, CLI, window, en/ru text catalog
python/               Python 3.14.9 embeddable + numpy, lz4, cryptography, cffi, pycparser (+ tkinter for the window)
recomp/, tools/       the lifter and the build, extraction and shader scripts the steps run (copied from the repo)
llvm/                 llvm-mingw, x86_64-only subset (Apache-2.0 with LLVM exception; mingw-w64 parts permissive)
runtime/              libtotkrt.a and the headers the generated code and modules.c/romfs.c/patches.c include
bin/                  totk_xlate.exe, hactool.exe, SDL3.dll, libopus-0.dll, ninja.exe
data/                 shader_list.tsv (MacNativeGames, MIT)
THIRD_PARTY_NOTICES.md
LICENSE, LICENSE-MIT  the project's terms (GPL-3.0-or-later; MIT for the files that came from TotK Recomp)
totk.ini.example, VERSION  the default ini; the release string (the repository's VERSION plus the commit)
```

`make_release.py --game dkb` makes the Bananza folder (`DKB-Setup-<version>-windows.zip` with `--zip`) from the
same layout, with these differences: the launcher is `DKB-Setup.exe` (its two messages are the `dkb.launcher.*` ids);
`runtime/` holds every runtime source instead of `libtotkrt.a`, because the installer compiles the whole runtime at
install time; `bin/` has no `hactool.exe` (Bananza reads plain ExeFS and RomFS folders or a `.nup`); `tools/` adds
`build_xlate.py`, `dkb_pack_romfs.py`, `dkb_shader_census.py`, `extract_nup.py`, `vendor/ryujinx` and `xlate` (the translator is built
at install); the lifted tree is `dkb100` instead of `v121`/`v143`.

- SDL3 comes from the upstream release, not MSYS2: MSYS2's DLL imports the LGPL libiconv.
- Every bundled third-party piece gets a row in `THIRD_PARTY_NOTICES.md`.
- The release contains nothing game-derived; `make_release.py` runs `tools/check_content.py` over the folder before it
  finishes.

## Linux release

TotK only. The AppImage described here is TotK's (`--game totk`); Bananza has the source packages below and the
self-contained image built with `--game dkb` (see above).

### Bananza Linux and macOS packages

Preview. `DKB-Setup-<version>-linux-x86_64.tar.gz` and `DKB-Setup-<version>-macos-arm64.zip` are built by
`tools/make_release_posix.py --platform linux|macos --pack` in `.github/workflows/dkb-release.yml`, next to the Windows
zip. They carry no binaries and no game data: the player already has the system packages, and the build runs on their
machine. The folder holds `installer/`, `recomp/`, `tools/` (with the translator and its vendored sources), every
`runtime/` source and header, `data/`, `licenses/`, `THIRD_PARTY_NOTICES.md`, `LICENSE*`, `VERSION` (version and commit, as
on Windows), `dkb.ini.example`, `README-SETUP.txt`, `recomplauncher.json`, `play.sh` and `setup.sh` (Linux) or
`setup.command` (macOS).

The flow: unzip (the executable bit is kept), run `./setup.sh --check` to list what is missing (clang, lld, ninja, SDL3,
Opus, libvpx, zstd, Vulkan headers, spirv-tools, .NET 10 SDK, uv, Git; Linux 6.7 or later, or macOS 14.4 or later on
arm64) with the apt, dnf, pacman or brew command that installs it, then `./setup.sh --exefs DIR --romfs DIR --dest DIR`.
Without `--check` the script creates `.venv` with uv (Python 3.12), installs `tools/requirements-installer.txt` and runs
`python -m installer --game bananza` with the same arguments. It never uses sudo. The CI jobs run `--check` and
`python -m installer --help` inside the assembled folder; they never see game files. The folder has no `python/`, so the
installer runs in its repository mode: system clang, ninja and libraries, temp folder `build/install-temp`. Status: on
Linux the build from source is verified on AMD and NVIDIA; macOS boots to the mode select without the destructible
terrain ([building.md](building.md)).

RecompLauncher installs the same package and reads its `recomplauncher.json` (from `tools/release/`, the launcher's
docs/DEVELOPERS.md in elripalda/RecompLauncher describes the format). The launcher asks the player for their copy, a
.nup file or a folder with `exefs/` and `romfs/`, and runs the profile's prepare step: `setup.sh --prepare --source
<copy> --dest <package folder>/game`. `--source` stands for `--nup`, or `--exefs` and `--romfs`, depending on what the
path is. `--prepare` prints the `progress <0..1> <text>` lines the launcher shows under its setup step instead of JSON
events (`events.py`, `LauncherReporter`): every step of the plan gets an equal share of the bar, and an error's text is
the last line. The launcher then starts `play.sh`, which runs `game/dkb` from its folder (and says so when it is not
built yet), and backs up `game/profile/`. It runs the step again after it installs a newer package, which resumes or
updates the recorded install as a second `setup.sh` run would. A missing system package stops the step with exit code 1
before Python starts; `./setup.sh --check` in a terminal names it.

`tools/make_release_linux.py` builds the same setup for Linux and packs it as `TotK-Setup-x86_64.AppImage` (the static
type-2 runtime: no libfuse2 needed on the host). The player makes it executable and starts it; on a Steam Deck, from
Desktop Mode. Building the release also needs `make` and `pkg-config`. The bundled Tcl/Tk 9.0.4 is rebuilt with
Xft against the sniper sysroot so Russian text uses system TrueType fonts. The setup builds the game into the folder they pick, as on Windows, and the game is `totk` there with
`libSDL3.so.0` beside it. The folder inside the AppImage:

The same script with `--game dkb` builds the Bananza folder (`DKB-Setup-x86_64.AppImage`): the same `python/`, `llvm/`
and sysroot, no hactool, `runtime/libtotkrt.a` built from `generated/dkb100`, and the Bananza installer, which links that
library instead of compiling `runtime/` from source. It needs a lifted game (or `--stub`, which builds the library
against `tools/ci/stub/dkb100`) and .NET 10 at pack time. The release workflow (`.github/workflows/dkb-release.yml`)
builds it with `--stub` and publishes it with a `.tar.gz` of the same folder (`--tarball`); the AppImage entry is `python/bin/python3 -m installer.gui` and `--check` and `--prove` apply as above.

```
AppRun                starts python/bin/python3 -m installer.gui (no bytecode written into the read-only mount)
totk-setup.desktop, totk-setup.png
installer/, recomp/, tools/, data/, licenses/, THIRD_PARTY_NOTICES.md, totk.ini.example, VERSION   as on Windows
python/               python-build-standalone 3.14 (with tkinter) + the installer's packages; a .pth puts the folder,
                      tools/ and recomp/ on sys.path
llvm/                 LLVM 23 (clang, lld, llvm-ar) and llvm/sysroot: the files of the Steam Runtime 3 "sniper" SDK the
                      game's compile and link read (about 130 files)
runtime/              libtotkrt.a, headers, the install-side sources, totk.png; deps/ with the Vulkan headers, VMA and a
                      static zstd
bin/                  totk_xlate, hactool, ninja, libSDL3.so.0
```

- What we compile is built against the sniper sysroot (glibc 2.31): the game, hostcheck, hactool and the translator
  need no newer glibc. The game links Opus, libvpx, zstd and libstdc++ statically and needs only glibc, the system's
  Vulkan loader and the SDL3 beside it. The setup itself needs glibc 2.34 (Ubuntu 22.04, Debian 12, Fedora 35,
  SteamOS 3): the official LLVM binaries and the `cryptography` wheel are built against it. `make_release_linux.py`
  checks every ELF file in the folder against that floor (`check_glibc`), and the setup's check step stops with one
  message on an older system.
- The setup's temp folder (`--temp` changes it) is `.setup-temp` inside the install folder for TotK. For Bananza it is
  `.dkb-setup` inside the install folder (AppImage, #402; a `--temp` inside the install folder is still refused), or
  `build/install-temp` in a source package. The AppImage is mounted
  read-only, and a Steam Deck's `/home` is often too full for the generated C while the game goes to the SD card.
- The game keeps Mesa's shader cache in `mesa-cache` beside `totk` unless `MESA_SHADER_CACHE_DIR` is set, for the
  same reason: RADV fills its default folder in `~/.cache` up to 1 GB. The game raises that cap to 10 GiB
  (`MESA_SHADER_CACHE_MAX_SIZE`, and `__GL_SHADER_DISK_CACHE_SIZE` for NVIDIA's driver) unless the variable is already
  set, so that the stage libraries the setup warmed are not evicted.
- The window follows `LANG` for English or Russian, cancels through SIGTERM to the CLI's process group (the CLI handles
  it as Ctrl+Break on Windows) and opens the finished folder with `xdg-open`.
- Requirements beyond Windows': Linux 6.7 or later (the write watch, `docs/building.md`) and glibc 2.34.
- Building the release needs one game version extracted and lifted (`tools/extract.py`, `recomp/gen.py`, either
  version): the runtime library includes the generated `layout.h`, whose shared part (`MOD_COUNT`, `MOD_MAIN_BASE`)
  is the same for both. `tools/make_release.py` takes `generated/v121`, else `generated/v143`.

## Constraints

- The installer steps contain no Win32 calls: Linux runs the same steps with its own compiler ("Linux release").
- The keys file path is passed to the tools; key values never appear in the UI, logs or files.
- The window is a front end over the CLI (`python -m installer --base ... --update ... --keys ... --lang ... --dest
  ...`), which prints one JSON progress event per line. Tests and CI use the CLI.
- Paths may hold any Unicode characters (a Cyrillic user name puts the temp and install folders under it). Every exe
  the setup runs or builds reads its arguments and paths as UTF-8 through an `activeCodePage` manifest: hactool
  (our patch, [building.md](building.md)), totk.exe (`runtime/totk.rc`), ninja (upstream). The Python steps write
  their files and logs as UTF-8, and `build_game.py` quotes paths with spaces on its Ninja command lines.
- A game file whose data fails its hash checks or ends early stops the setup with `check.input_damaged`, which names
  the file and asks to copy it again (`extract.DamagedFile`). A failed merge checks the base Program NCA alone to tell
  which file is damaged.
- Unsigned exe: SmartScreen warns on first start.

## Later

- Re-warm the driver cache on start after a move, GPU or driver change.
- NSZ and XCZ input.
