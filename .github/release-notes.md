DKB Recomp 0.5.4: a static recompilation of *Donkey Kong Bananza* (Switch 2) to a native Windows exe with a Vulkan renderer. It is still experimental (0.x). This is a small release: it makes the game build and start from **RecompLauncher** on Windows, turns the replay threads on for discrete AMD cards under RADV on Linux, and adds diagnostics for the open stall and audio reports.

**The packages hold no game data, and no release ever will.** You build the game on your own PC from your own dump of **1.0.0 or the 3.2.0 update**, given as a `.nup` or `.nx` file or as ExeFS and RomFS folders. Other updates are not supported.

## What is new since 0.5.3

- **RecompLauncher on Windows.** The setup failed its first step after 3 seconds and the game was never built. RecompLauncher started it with `TMP` and `TEMP` naming a folder that does not exist, and clang stopped with "unable to make temporary file". The setup now uses its own temporary folder when the inherited one is missing. A valid `TEMP` is kept. Together with 0.5.3's fix (`DKB-Setup.exe` waits for the build and the game, #547), the launcher's Install, Set up and Play now work end to end. If you set the game up in RecompLauncher with 0.5.2 or 0.5.3, update it there and run the setup again.
- **Replay threads on RADV (#570, #479).** Discrete AMD GPUs on Mesa's RADV driver now get the same default as on AMD's Windows driver: 6 replay threads on 12 or more logical processors, 4 on 6 to 11. Players set it by hand on an RX 6800 XT, an RX 9070 XT and an RX 7900 XTX without device loss, and the 9070 XT went from 30 to 39 fps in the Hilltop Layer. Integrated GPUs, the Steam Deck among them, stay off (`replay_threads` overrides the default).
- **Shadow sync follow-up (#577, thanks @roverdrubber).** A pool is marked for the next shadow sync as soon as its mirror queues pages, which closes the last small window behind the stray triangles of #479.
- **Diagnostics.** A stalled thread's dump names the host function and module it is stuck in (#575). A fence wait of a second or more that runs out is logged. `DKB_AUDIO_DUMP=<file>` records the game's mixed audio before the volume, for crackle reports (#576).
- **Probes (#578, #579, #581, thanks @roverdrubber).** `--probe-vkexec` and `--probe-vkcopy-astc` run to their end again, with and without shadow storage.
- **Chinese.** The 19 installer strings added for amiibo files and the shader precompile are translated.
- **GitHub Sponsors.** The project can now be sponsored on GitHub too, next to Ko-fi.

## Pull requests in 0.5.4

- @roverdrubber: #577, #579, #581

## Issues fixed or answered

#570 (Hilltop performance on RADV, @anon0730, with the logs that settled the default), #578 (probe crash, @roverdrubber), RecompLauncher on Windows (found by testing it here).

## Thanks

roverdrubber for the pull requests; anon0730 and everyone who sent logs, saves and recordings. Thank you.

## Download

| Package | For | Status |
|---|---|---|
| `DKB-Launcher.exe` | Windows 10/11 x64 | The simplest way. Put it in a folder of its own and start it. It downloads the tools zip below, checks its SHA-256, builds the game from your dump and offers each later release with one click (docs/installer.md, "Launcher and updates"). |
| `DKB-Setup-<version>-windows.zip` | Windows 10/11 x64 | Supported. Carries its own tools and the installer window: unpack, run `DKB-Setup.exe`, pick your dump. |
| `dkb-release.txt` | the launcher | The version, file names, sizes and SHA-256 the launcher checks. |
| `DKB-Setup-<version>-linux-x86_64.tar.gz` | Linux x86-64, kernel 6.7 or later | Preview. Source package with `setup.sh`. You install the system packages it lists (`./setup.sh --check`), then it runs the command-line installer. |
| `DKB-Setup-<version>-x86_64.AppImage` | Linux x86-64 (SteamOS, any distribution with glibc 2.31 or later) | Preview. Self-contained: bundles Python, LLVM and a sysroot, so no system packages are needed. Make it executable and run it. |
| `DKB-Setup-<version>-macos-arm64.zip` | macOS 14.4 or later, Apple silicon | Preview. Same idea with `setup.command`. Play is not verified, and the destructible terrain is missing because Metal has no geometry stage. |

## Requirements (Windows)

- NVIDIA GPU with a recent driver (tested on an RTX 5060 Ti and an RTX 3070 Ti Laptop). AMD and Intel are experimental. On AMD's Windows driver the device loss most players hit is fixed (recorder thread off), but RADV on Linux can still hang (#99, #122, #198, #204). Intel Arc: #3.
- 16 GB of RAM plus a large page file (the process commits about 9 GB); 32 GB is comfortable. Cards with 4 GB of video memory should use `preset = 1` or `2` (docs/requirements.md).
- A controller is recommended. Switch 2 controllers on USB work without Steam.

## Known issues

- GPU hangs under RADV on Linux (#99, #122, #198, #204), and a driver fault on Intel Arc (#3).
- Black shards where salt or acid meets slime (#139, #266, #387). The muck cases of #439 are fixed in 0.5.2 and 0.5.3; retest and tell us if you still see them.
- Crashes reported without a reproduction here: #126 (Lagoon to Hilltop tunnel, retest asked) and #148 (Linux, after the first boss).
- Hitches in the Resort and Tempest layers on 4 GB cards may remain in some scenes (#437; try `astc_bc = 1`).
- Another device loss on an RTX 5080 at B1000 (#336).
- On CPUs with 4 to 6 threads, heavy scenes stay around 40 to 45 fps. A first run with an empty pipeline cache still stalls about 3 s on its first heavy frame.
- Handheld mode can show a cropped picture (#549); `operation_mode = docked` avoids it.
- Checkerboard textures (#5), #112 and unrecorded NVN 2 calls (#6).

## Reporting a bug

Open an issue with the log (`dkb.log`), the build line at its top (`dkb 0.5.4+<commit>`), your GPU and driver, and what you did before it failed. **Never share generated files, keys or screenshots that show game content.** Questions and chat: the [Discord server](https://discord.gg/kCkDsqu37y) (#help, #bugs, and #brasil in Portuguese).

## Support the project

Tips go to [ko-fi.com/lucasblippng](https://ko-fi.com/lucasblippng) or [GitHub Sponsors](https://github.com/sponsors/Lucas-blip-png), once or monthly. A tip pays for time on the project; it buys no game files, builds or early access, and everything stays open source. The current goal is US$20 a month for the Claude Pro plan used to work on the project. Starring the repository and sharing it with people who know GPUs, ARM64 or the Switch helps too.

Roadmap: #10.


**Full Changelog**: https://github.com/Lucas-blip-png/dkb-pc-recomp/compare/v0.5.3...v0.5.4
