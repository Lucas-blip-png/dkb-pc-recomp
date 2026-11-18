// The start-up screen (vk.h; docs/dkb-testing.md, the DKB_SPLASH rows): from the moment the window opens to the game's
// first picture, the window shows a scene of our own (shaders/splash.comp draws it from arithmetic, no image file) with
// what the start is doing: the step, its percentage and count, the time it has left once that can be said, and a tip.
// Without it the window opened only at the game's first present, several seconds into the start, and then showed the
// game's black frames for some seconds more.
//
// Threads. The screen has a thread of its own (screen_main), started first thing in the boot (vk_splash
// GFX_SPLASH_START): it makes the device (vk_init, which the prewarm would make anyway, under the same lock), compiles
// the screen's one compute pipeline, opens the window (gfx_window_start) and then draws and presents a frame every 16
// or 33 ms through vk_present_splash, reading boot_progress.h, while the boot's own threads prepare shaders and
// pipelines beside it. It takes no lock the preparation holds for long, and the GPU has nothing else to do then.
//
// The end. The game's first frames are black. The present thread (vk_backend.c present_main) asks vk_splash_hold
// before each game frame: while the screen is up the frame is not shown, and every 30 ms one of them is checked on
// the GPU (shaders/splash_check.comp reads every texel). The first frame with anything in it ends the screen: the
// present thread asks the screen's thread to stop, waits for its last present, and shows that frame. A frame that
// cannot be checked ends the screen at once, and so does a minute of black frames, so the screen never hides the game
// for long. The window closed meanwhile ends it too.
//
// DKB_SPLASH=0 (splash = 0 in dkb.ini) turns it off: the window opens at the game's first present, as before, and
// DKB_SPLASH_WATCH=1 then logs the first picture's time without holding frames. DKB_SPLASH_TEST=<seconds> shows a
// simulated long start of that length (splash_test_progress) and holds the guest until it ends; DKB_SPLASH_SHOTS=<dir>
// saves the presented screen as <dir>/splash-<n>.png every two seconds (only the screen: the game's frames never reach
// it). --probe-splash (probe_splash) draws a set of states and sizes without a window into that folder.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_progress.h"
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "splash.h"
#include "splash_check_spv.h"
#include "splash_spv.h"
#include "ui_text.h"
#include "vk.h"
#include "platform/platform.h"

enum { BOXES = 16 };
typedef struct { float rect[4], src[4], color[4], shade[4]; } Box;
typedef struct {   // shaders/splash.comp Params, std430
    int32_t size[4];
    int32_t atlas[4];
    float t[4], bar[4], mark[4], misc[4];
    Box box[BOXES];
} Params;

// ---- state shared with the present thread
enum { ST_OFF, ST_UP, ST_ENDING, ST_DONE };
static volatile int32_t g_state;            // ST_*
static PlatEvent g_done;                    // set when the screen's thread presents no more
static PlatEvent g_test_done;               // DKB_SPLASH_TEST: the simulated start is over
static int g_test;                          // DKB_SPLASH_TEST is set
static int g_watch;                         // DKB_SPLASH_WATCH=1 with the screen off: the first picture is logged

int vk_splash_up(void) {
    int32_t s = plat_cas32(&g_state, 0, 0);
    return s == ST_UP || s == ST_ENDING;
}

// ---- the renderer: everything the screen's thread (or the probe) draws with
typedef struct {
    VkDevice dev;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    int pending;                            // cb was submitted and its fence not waited for
    VkImage frame, back;
    VmaAllocation frame_a, back_a;
    VkImageView frame_v, back_v;
    uint32_t w, h;
    int fresh;                              // the images are new: their layout is UNDEFINED
    VkBuffer params, atlas;
    VmaAllocation params_a, atlas_a;
    Params *pp;
    uint8_t *ap;
    size_t atlas_cap;
    float back_dawn;                        // the dawn the backdrop was painted at; negative: paint it
    SplashText *text;
    SplashView shown;                       // the texts in the atlas now
    uint32_t text_w, text_h;                // the window size they were laid out for
    int nbox;
    Box box[BOXES];
    uint32_t tip_boxes;                     // bit i: box i is part of the tip, which fades in and out
    float bar[4], mark[3];
} Renderer;

static void renderer_free_images(Renderer *r) {
    VkCtx *c = vk_ctx();
    if (r->frame_v) vkDestroyImageView(r->dev, r->frame_v, NULL);
    if (r->back_v) vkDestroyImageView(r->dev, r->back_v, NULL);
    if (r->frame) vmaDestroyImage(c->vma, r->frame, r->frame_a);
    if (r->back) vmaDestroyImage(c->vma, r->back, r->back_a);
    r->frame_v = r->back_v = VK_NULL_HANDLE, r->frame = r->back = VK_NULL_HANDLE, r->w = r->h = 0;
}

