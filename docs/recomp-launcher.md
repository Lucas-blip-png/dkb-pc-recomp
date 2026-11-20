# RecompLauncher

RecompLauncher is a third-party launcher (elripalda/RecompLauncher) that lists recompiled games, downloads their
releases and runs them. It lists DKB Recomp since [#213](https://github.com/Lucas-blip-png/dkb-pc-recomp/pull/213).
Every package uses the profile in `tools/release/recomplauncher.json`. Players can set HDR output, the performance
overlay and the direct mouse camera in the launcher. These settings use the installed game's `game/dkb.ini`.
The Windows installer is separate ([installer.md](installer.md)), and so is
the project's own Windows launcher, `DKB-Launcher.exe`, which installs and updates the tools from GitHub releases
([installer.md](installer.md), "Launcher and updates").

## How it sets the game up

The package carries `recomplauncher.json` (source: `tools/release/recomplauncher.json`). The launcher reads it and:

1. Asks for your own copy of the game: a `.nup` file, or a folder with `exefs/` and `romfs/`. Base 1.0.0 and update
   3.2.0 are both accepted.
2. Runs the prepare step with `--prepare --source <copy> --dest <package folder>/game` using `DKB-Setup.exe` on
   Windows, `setup.sh` on Linux or `setup.command` on macOS. The profile allows 4 hours for the build.
3. Starts the installed game. The Windows and bundled Linux installers take `--play <package folder>/game`.
   The Linux and macOS source packages start `play.sh`.
4. Backs up `game/profile` (the saves).

`--source PATH` stands for `--nup` (a file) or `--exefs` and `--romfs` (a folder). `--prepare` prints
`progress <0..1> <text>` lines for the launcher instead of JSON events. Running the step again resumes or updates the
recorded install. More in [installer.md](installer.md), "Bananza Linux and macOS packages".

If a system package is missing, the step stops with exit code 1 before Python starts. Run `./setup.sh --check` in a
terminal inside the package folder: it names what is missing and the command that installs it.

## Where the output and logs are

- The built game is in `game/` inside the package folder. Saves are in `game/profile`.
- The game log: one player found it at
  `~/.local/share/com.recomplauncher.app/game-output/dkb-pc-recomp.log` on Linux
  ([#218](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/218)). The launcher overwrites it at each start, so
  copy it before you start the game again.
- The setup logs: the installer writes one file per step under `logs/` in its temp folder, which is
  `build/install-temp` in the package folder.

## For launcher authors

What another frontend can rely on, on every platform (#402):

- **Setup.** `python -m installer` (`setup.sh` on Linux, `setup.command` on macOS, `DKB-Setup.exe` on Windows and the
  AppImage on Linux, which open the setup window but hand the command line to the setup when it holds `--dest`,
  `--source`, `--prepare` or another setup-only option) takes `--source PATH` or `--nup`/`--exefs`/`--romfs`, and `--dest DIR`. With `--prepare` it
  prints `progress <0..1> <text>` lines; without, JSON events. Exit codes: 0 done, 1 failed, 2 bad command line,
  3 cancelled. Running it again on the same `--dest` resumes or updates the install. `--dest DIR --clean` deletes the
  kept build files (generated C, objects, logs) of that install and prints the freed size. `--dest DIR --uninstall` removes the install and its build files but keeps `profile/`, `dkb.ini` and `mods/` (`--delete-saves` removes those too). `--help` lists every option.
- **Play.** `DKB-Setup.exe --play [DIR]` (or the AppImage) starts the finished game in `DIR`, by default the install
  folder the launcher remembers, with no window, waits for it and returns its exit code; 1 when there is no finished
  install there.
- **Game.** Start `dkb` (`dkb.exe` on Windows) from its own folder. `--set KEY=VALUE`, repeatable, gives a `dkb.ini`
  key (or a full `TOTK_`/`DKB_` variable name) a value for that run only, without changing `dkb.ini`; an unknown key
  exits with code 2. `--version` prints the port version and the game update; `--help` lists the options.
- **Portable.** Saves (`profile/`), `dkb.ini`, logs (`logs/dkb.log`) and shader caches live beside the executable,
  so the install folder can be moved, copied or deleted as a whole ([installer.md](installer.md), the install folder
  layout).

## Reporting a bug

Use the issue form (crash, graphics or bug). Add:

- the release (the `VERSION` file in the package folder) and the game version, 1.0.0 or 3.2.0;
- GPU, driver and operating system, and the RecompLauncher version;
- what you did just before it stopped;
- the log file above, as an attachment. For a failed setup, send the last lines the launcher showed and the step logs.

If the game crashed, the log names the faulting function. Never attach game files, keys, builds or the shader cache.
See [dkb-testing.md](dkb-testing.md) for the rest of the checklist.
