# Frame generation (experimental)

Off by default. Code: `runtime/vk_framegen.c`, pacing arithmetic in `runtime/fg_pace.h`, shaders in `runtime/shaders/fg_*.comp`; AMD FSR 3.1's frame interpolation as the other way to make the frames at 2x: `runtime/vk_fsr3fi.c` (see "AMD FSR 3.1 frame interpolation" below).

## What it does

Auto, or a fixed 2x to 8x. For each pair of game frames the present thread makes the frames between them:

1. The cropped window texture is copied into an RGBA8 history image and reduced to quarter-size luma.
2. Block matching of that luma against the previous frame's gives one motion vector per 16x16 pixels (within +-32 pixels), followed by a 3x3 median.
3. A made frame at time t between the two blends the previous frame moved forward and the new one moved back along the vectors.

The interval is an exponential average of the gaps between game frames. Every shown frame goes through the normal present chain (colour, FSR, SMAA, HDR, overlay).

### 2x

One made frame (t = 0.5) for each pair of game frames, shown first, and the game's own frame half an interval later. The present thread shows both itself (`vk_fg_show`), as it always did. When the display shows fewer than 2 frames per game frame the made frame is skipped (`over refresh` in the log).

### A fixed multiplier (`3x` to `8x`)

Each game frame gets as many frames as the display can show in the game's frame time, the multiplier at most (`fg_pace_fixed_frames`): 60 fps on a 165 Hz display gives 2 at any multiplier, 30 fps gives up to 5. The made frames are at t = k / n and are shown first, the game's own frame last, each interval / n after the one before. When the display shows only one frame per game frame, none is made (`over refresh` in the log). A log line says when fewer frames are made than asked. These and Auto are shown by a display thread (below).

### Auto

Each refresh of the display between two game frames gets a frame made at its place between them (`fg_pace_auto_slots`); a refresh within a quarter period of a game frame shows that frame itself. The frames are shown one game frame time later, each at its refresh's time, so Auto adds one game frame of delay. A game frame may get up to the refresh over 30 frames (2 to 4). When the game runs faster than the display, a game frame that no refresh falls on is not shown. An unknown refresh counts as 60 Hz.

### Two threads, as in Hookshot (Auto and 3x to 8x)