static void renderer_free(Renderer *r) {
    VkCtx *c = vk_ctx();
    if (!c || !r->dev) return;
    if (r->pending) vkWaitForFences(r->dev, 1, &r->fence, VK_TRUE, 2000000000ull), r->pending = 0;
    renderer_free_images(r);
    if (r->params) vmaDestroyBuffer(c->vma, r->params, r->params_a);
    if (r->atlas) vmaDestroyBuffer(c->vma, r->atlas, r->atlas_a);
    if (r->fence) vkDestroyFence(r->dev, r->fence, NULL);
    if (r->pool) vkDestroyCommandPool(r->dev, r->pool, NULL);
    if (r->pipe) vkDestroyPipeline(r->dev, r->pipe, NULL);
    if (r->layout) vkDestroyPipelineLayout(r->dev, r->layout, NULL);
    if (r->dsl) vkDestroyDescriptorSetLayout(r->dev, r->dsl, NULL);
    splash_text_free(r->text);
    memset(r, 0, sizeof *r);
}

static int host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b, VmaAllocation *a, void **map) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, b, a, &info) != VK_SUCCESS || !info.pMappedData) {
        if (*b) vmaDestroyBuffer(vk_ctx()->vma, *b, *a);
        *b = VK_NULL_HANDLE;
        return 0;
    }
    *map = info.pMappedData;
    return 1;
}

static VkPipeline compute_pipeline(VkDevice dev, const uint32_t *code, size_t bytes, VkPipelineLayout layout) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev, m, NULL);
    return p;
}

static int renderer_init(Renderer *r) {
    VkCtx *c = vk_ctx();
    memset(r, 0, sizeof *r);
    r->dev = c->device;
    r->back_dawn = -1;
    r->push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!r->push || !(r->text = splash_text_new())) return 0;
    VkDescriptorSetLayoutBinding b[4] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &r->dsl};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateDescriptorSetLayout(r->dev, &dci, NULL, &r->dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(r->dev, &li, NULL, &r->layout) != VK_SUCCESS ||
        !(r->pipe = compute_pipeline(r->dev, splash_spv, sizeof splash_spv, r->layout)) ||
        vkCreateCommandPool(r->dev, &pci, NULL, &r->pool) != VK_SUCCESS || vkCreateFence(r->dev, &fi, NULL, &r->fence) != VK_SUCCESS)
        return 0;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = r->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(r->dev, &ai, &r->cb) != VK_SUCCESS) return 0;
    void *pp = NULL;
    if (!host_buffer(sizeof(Params), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &r->params, &r->params_a, &pp)) return 0;
    r->pp = pp;
    return 1;
}

static VkImage image2d(uint32_t w, uint32_t h, VkImageUsageFlags usage, VmaAllocation *a, VkImageView *v) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img = VK_NULL_HANDLE;
    if (vmaCreateImage(c->vma, &ii, &ai, &img, a, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, v) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, img, *a);
        return VK_NULL_HANDLE;
    }
    return img;
}

static void wait_pending(Renderer *r) {
    if (!r->pending) return;
    vkWaitForFences(r->dev, 1, &r->fence, VK_TRUE, 2000000000ull);
    vkResetFences(r->dev, 1, &r->fence);
    r->pending = 0;
}

static int ensure_images(Renderer *r, uint32_t w, uint32_t h) {
    if (r->frame && r->w == w && r->h == h) return 1;
    // a new size: the last present may still read the old frame
    wait_pending(r);
    vk_queue_lock();
    vkQueueWaitIdle(vk_ctx()->gfx_queue);
    vk_queue_unlock();
    renderer_free_images(r);
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!(r->frame = image2d(w, h, u, &r->frame_a, &r->frame_v)) || !(r->back = image2d(w, h, u, &r->back_a, &r->back_v))) {
        renderer_free_images(r);
        return 0;
    }
    r->w = w, r->h = h, r->fresh = 1, r->back_dawn = -1;
    return 1;
}

// ---- layout of the text: sizes in units of 1080 window lines, the margins and the line, as shaders/splash.comp
// expects them (the darker pools behind the text are placed from the same numbers)
static const float CREAM[3] = {0xf3 / 255.f, 0xe9 / 255.f, 0xd2 / 255.f}, MUTED[3] = {0xc4 / 255.f, 0xb4 / 255.f, 0x94 / 255.f},
                   BANANA[3] = {0xf6 / 255.f, 0xc5 / 255.f, 0x2b / 255.f};

