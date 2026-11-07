# perf_run: controlled dkb.exe runs with an evidence bundle

Scripts: `tools/perf_run.ps1` (launch, sample, bundle) and `tools/perf_evidence.py` (statistics and verdicts).
Output: `build/agent-perf/<yyyyMMdd-HHmmss>-<first|repeat>/`. Nothing here changes game code.

## Run types, one command each

Every command: `pwsh -NoProfile -ExecutionPolicy Bypass -File tools\perf_run.ps1 ...` from the repo root. Each run needs
no other `dkb.exe` running: the script refuses to start while one runs, and it never stops a process it did not start.

| Run type | Command arguments | Harness or measurement |
|---|---|---|
| Dry run (no game) | `-DryRun` | Checks inputs, guard, bundle. |
| Walking, first visit, flight (per frame) | `-Visit first -Script walk -Seconds 180` | Harness test. |
| Walking, repeat visit | `-Visit repeat -From build\agent-perf\<stamp>-first -Script walk -Seconds 180` | Harness test. |
| Idle script (old comparison) | `-Visit first -Script idle -Seconds 180` | Harness test. |
| Verification with thumbnails | `-Visit first -Script walk -Thumbs -Seconds 150` | Not a perf run: thumbnails cost frame time. Use to look at the scene. |
| Diagnostics off (gate 14 method) | `-Visit repeat -From <first bundle> -Measure summary -Script walk -Seconds 600` | Diagnostics-off measurement; see "Measure modes". |
| Realtime build | add `-Exe build\native-port-a6\dkb.exe -Realtime 1` | Timing patch set on; check the log for "real-time timing enabled". |

Summarise any bundle again: `.venv\Scripts\python.exe -I tools\perf_evidence.py build\agent-perf\<bundle> --warm-after 60`.

## Parameters

- `-Visit first|repeat`. `first`: a fresh, empty profile and an empty shader cache. `repeat` (needs `-From`): copies the
  profile and shader cache of a completed first-visit bundle. A bundle is reusable only with `complete: true`.
- `-Exe` (default `build\native-port-playtest\dkb.exe`). The runner copies the exe folder's `dkb.exe`, `*.dll`,
  `*.bin` and `dkb.ini` (or `totk.ini` for a TotK build) into `<bundle>\bin\` and runs the copy. The source folder is not written to.
- `-Script walk|idle`. `idle`: the menu presses that reach the cave scene, then nothing. `walk` (default): the same
  presses, then from tick 3000 a 600-tick cycle of forward walk (5 s), diagonal walk (2 s), camera turn right (1.5 s)
  and walk back (1.5 s), until the run ends. Ticks are 60 Hz from the first present. Stick values are +-32767.
  The walk starts after the menu sequence: at tick 1800 the game was still in an intro caption.
- `-Realtime` (default 1): sets `TOTK_REALTIME`. Builds without the realtime patch set ignore it.
- `-Measure flight|summary`. See below.
- `-Seconds` (wall clock, default 180), `-WarmAfter` (default 60), `-SampleSeconds` (default 5), `-Label`.
- `-Thumbs`: verification only (flight thumbnails, 120-frame ring).

## Measure modes

- **flight** (default): per-frame records from the flight recorder: one dump per 1200 presents, plus one per frame over
  50 ms. `cfg.c` files `TOTK_FLIGHT` under "diagnostics", so these runs are NOT diagnostics-off (gate 14 FAIL).
- **summary**: no flight recorder. Frame stats come from the game's frametime lines (every 60 s and at exit): mean, 1%
  low, counts over 25/50/100 ms, and the five worst frames with their compile waits. Coarser: per-period.

## Guards

- Refuses to start while any `dkb.exe` runs. Never stops a process it did not start.
- The game is closed by `CloseMainWindow()`, the normal exit path (flushes logs). It is killed only after 60 s with no
  exit, and the run is marked `forced_close` and not reusable. In the walking verification runs one exit took over 20 s.
- Inherited `TOTK_*` and `DKB_*` variables are cleared for the run and restored afterwards.
- Profile and shader cache are per bundle. The user's save is never touched.

## Bundle contents

| File | Content |
|---|---|
| `run.json` | Status, script, measure mode, exe source and run-copy paths, SHA-256, copied files, `dkb.ini` text and hash, settings, cleared variables, machine, stop reason, exit code. |
| `samples.csv` | Every 5 s: AC line, GPU temp, power, SM and memory clock, utilisation, VRAM, throttle mask (`nvidia-smi`), process working set, commit, peak, handles, threads. |
| `flight/<dump>/frames.tsv` | Per-frame records (flight mode): interval and render-thread waits (pace, fence, drain, pipe, xlate, tex, astc) and counts. |
| `flight/<dump>/f*.png` | Thumbnails (only with `-Thumbs`). |
| `bin/logs/dkb.log` | The game log (`bin/logs/totk.log` for a TotK build). Written here because the runner runs the copy with stderr on the console (see below). |
| `input-<script>.txt` | The scripted input that was used. |
| `visit.json` | `visit`, `complete`, `stop`, `exit_code`. |
| `evidence.txt` / `evidence.json` | Statistics, anatomy of slow frames, and verdicts. |
| `profile/`, `shader-cache/` | Raw state; a repeat visit reuses them. |

## Log location

`runtime/main.c` `log_open` writes `logs\dkb.log` next to the exe only when stderr is not a file (its comment:
a run whose stderr is not a file "also writes its log" next to the exe; `2> file` keeps its own file). So a run with stderr redirected to a file keeps its log in that file, as designed. An earlier note called
this a bug; that was wrong. The harness does not redirect stderr, and it runs a copy of the exe folder, so the log is
at `<bundle>\bin\logs\dkb.log`. The log text is also available from the stderr file in older bundles.

## Verdicts (gates 10 to 14 of the native-port acceptance plan)

| Gate | Threshold used | Source |
|---|---|---|
| 10 | First- and repeat-visit coverage separate | NOT-MEASURED. One bundle is one visit. |
| 11 | Fixed 1080p | PASS if settings request 1920x1080 and the log reports the window at 1920x1080. |
| 11 | Sustained 60 fps | Warmed mean at least 59.5 fps. |
| 11 | 1% low at least 55 fps | Warmed 1% low (mean of the slowest 1%). |
| 11 | No stall over 50 ms in warmed play | Zero warmed frames over 50 ms. "Reproducible" needs repeated runs. |
| 11 | Loading reported separately | NOT-MEASURED. |
| 12 | No compile freeze of 1 s or more | No frame with 1 s or more of pipeline and translation waits. |
| 14 | Diagnostics off | PASS only in summary mode. |

"Warmed" means frames at least `-WarmAfter` s after the first present. It is a time window, not a scene detector.
NOT-MEASURED means no evidence, not a pass.

## Limits

- A 3-minute run is a harness test; no run so far is acceptance evidence.
- Movement is verified only by thumbnails (`-Thumbs`), by looking at them. The runner does not read player position.
- Flight coverage is below 100%: frames after the last dump are not recorded (no dump at exit).
- The game's own log is the only source of window size and frame limit lines.
- Gate 13 (six-hour soak) is not covered; the commit samples are there for it.
