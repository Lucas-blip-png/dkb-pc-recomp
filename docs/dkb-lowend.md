# Running on a weak PC

Measurements from `tools/lowend_bench.py`, which runs `dkb.exe` on the first N logical processors with the perf trace and
the draw profile on, and prints one table. This page is the baseline the low-end work is compared against.

For systems with 4 GB of VRAM and gaming laptops, see [4GB VRAM & Laptop Optimization Guide](4gb-vram-optimization.md).

## Baseline

Bananza 1.0.0, first level, the scripted input of about 2.4 minutes, 70 s per run, RTX 5060 Ti, 16 GB of RAM, before the
descriptor memos of pull request 181. This route is light: it does not show the cost of an open area (a 3070 Ti Laptop
reports 31 to 35 fps there with about 8,300 draws a frame), so treat it as a floor for the CPU limit, not a ceiling.

| Logical processors | Mean fps | Steady fps | Worst window (ms) | Replay avg (ms) | GPU avg (ms) | Commit max (MB) | Working set max (MB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 57.0 | 60.0 | 941 | 3.89 | 1.80 | 14,306 | 5,837 |
| 8 | 55.9 | 60.0 | 938 | 6.36 | 3.02 | 14,133 | 5,704 |
| 4 | 52.7 | 58.9 | 997 | 6.61 | 3.11 | 14,087 | 5,564 |
| 2 | 42.9 | 55.6 | 1,249 | 13.12 | 1.87 | 14,075 | 5,636 |

- Down to 4 logical processors the steady rate holds near 60 fps. At 2 it falls to about 56 fps and the replay thread
  takes 13 ms a frame.
- The worst window is about one second at every count. It falls in the first seconds, where shaders are translated.
- Process commit is 14 GB at every count, so memory, not the CPU, is what a 16 GB machine runs out of first.

## Where the commit goes

`DKB_MEM_REPORT=N` logs the breakdown every N frames and at exit. Around frame 2400 of the first level, of about 14.4 GiB:

- guest heap and stack: 9.2 GiB, of which only about 4.2 GiB is resident in the working set;
- GPU device-local memory and mirrors: 2.6 to 5.1 GiB, growing during the run;
- the exe image: about 0.5 GiB.

`DKB_APP_POOL` below 8 GiB makes the game crash at start (access violation reading 0xc8), so it is not a way to save memory.

What "other private" holds (24 November, 1.0.0 tutorial cave, RTX 5060 Ti on driver 621.01, 1920x1080): at exit
it was 4826 MiB next to 3192 MiB of device-local memory, and it grew with it (2286 next to 1206 at frame 600). A walk of
the process's committed private memory (`VirtualQueryEx`, grouped by allocation) at 110 s found:

- 3293 MiB of write-combined pages (`PAGE_READWRITE | PAGE_WRITECOMBINE`), mostly in 32 MiB allocations. They follow
  the device-local total, so the NVIDIA driver on Windows charges VRAM allocations to the process commit as well. Every
  MiB of VRAM the renderer keeps (mirrors, textures, headroom blocks) costs a MiB of commit on such a machine, and
  `DKB_VRAM_LIMIT` lowers both.
- About 1.3 GiB of plain read/write allocations: one of 526 MiB, a 256 MiB block (the host-cached VMA block of memory
  type 3 the headroom allocates), 18 of 16 MiB, two of 64 MiB and smaller ones. The 526 MiB one is not identified yet.

So on NVIDIA the commit that is not the guest heap is mostly the VRAM in use counted a second time, not a leak of ours.

## Thread knobs on 2 and 4 logical processors

Same route, 60 s runs, one knob changed from the defaults at a time. Repeated runs of the same setting differ by about
1 fps in the mean and 0.5 ms in the replay average.

| Logical processors | Setting | Mean fps | Steady fps | Replay avg (ms) |
| ---: | --- | ---: | ---: | ---: |
| 2 | defaults | 46.5 to 42.9 | 55.6 to 55.7 | 11.8 to 13.1 |
| 2 | `VK_RECORDER=0` | 48.2 to 48.4 | 58.9 to 60.0 | 5.9 to 6.6 |
| 2 | `REPLAY_ASYNC=1` | 45.6 | 56.1 | 12.4 |
| 2 | `SPLIT_DRAWS=0` | 46.4 | 57.4 | 9.2 |
| 2 | `SPLIT_DRAWS=256` | 46.2 | 57.5 | 12.4 |
| 2 | `FREE_CORES=1` | the run stopped at frame 1353 | | |
| 4 | defaults | 52.7 to 53.1 | 58.9 to 59.0 | 6.3 to 6.6 |
| 4 | `VK_RECORDER=0` | 50.3 | 59.0 | 6.3 |

- With 2 logical processors the recorder thread competes with the replay for a core. Turning it off halves the replay
  time and brings the steady rate to 60 fps. With 4 it changes nothing.
- `FREE_CORES=1` with 2 logical processors stopped producing frames after about 25 s. It is off by default and stays off.
- The worst window stays near 1 to 1.7 s whatever the setting: it is the shader translation at the start, not a thread
  setting.

This route is light, so these are not yet reasons to change a default. A low-end profile would turn the recorder off at
2 logical processors once a heavy scene confirms it.

## Heavy scene: layer -101

A player's recording in layer -101 on an RTX 5060 Ti at 1920x1080 with a 60 fps limit, measured on 13 November over
the first minute with a warm shader cache. Each setting ran once or twice; two values are the two runs.

0.3.0 build:

| Setting | fps | 1% low | Frames over 25 ms |
| --- | --- | --- | --- |
| defaults | 50.5 and 49.8 | 21.4 and 23.1 | 346 and 370 |
| `DKB_REPLAY_PAR=1` | 51.3 | 23.4 | 314 |
| `DKB_REPLAY_THREADS=4` | 54.3 | 25.6 | 146 |
| `DKB_REPLAY_THREADS=8` | 53.1 | 23.1 | 198 |

Build 89645fc (warm cache):

| Setting | fps | Frames over 25 ms |
| --- | --- | --- |
| defaults | 50.3 and 51.6 | not recorded |
| `DKB_REPLAY_THREADS=4` | 54.2 and 53.6 | 188 and 181 |

- A cold shader cache cost about 5 fps in the first run.
- A stall of about 0.9 s around frame 1775, almost all of it replay, shows in every run.
- Cause of that stall: the first frame of the new area imports guest memory pools on their first GPU use
  (`DKB_POOL_LAZY`, `vk_memory.c` `lazy_import`). A sampling profile of the frame (`DKB_PROFILE_SLOW`) put about 70%
  of the render thread in `import_pool` (`vkAllocateMemory` of host memory); 673 pools were imported between frames 600
  and 1800, about 960 ms in all, 1.4 ms each, most of them in that frame. The GPU uses only two pool kinds: of 3325
  imported pools at frame 3600, 430 of 1651 `CPU_UNCACHED|GPU_CACHED` and 638 of 1076 `CPU_CACHED|GPU_CACHED` were
  resolved, and almost none of the other kinds.
- Tried and dropped: a background thread that imported every lazy pool older than 2 s. The worst frame fell from
  about 750 ms to 350 ms, but the import time rose from 1.5 s to 5.7 s, held the pool lock the guest's threads wait on,
  and the run got worse overall: 47.1 and 49.1 fps against 52.7 and 53.6, a 1% low of 9.4 and 10.6 against 25.6 and
  27.8, and 29 to 32 frames over 100 ms against 4 to 5.
- Merging neighbouring pools into one import was ruled out by measurement (the census line "imports took", new
  defaults, 13 November). At frame 1800, 717 lazy imports took 431 ms in all, about 0.6 ms each: 301 ms in
  `vkAllocateMemory` and 129 ms committing and touching the pages. The cost follows the size, not the number of calls:
  a single 450 MiB pool took 114 ms of `vkAllocateMemory` in the 485 ms worst frame (1753). 541 of the 711 used pools
  start within 1 MiB of the previous one, but joining small pools would not shrink the large ones. What is left to try
  is importing only the part of a large pool the GPU uses.

Defaults chosen from these runs (13 November, build of bananza with the pre-import dropped, warm cache, two runs each):

| Setting | fps | 1% low | Frames over 25 ms | Worst frame |
| --- | --- | --- | --- | --- |
| old defaults | 52.7 and 53.6 | 25.6 and 27.0 | 177 and 226 | about 760 ms |
| `DKB_REPLAY_THREADS=4` | 57.0 and 57.2 | 28.8 and 29.0 | 63 and 77 | about 775 ms |
| `DKB_HEAP_LAZY=1` | 54.3 and 54.3 | 28.2 and 28.4 | 168 and 193 | about 473 ms |
| new defaults (both) | 57.9 and 57.6 | 29.6 and 29.6 | 62 and 65 | about 456 ms |
| 8 logical processors, old defaults | 49.8 and 50.5 | 23.7 and 26.3 | 371 and 389 | |
| 8 logical processors, `DKB_REPLAY_THREADS=4` | 54.1 and 54.3 | 26.1 and 23.5 | 165 and 185 | |

So `DKB_REPLAY_THREADS` defaults to 4 from 8 logical processors (6 from 12, 3 from 4 since 18 November; on AMD only discrete GPUs on the proprietary driver, the setups measured) and `DKB_HEAP_LAZY`
to 1 on Windows.
- Run-to-run noise is about 2 fps, and 3 in the 1% low.

## Linux: the write-back of GPU results (14 November)

Measured on an RX 7900 XTX with RADV (Mesa 26.2.7) and a 16-thread CPU, standing still in the cave after the tutorial
on 1.0.0 at 1920x1080: 5,230 draws a frame, the GPU busy about 5 ms a frame, and 46 to 47 fps. No thread was busy for a
whole frame (the busiest, the one that replays, 51 % of a core in a 20 s `perf` sample), so the frame was waiting, not
working: the game's threads waited 7.8 ms a frame on fences.

Those fences are held up by the shadow storage's write-back (`vk_memory.c`, "shadow storage"): the frame reports about
4,400 GPU writes into CPU-visible memory, a thousand 15,000-byte storage buffers among them, 15 MiB in all, and the
completion thread copied each back with its own search of the pending list, its own locks and a write-protection
fault per page, 6.7 ms of CPU a frame. `vk_gpu_written_many` does a submission's ranges in one pass.

| Build | fps | Frame | Fence wait of the game's threads |
| --- | --- | --- | --- |
| one call per range | 46.8 and 47.1 | 21.3 ms | 7.8 ms |
| one pass per submission | 56.9 and 57.1 | 17.6 ms | 4.5 ms |
| the 15,000-byte write-backs skipped (a test, not a fix: the game reads some results back) | 59.8 and 59.8 | 16.7 ms | 2.2 to 3.8 ms |

Ruled out in the same scene: `DKB_REPLAY_THREADS` 2, 4 and 8 (46 to 48 fps), the generated code at `-O2` instead of
`-O1` (46.7 and 47.4 against 46.8), `DKB_REPLAY_ASYNC=1` (32 fps: the game falls to two vsyncs a frame) and
`DKB_SHADOW=0` (2 fps on Linux with RADV, as the shadow storage's comment says). What is left on the replaying thread is the
write-watch query before each submit, about 2.5 ms a frame over 530 MiB. Copying a GPU write back only when the CPU
touches its pages would remove the rest of the write-back; the last table row is its upper bound here.

Again on 3.2.0 (16 November, `bananza` dba3a15 without and with this change, the same card and driver): standing still
in the first cave of Ingot Isle at 1920x1080, the game's own upscaler, frame generation off: 49.2 and 49.3 fps before,
59.6 and 59.1 after; the slowest frame 71.5 and 64.5 ms before, 25.7 and 26.3 ms after; the replaying thread busy
20.3 ms a frame before, 14.2 and 16.1 ms after.

`DKB_WB_LAZY=1` (`wb_lazy = 1`, Linux, off by default) does that (`vk_memory.c`, "lazy write-back"): a storage
buffer a shader wrote is not copied back at its fence; its pages are closed, and the first CPU access to one copies
what it is owed and opens it. At the start of Ingot Isle on 3.2.0 (17 November, `bananza` 9ed1784 with it, RX 7900 XTX,
Mesa 26.2.7, the default settings: the game's own upscaler from 1280x720 to 1920x1080, frame generation off, SDR),
four runs each way in turn: render busy 18.42, 17.79, 18.44 and 17.49 ms off, 14.52, 13.40, 12.23 and 13.60 ms on,
lower in every pair; 52.7, 55.1, 51.9 and 56.8 fps off, 55.7, 57.6, 59.1 and 56.7 on, higher in three pairs of four.
About 12,000 pages stay closed and 14 MiB a frame is left uncopied, with 4 to 8 pages touched every 60 frames. In the
crater scene the game already holds 60 fps, and it made no difference there. It stays off by default until more of
the game has been played with it.

## Layer -200 on 6 and 4 logical processors (18 November)

A Ryzen 7 5700X held to its first 6 or 4 logical processors (the process starts with that affinity, so the runtime
counts only those), RTX 5060 Ti, the layer -200 recording on a player's save (3.2.0), 1920x1080, 60 fps limit, warm
caches. The second minute and the end move with where the recording ends, so the first minute compares best.

| Logical processors, `DKB_REPLAY_THREADS` | first minute fps | frames over 50 ms | second minute fps, 1% low |
| --- | --- | --- | --- |
| 16, default (6) | 57.4 | 9 | 60.0, 53.3 |
| 6, 0 (the old default) | 40.1 and 40.3 | 169 and 155 | 42.8 and 48.2, 19.4 and 23.0 |
| 6, 2 | 40.6 | 138 | 42.5, 20.0 |
| 6, 3 (the new default) | 43.7 and 43.2 | 15 and 17 | 49.4 and 50.0, 27.4 and 27.4 |
| 6, 4 | 43.7 and 43.6 | 20 and 25 | 53.8 and 48.3, 28.8 and 28.0 |
| 4, 0 (the old default) | 37.6 | 203 | 36.9, 18.3 |
| 4, 2 | 38.8 | 148 | 36.8, 18.1 |
| 4, 3 (the new default) | 41.0 | 47 | 41.6, 21.2 |

So from 4 logical processors the default is 3 threads. Below 60 fps the scene stays bound by the replay thread.

### Where the replay's time goes on 6 logical processors

Same setup, 3 threads, `DKB_TRACE=perf`. In the heavy part (frames 1740 to 2800, about 24,000 draws a frame) the game
presents every second vsync (33.3 ms) and the replay takes 20 to 26 ms a frame against 11.5 ms on 16 logical processors.

- The workers record about 26,000 draws a frame at 1.5 to 1.7 us a draw, 33 to 43 ms of CPU a frame
  (`slow frame ... slice workers by part`, CPU about equal to the wall time, so they are not preempted). On 16 logical
  processors the cost per draw is the same; five workers share it.
- The walk waits 10 to 15 ms a frame for held slices (`waits for held slices`: 700 to 880 ms per 60 frames, against 150
  on 16). A probe of every wait by its cause and by the op the walk was in put nearly all of them at the take-back of a
  released render target in `vk_texture.c` `entry_of`, reached from the target resolution of `CLEAR_COLOR` and
  `CLEAR_DS` (about 4 a frame, about 3 ms each). Those targets share their storage with other entries and come back
  with a new initialization serial, so the barrier is needed: a held slice's finalized slot at that address would
  follow the taken-back entry instead of the one its pass saw (`slot_resolve`).
- Tried without effect on the first minute (42.8 to 43.3 fps, 21 to 38 frames over 50 ms, inside the run-to-run
  spread): letting the walk record held slices while it waits at those clears (it found none left to take: the
  workers had them all in hand), finer slices (`(expected - drawn) / 8`) so it found some (about 10 a frame),
  `DKB_PAR_DEFER_DEPTH=1`, and `DKB_PAR_DEFER=0`.
- The process uses 3.3 to 4 logical processors of the 6 in that part, and the four busiest threads (the walk, the two
  workers and the game's main thread) about half a processor each. Six logical processors are three cores, so the
  workers and the game's threads share cores through SMT; a fourth replay thread made each draw slower (1.8 to 1.9 us),
  which is why 4 threads matched 3.
- Reaching 60 fps there needs the frame's CPU work (about 80 ms over every thread) to fall by a third, which means a
  cheaper draw on the workers: by `DKB_DRAW_PROFILE` no one section dominates a slice's 1.7 us (bind-sig 250 ns, sets
  225, vertex buffers 230, uniform buffers 210, the other ops 270).
- The level transition (frames 1530 to 1620, about 9 draws a frame) replayed in 11 to 12 ms a frame on 6 logical
  processors against 0.2 ms on 16, under "rest of the walk" with the CPU busy (up to 77 ms of CPU in a 125 ms replay).
  The time was the walk's `rec_drain` at the end of each submission: the recorder had 0.1 ms of work but woke 5 to 30
  ms after its event was set, while the walk spun and yielded. During the load six of the game's threads keep every
  logical processor busy at normal priority, as did the recorder and the replaying thread, so each waited a quantum
  behind them (a sampling profile hid it: the sampler's own thread changed the scheduling, and those frames replayed in
  under 1 ms). The recorder now runs at the highest priority and `submit_record` raises the replaying thread above
  normal for the replay. Transition window replay 0.25 and 0.23 ms a frame (was 11 to 12.4), first minute 43.8 and
  43.5 fps with 15 and 21 frames over 50 ms (was 42.7 to 42.9 fps, 19 to 30); on 16 logical processors 58.5 fps, 6
  over 50 ms. What is left over 50 ms in the first minute is the game's own loading outside the replay and the first
  frame of the heavy part (pool imports).
- A sampling profile of the slices (`DKB_PROFILE`) put the binary search of `vk_usnap.c` `first_above` at about half
  of `bind_resources`' own samples: every uniform binding of every draw asks it. Four cuts of redundant per-draw work
  followed: the search starts from its last answer for the address's 64 KiB block; an index buffer the command buffer
  has bound already is not bound again, and only the changed range of the 17 vertex bindings is; `DKB_DESC_C0_SERIAL`
  and `DKB_DESC_STAGE_ORIGIN`, measured gains left off, are on; and `vk_pipeline_set_dynamic_from` records only the
  dynamic states whose inputs differ from the last recorded ones, across pipeline binds too, since every replay pipeline
  has the same dynamic states. `DKB_DRAW_PROFILE` slices: 1809 ns a draw before, 1559 after (uniform buffers 248 to
  163, state 147 to 106, vertex buffers 146 to 112, other ops 266 to 230). Two runs each at 3F: replay 0.99 and 0.96 us
  a draw in the heavy part before, 0.89 and 0.89 after; first minute 43.8 and 43.6 fps before, 43.5 and 43.6 after
  (the heavy part still presents every second vsync), frames over 50 ms 22 and 23 before, 14 and 16 after; second
  minute 50.8 and 49.1 fps before, 53.5 and 55.1 after.

## Weak GPU (clock locked at 900 MHz)

The RTX 5060 Ti with its graphics clock locked at 900 MHz (`nvidia-smi -lgc 210,900`, by an administrator) stands in
for a weak GPU: about a third of its boost clock. Same layer -200 recording on a player's save (3.2.0), 16 logical
processors, 60 fps limit, warm caches, the game's own upscaler, frame generation off, SDR. `gpu avg` is the perf
trace's GPU time per frame in the heavy part (frames 1800 to 3000).

| Output, `scene_res` | 3D scene | first minute fps | second minute fps | `gpu avg` (ms) |
| --- | --- | --- | --- | --- |
| 1920x1080, auto (default) | 931x524 to 1280x720 | 55.8, 55.8, 55.5 | 58.7, 58.9, 58.6 | 9.3 to 9.7 |
| 1920x1080, full | 1920x1080 | 54.3 | 56.5 | 11.6 to 11.8 |
| 2560x1440, auto | 1241x699 to 1706x960 | 52.6 | 56.8 | 11.7 to 12.0 |
| 2560x1440, full | 2560x1440 | 50.8, 51.1, 50.7 | 50.2, 51.0, 49.9 | 14.8 to 15.3 |

`DKB_PASS_GPU=1` (`docs/dkb-testing.md`) splits the GPU time by pass and op. In the heavy part at 1920x1080 auto,
per frame: the scene passes at the 3D size about 3.6 ms (two 2-target passes 2.2 ms, about seven 3-target passes 1.3
ms), dispatches 3.0 ms in 87 a frame (one compute shader 1.5 ms in 43 dispatches of about 34 us, its count following
the terrain: 6 to 76 a frame), 35 colour clears 0.33 ms, the shadow map 0.2 ms, 17 post-process passes at 1280x720
0.2 ms. At 2560x1440 full the 3-target passes take 3.9 to 5.0 ms and the 2-target ones 2.4 ms: the scene's own
shading grows with its size, and that is where a weak GPU goes.

What the runtime adds is small next to that:

- The mirror prologues (guest memory copied to the GPU before a part) 0.19 ms, uploads 0.02 to 0.07 ms, the window's
  present 0.06 to 0.09 ms. No depth twin was copied in this scene.
- The full barrier before each pass, clear and dispatch (`vk_exec.c` `barrier_to`), as yuzu and Eglantine end every
  render pass with one: what runs between a pass's end and the next mark is about 1 ms at a full-size scene. At a
  scaled scene the figure reads 3.1 ms, but the sum of all keys then runs 2.4 ms over `gpu avg`; held-back parts of
  one submission complete in one callback, so that line counts some time twice.
- `DKB_CS_BARRIER=1` and `game` changed nothing here (2.95 and 2.84 ms of dispatches against 3.03, 50.8 fps at
  2560x1440 full against 50.8): the dispatches follow passes, not other dispatches, so the full barrier before them
  stays.
- Depth-stencil D24S8 is drawn as D32S8 (`vk_formats.c`), on two passes a frame at the full size (0.13 ms at 931x524,
  0.83 ms at 2560x1440). Using D24S8 where the device has it would change depth precision, so it is not a change
  without a rendering difference.

The GPU is not what holds the default setting back: at 1920x1080 auto it is busy 9.4 ms of a 16.7 ms frame, and the
first minute's misses come from the level transition and the first frames of the heavy part. At 2560x1440 full it is
busy 15 ms of a 20 ms frame (50 fps).

Recommended for a weak GPU: `scene_res = auto`, so the game's dynamic resolution draws the scene at two thirds of the
output on each side, with the game's own upscaler (the default; DLSS and FSR were not measured here). `full` costs 2.3
ms a frame of GPU time at 1920x1080 and 3.3 ms at 2560x1440. Each step up in output resolution costs about as much as
`full` at the step below: keep the output at the monitor's size, and lower it before turning `full` on.

## Low-spec options

Four opt-in settings ported from evolis-game's low-spec fork of this runtime (`donkey-kong-bananza-pc-lowspec`, commit
e78174cac4) and a profile that sets them for an integrated GPU. They all default to off. Each trades picture quality for
frame time, so turn them on one at a time and compare.

| Setting | Effect |
| --- | --- |
| `shadow_rate = N` (2 to 60) | Renders each shadow map one frame in N. Shadows lag by up to N frames; the fork ran 8 on an integrated GPU. |
| `cubemap_rate = N` (2 to 60) | Renders the dynamic cubemap faces (reflections) one step in N of 6 frames. The fork ran 6. |
| `smaa_bypass = 1` | Replaces the game's SMAA passes with a copy of their input: no anti-aliasing from the game, three fewer full-screen passes. |
| `tex_uma_host = 1` (or 2) | On an integrated GPU, new textures go to the host heap when video memory is above 74% of its budget (50% for compressed ones), falling back to device memory on failure. Ignored on a discrete GPU. |
| `tex_frame_epoch = 1` | Checks an uploaded texture for guest writes once per frame, not once per submit. A texture the game rewrites mid-frame shows the change a frame late. |