static float unit_of(uint32_t w, uint32_t h) {
    float u = fminf((float)h / 1080.0f, (float)w / 1250.0f);
    return u < 0.5f ? 0.5f : u;
}

static void put_box(Renderer *r, SplashGlyphBox g, const float rgb[3], float alpha, float u) {
    if (g.w <= 0 || r->nbox >= BOXES) return;
    if (alpha < 0) r->tip_boxes |= 1u << r->nbox, alpha = 1;   // a tip box: its opacity is set each frame
    Box *b = &r->box[r->nbox++];
    *b = (Box){{g.x, g.y, g.w, g.h}, {(float)g.ax, (float)g.ay, fmaxf(1.0f, roundf(2.0f * u)), fmaxf(1.0f, 1.6f * u)},
               {rgb[0], rgb[1], rgb[2], alpha}, {0.03f, 0.02f, 0.01f, 0.80f}};
}

// The tip in at most two lines of width at most max: the words that fit on the first, the rest on the second
static void wrap_tip(SplashText *t, const char *tip, float px, float max, char *l1, char *l2, size_t n) {
    snprintf(l1, n, "%s", tip);
    l2[0] = 0;
    if (splash_text_width(t, l1, px, 0) <= max) return;
    size_t len = strlen(tip), cut = 0;
    for (size_t i = 1; i < len; i++) {
        if (tip[i] != ' ') continue;
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%.*s", (int)i, tip);
        if (splash_text_width(t, tmp, px, 0) > max) break;
        cut = i;
    }
    if (!cut) return;
    snprintf(l1, n, "%.*s", (int)cut, tip);
    snprintf(l2, n, "%s", tip + cut + 1);
}

// Lays the view's text out for a w x h window into the atlas and the boxes; the tunnel and the mark with it
static void layout_text(Renderer *r, const SplashView *v, uint32_t w, uint32_t h) {
    SplashText *t = r->text;
    float u = unit_of(w, h), W = (float)w, H = (float)h;
    float margin = fmaxf(24.0f, fminf(96.0f * u, 0.075f * W));
    float x0 = margin, x1 = W - margin - 70.0f * u;   // the bunch at the tunnel's end has the last 70 units
    float ly = H - 150.0f * u;
    int narrow = x1 - x0 < 820.0f * u;
    if (narrow) ly = H - 230.0f * u;                    // room under the line for the count and the tip
    r->bar[0] = x0, r->bar[1] = x1, r->bar[2] = ly;
    r->nbox = 0, r->tip_boxes = 0;
    splash_text_begin(t);
    // the name and the version at the top left, beside the mark
    // the hand of bananas has its crown at mark[0..1], mark[2] pixels a unit of its shape (shaders/splash.comp bunch:
    // from 0.3 left of the crown to 1.75 right of it, 1.1 above it to 0.66 under it), centred on the name's capitals
    float name_px = 36.0f * u, mark_s = 22.0f * u;
    float ny = 50.0f * u + splash_text_ascent(t, name_px) * 0.78f, nx = x0 + 2.1f * mark_s + 12.0f * u;
    r->mark[0] = x0 + 0.3f * mark_s, r->mark[1] = ny - 0.36f * name_px + 0.22f * mark_s, r->mark[2] = mark_s;
    put_box(r, splash_text_add(t, splash_project_name(), name_px, 0.4f * u, nx, ny, 0), CREAM, 1, u);
    char ver[128];
    splash_version_text(ver, sizeof ver);
    put_box(r, splash_text_add(t, ver, 15.0f * u, 0.6f * u, nx + 1.0f * u, ny + 26.0f * u, 0), MUTED, 1, u);
    // the step's name in spaced capitals, the large number and its sign over the line
    float num_px = 150.0f * u, label_y = ly - 186.0f * u - (v->pct[0] ? 0 : -110.0f * u);
    put_box(r, splash_text_add(t, v->label, 17.0f * u, 4.2f * u, x0 + 2.0f * u, label_y, 0), BANANA, 1, u);
    if (v->pct[0]) {
        float base = ly - 34.0f * u;
        SplashGlyphBox nb = splash_text_add(t, v->pct, num_px, -2.0f * u, x0 - 6.0f * u, base, 0);
        put_box(r, nb, CREAM, 1, u);
        float nw = splash_text_width(t, v->pct, num_px, -2.0f * u);
        put_box(r, splash_text_add(t, "%", 50.0f * u, 0, x0 - 6.0f * u + nw + 10.0f * u, base - 64.0f * u, 0), MUTED, 1, u);
    }
    // the count and the time left: at the right over the line, or under it in a narrow window
    float small = 19.0f * u;
    float rx = narrow ? x0 : x1, ry = narrow ? ly + 44.0f * u : ly - 52.0f * u;
    int align = narrow ? 0 : 1;
    if (v->count[0]) put_box(r, splash_text_add(t, v->count, small, 0.2f * u, rx, ry, align), MUTED, 1, u), ry += 28.0f * u;
    else if (!narrow) ry += 28.0f * u;
    if (v->left[0]) put_box(r, splash_text_add(t, v->left, small, 0.2f * u, rx, ry, align), CREAM, 1, u);
    // the tip under the line, after a dash in the banana's yellow
    if (v->tip[0]) {
        char l1[256], l2[256];
        float tx = x0 + 34.0f * u, ty = narrow ? ly + 44.0f * u + 2 * 28.0f * u + 16.0f * u : ly + 48.0f * u;
        wrap_tip(t, v->tip, small, x1 + 70.0f * u - tx, l1, l2, sizeof l1);
        put_box(r, splash_text_add(t, "\xe2\x80\x94", small, 0, x0, ty, 0), BANANA, -1, u);
        put_box(r, splash_text_add(t, l1, small, 0.15f * u, tx, ty, 0), MUTED, -1, u);
        if (l2[0]) put_box(r, splash_text_add(t, l2, small, 0.15f * u, tx, ty + 27.0f * u, 0), MUTED, -1, u);
    }
}

