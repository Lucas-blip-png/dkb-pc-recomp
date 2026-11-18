// What the start prepares before the game's first picture, for the start-up screen (vk_splash.c): the step that runs,
// how many of its items are done and how many it has. The boot (boot.c), the shader prewarm (gfx_shader.c) and the
// pipeline prewarm (vk_pipeline.c) write it; the screen reads it. Any thread, no lock: each field is one atomic word,
// and a reader that sees a step's new number before its new total sees a total of 0 (not counted) for a moment.
#pragma once
#include <stdint.h>

enum {
    BOOT_STEP_NONE,        // nothing reported yet
    BOOT_STEP_DEVICE,      // the Vulkan device and the pipeline cache are made (no count)
    BOOT_STEP_SHADERS,     // gfx_shader_prewarm: every stored shader input is loaded or translated (counted)
    BOOT_STEP_PIPELINES,   // vk_pipeline_prewarm: the pipelines earlier runs recorded are built (counted)
    BOOT_STEP_GAME,        // the guest runs and has not shown a picture yet (no count)
    BOOT_STEP_COUNT
};

typedef struct {
    int step;              // BOOT_STEP_*
    uint64_t done, total;  // items of the step; total 0: the step has no count
    uint64_t work;         // items of the step that needed real work (shaders translated, not found in the cache)
    uint64_t since_ns;     // plat_now_ns() when the step began
} BootProgress;

void boot_progress_step(int step, uint64_t total);   // a new step begins, with its total (0: not counted)
void boot_progress_total(uint64_t total);             // the running step's total, once it is known
void boot_progress_add(uint64_t n);                   // n more items of the running step are done
void boot_progress_work(uint64_t n);                  // n of them needed real work
void boot_progress_read(BootProgress *out);