The design is Hookshot's (`gpu/vk/vk_framegen.cpp`: `framegen_record`, `display_main`). The present thread (`vk_backend.c` `present_main`) calls `vk_fg_record` in place of its blit: it makes the game frame's set (the made frames and a copy of the game's own, since the history image is written again two frames on) into one of three sets, submits it and queues it, and is free for the next game frame at once. It waits only when all three sets are still being shown, which holds the game back as Hookshot's sets do (`waited for a set` in its 5 s line). Each set has images for the most frames the mode may make (Auto's cap, or the multiplier's), at the window texture's crop size in RGBA8, made when the mode or the size changes: 8x keeps 24 of them, about 800 MB at 3840x2160, even when the display lets fewer be made.

The display thread (`display_main`) takes the sets in order and presents each frame through the whole present chain on an even line (`fg_pace_show_due`): the game's frame time divided by the set's frames apart, never more than one game frame behind what the game handed over, and from now again when it fell behind; Auto's frames go out at their own refreshes' times. A made frame is left out only when the next set is already waiting. A frame the display thread does not get (frame generation turned off, a skip) is presented by the present thread once every set before it was shown.

Not ported: Hookshot's "present ahead", which submits a set's copies into the window together and keeps only the presents on the line, so a copy does not wait behind the game's next frame on the GPU. Here each frame's copy is submitted at its time, behind whatever the game queued before it. It needs `vk_present.c` to hold several window images with their presents pending, where it holds one now.

### Measured

RX 7900 XTX (Mesa 26.2.7), 165 Hz display, the first cave of Ingot Isle at about 46 game frames a second, the game's own
upscaler, SDR, mailbox, 2026-11-16 (`bananza` b99dd34 with Auto and the multipliers); each run 90 s, the last 5 s lines:

| Setting | Game fps | Made a second | Frames shown per 5 s | Shown apart |
| --- | --- | --- | --- | --- |
| off | 46.4 | | | |
| 2x | 45.8 | 47 | 472 to 475 (94.5 to 95.0 a second) | 0 to 1 late over 2 ms |
| Auto | 46.5 | 142 | 819 on time, 5 made ones left out | 777 under 7 ms, 38 at 7 to 9 ms; 0 late |
| 4x (3x made: 165 Hz over 44 fps fits 3) | 47.2 | 97 | 732 on time | 679 under 7 ms, 53 at 7 to 9 ms; 0 late |

Hookshot's own log on the same machine at Auto: 806 to 827 frames per 5 s, 714 to 756 of them under 7 ms. The `fg: frame interpolation:` line counts frames presented by the display thread; when they reached the screen was not measured.

## Turning it on

The Display tab of the settings menu (F1) has the Frame generation row (a note under it says that VSync makes no frames) and, at 2x, a Made by row for `frame_gen_by`. Or set `frame_gen` in `dkb.ini` (or `DKB_FRAME_GEN` in the environment) to `auto` or `2x` to `8x`. It can be changed while running (turning it off frees the images; another multiplier rebuilds them and starts the history again).

## AMD FSR 3.1 frame interpolation (`frame_gen_by = fsr3`, experimental)

`frame_gen_by` (`DKB_FRAME_GEN_BY`, live) chooses what makes the frame at 2x: `builtin` (the default) is the interpolation above, which sees the picture only; `fsr3` is AMD's FSR 3.1 frame interpolation, which also uses the scene's depth and motion and an optical flow over the two pictures. The passes are AMD's own (FidelityFX SDK v1.1.4, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`: ten frame interpolation passes, seven optical flow passes) compiled from `runtime/third_party/fidelityfx` by `tools/build_shaders.py` (`runtime/fsr3_fi_*_spv.h`, `runtime/fsr3_of_*_spv.h`) and recorded by `runtime/vk_fsr3fi.c` as compute dispatches; no AMD library is loaded.

The depth and motion are the temporal upscaler's: `vk_dlss.c` `vk_temporal_inputs` makes them for DLSS, FSR 3.1 and FSR 4, and `vk_temporal_done` hands them to `vk_fsr3fi_note`. So AMD's frame is made only when:

- `frame_gen = 2x` (FSR 3.1's interpolation makes the midpoint only; Auto and 3x to 8x always use the built-in one);
- a temporal upscaler drew this frame's 3D (`upscaler = dlss`, `fsr3` or `fsr4`, with the 3D below the output size, so that a scene was noted at the input size), and the projection is a perspective one;
- the GPU has push descriptors, storage images without a format and the subgroup arithmetic and quad operations the passes need (the log says what is missing);
- the history is there: the first frame after a gap, a resize or a camera cut is made by the built-in interpolation, as is any frame the passes refuse (a picture whose shape is not the 3D's).
- the history is an SDR picture: AMD's passes read 8-bit colour, so with the game's own HDR picture (`hdr_native`, a PQ history) the built-in interpolation makes the frame, which works on the PQ code values (`vk_framegen.c` `record`);

Otherwise the built-in frame is made, with no other change to frame generation.

### The HUD

The game draws its HUD into the same picture the upscaler wrote, so AMD's interpolation, given only the picture as shown, moves the HUD with the scene: smeared hearts and counters, garbled text. The SDK takes the frames without their HUD as well (`HUDLessColor`, `HUDLessAttachedFactor`), for this frame and the one before: the flow and the interpolation then work on the scene alone, and the inpainting pass puts back every pixel where the two pictures differ, the HUD, as it is shown (`ffx_frameinterpolation_inpainting.h` `computeInpainting`). The idea of finding such a picture in a game that gives none is OptiScaler's HUDfix (<https://github.com/optiscaler/OptiScaler>, `hudfix/Hudfix_Vk.cpp`); no code of it is used.

Bananza draws the upscale into a 3D-sized R11G11B10F picture, then draws the HUD into that same image with blended quads, and one last draw reads it with bilinear filtering and writes the window texture, whose sRGB format encodes it (no tone mapping between them). So:

1. `vk_fsr3fi_note_picture` (called by `vk_fsr3.c`, `vk_fsr4.c` and `vk_dlss.c` right after the upscaler wrote the output) copies the output into a ring of three images before the HUD is drawn, and notes the copy with the frame's depth and motion.
2. `shaders/fg_hudless.comp` (run by `vk_framegen.c` before AMD's passes) does the last draw's work on the copy: bilinear to the window texture's size, sRGB encoded. Where that is within 3 levels of the frame as shown, it keeps the shown pixel exactly, so AMD's test sees no HUD there; elsewhere, the HUD's pixels, it gives the scene behind.
3. The pictures of this frame and the one before go to `vk_fsr3fi_generate` as its sources, and the frame as shown is the inpainting pass's backbuffer. A frame without them (the first one after a change, a picture of another shape than the window texture, a crop of part of it, a frame whose upscaler did not note a picture) is interpolated from the frames as shown, with the HUD moved as before.

The log says `fsr3fi: the upscaled picture is copied before the game's HUD` when the copies are made, and the 2x `fg:` line counts the made frames that used the frames without their HUD. With FSR 4 (a Windows build, `vk_fsr4.c`) and DLSS (`vk_dlss.c`) the copy is taken the same way, from the same output image; those two were compiled but not run on the machine this was written on.

Log lines: `fsr3fi: AMD FSR 3.1 frame interpolation can run on this GPU` and `fsr3fi: 17 pass pipelines ready` once the setting is chosen on a capable device, `fsr3fi: making the 2x frames at WxH from RWxRH depth and motion` at the first frame it makes, and `by AMD FSR 3.1 N, by DLSS Frame Generation D; K of them with the frames without their HUD` in the 2x `fg:` line (below), which counts the made frames AMD's passes made and those of them that had the frames without their HUD. `fg: frame_gen_by = fsr3 or dlss needs the depth and motion of a temporal upscaler ...` says once that a frame had none.

## NVIDIA DLSS Frame Generation through D3D12 (`frame_gen_by = dlss`, experimental)

NGX offers DLSS Frame Generation (DLSS-G) from the RTX 40 series on (NGX decides). This route makes the frames through NGX's D3D12 side, on a D3D12 device of its own on the same GPU, from images it shares with the Vulkan renderer. No NVIDIA file is shipped. NVIDIA's `nvngx_dlssg.dll`, which NGX looks for beside `dkb.exe` as it does for `nvngx_dlss.dll`, comes with the setup's NVIDIA DLSS download ([dkb-dlss.md](dkb-dlss.md)).

`frame_gen_by = dlss` (Windows only, the menu's Made by row reads NVIDIA DLSS) makes the 2x frame this way. It takes the same inputs as AMD's (the temporal upscaler's depth and motion, the camera, the picture before the HUD), so it runs only under the same conditions as AMD's above, and a frame it cannot make gets the built-in one. `runtime/vk_dlssg.cpp` does it in these steps:

- At device creation, on an NVIDIA GPU, `vk_device.c` enables `VK_KHR_external_memory_win32` and `VK_KHR_external_semaphore_win32` where the device has them (`vk_dlssg_device_exts`).
- At the first frame with the setting chosen, it makes a D3D12 device on the Vulkan device's adapter (found by its LUID), a queue, three command lists, and the images both APIs share: the picture, the picture before the HUD, the depth (R32_SFLOAT, written from the scene's depth by `runtime/shaders/dlssg_depth.comp`, since a depth image cannot be shared here), the motion and the made frame. One timeline semaphore, the imported D3D12 fence, orders the two queues; a second one, Vulkan-only, orders the copy-back ring.
- Each 2x frame, `vk_framegen.c` records the copies into the shared images in its own command buffer, and `vk_dlssg_record` sets NGX's parameters: the camera (projection in NGX's 0 to 1 depth, view, the clip-to-previous-clip matrix), the jitter, the motion scale and the reset flag. `vk_dlssg_submit` then submits that command buffer with a signal of the shared fence, the D3D12 queue waits for it, evaluates the feature and signals again, and a second Vulkan submission waits for that and copies the made frame into the frame generation pass's own image.
- Any failure (an image or fence that cannot be shared, NGX refusing the feature, a queue that does not finish within 5 s) turns the route off for the run, logged once as `dlssg: <reason>; DLSS Frame Generation is off for this run, the built-in interpolation makes the frames`.

Log lines: `dlssg: a D3D12 device on <GPU> (LUID H:L) makes the frames: NGX's D3D12 frame generation is available`, `dlssg: frame generation feature made for WxH from a RWxRH scene`, `dlssg: the first frame made by DLSS Frame Generation (...)`, every 3000 made frames how many had the picture before the HUD and how many reset the history, and `by DLSS Frame Generation N` in the 2x `fg:` line.

## Requirements

Without these, no made frames are produced (one log line says why) and presentation is unchanged:

- Bananza only.
- Present mode `mailbox` or `immediate` (`DKB_VK_PRESENT`). With `fifo` the several presents per game frame would block the present thread.
- Window texture format 8-bit UNORM (RGBA8 or BGRA8) with blit-source support, or the game's own HDR picture (A2B10G10R10 with `hdr_native = 1`; see HDR below) on a GPU that can store to, filter and blit that format.

If set-up fails (missing push descriptors, format support, pipeline or image creation, fence timeout, submit failure), the pass turns itself off for the session.

## HDR

Both HDR modes get made frames, in every mode of `frame_gen` (2x, Auto and 3x to 8x; `vk_framegen.c`, header comment):

- `hdr = 1` alone (the SDR picture lifted to HDR10, `vk_hdr.c`): the window texture is the game's SDR picture, so the frames are made as in SDR, and every frame shown, made or the game's own, goes through the HDR pass.
- `hdr_native = 1` (the game's own HDR picture, 3.2.0): the window texture holds Rec. 2020 PQ code values in A2B10G10R10. The history, the made-frame image and each set's frame images (Auto and 3x to 8x, including the copy of the game's own frame) are then A2B10G10R10 as well, so the picture keeps its 10 bits, and each frame is shown as the game's own HDR picture (`vk_present.c`, native HDR), by the present thread at 2x and by the display thread at Auto and 3x to 8x, which presents each set with the format the set was made in. The blend works on the PQ code values, which is as sound as blending sRGB's: both are perceptual codings. The comparisons in `fg_luma.comp` and `fg_synth.comp` (block matching's cost, what counts as moved, whether the two moved frames agree) first take each value to the SDR picture's scale, the sRGB code of its light over paper white (`hdr_game_nits`, `vk_hdr_game_nits`), so that their thresholds mean what they do in SDR. The shaders get this through their `pq` and `white` push constants.

Where the GPU cannot use A2B10G10R10 this way (storage image, linear filtering, blit source and destination), no frame is made from the game's own HDR picture and the log says so once.

To see it work, run with `hdr = 1`, `hdr_native = 1` and `frame_gen = 2x`, `auto` or `4x` on an HDR10 display. The log has `hdr: HDR10 swapchain` and then the set-up line (`frame generation 2x on`, `Auto on` or `4x on`, the last two with the number of sets) with `history RGB10A2 (the game's own HDR picture, PQ)` once the game's HDR picture starts (the line says `RGBA8` for an SDR picture, and appears again when the format changes, such as when the game switches between them). The 5 s `fg:` lines then show `made` frames and `shown` near the multiplier times `real` (at Auto and 3x to 8x the `frame interpolation` line shows the frames shown on time), as in SDR; the line `fg: native HDR picture: no made frames` no longer appears.

## When a made frame is skipped

- No history: the first frame, after a hitch, a crop change, or after a skip for the reasons above.
- Hitch: more than 100 ms since the previous game frame (loading, pause). The history starts again.
- Crop change: the window texture crop size changed; images are rebuilt and the history starts again.
- Over refresh: at 2x, 2 / interval exceeds the monitor refresh by more than 5% (60 Hz is assumed if the refresh is unknown); at a fixed multiplier, the display shows one frame per game frame. Auto and 3x to 8x make as many frames as the display can show, so they make none only when it shows one a game frame.
- Left out (Auto and 3x to 8x): the next set was already waiting when a made frame's turn came (the display thread is a set behind).

A skipped frame is shown as the game's frame alone.

## Log lines

At 2x, every 5 seconds:

`fg: 5.0 s: real R fps, shown S fps, made N (by AMD FSR 3.1 M, K of them with the frames without their HUD); skipped: no history A, hitch B, over refresh C, crop D; real cut short E; late >2 ms F`

- real: game frames per second.
- shown: real plus made frames per second.
- made: made frames in the period; by AMD FSR 3.1 and by DLSS Frame Generation: those AMD's passes (`frame_gen_by = fsr3`) and NVIDIA's feature (`frame_gen_by = dlss`) made, the rest the built-in interpolation; K: those of M that had the frames without their HUD.
- skipped: made frames not produced, by reason (see above).
- real cut short: the game's frame was flipped before its half-interval wait ended because a newer frame was already waiting.
- late >2 ms: the game's frame flipped more than 2 ms after its due time.

With Auto and 3x to 8x, from the present thread:

`fg: 5.0 s: real R fps, made N (M a second); game frames without made ones: no history A, hitch B, over refresh C, crop D; waited for a set W`

and from the display thread, worded as Hookshot's line so the two logs compare directly:

`fg: frame interpolation: N frames shown on time, L late (over 2 ms), S made ones left out (the game's next frame waiting); shown apart: <7 ms a, 7-9 b, 9-12 c, 12-17 d, 17-25 e, over 25 f`

- waited for a set: game frames whose set had to wait for the display thread to finish showing an older one (the present thread, and the game behind it, wait then).
- on time and late: presented within 2 ms of the frame's due time, or later. After a held-up present the line's due time can be past, and the set's frames then go out at once and count as late, as in Hookshot.
- left out: made frames not shown because the next set was already waiting.
- shown apart: the time between two presents, counted by range.

The set-up line says the mode: `fg: frame generation Auto on (experimental): a frame for each of the display's refreshes (165 Hz; up to 4 a game frame), made at WxH, history RGBA8, 3 sets`, or `fg: frame generation 4x on ...`. A fixed multiplier the display cannot fill logs `fg: 4x asked, 3x made: the display's 165 Hz over the game's 45 fps (the game keeps its rate)`.

## Known limits

- The overlay is drawn last on every shown frame and is never interpolated. The game's own UI is part of the game picture the made frame is built from.
- Input latency: at 2x it is unchanged (the made frame is not newer than the real one); a fixed multiplier above 2x shows the game's frame up to (n - 1) / n of a frame time later; Auto one frame time later.
- Extra GPU cost: a blit, luma, block matching and median pass per game frame, and a full-size synthesis pass per made frame.
- Experimental: artifacts around fast or occluded motion are expected.

## AMD Anti-Lag 2

`antilag = 1` (`DKB_ANTILAG=1`; Display > Frame rate > AMD Anti-Lag 2, off by default, experimental), independent of frame generation. Code: `runtime/vk_antilag.c`. The row is shown only where the device has `VK_AMD_anti_lag` with its `antiLag` feature. The extension is enabled whenever it is there, so the setting applies at once.

On Linux, RADV has no `VK_AMD_anti_lag` of its own: Mesa ships it as the implicit layer `VK_LAYER_MESA_anti_lag`, which the Vulkan loader loads only with `ENABLE_LAYER_MESA_ANTI_LAG=1`. `vk_device.c` sets that variable before the instance when `antilag = 1` at start, unless the player set it either way (`DISABLE_LAYER_MESA_ANTI_LAG=1` still keeps the layer out). With `antilag = 0` at start the layer is not loaded, so on RADV turning Anti-Lag 2 on takes a restart, and its menu row appears only after one.

Each game frame gives the driver two stages with the same index through `vkAntiLagUpdateAMD`: the input stage right after the game presented the frame before (`vk_backend.c` `vk_be_present`), where the driver may wait so the frame starts later, and the present stage right before that frame's present on the present thread (`present_main`). Each present job carries its frame's index. With frame generation only the game's own frame gives a present stage and the made frames give none, so the driver sees one present stage per game frame at every mode: at 2x `vk_fg_show` gives it right before the game frame's flip, and at Auto and 3x to 8x the set keeps the game frame's index from when it was recorded (`vk_fg_record`) and the display thread (`display_main`) gives it right before it presents the set's own game frame, under the present lock and only when the set is still live (the dead flag and the generation counter), as for the present itself. A set that is dropped or released gives no present stage. The game frame comes last in its set, so the stage lands after the made frames of that set have gone out. The log has `vk: AMD Anti-Lag 2: available (VK_AMD_anti_lag)`, `on` and `off` lines. The input stage is given on the thread that presents, which under asynchronous replay is up to a frame behind the game's simulation.

## Attribution

The shaders and the approach come from Hookshot (<https://github.com/NotRexed/Hookshot>, GPL-3.0); Auto, the multipliers above 2x, the sets and the display thread from <https://git.notrexed.dev/Hookshot/Hookshot>. AMD FSR 3.1 frame interpolation: the passes are AMD's FidelityFX SDK (MIT), the dispatches follow Hookshot's `vk_fsr3fi.cpp` (<https://git.notrexed.dev/Hookshot/Hookshot>). See `THIRD_PARTY_NOTICES.md`.