// The atlas into its buffer, grown when it must
static int upload_atlas(Renderer *r) {
    int aw, ah;
    const uint8_t *a = splash_text_atlas(r->text, &aw, &ah);
    size_t need = (size_t)aw * (ah ? ah : 1);
    if (need < 64) need = 64;
    if (need > r->atlas_cap) {
        wait_pending(r);
        if (r->atlas) vmaDestroyBuffer(vk_ctx()->vma, r->atlas, r->atlas_a);
        r->atlas = VK_NULL_HANDLE;
        size_t cap = need + need / 2;
        void *p = NULL;
        if (!host_buffer(cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &r->atlas, &r->atlas_a, &p)) return r->atlas_cap = 0, 0;
        r->ap = p, r->atlas_cap = cap;
    }
    if (a && ah) memcpy(r->ap, a, (size_t)aw * ah);
    vmaFlushAllocation(vk_ctx()->vma, r->atlas_a, 0, VK_WHOLE_SIZE);
    r->pp->atlas[0] = aw, r->pp->atlas[1] = ah;
    return 1;
}

static int same_text(const SplashView *a, const SplashView *b) {
    return !strcmp(a->label, b->label) && !strcmp(a->pct, b->pct) && !strcmp(a->count, b->count) &&
           !strcmp(a->left, b->left) && !strcmp(a->tip, b->tip);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

// Draws the view at secs into the frame image (w x h) and submits it; the frame is in GENERAL with its writes on the
// queue when this returns. fade: 0 to 1, the screen coming in. 0 when it could not.
static int render(Renderer *r, const SplashView *v, double secs, uint32_t w, uint32_t h, float fade) {
    if (!ensure_images(r, w, h)) return 0;
    wait_pending(r);
    if (r->text_w != w || r->text_h != h || !same_text(v, &r->shown) || !r->atlas) {
        layout_text(r, v, w, h);
        if (!upload_atlas(r)) return 0;
        r->shown = *v, r->text_w = w, r->text_h = h;
    }
    Params *P = r->pp;
    float u = unit_of(w, h);
    // the dawn moves in steps of 1/64: the backdrop is painted again only then
    float dawn = roundf(v->dawn * 64.0f) / 64.0f;
    int paint = r->back_dawn != dawn;
    P->size[0] = (int)w, P->size[1] = (int)h, P->size[3] = r->nbox;
    P->t[0] = (float)secs, P->t[1] = u, P->t[2] = dawn, P->t[3] = fade;
    P->bar[0] = r->bar[0], P->bar[1] = r->bar[1], P->bar[2] = r->bar[2], P->bar[3] = v->fraction;
    P->mark[0] = r->mark[0], P->mark[1] = r->mark[1], P->mark[2] = r->mark[2];
    P->mark[3] = v->fraction >= 0 ? v->fraction * v->fraction : 0.15f;
    P->misc[0] = 0.70f;
    for (int i = 0; i < r->nbox; i++) {   // the tip fades every frame without a new atlas
        P->box[i] = r->box[i];
        if (r->tip_boxes >> i & 1) P->box[i].color[3] = v->tip_alpha;
    }
    VkCommandBuffer cb = r->cb;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkImageLayout from = r->fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    // the last present's blit read the frame; this pass writes it
    barrier(cb, r->frame, from, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (r->fresh) barrier(cb, r->back, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    r->fresh = 0;
    VkDescriptorImageInfo fi = {VK_NULL_HANDLE, r->frame_v, VK_IMAGE_LAYOUT_GENERAL}, bk = {VK_NULL_HANDLE, r->back_v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo pb = {r->params, 0, sizeof(Params)}, ab = {r->atlas, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &fi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &bk},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &pb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ab}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipe);
    r->push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->layout, 0, 4, wr);
    uint32_t gx = (w + 7) / 8, gy = (h + 7) / 8;
    // The pass number is in the parameters, which both passes of one submission would read alike: the backdrop pass,
    // rare, goes in a submission of its own first
    if (paint) {
        P->size[2] = 0;
        vmaFlushAllocation(vk_ctx()->vma, r->params_a, 0, VK_WHOLE_SIZE);
        vkCmdDispatch(cb, gx, gy, 1);
        vkEndCommandBuffer(cb);
        VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
        vk_queue_lock();
        VkResult res = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, &si, r->fence);
        vk_queue_unlock();
        if (res != VK_SUCCESS) return 0;
        r->pending = 1;
        wait_pending(r);
        r->back_dawn = dawn;
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipe);
        r->push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->layout, 0, 4, wr);
    }
    barrier(cb, r->back, VK_IMAGE_LAYOUT_GENERAL, CS | ALL, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    P->size[2] = 1;
    vmaFlushAllocation(vk_ctx()->vma, r->params_a, 0, VK_WHOLE_SIZE);
    vkCmdDispatch(cb, gx, gy, 1);
    barrier(cb, r->frame, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
    vk_queue_lock();
    VkResult res = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, &si, r->fence);
    vk_queue_unlock();
    if (res != VK_SUCCESS) return 0;
    r->pending = 1;
    return 1;
}

