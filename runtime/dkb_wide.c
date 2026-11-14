// Bananza's virtual display and camera defaults are game state, separate from
// Vulkan's presentation rectangle. The generated dkbwide sites read this page.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

static unsigned wide_output[2] = {2880, 1620};
static int wide_enabled;

// The game's render-target pool is a fixed 200 MiB sized for 2880x1620 (main+0x458adc in
// f_7000458a88). Grow it with the output pixel count, rounded up to 2 MiB, never below the
// default. The highest accepted output (7680x4320) needs 1424 MiB (about 1.39 GiB), which fits uint32_t.
static uint32_t dkb_wide_pool_bytes(unsigned w, unsigned h) {
    const uint64_t base = 0xc800000, align = 2u << 20;
    uint64_t bytes = (base * w * h + 2880ull * 1620 - 1) / (2880ull * 1620);
    bytes = (bytes + align - 1) / align * align;
    return (uint32_t)(bytes > base ? bytes : base);
}

int dkb_wide_output(unsigned out[2]) {
    if (out) memcpy(out, wide_output, sizeof wide_output);
    return wide_enabled;
}

// TOTK_SCENE_RES: "auto" leaves the game's dynamic resolution alone. "full" (ratio 1.0) or a ratio from 1.0 to 3.0
// draws the 3D scene at the virtual display divided by that ratio: the patched store in the controller at main
// 3c12b4 (patches-dkb100.txt, set dkbscene) loads the ratio from page +0x118. Under emulation the game's own
// control settles at 1.5, so the pool the game sizes for that 3D size (page +0x110) grows by (1.5 / ratio)^2.
// Returns the ratio in effect, 0 for auto. Needs the dkbwide pool word, so it runs after that set is configured.
static float dkb_scene_ratio(void) {
    const char *value = cfg_value("TOTK_SCENE_RES");
    float ratio = 0;
    char tail;
    if (!strcmp(value, "full")) ratio = 1.0f;
    else if (sscanf(value, "%f%c", &ratio, &tail) != 1 || ratio < 1.0f || ratio > 3.0f) ratio = 0;
    return ratio;
}

// WORKAROUND, not a fix: the largest render-target pool scene_res grows. A larger one crashes the game before it shows
// anything (access violation reading 0xa0 in main+0x128d40 on 3.2.0, an allocation that comes back null), whatever the
// scene size: a 5760x3240 output on auto, an 800 MiB pool and a small scene, crashes the same way. At a 3840x2160
// output, pools of 452 and 474 MiB ran, 514 MiB and up crashed; the limit is likely the heap the pool is carved from,
// still to be found, and this cap should go once it is. A ratio whose pool would pass it is raised to the smallest
// ratio whose pool fits, so the scene keeps the sizes the pool is grown for.
#define DKB_SCENE_POOL_MAX (474u << 20)

static void dkb_wide_scene(uint8_t *page, float ratio) {
    int set = rt_patch_set("dkbscene");
    if (set < 0) {
        rt_log("Bananza resolution: this build or game version has no 3D scene patch; scene_res has no effect");
        return;
    }
    uint32_t pool;
    memcpy(&pool, page + 0x110, 4);
    // The pool grows by (1.5 / ratio)^2, so the smallest ratio that keeps it within the cap is 1.5 * sqrt(pool / cap).
    // At 1.5 it does not grow, so the ratio is not raised past that: an output whose own pool is past the cap (5760x3240
    // and up) is the open crash above, and a smaller scene would not help it.
    float floor_ratio = (float)(1.5 * sqrt((double)pool / DKB_SCENE_POOL_MAX));
    if (floor_ratio > 1.5f) floor_ratio = 1.5f;
    if (ratio < floor_ratio) {
        rt_log("Bananza resolution: scene_res ratio %.3f would grow the render-target pool to %u MiB, past the %u MiB "
               "the game was seen to run with; using %.3f",
               ratio, (unsigned)((double)pool * (1.5 / ratio) * (1.5 / ratio)) >> 20, DKB_SCENE_POOL_MAX >> 20,
               floor_ratio);
        ratio = floor_ratio;
    }
    float grow = (1.5f / ratio) * (1.5f / ratio);
    if (grow > 1.0f) {
        const uint64_t align = 2u << 20;
        uint64_t bytes = (uint64_t)((double)pool * grow + 0.999);
        bytes = (bytes + align - 1) / align * align;
        if (bytes > DKB_SCENE_POOL_MAX) bytes = DKB_SCENE_POOL_MAX;   // the rounding up to 2 MiB can pass it by one step
        pool = (uint32_t)bytes;
        memcpy(page + 0x110, &pool, 4);
    }
    memcpy(page + 0x118, &ratio, 4);
    rt_patch_switch(set, 1);
    // Update 3.2.0 makes each display's 3D image a scale from its render parameters times the display size, 2/3, read
    // at main+79024/79028 (patches-dkb458752.txt, set render3d); the controller's ratio divides that image, so the
    // scene is display x scale / ratio. With the controller alone, scene_res = full at a 2880x1620 display drew the
    // scene at 1920x1080 (the upscaler no longer ran, and the image was stretched). The render3d sites read a scale of
    // 1.0 from page +0x120/+0x124, which makes the scene display / ratio, as on 1.0.0, and its sizes those 1.0.0 has
    // at that ratio, which the pool above is grown for.
    int scale_set = rt_patch_set("render3d");
    if (scale_set >= 0) {
        const float one = 1.0f;
        memcpy(page + 0x120, &one, 4);
        memcpy(page + 0x124, &one, 4);
        rt_patch_switch(scale_set, 1);
    }
    rt_log("Bananza resolution: 3D scene ratio %.3f (scene drawn at 1/%.3f of the virtual display%s, render-target pool %u MiB)",
           ratio, ratio, scale_set >= 0 ? ", the 3.2.0 image scale at 1.0" : "", pool >> 20);
}

