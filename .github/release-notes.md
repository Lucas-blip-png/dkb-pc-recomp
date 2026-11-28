DKB Recomp 0.5.3: a static recompilation of *Donkey Kong Bananza* (Switch 2) to a native Windows exe with a Vulkan renderer. It is still experimental (0.x). This release brings amiibo from your own tag files, selectable button prompts and a Player 2 controller for co-op, NVIDIA DLSS Frame Generation, a Chinese menu and installer, fewer shader stalls, gyro under Steam Input, a low-spec profile for integrated GPUs, and a long list of replay speed-ups.

**The packages hold no game data, and no release ever will.** You build the game on your own PC from your own dump of **1.0.0 or the 3.2.0 update**, given as a `.nup` or `.nx` file or as ExeFS and RomFS folders. Other updates are not supported.

## What is new since 0.5.2

- **Amiibo (#567, thanks @roverdrubber, #508).** The game reads your own amiibo tag dumps (`.bin`, or Flipper `.nfc`) from an amiibo folder. You pick the figure in the new F1 > Amiibo tab, and the setup's home screen can copy files in. No amiibo data comes with the project (docs/amiibo.md).
- **Button prompts and co-op controllers (#569, thanks @shadowlink1174, #505, #550).** F1 > Controls has a Button Prompt Style (Nintendo, Xbox, PlayStation or Automatic), separate from the button layout, and a Player 2 Controller row for co-op. A pad chosen for Player 2 is kept for Player 2 while another pad can play Player 1.
- **NVIDIA DLSS Frame Generation (#561, thanks @Zorkats).** `frame_gen_by = dlss` makes the 2x frames with DLSS-G through a D3D12 bridge, on an RTX 40 or newer as NGX decides. The setup fetches NVIDIA's `nvngx_dlssg.dll` after you accept NVIDIA's licence. If anything fails, the game's own frame is shown.
- **Fewer shader stalls (#564, thanks @wbkepler, #560).** The setup and the launcher precompile the pipelines the game is missing before it starts. Shader cache reads are batched, and the boot warm has its own thread count (`DKB_WARM_THREADS`). A cached shader is still loaded where the draw needs it, so nothing is skipped for a frame.
- **Gyro under Steam Input (#551).** When Steam starts the game with Steam Input on, motion is read from the real controller behind Steam's virtual pad. Buttons and sticks still come from Steam, so your remapping holds. Your Steam layout must bind the gyro to something, or Steam switches it off (docs/controls.md). Ported from TotK Recomp (St5sz2Jk's patch). Not yet tested with a real pad under Steam: please report.
- **Chinese (#553, #554, thanks @cirnoice9).** The installer, the launcher, the settings menu, the overlay and the start-up screen are in Simplified Chinese.
- **Performance overlay with frame generation (#566, thanks @MatyMeatBoy).** It shows the frames on screen, with the game's own rate in brackets.
- **Replay and memory speed-ups (thanks @wbkepler).** This covers #529 to #544 and #494. Indirect resets run in compute (on NVIDIA), and hot binds are fused. Host calls and descriptor keys cost less. Command recording takes fewer locks, and the command-copy index is sharded (#539). Uniform snapshots are streamed with AVX2, and only the changed mirror bytes are written back. The FP and SIMD lowering is lighter, and FP compares fold into their branches (#541, #542).
- **Generated code keeps ten guest registers in C locals** instead of six (59.2 against 58.8 fps in the tutorial cave). FCMP of a denormal under flush-to-zero now branches right.
- **Linux shadow storage (thanks @kylianbrun and @roverdrubber).** It is off by default on NVIDIA's proprietary driver, where importing is cheap (#556). `shadow = 0|1` in `dkb.ini` works now. A second shadow sync after a part's mirror copies removes the stray triangles of #479 (#557).
- **Small reads of GPU-written storage (#548, thanks @kylianbrun).** With `nx2_read_nowait` and `wb_ahead`, both opt-in, these reads skip the wait on the sync replay.
- **Long paths on Windows (#526).** Paths of 248 characters or more open in Windows' long form, so a deep RomFS folder no longer reads as missing (the crash talking to the first NPC).
- **Stretched triangles where salt meets the muck (#439).** The shader translator re-ran a ballot inside the leader's branch of the muck mesh compute, so the subgroups' vertices overlapped (stretched triangles and NaN normals). Ballots, votes and shuffles now run once, where the guest runs them. **Shaders are translated once more on the first start after updating.**
- **Ultrawide (#535, thanks @Mega-Man-X7).** The Bananza badge inside the Bananergy ring and the transformation carousel are no longer clipped at 21:9.
- **RecompLauncher (#547).** `DKB-Setup.exe` now waits for the setup and the game and passes the progress lines through. The manifest uses the current `inputs` format.
- **Xbox prompts note.** The Controls menu no longer tells Bananza players to run the setup again for Xbox prompts.

## Low-spec profile (opt-in)

Ported from evolis-game's low-spec fork of this runtime ([donkey-kong-bananza-pc-lowspec](https://github.com/evolis-game/donkey-kong-bananza-pc-lowspec), GPL-3.0-or-later, credited in THIRD_PARTY_NOTICES.md). Every setting is off by default, so nothing changes unless you turn it on. Details are in docs/dkb-lowend.md.

- `preset = lowspec` in `dkb.ini` (or `DKB_PRESET=lowspec`) applies the fork's integrated GPU profile: `astc = gpu`, `astc_bc = 1`, `tex_uma_host = 1`, `tex_vram_pressure = 72`, `vram_headroom = 0`, `shadow_rate = 8`, `cubemap_rate = 6` and `replay_threads = 2`. A setting you write yourself still wins.
- `shadow_rate = N` renders each shadow map one frame in N, so shadows can lag by up to N frames.
- `cubemap_rate = N` renders the dynamic reflection cubemaps one step in N.
- `smaa_bypass = 1` replaces the game's SMAA passes with a copy of their input, removing three full-screen passes but also the game's own anti-aliasing.
- `tex_uma_host = 1`: on an integrated GPU near its memory budget, new textures go to the host heap.
- `tex_frame_epoch = 1` checks uploaded textures for game writes once per frame, not once per submit.
- `output_res` now goes down to 640x360, and `scene_res` up to 4.0.
- `mvk_upscale_blit = 1` also works outside macOS.

On the RTX 5060 Ti used for testing, `preset = lowspec` held 58.1 fps in the tutorial cave with no validation errors. It is meant for integrated GPUs and has not been measured on one yet, so please share numbers and logs in #498.

## Pull requests in 0.5.3

Merged since 0.5.2, by author:

- @wbkepler: #494, #529, #530, #531, #532, #534, #536, #537, #538, #539, #540, #541, #542, #543, #544, #564
- @roverdrubber: #519, #520, #521, #522, #557, #567
- @kylianbrun: #548, #556
- @cirnoice9: #553, #554
- @Zorkats: #561
- @MatyMeatBoy: #566
- @shadowlink1174: #569

## Issues fixed or answered

#508 (amiibo) and #505 (Xbox prompts, both @SercanDmr), #550 (PlayStation prompts, @jesusen904), #551 (gyro under Steam Input, @gubbywumpus), #560 (shader stutter, @MaLDox77), #526 (crash talking to the first NPC, @tmess), #535 (ultrawide badge, @Mega-Man-X7), #547 (RecompLauncher, @conceitosemprendimentos-lab), #439 (stretched triangles in the muck, @Dimiduh).

## Thanks

Pull requests in 0.5.3: wbkepler, roverdrubber, kylianbrun, cirnoice9, Zorkats, MatyMeatBoy, shadowlink1174. Low-spec work: evolis-game. Issues, logs, saves and recordings: everyone named above. Thank you.

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

Open an issue with the log (`dkb.log`), the build line at its top (`dkb 0.5.3+<commit>`), your GPU and driver, and what you did before it failed. **Never share generated files, keys or screenshots that show game content.** Questions and chat: the [Discord server](https://discord.gg/kCkDsqu37y) (#help, #bugs, and #brasil in Portuguese).

## Support the project

Tips go to [ko-fi.com/lucasblippng](https://ko-fi.com/lucasblippng). A tip pays for time on the project; it buys no game files, builds or early access, and everything stays open source. The current goal is US$20 a month for the Claude Pro plan used to work on the project, and its progress bar is on the Ko-fi page. Starring the repository and sharing it with people who know GPUs, ARM64 or the Switch helps too.

Roadmap: #10.


**Full Changelog**: https://github.com/Lucas-blip-png/dkb-pc-recomp/compare/v0.5.2...v0.5.3