// ---- the screen's thread
static double now_s(uint64_t t0) { return (double)(plat_now_ns() - t0) / 1e9; }

static void screen_main(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns();
    Renderer r;
    if (!vk_init() || !renderer_init(&r)) {
        rt_log("splash: the start-up screen could not be made; the window opens at the game's first frame");
        if (vk_ctx()) renderer_free(&r);
        plat_xchg32(&g_state, ST_DONE);
        plat_event_set(&g_done);
        plat_event_set(&g_test_done);
        return;
    }
    double test_s = g_test ? atof(cfg_get("TOTK_SPLASH_TEST")) : 0;
    const char *shots = cfg_get("TOTK_SPLASH_SHOTS");
    if (shots && *shots) plat_file_mkdir_p(shots);
    gfx_window_start();   // the window, its surface and swapchain; the device is made already
    rt_log("splash: the start-up screen is up %.0f ms after the process started (splash = 0 turns it off)",
           (double)plat_process_uptime_ns() / 1e6);
    SplashEta eta;
    splash_eta_reset(&eta, -1);
    int test_running = test_s > 0, shot_n = 0, frames = 0;
    double next_shot = 0.5, opened = -1, text_at = -1, last_secs = 0;
    float dawn = -1;
    for (;;) {
        if (gfx_window_exiting() || plat_cas32(&g_state, 0, 0) != ST_UP) break;
        double secs = now_s(t0);
        BootProgress p;
        boot_progress_read(&p);
        if (test_running && !(test_running = splash_test_progress(secs, test_s, &p))) {
            rt_log("splash: DKB_SPLASH_TEST: the simulated start of %.0f s is over", test_s);
            plat_event_set(&g_test_done);
            boot_progress_read(&p);
        }
        double step_secs = test_running ? secs : (p.since_ns ? (double)(plat_now_ns() - p.since_ns) / 1e9 : secs);
        if (test_running) {   // the step's start in the simulation: where its count went from 0
            static int last_step = -1;
            static double since = 0;
            if (p.step != last_step) last_step = p.step, since = secs;
            step_secs = secs - since;
        }
        SplashView v;
        splash_view(&v, &p, secs, step_secs, &eta);
        // The sky moves toward the dawn of the step over about half a second rather than jumping when a step begins
        // (a start from a warm cache goes through all of them in a few seconds)
        dawn = dawn < 0 ? v.dawn : dawn + (v.dawn - dawn) * (float)fmin(1.0, (secs - last_secs) / 0.5);
        v.dawn = dawn, last_secs = secs;
        // The words and numbers change eight times a second at most, the picture every frame: each new text is laid
        // out and rasterised again (splash_text.c), which a count that moves every frame would do 60 times a second
        if (secs - text_at < 0.125 && v.label[0] && !strcmp(v.label, r.shown.label)) {
            memcpy(v.pct, r.shown.pct, sizeof v.pct), memcpy(v.count, r.shown.count, sizeof v.count);
            memcpy(v.left, r.shown.left, sizeof v.left);
        } else {
            text_at = secs;
        }
        uint32_t w, h;
        vk_present_extent(&w, &h);
        if (!w || !h) {   // minimised, or the swapchain is being made again
            plat_sleep_ms(50);
            continue;
        }
        if (opened < 0) opened = secs;
        float fade = (float)fmin(1.0, (secs - opened) / 0.45);
        if (!render(&r, &v, secs, w, h, fade)) {
            rt_log("splash: a frame of the start-up screen could not be drawn; the screen ends");
            break;
        }
        char shot[1024];
        const char *shot_path = NULL;
        if (shots && *shots && secs >= next_shot) {
            snprintf(shot, sizeof shot, "%s/splash-%03d.png", shots, shot_n++);
            shot_path = shot;
            next_shot = secs + 2.0;
        }
        vk_present_splash(r.frame, (int)w, (int)h, shot_path);
        frames++;
        // 60 frames a second up to 2.6 million pixels, 30 above: the GPU is free, but a laptop need not run hot
        double every = (uint64_t)w * h > 2600000u ? 1.0 / 30 : 1.0 / 60;
        double left = every - (now_s(t0) - secs);
        if (left > 0.001) plat_sleep_ns((uint64_t)(left * 1e9));
    }
    double up = now_s(t0);
    plat_xchg32(&g_state, ST_DONE);
    plat_event_set(&g_done);
    plat_event_set(&g_test_done);
    rt_log("splash: the start-up screen ends after %.1f s and %d frames", up, frames);
    if (gfx_window_exiting()) return;   // the device goes with the process
    // The last frame's present may still be on the queue: each present waits for the fence of the present two before
    // it, so once the game has presented three frames, nothing reads the screen's images
    uint64_t f0 = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < 300 && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) < f0 + 3; i++) plat_sleep_ms(10);
    if (__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) < f0 + 3) {
        vk_queue_lock();
        vkQueueWaitIdle(vk_ctx()->gfx_queue);
        vk_queue_unlock();
    }
    renderer_free(&r);
}