void dkb_wide_init(void) {
    wide_enabled = 0;
    wide_output[0] = 2880;
    wide_output[1] = 1620;
    int scene_set = rt_patch_set("dkbscene"), scale_set = rt_patch_set("render3d");
    if (scene_set >= 0) rt_patch_switch(scene_set, 0);   // on only once dkb_wide_scene has written the ratio
    if (scale_set >= 0) rt_patch_switch(scale_set, 0);   // and the 3.2.0 image scale
    int set = rt_patch_set("dkbwide");
    if (set < 0) {
        // The output resolution, ultrawide and frame-rate settings all work through code patches (recomp/patches-dkb100.txt
        // for 1.0.0, recomp/patches-dkb458752.txt for update 3.2.0). A build generated without them has no such set: say
        // so, so a setting that has no effect is not a mystery (#131).
        rt_log("Bananza resolution: this build has no resolution patch; output stays 2880x1620 and the "
               "resolution, ultrawide and real-time settings have no effect");
        if (dkb_scene_ratio()) rt_log("Bananza resolution: scene_res has no effect either");
        return;
    }
    rt_patch_switch(set, 0);
    float scene = dkb_scene_ratio();
    int allow_wide = cfg_bool("TOTK_ULTRAWIDE");
    unsigned width = 0, height = 0;
    char tail;
    const char *value = cfg_value("TOTK_OUTPUT_RES");
    if (sscanf(value, "%ux%u%c", &width, &height, &tail) != 2 ||
        width < 1280 || width > 7680 || height < 720 || height > 4320 ||
        width % 2 || height % 2 || width * 9 < height * 16 ||
        (!allow_wide && width * 9 != height * 16)) {
        rt_patch_switch(set, 0);
        rt_log("Bananza resolution: use an even 16:9 resolution, or enable ultrawide for a wider resolution; using stock 2880x1620");
        if (scene) rt_log("Bananza resolution: scene_res needs a valid output resolution and is not applied");
        return;
    }
    uint8_t *page = rt_patch_page();
    float aspect = (float)width / height;
    float factor = aspect / (16.0f / 9.0f);
    memcpy(page + 0x100, &width, 4);
    memcpy(page + 0x104, &height, 4);
    memcpy(page + 0x108, &aspect, 4);
    memcpy(page + 0x10c, &factor, 4);
    uint32_t pool = dkb_wide_pool_bytes(width, height);
    memcpy(page + 0x110, &pool, 4);
    wide_output[0] = width;
    wide_output[1] = height;
    wide_enabled = width * 9 > height * 16;
    rt_patch_switch(set, 1);
    rt_log("Bananza resolution: virtual display %ux%u, camera aspect %.6f, ultrawide %s",
           width, height, aspect, wide_enabled ? "on" : "off");
    if (scene) dkb_wide_scene(page, scene);
}
