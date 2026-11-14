# 4GB VRAM & Laptop Optimization Guide

Tuning and telemetry for running Donkey Kong Bananza PC Recomp on budget discrete GPUs and gaming laptops with 4GB of VRAM (e.g., NVIDIA GeForce RTX 3050 Ti Laptop, GTX 1650/1050 Ti, 4GB desktop GPUs) paired with 6 to 8-core CPUs.

---

## The 4GB VRAM Bottleneck

On systems with 4GB of VRAM, Windows Desktop Window Manager (DWM) and background applications typically consume 400–800 MB of VRAM, leaving only ~3.2 to 3.5 GB of usable video memory for the game.

During initial telemetry on an 11th Gen Intel Core i7-11800H and RTX 3050 Ti Laptop GPU:
* **Initial VRAM usage:** ~2.2 GB (stable 59.7 FPS).
* **VRAM creep over 10–12 minutes:** Without texture eviction, resident textures accumulated past **3.48 GB**, hitting the engine's internal budget ceiling (`2929 of 3041 MiB in use`).
* **Symptom:** Emergency GPU pool releases caused severe 1.6-second freezes (`replay: 1648.2 ms`), progressively degrading gameplay framerate from 60 FPS down to 27 FPS.

---

## Recommended `totk.ini` Configuration for 4GB VRAM

Add or uncomment the following settings in your configuration file (`totk.ini` / `dkb.ini`):

```ini
[frame rate]
fps_limit = 60
realtime = 1
cloth_realtime = 1
highfps_fix = 1

[performance]
# Virtual core preemption: forces guest threads to yield when waiting, preventing thread starvation
async_preempt = 1

# Replay threading & smaller submission batches (eliminates 35-86ms replay queue freezes)
replay_async = 1
replay_threads = 6
split_draws = 16
split_us = 100
timer_1ms = 1

# Pipeline compilation hitch prevention
pipe_skip = fullscreen
lib_prebuild = 1
lib_prebuild_threads = 6
pipe_prebuild = 1
prewarm_xlate = 8

# Memory management & texture eviction for 4GB VRAM
astc = gpu
memory = auto
tex_alias = 1
tex_vram_pressure = 75
vk_subgroup_pin = 1

[display]
# Tear-free low-latency presentation on high-refresh (144Hz+) panels
vk_present = mailbox
window_mode = borderless
fullscreen = 0
assume_focus = 1
perf_overlay = fps
gpu_index = 0

# Native docked raster upscaled with FSR
output_res = 1920x1080
upscaler = fsr
sharpness = 40
frame_gen = off

[diagnostics]
nx2_read_nowait = 1
```

---

## Key Tuning Insights

### 1. Stale Texture Eviction (`tex_vram_pressure = 75`)
* **What it does:** Instructs the Vulkan texture allocator to release out-of-view, cached textures whenever VRAM utilization crosses 75% of total capacity.
* **Impact:** Caps total VRAM utilization around 2.3–2.6 GB, preventing GPU memory thrashing over the PCIe bus and eliminating long-session framerate degradation.

### 2. Draw Batch Slicing (`split_draws = 16`, `split_us = 100`)
* **What it does:** Cuts NVN draw submission slices down from the default (64 draws / 300 µs) to 16 draws / 100 µs.
* **Impact:** Large geometry passes (2,000+ draws) are smoothly distributed across CPU replay worker threads instead of stalling on single-threaded fallback execution (`TOTK_REPLAY_PAR=0`).

### 3. Presentation on 144Hz Displays (`vk_present = mailbox`)
* **What it does:** Uses Vulkan mailbox presentation (triple-buffered fast sync) instead of standard FIFO VSync.
* **Impact:** On 144Hz laptop screens, standard FIFO VSync causes micro-stutter when targeting 60 FPS due to non-integer refresh intervals (144 / 60 = 2.4). Mailbox delivers tear-free, perfectly paced 60 FPS frames.

### 4. Avoiding the Handheld 480p Downscale Bug
* **Finding:** Setting `output_res = 1280x720` forces Bananza into handheld rendering mode, which aggressively drops the internal 3D raster to **854x480 (standard definition)**. Applying AMD FSR with high sharpness to a 480p image results in extreme ringing, crushed contrast, and washed-out colors.
* **Fix:** Keep `output_res = 1920x1080`. The engine will render at its native 1600x900 docked size and scale cleanly to 1080p via FSR.

### 5. External Frame Generation Compatibility
* Setting `assume_focus = 1` and `window_mode = borderless` ensures external capture tools (such as Lossless Scaling or MFG Super Motion) can hook the borderless swapchain and double the stable 60 FPS base to **120 FPS** on 144Hz panels with zero focus-loss stutter.

---

## Hardware Caveats on Laptops
* **AC Power Required:** Running on battery power engages NVIDIA Battery Boost and hardware power caps (typically halving TGP from 75W to 35W and capping CPU power to 15W–25W), making locked 60 FPS impossible regardless of configuration.
* **Thermal Pacing:** Ensure the laptop is elevated or cooling profiles are set to *Performance* or *Turbo* to avoid the 86°C GPU thermal throttling threshold.