void vk_splash(int when) {
    if (when == GFX_SPLASH_START) {
        const char *gpu = rt_config()->gpu;
        if ((gpu && !strcmp(gpu, "null")) || rt_config()->shot) return;
        if (!cfg_bool("TOTK_SPLASH")) {
            const char *w = cfg_get("TOTK_SPLASH_WATCH");
            g_watch = w && !strcmp(w, "1");
            return;
        }
        const char *test = cfg_get("TOTK_SPLASH_TEST");
        g_test = test && atof(test) > 0;
        plat_event_init(&g_done, 1, 0);
        plat_event_init(&g_test_done, 1, 0);
        plat_xchg32(&g_state, ST_UP);
        if (!plat_thread_spawn(screen_main, NULL, "splash", 0)) {
            plat_xchg32(&g_state, ST_OFF);
            rt_log("splash: no thread for the start-up screen");
        }
        return;
    }
    // GFX_SPLASH_GUEST: the game starts; the screen says so, and a test sequence holds it until it is over
    boot_progress_step(BOOT_STEP_GAME, 0);
    if (g_test && plat_cas32(&g_state, 0, 0) != ST_OFF) plat_event_wait(&g_test_done);
}

// ---- the end: is a game frame blank? (present thread)
static struct {
    int tried, ok;
    VkDevice dev;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer res;
    VmaAllocation res_a;
    volatile uint32_t *res_p;
} K;

static int check_init(void) {
    K.tried = 1;
    VkCtx *c = vk_ctx();
    K.dev = c->device;
    K.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &K.dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!K.push || vkCreateDescriptorSetLayout(K.dev, &dci, NULL, &K.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(K.dev, &li, NULL, &K.layout) != VK_SUCCESS || vkCreateSampler(K.dev, &si, NULL, &K.sampler) != VK_SUCCESS ||
        !(K.pipe = compute_pipeline(K.dev, splash_check_spv, sizeof splash_check_spv, K.layout)) ||
        vkCreateCommandPool(K.dev, &pci, NULL, &K.pool) != VK_SUCCESS || vkCreateFence(K.dev, &fi, NULL, &K.fence) != VK_SUCCESS)
        return 0;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = K.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(K.dev, &ai, &K.cb) != VK_SUCCESS) return 0;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo mi = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &mi, &K.res, &K.res_a, &info) != VK_SUCCESS || !info.pMappedData) return 0;
    K.res_p = info.pMappedData;
    return K.ok = 1;
}

static void check_free(void) {
    VkCtx *c = vk_ctx();
    if (!K.dev || !c) return;
    if (K.res) vmaDestroyBuffer(c->vma, K.res, K.res_a);
    if (K.fence) vkDestroyFence(K.dev, K.fence, NULL);
    if (K.pool) vkDestroyCommandPool(K.dev, K.pool, NULL);
    if (K.pipe) vkDestroyPipeline(K.dev, K.pipe, NULL);
    if (K.sampler) vkDestroySampler(K.dev, K.sampler, NULL);
    if (K.layout) vkDestroyPipelineLayout(K.dev, K.layout, NULL);
    if (K.dsl) vkDestroyDescriptorSetLayout(K.dev, K.dsl, NULL);
    memset(&K, 0, sizeof K);
    K.tried = 1;
}

// 1 when the frame has a picture, 0 when it is black, -1 when it cannot be told. The frame is in GENERAL with the
// game's writes submitted (vk_present_blit's contract); this reads it on the queue and waits for the answer.
static int frame_lit(VkImage image, VkFormat fmt, int w, int h) {
    if (!K.tried && !check_init()) K.ok = 0;
    if (!K.ok || w <= 0 || h <= 0) return -1;
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(K.dev, &vi, NULL, &view) != VK_SUCCESS) return -1;
    VkCommandBuffer cb = K.cb;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    vkCmdFillBuffer(cb, K.res, 0, 4, 0);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo md = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &md);
    // the game's writes become visible to this read; its later writes wait for it
    barrier(cb, image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkDescriptorImageInfo ii = {K.sampler, view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo rb = {K.res, 0, 64};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &rb}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, K.pipe);
    K.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, K.layout, 0, 2, wr);
    struct { int32_t size[2]; float threshold; int32_t pad; } pc = {{w, h}, 3.0f / 255.0f, 0};
    vkCmdPushConstants(cb, K.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, ((uint32_t)w + 15) / 16, ((uint32_t)h + 15) / 16, 1);
    barrier(cb, image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    VkMemoryBarrier2 hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo hd = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &hb};
    vkCmdPipelineBarrier2(cb, &hd);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, &si, K.fence);
    vk_queue_unlock();
    int lit = -1;
    if (r == VK_SUCCESS && vkWaitForFences(K.dev, 1, &K.fence, VK_TRUE, 1000000000ull) == VK_SUCCESS) {
        vmaInvalidateAllocation(vk_ctx()->vma, K.res_a, 0, 64);
        lit = *K.res_p != 0;
    }
    vkResetFences(K.dev, 1, &K.fence);
    vkDestroyImageView(K.dev, view, NULL);
    return lit;
}

int vk_splash_hold(VkImage image, VkFormat fmt, int w, int h) {
    static uint64_t first_ns, last_check_ns, frames;
    int32_t st = plat_cas32(&g_state, 0, 0);
    if (st == ST_DONE || (st == ST_OFF && !g_watch)) {
        if (!first_ns && st == ST_OFF) {   // the time a start without the screen is measured by
            first_ns = plat_now_ns();
            rt_log("splash: off; the game's first frame came %.0f ms after the process started",
                   (double)plat_process_uptime_ns() / 1e6);
        }
        return 0;
    }
    uint64_t now = plat_now_ns();
    frames++;
    if (!first_ns) {
        first_ns = now;
        rt_log("splash: the game's first frame came %.0f ms after the process started", (double)plat_process_uptime_ns() / 1e6);
    }
    if (st == ST_OFF) {   // DKB_SPLASH_WATCH: the frames are shown as they come; the first with a picture is logged
        if (now - last_check_ns < 30000000ull) return 0;
        last_check_ns = now;
        int lit = frame_lit(image, fmt, w, h);
        if (lit == 0 && (double)(now - first_ns) / 1e9 < 60) return 0;
        rt_log("splash: off; %s at frame %llu, %.0f ms after the process started",
               lit > 0 ? "the game's first picture" : lit < 0 ? "a game frame that cannot be checked" : "a minute of black frames",
               (unsigned long long)frames, (double)plat_process_uptime_ns() / 1e6);
        g_watch = 0;
        check_free();
        return 0;
    }
    if (st == ST_UP && now - last_check_ns < 30000000ull) return 1;
    last_check_ns = now;
    int lit = st == ST_UP ? frame_lit(image, fmt, w, h) : 1;
    double held_s = (double)(now - first_ns) / 1e9;
    if (lit == 0 && held_s < 60) return 1;
    plat_xchg32(&g_state, ST_ENDING);
    int ended = plat_event_wait_ns(&g_done, 2000000000ull);
    rt_log("splash: %s at frame %llu, %.0f ms after the process started (%.1f s of black frames held)%s",
           lit > 0 ? "the game's first picture" : lit < 0 ? "a game frame that cannot be checked" : "a minute of black frames",
           (unsigned long long)frames, (double)plat_process_uptime_ns() / 1e6, held_s,
           ended ? "" : "; the screen did not stop in time");
    check_free();
    return 0;
}

// ---- --probe-splash <dir>: the screen in a set of states and sizes, drawn without a window and saved as PNGs
static int save_frame(Renderer *r, const char *path) {
    VkCtx *c = vk_ctx();
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation a;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)r->w * r->h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &a, &info) != VK_SUCCESS) return 0;
    wait_pending(r);
    VkCommandBuffer cb = r->cb;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {r->w, r->h, 1}};
    vkCmdCopyImageToBuffer(cb, r->frame, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bc);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
    vk_queue_lock();
    VkResult res = vkQueueSubmit2(c->gfx_queue, 1, &si, r->fence);
    vk_queue_unlock();
    int ok = res == VK_SUCCESS;
    if (ok) r->pending = 1, wait_pending(r);
    if (ok) {
        vmaInvalidateAllocation(c->vma, a, 0, VK_WHOLE_SIZE);
        rt_write_png(path, info.pMappedData, r->w, r->h);
    }
    vmaDestroyBuffer(c->vma, buf, a);
    return ok;
}

int probe_splash(void) {
    const char *dir = cfg_get("TOTK_SPLASH_SHOTS");
    if (!dir || !*dir) dir = "splash-probe";
    if (!vk_init()) {
        printf("probe-splash: FAIL no Vulkan device\n");
        return 1;
    }
    Renderer r;
    if (!renderer_init(&r)) {
        printf("probe-splash: FAIL the renderer could not be made\n");
        renderer_free(&r);
        return 1;
    }
    plat_file_mkdir_p(dir);
    static const struct { uint32_t w, h; const char *name; } sizes[] = {
        {1920, 1080, "1080p"}, {1280, 720, "720p"}, {3840, 2160, "2160p"}, {720, 1000, "narrow"}, {3440, 1440, "ultrawide"}};
    // states: a step's progress and the seconds the screen has been up (the tip and the sparks move with them)
    static const struct { int step; uint64_t done, total; double secs, step_secs; const char *name; } states[] = {
        {BOOT_STEP_DEVICE, 0, 0, 0.9, 0.9, "starting"},
        {BOOT_STEP_SHADERS, 12417, 33936, 47.3, 45.0, "shaders"},
        {BOOT_STEP_PIPELINES, 3050, 3550, 131.5, 20.0, "pipelines"},
        {BOOT_STEP_GAME, 0, 0, 146.2, 4.0, "game"}};
    int fails = 0;
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
        for (size_t k = 0; k < sizeof states / sizeof *states; k++) {
            BootProgress p = {.step = states[k].step, .done = states[k].done, .total = states[k].total};
            SplashEta eta;
            splash_eta_reset(&eta, p.step);
            // a steady rate behind the shown state, so the time left reads as a long start would show it
            for (int i = 0; i <= 60 && p.total; i++) {
                double t = states[k].secs - 15 + i * 0.25;
                uint64_t d = (uint64_t)((double)p.done * (t / states[k].secs));
                splash_eta_note(&eta, t, states[k].secs - states[k].step_secs, d, p.total);
            }
            SplashView v;
            splash_view(&v, &p, states[k].secs, states[k].step_secs, &eta);
            if (k == 1 && s == 3) snprintf(v.tip, sizeof v.tip, "%s", ui_text_in("en", UI_MENU_SPLASH_TIP_SETTINGS));
            v.tip_alpha = 1;
            char path[1024];
            snprintf(path, sizeof path, "%s/splash-%s-%s.png", dir, sizes[s].name, states[k].name);
            int ok = render(&r, &v, states[k].secs, sizes[s].w, sizes[s].h, 1.0f) && save_frame(&r, path);
            printf("probe-splash: %s %s\n", ok ? "wrote" : "FAIL", path);
            fails += !ok;
        }
    renderer_free(&r);
    printf("probe-splash: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
