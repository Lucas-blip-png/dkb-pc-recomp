// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 4 temporal upscaling in place of the game's upscaler (upscaler = fsr4), next to FSR 3.1 (vk_fsr3.c) and DLSS
// (vk_dlss.c). Unlike FSR 3.1, nothing of AMD's is built in: AMD's own library is loaded at run time, the way vk_dlss.c loads
// NGX, and driven through the FidelityFX API (runtime/third_party/ffx_api, MIT). It is looked for in the folder of the
// fsr_dir setting and then in the executable's:
//   Windows: AMD's signed amd_fidelityfx_loader_vk.dll, with amd_fidelityfx_upscaler_vk.dll beside it (FidelityFX SDK
//            v2.4.0 release, Kits/FidelityFX/signedbin)
//   Linux:   amd_fidelityfx_upscaler_vk.so, a Vulkan FSR 4 build that exports the same entry points (AMD publishes none;
//            fsr4-vk is one). No loader: the library lists its own upscalers.
// The library lists the upscalers this GPU can run; the newest 4.x is taken, and without the library or a 4.x provider
// (as on NVIDIA) upscaler = fsr4 logs one line and vk_fsr3.c runs instead (vk_fsr3_wanted). The inputs are DLSS's, from
// vk_dlss.c (vk_temporal_inputs).
//
// One small pass of this file (shaders/fsr4_prep.comp) turns them into what the library takes, at the render size: the
// colour as RGBA16F (a NaN is 0, nothing is negative or past 64000, so none stays in the library's history), the scene
// depth as 1 - d in R32F (the game's depth is a D24S8 image the library cannot read) and the camera motion vectors,
// which sit in an image of the output size, cropped to the render size. The library resolves them into an RGBA16F
// picture at the output size, and FSR 3.1's output pass (vk_fsr3.c vk_fsr3_io_out) writes that into the game's output
// image.
//
// What the library is told: the depth is reversed (FFX_UPSCALE_ENABLE_DEPTH_INVERTED, as the pass stores 1 - d) and,
// when the projection's far plane is at infinity, which is how Bananza's is, FFX_UPSCALE_ENABLE_DEPTH_INFINITE, with
// the true near and far distances from the projection (camera); the context is made again when the infinite flag
// changes.
//
// Derived from Hookshot's FSR 4 engine (gpu/vk/vk_fsr4.cpp, https://github.com/NotRexed/Hookshot
// @ fab8cb37641714bd839ae83c2a7bc582bedc4d70, Copyright 2026 Hookshot contributors, GPL-3.0-or-later), rewritten in C
// for this runtime. Its host side follows the API guide of FidelityFX SDK 2.4 (Kits/FidelityFX/docs/getting-started/
// ffx-api.md): a Vulkan backend description chained to the upscale context description, the inputs read in
// SHADER_READ_ONLY_OPTIMAL, the output written in GENERAL. Hookshot's was first written for Lichii's OoT branch
// (madebyjordan, vk_fsr4_upscaler.cpp, GPL-3.0-or-later). The differences: the dispatch is recorded after rec_drain
// (vk_rec.h) like NGX's, an old context is destroyed once the submission that may use it is done (and never earlier: with no
// free slot FSR 4 skips that frame), and the camera's near and far distances and field of view come from the scene's
// projection.
//
// The Linux library path, the input pass and the unit exposure follow Longshot's FSR 4 engine (gpu/vk/vk_fsr4.cpp,
// vk_fsr4.h and gpu/vk/shaders/fsr3_prep.comp, https://git.magicbean.dev/MagicBeanDev/Longshot @
// cf605c85bbaeaaa78176b933b3bc1a3366e13a78, Copyright (C) 2026 the Longshot contributors, GPL-3.0-or-later; its Linux
// path by N3oTraX): the older Vulkan backend description chained to the context, the 1x1 exposure of 1 a Vulkan FSR 4
// build wants (unit_exposure), fsr4_wants_descriptor_buffer and the one pass that makes the three inputs. Not taken:
// its tuning of AMD's passes (fsr4_tune.h), its temporal chain and its menu probe.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "temporal_idle.h"
#include "vk.h"
#include "vk_rec.h"

#include "fsr4_prep_spv.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define LIB_NAME "amd_fidelityfx_loader_vk.dll"
#define LIB_BESIDE "amd_fidelityfx_upscaler_vk.dll"   // what AMD's loader loads, from its own folder
#else
#include <dlfcn.h>
#define LIB_NAME "amd_fidelityfx_upscaler_vk.so"   // no AMD loader on Linux: the upscaler exports the entry points
// AMD's header marks its entry points __declspec(dllexport) on every compiler; they are only declared here.
#define __declspec(x)
#endif
#include "third_party/ffx_api/api/include/ffx_api.h"
#include "third_party/ffx_api/api/include/ffx_api_types.h"
#include "third_party/ffx_api/api/include/vk/ffx_api_vk.h"
#include "third_party/ffx_api/upscalers/include/ffx_upscale.h"
#ifndef _WIN32
#undef __declspec
#endif

#define GRAVE 4

// An image of this file's own: the library's inputs, its output and the unit exposure.
typedef struct {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    int laid_out;   // out of UNDEFINED: in GENERAL between evaluations
} Img;

// An upscale context and the descriptions it was made from: AMD's library keeps pointers into them until the context is
// destroyed, so they live in the same allocation.
typedef struct {
    struct ffxCreateBackendVKDesc backend;
    struct ffxOverrideVersion version;
    struct ffxCreateBackendVKDesc_DEPRECATED legacy_backend;   // Linux: the one a Vulkan FSR 4 build reads
    struct ffxCreateContextDescUpscale upscale;
    ffxContext context;
    uint32_t width, height;   // its output, which is also the most the render size can be
    int non_linear;           // the colour is perceptual (not an _SRGB or float format)
    int infinite;             // made for a depth with its far plane at infinity (FFX_UPSCALE_ENABLE_DEPTH_INFINITE)
} Context;

static struct {
    int state;   // 0 not tried, 1 loaded, -1 not available
    void *lib;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    uint64_t version_id;
    VkDeviceContext device_context;
    int ldr;   // the FSR4_LDR setting: the colour is taken as perceptual up to 1, whatever its format
    Context *context;
    struct { Context *c; uint64_t fence; } grave[GRAVE];   // old contexts, until the submission that may use them is done
    Img color, depth, motion, output, exposure;
    int passes;   // 1 the input and output passes are made, -1 they could not be
    VkDescriptorSetLayout prep_set;   // the input pass (shaders/fsr4_prep.comp)
    VkPipelineLayout prep_layout;
    VkPipeline prep;
    PFN_vkCmdPushDescriptorSetKHR push;
    int exposure_set;
    int failed;
    uint64_t frames, temporal, spatial, resets, last_ns;
} G;

static PlatMutex load_lock = PLAT_MUTEX_INIT;
static int has_descriptor_buffer = -1;   // VK_EXT_descriptor_buffer on the device (vk_device.c), -1 not asked for
#ifdef __linux__
static int has_linear_derivatives;

void vk_fsr4_linear_derivatives(int enabled) { has_linear_derivatives = enabled; }
#endif

static uint64_t frames_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

// The library's messages (ffxCreateContextDescUpscale.fpMessage). wchar_t is 16 bits wide on Windows and 32 on Linux,
// and the text is ASCII either way.
static void on_message(uint32_t type, const wchar_t *message) {
    char text[512];
    size_t n = 0;
    if (message)
        for (; message[n] && n < sizeof text - 1; n++) text[n] = message[n] < 0x80 ? (char)message[n] : '?';
    text[n] = 0;
    rt_log("fsr4: AMD's %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

// The folders the library is looked for in, in order: the fsr_dir setting, then the executable's. i: 0 or 1; 0 when
// there is no such folder.
static int library_dir(int i, char *out, size_t cap) {
    if (i == 0) {
        const char *e = cfg_get("TOTK_FSR_DIR");
        if (!e || !*e || strlen(e) >= cap) return 0;
        strcpy(out, e);
        return 1;
    }
    if (!plat_exe_path(out, cap)) return 0;
    char *sep = plat_last_sep(out);
    if (!sep) return 0;
    *sep = 0;
    return 1;
}

// The path of file in the i-th folder when the file is there.
static int library_file(int i, const char *file, char *out, size_t cap) {
    char dir[1024];
    if (!library_dir(i, dir, sizeof dir)) return 0;
    int n = snprintf(out, cap, "%s/%s", dir, file);
    return n > 0 && (size_t)n < cap && plat_file_exists(out);
}

static void *symbol(const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)G.lib, name);
#else
    return dlsym(G.lib, name);
#endif
}

// The library, the entry points, and the newest 4.x upscaler it lists for this device (the first of equal versions, so
// a provider's Auto, which picks the GPU's model, comes before INT8 and FP8). 1 when FSR 4 can run here; the log says
// why not otherwise. Once; the device must exist.
static int load(void) {
    VkCtx *c = vk_ctx();
    char path[1280], dirs[2100] = "";
    int found = 0;
    for (int i = 0; i < 2 && !found; i++) {
        char dir[1024];
        if (!library_dir(i, dir, sizeof dir)) continue;
        snprintf(dirs + strlen(dirs), sizeof dirs - strlen(dirs), "%s%s", *dirs ? ", " : "", dir);
        found = library_file(i, LIB_NAME, path, sizeof path);
    }
    if (!found)
        return rt_log("fsr4: no %s in %s (docs/dkb-dlss.md); FSR 3.1 runs instead", LIB_NAME,
                      *dirs ? dirs : "any folder (the executable's is unknown)"), 0;
#ifdef _WIN32
    // The upscaler DLL the loader picks lies beside it: the loader's folder first in the search. Those search flags
    // want an absolute path, which a relative fsr_dir is made into.
    char full[1280], beside[1400];
    if (plat_full_path(path, full, sizeof full)) strcpy(path, full);
    snprintf(beside, sizeof beside, "%s", path);
    char *sep = plat_last_sep(beside);
    if (sep && (size_t)(sep + 1 - beside) + sizeof LIB_BESIDE <= sizeof beside) strcpy(sep + 1, LIB_BESIDE);
    if (!plat_file_exists(beside))
        return rt_log("fsr4: %s is there but %s is not beside it; FSR 3.1 runs instead", path, LIB_BESIDE), 0;
    wchar_t wide[1280];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1280))
        return rt_log("fsr4: the library's path is not valid UTF-8; FSR 3.1 runs instead"), 0;
    G.lib = (void *)LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!G.lib)
        return rt_log("fsr4: %s was not loaded (Windows error %lu); FSR 3.1 runs instead", path,
                      (unsigned long)GetLastError()), 0;
#else
    G.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!G.lib) {
        const char *e = dlerror();
        return rt_log("fsr4: %s was not loaded: %s; FSR 3.1 runs instead", path, e ? e : "no reason given"), 0;
    }
#endif
    G.create = (PfnFfxCreateContext)symbol("ffxCreateContext");
    G.destroy = (PfnFfxDestroyContext)symbol("ffxDestroyContext");
    G.query = (PfnFfxQuery)symbol("ffxQuery");
    G.dispatch = (PfnFfxDispatch)symbol("ffxDispatch");
    if (!G.create || !G.destroy || !G.query || !G.dispatch)   // (the handle stays: unloading a vendor library is not safe)
        return rt_log("fsr4: %s lacks the FidelityFX API's entry points; FSR 3.1 runs instead", path), 0;
    rt_log("fsr4: loaded %s", path);
    const char *ldr = cfg_value("TOTK_FSR4_LDR");
    G.ldr = *ldr && strcmp(ldr, "0");
    G.device_context = (VkDeviceContext){c->device, c->phys, vkGetDeviceProcAddr};
    struct ffxQueryDescGetVersions v = {0};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    v.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    v.device = &G.device_context;
    uint64_t count = 0;
    v.outputCount = &count;
    if (G.query(NULL, &v.header) != FFX_API_RETURN_OK || !count)
        return rt_log("fsr4: AMD's library lists no upscaler for this GPU; FSR 3.1 runs instead"), 0;
    uint64_t *ids = calloc(count, sizeof *ids);
    const char **names = calloc(count, sizeof *names);
    int ok = 0;
    if (ids && names) {
        v.versionIds = ids;
        v.versionNames = names;
        if (G.query(NULL, &v.header) != FFX_API_RETURN_OK) {
            rt_log("fsr4: AMD's upscalers could not be listed; FSR 3.1 runs instead");
        } else {
            char listed[256] = "";
            unsigned best[3] = {0, 0, 0};
#ifdef __linux__
            const char *selected_name = "";
#endif
            for (uint64_t i = 0; i < count; i++) {
                const char *name = names[i] ? names[i] : "";
                size_t used = strlen(listed);
                snprintf(listed + used, sizeof listed - used, "%s%s", used ? ", " : "", name);
                unsigned u[3] = {0, 0, 0};
                sscanf(name, "%u.%u.%u", &u[0], &u[1], &u[2]);
                if (u[0] == 4 && (!G.version_id || u[1] > best[1] || (u[1] == best[1] && u[2] > best[2]))) {
                    memcpy(best, u, sizeof best);
                    G.version_id = ids[i];
#ifdef __linux__
                    selected_name = name;
#endif
                }
            }
            if (!G.version_id)
                rt_log("fsr4: no FSR 4 for this GPU (AMD's library has: %s); FSR 3.1 runs instead", listed);
#ifdef __linux__
            // fsr4-vk's Auto provider can select INT8 internally. Reject it before it creates a shader when the
            // device lacks linear derivatives. An explicit FP8 provider or another library keeps its own checks.
            else if (!has_linear_derivatives &&
                     (!strcmp(selected_name, "4.1.1 Vulkan Auto") || !strcmp(selected_name, "4.1.1 Vulkan INT8")))
                rt_log("fsr4: %s needs VK_KHR_compute_shader_derivatives with computeDerivativeGroupLinear enabled; "
                       "FSR 3.1 runs instead", selected_name);
#endif
            else
                ok = 1, rt_log("fsr4: AMD's upscalers: %s; using %u.%u.%u%s", listed, best[0], best[1], best[2],
                               G.ldr ? " (FSR4_LDR: perceptual colour up to 1)" : "");
        }
    }
    free(ids), free(names);
    return ok;
}

// FSR 4 can run here (asked once, the first time the setting names it; the device exists by then).
static int usable(void) {
    if (__atomic_load_n(&G.state, __ATOMIC_ACQUIRE)) return G.state > 0;
    if (!vk_ctx()) return 0;
    plat_mutex_lock(&load_lock);
    if (!G.state) __atomic_store_n(&G.state, load() ? 1 : -1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&load_lock);
    return G.state > 0;
}

// The setting is read once a frame (the draws ask for it thousands of times).
int vk_fsr4_wanted(void) {
    static uint64_t seen = ~0ull;
    static int chosen;
    uint64_t f = frames_now();
    if (f != __atomic_load_n(&seen, __ATOMIC_RELAXED)) {
        __atomic_store_n(&chosen, !strcmp(cfg_value("TOTK_UPSCALER"), "fsr4"), __ATOMIC_RELAXED);
        __atomic_store_n(&seen, f, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&chosen, __ATOMIC_RELAXED) && !__atomic_load_n(&G.failed, __ATOMIC_RELAXED) && usable();
}

static int fail(const char *why) {
    if (!__atomic_load_n(&G.failed, __ATOMIC_RELAXED)) rt_log("fsr4: %s; FSR 3.1 runs", why);
    __atomic_store_n(&G.failed, 1, __ATOMIC_RELAXED);
    return 0;
}

// ---- the device: VK_EXT_descriptor_buffer

void vk_fsr4_descriptor_buffer(int enabled) { has_descriptor_buffer = enabled; }

int vk_fsr4_wants_descriptor_buffer(void) {
#ifdef _WIN32
    return 0;   // AMD's own library asks for nothing the device does not have on already
#else
    // A Vulkan FSR 4 build such as fsr4-vk records with descriptor buffers, which the device has to enable when it is
    // created (Longshot vk_fsr4.cpp fsr4_wants_descriptor_buffer): only with upscaler = fsr4 at the start and the
    // library in one of its folders.
    if (strcmp(cfg_value("TOTK_UPSCALER"), "fsr4")) return 0;
    char path[1280];
    return library_file(0, LIB_NAME, path, sizeof path) || library_file(1, LIB_NAME, path, sizeof path);
#endif
}

// ---- the contexts

static void destroy_context(Context *c) {
    if (!c) return;
    if (c->context) G.destroy(&c->context, NULL);
    free(c);
}

// Old contexts that no submission can still use go.
static void sweep(void) {
    uint64_t done = vk_sync_fence_done();
    for (int i = 0; i < GRAVE; i++)
        if (G.grave[i].c && G.grave[i].fence <= done) destroy_context(G.grave[i].c), G.grave[i].c = NULL;
}

// The current context is let go: destroyed once the submission being recorded is done. 0 when no slot of the graveyard
// is free (sizes changing every frame): the context stays, and FSR 4 skips this frame, since a context is never
// destroyed while a submission may still use it.
static int retire(uint64_t fence) {
    if (!G.context) return 1;
    int slot = 0;
    while (slot < GRAVE && G.grave[slot].c) slot++;
    if (slot == GRAVE) return 0;
    G.grave[slot].c = G.context, G.grave[slot].fence = fence;
    G.context = NULL;
    return 1;
}

// A context for outputs of w x h, any render size up to that (the game's dynamic resolution changes it every so often: one
// context takes them all).
// Float and _SRGB colour is not perceptual data the upscaler should treat as gamma coded.
static int perceptual(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_B8G8R8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return 0;
    default:
        return 1;
    }
}

static Context *make_context(uint32_t w, uint32_t h, int non_linear, int infinite) {
    Context *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    c->backend.deviceContext = G.device_context;
    c->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    c->version.versionId = G.version_id;
    c->backend.header.pNext = &c->version.header;
#ifndef _WIN32
    // A Vulkan FSR 4 build such as fsr4-vk looks for the SDK's older backend description and finds no device without it
    // (creation then returns FFX_API_RETURN_ERROR_PARAMETER): both stay chained (Longshot vk_fsr4.cpp make_context).
    c->legacy_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    c->legacy_backend.vkDevice = G.device_context.vkDevice;
    c->legacy_backend.vkPhysicalDevice = G.device_context.vkPhysicalDevice;
    c->legacy_backend.vkDeviceProcAddr = G.device_context.vkDeviceProcAddr;
    c->version.header.pNext = &c->legacy_backend.header;
#endif
    c->upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    c->upscale.header.pNext = &c->backend.header;
    // The scene's colour is the game's LDR, perceptual image (no auto exposure); the depth is reversed by the input
    // pass (1 - d), and its far plane is at infinity when the projection says so.
    c->non_linear = non_linear;
    c->infinite = infinite;
    c->upscale.flags = (non_linear ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0) | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
                       FFX_UPSCALE_ENABLE_DEPTH_INVERTED | (infinite ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0);
    c->upscale.maxRenderSize = (struct FfxApiDimensions2D){w, h};
    c->upscale.maxUpscaleSize = (struct FfxApiDimensions2D){w, h};
    c->upscale.fpMessage = &on_message;
    ffxReturnCode_t r = G.create(&c->context, &c->upscale.header, NULL);
    if (r != FFX_API_RETURN_OK || !c->context) {
        rt_log("fsr4: AMD's upscaler was not made (%d) for %ux%u%s", (int)r, w, h,
               has_descriptor_buffer == 0 ? "; VK_EXT_descriptor_buffer is not enabled on this device"
               : has_descriptor_buffer < 0 ? "; VK_EXT_descriptor_buffer was not asked for: set upscaler = fsr4 before the start"
                                           : "");
        c->context = NULL;
        free(c);
        return NULL;
    }
    c->width = w, c->height = h;
    struct ffxQueryGetProviderVersion pv = {0};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    int known = G.query(&c->context, &pv.header) == FFX_API_RETURN_OK && pv.versionName;
    rt_log("fsr4: FSR %s made for outputs of %ux%u (depth inverted, far plane %s, %s colour)", known ? pv.versionName : "?", w, h,
           infinite ? "infinite" : "finite", non_linear ? "perceptual" : "linear or sRGB-decoded");
    return c;
}

// ---- the input and output passes, the images

static void drop_image(Img *t) {   // freed once the submissions recorded so far are done (vk_defer_*)
    if (t->view) vk_defer_view(t->view);
    if (t->image) vk_defer_image(t->image, t->alloc);
    memset(t, 0, sizeof *t);
}

static int make_image(Img *t, VkFormat format, uint32_t width, uint32_t height) {
    VkCtx *c = vk_ctx();
    drop_image(t);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
                            .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &t->image, &t->alloc, NULL) != VK_SUCCESS) {
        t->image = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &t->view) != VK_SUCCESS) {
        t->view = VK_NULL_HANDLE;
        return 0;
    }
    t->format = format, t->width = width, t->height = height;
    return 1;
}

// A Vulkan FSR 4 build wants a 1x1 exposure texture of 1 when AMD's own auto exposure is not on, which it never is
// here; AMD's library on Windows takes none (Longshot vk_fsr4.cpp unit_exposure).
static int unit_exposure(void) {
#ifdef _WIN32
    return 0;
#else
    return 1;
#endif
}

// The library's inputs for the render size w x h and its output for ow x oh, made again when a size changes (the
// library's history is its own, so a reset is not needed: the pass writes every pixel the library reads).
static int images(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    if (!G.color.image || G.color.width != w || G.color.height != h) {
        if (!make_image(&G.color, VK_FORMAT_R16G16B16A16_SFLOAT, w, h) || !make_image(&G.depth, VK_FORMAT_R32_SFLOAT, w, h) ||
            !make_image(&G.motion, VK_FORMAT_R16G16_SFLOAT, w, h)) {
            drop_image(&G.color), drop_image(&G.depth), drop_image(&G.motion);
            return 0;
        }
    }
    if (!G.output.image || G.output.width != ow || G.output.height != oh) {
        if (!make_image(&G.output, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh)) return drop_image(&G.output), 0;
    }
    if (unit_exposure() && !G.exposure.image) {
        if (!make_image(&G.exposure, VK_FORMAT_R32_SFLOAT, 1, 1)) return drop_image(&G.exposure), 0;
        G.exposure_set = 0;
    }
    return 1;
}

// The input pass's pipeline (shaders/fsr4_prep.comp): three sampled images in, three storage images out, push
// descriptors, the render size as its push constant. 1 when it is made.
static int make_prep(void) {
    if (G.prep) return 1;
    VkDevice d = vk_ctx()->device;
    G.push = vk_pipeline_push_fn();
    if (!G.push) return 0;
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                              VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = 6, .pBindings = b};
    if (vkCreateDescriptorSetLayout(d, &li, NULL, &G.prep_set) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.prep_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(d, &pi, NULL, &G.prep_layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fsr4_prep_spv,
                                   .pCode = fsr4_prep_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.prep_layout};
    VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &G.prep);
    vkDestroyShaderModule(d, m, NULL);
    if (r != VK_SUCCESS) G.prep = VK_NULL_HANDLE;
    return G.prep != VK_NULL_HANDLE;
}

// The input pass: scene (the upscaler's input), zv (the scene depth) and mv (the camera motion vectors, in an image
// that may be larger than the render size) into color (RGBA16F), depth (R32F, 1 - d) and motion (RG16F), storage views
// of images in GENERAL at the render size w x h. The recorder takes the push like vk_fsr3.c's passes.
static void prep(VkCommandBuffer cb, VkImageView scene, VkImageView zv, VkImageView mv, uint32_t w, uint32_t h) {
    const int32_t size[2] = {(int32_t)w, (int32_t)h};
    VkDescriptorImageInfo ii[6] = {{VK_NULL_HANDLE, scene, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, zv, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, G.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, G.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, G.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[6];
    for (uint32_t i = 0; i < 6; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1,
                                       .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.prep);
    rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.prep_layout, 0, 6, wr);
    vkCmdPushConstants(cb, G.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, size);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
}

// A view of one of the game's images for this frame only (it goes when the submission is done).
static VkImageView view_of(VkImage img, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageViewUsageCreateInfo u = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &u, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(vk_ctx()->device, &vi, NULL, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

// The FidelityFX surface format of a Vulkan one (ffxApiGetSurfaceFormatVK is C++ only in the SDK's header); UNKNOWN for
// the formats the upscaler's inputs and outputs do not come in.
static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R32_UINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static int is_depth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static int has_stencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// An image as the dispatch names it (the SDK's ffxApiGetImageResourceDescriptionVK and ffxApiGetResourceVK).
static struct FfxApiResource resource(VkImage image, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                                      uint32_t state) {
    struct FfxApiResource r = {0};
    r.resource = (void *)image;
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = surface_format(fmt);
    r.description.width = w, r.description.height = h, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY | (is_depth(fmt) ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0) |
                          (has_stencil(fmt) ? FFX_API_RESOURCE_USAGE_STENCILTARGET : 0) |
                          ((usage & VK_IMAGE_USAGE_STORAGE_BIT) ? FFX_API_RESOURCE_USAGE_UAV : 0);
    return r;
}

static VkImageMemoryBarrier2 barrier(const Img *t, VkAccessFlags2 src, VkAccessFlags2 dst, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = src,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = dst,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->image,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}

static void barriers(VkCommandBuffer cb, const VkImageMemoryBarrier2 *b, uint32_t n) {
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n, .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &di);
}

// The scene's camera from its projection (row-major, OpenGL style: the depth buffer holds (ndc z + 1) / 2, as in
// vk_fsr3.c vk_fsr3_camera_factors): the near and far distances and the vertical field of view. With a view distance v in front of the
// camera the stored depth is d = a + b / v, a = (1 - p[10]) / 2 and b = p[11] / 2 (b < 0: 0 at the near plane), so the near
// distance is -b / a and the far one b / (1 - a); an infinite projection (a = 1) has its far plane at FLT_MAX and sets
// *infinite. 0 for a matrix that is not a perspective projection. (Longshot gpu/vk/fsr3_math.h camera_from_projection.)
static int camera(const float p[16], float *near_z, float *far_z, float *fov_y, int *infinite) {
    if (p[12] != 0.0f || p[13] != 0.0f || p[14] != -1.0f || p[15] != 0.0f) return 0;
    if (!(p[0] > 0.0f) || !(p[5] > 0.0f) || !isfinite(p[0]) || !isfinite(p[5])) return 0;
    *fov_y = 2.0f * atanf(1.0f / p[5]);
    *near_z = 1.0f, *far_z = 5000.0f;   // (the game's own, when the matrix gives no sane ones)
    *infinite = 0;
    if (p[11] > 0.0f) return 0;   // as vk_fsr3.c vk_fsr3_camera_factors: depth 0 is at the near plane
    double a = (1.0 - (double)p[10]) * 0.5;
    double n = (double)p[11] / ((double)p[10] - 1.0), f = (double)p[11] / ((double)p[10] + 1.0);
    if (isfinite(n) && n > 0.0) {
        *near_z = (float)n;
        if (a >= 1.0 - 1e-6) *far_z = 3.4e38f, *infinite = 1;   // the far plane at infinity
        else if (isfinite(f) && f > n) *far_z = (float)f;
        else if (*far_z <= *near_z) *far_z = *near_z * 5000.0f;
    }
    return 1;
}

static float frame_ms(void) {
    uint64_t now = kern_now_ns(), prev = G.last_ns;
    G.last_ns = now;
    if (!prev) return 1000.0f / 30.0f;
    float ms = (float)(now - prev) / 1e6f;
    return ms < 0.0f ? 0.0f : ms > 1000.0f ? 1000.0f : ms;
}

// A frame that goes to the game's upscaler after the inputs were made (no scene the library can use, no context this
// frame): the jitter and the camera are reset for the next frame. -1 when the motion pass may have bound its own
// pipeline (the caller binds the game's again), else 0.
static int give_up(VkTemporalIn *t, uint32_t w, uint32_t h, uint32_t ow) {
    int bound = t->temporal;
    t->temporal = 0;   // no jitter for the next frame either
    vk_temporal_done(t, w, h, ow);
    G.spatial++;
    return bound ? -1 : 0;
}

int vk_fsr4_record(VkCommandBuffer cb, uint64_t fence, const VkTexImage *in, const VkTexImage *out) {
    if (!vk_fsr4_wanted()) return 0;
    uint32_t w = in->width, h = in->height, ow = out->width, oh = out->height;
    if (w < 32 || h < 32 || w > ow || h > oh) {   // this pair of images only: the game's upscaler runs for it
        static int logged;
        if (!logged++) rt_log("fsr4: unsupported size %ux%u -> %ux%u: the game's upscaler runs for it", w, h, ow, oh);
        return 0;
    }
    if (!G.passes) {   // the input pass and FSR 3.1's output pass, once (here: the thread that records)
        G.passes = vk_fsr3_io_init() && make_prep() ? 1 : -1;
        if (G.passes < 0) fail("the input and output passes could not be made (no push descriptors?)");
    }
    if (G.passes < 0) return 0;
    sweep();
    // Everything that can fail without a scene comes before vk_temporal_inputs, so a 0 never follows recorded commands
    // (the motion pass binds its own pipeline, and on 0 the game's upscaler would dispatch on it).
    VkImageView iv = view_of(in->image, in->format, VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView ov = view_of(out->image, out->format, VK_IMAGE_USAGE_STORAGE_BIT);
    vk_defer_view(iv), vk_defer_view(ov);   // views of the game's images for this frame only: they go when it is done
    if (!iv || !ov) return fail("no sampled view of the input or storage view of the output");
    VkTemporalIn t;
    if (!vk_temporal_inputs(cb, w, h, ow, oh, 0, &t)) return fail("its motion-vector image could not be made");
    float near_z, far_z, fov_y;
    int infinite;
    // Without a scene at the input size (a menu, a cutscene's own passes), a projection FSR cannot use or a depth view
    // of a level or layer other than the first, the game's upscaler runs this frame.
    if (!t.temporal || !camera(t.proj, &near_z, &far_z, &fov_y, &infinite) || t.depth_level || t.depth_layer)
        return give_up(&t, w, h, ow);
    int reset_ctx = 0;
    const int nl = G.ldr || perceptual(in->format);
    static int fmt_logged;
    if (!fmt_logged++) rt_log("fsr4: colour format %d, %s", (int)in->format, nl ? "perceptual" : "linear or sRGB-decoded");
    if (!G.context || G.context->width != ow || G.context->height != oh || G.context->non_linear != nl ||
        G.context->infinite != infinite) {
        // The old context is never destroyed under a submission that may use it: with every slot of the graveyard still
        // in flight, the game's upscaler (or FSR 3.1) runs this frame and the next one tries again.
        if (!retire(fence)) {
            static int logged;
            if (!logged++) rt_log("fsr4: every old context is still in flight: the game's upscaler runs until one is done");
            return give_up(&t, w, h, ow);
        }
        G.context = make_context(ow, oh, nl, infinite);
        if (!G.context) {
            fail("its upscaler could not be made");
            return give_up(&t, w, h, ow);
        }
        reset_ctx = 1;
    }
    if (!images(w, h, ow, oh)) {
        fail("its images could not be made");
        return give_up(&t, w, h, ow);
    }

    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL, RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAccessFlags2 any = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    const int reset = t.reset || reset_ctx;
    // The caller's full barrier came before this (vk_exec_draw.c op_dispatch): the game's images are in GENERAL and
    // their writes are visible. Ours are in GENERAL between evaluations; new ones get there from nothing.
    Img *all[5] = {&G.color, &G.depth, &G.motion, &G.output, &G.exposure};
    const uint32_t n = G.exposure.image ? 5 : 4;
    VkImageMemoryBarrier2 first[5];
    for (uint32_t i = 0; i < n; i++) {
        first[i] = barrier(all[i], any, any, all[i]->laid_out ? GEN : VK_IMAGE_LAYOUT_UNDEFINED, GEN);
        all[i]->laid_out = 1;
    }
    barriers(cb, first, n);
    if (G.exposure.image && !G.exposure_set) {   // the 1x1 exposure of 1, written once
        const VkClearColorValue one = {.float32 = {1.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, G.exposure.image, GEN, &one, 1, &range);
        G.exposure_set = 1;
    }
    prep(cb, iv, t.depth, t.mv, w, h);
    // The library reads its inputs in SHADER_READ_ONLY_OPTIMAL and writes its output in GENERAL (the API guide of
    // FidelityFX SDK 2.4, docs/getting-started/ffx-api.md).
    VkImageMemoryBarrier2 before[5] = {barrier(&G.color, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, GEN, RO),
                                       barrier(&G.depth, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, GEN, RO),
                                       barrier(&G.motion, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, GEN, RO),
                                       barrier(&G.output, any, any, GEN, GEN), {0}};
    if (n == 5) before[4] = barrier(&G.exposure, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, GEN, RO);
    barriers(cb, before, n);

    struct ffxDispatchDescUpscale d = {0};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = resource(G.color.image, G.color.format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(G.depth.image, G.depth.format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(G.motion.image, G.motion.format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT,
                               FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(G.output.image, G.output.format, ow, oh, VK_IMAGE_USAGE_STORAGE_BIT,
                        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    if (G.exposure.image)
        d.exposure = resource(G.exposure.image, G.exposure.format, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.jitterOffset = (struct FfxApiFloatCoords2D){t.jx, t.jy};   // render pixels, as the projection was offset: DLSS's
    d.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};   // render pixels already, towards the frame before
    d.renderSize = (struct FfxApiDimensions2D){w, h};
    d.upscaleSize = (struct FfxApiDimensions2D){ow, oh};
    d.enableSharpening = false;   // the Sharpness row sharpens at the window, after the HUD
    d.frameTimeDelta = frame_ms();
    d.preExposure = 1.0f;
    d.reset = reset;
    d.cameraNear = near_z, d.cameraFar = far_z, d.cameraFovAngleVertical = fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = nl ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;
    rec_drain();   // AMD records into cb on this thread: everything queued for the recorder first
    ffxReturnCode_t r = G.dispatch(&G.context->context, &d.header);

    VkImageMemoryBarrier2 after[5] = {barrier(&G.color, VK_ACCESS_2_SHADER_READ_BIT, any, RO, GEN),
                                      barrier(&G.depth, VK_ACCESS_2_SHADER_READ_BIT, any, RO, GEN),
                                      barrier(&G.motion, VK_ACCESS_2_SHADER_READ_BIT, any, RO, GEN),
                                      barrier(&G.output, any, any, GEN, GEN), {0}};
    if (n == 5) after[4] = barrier(&G.exposure, VK_ACCESS_2_SHADER_READ_BIT, any, RO, GEN);
    barriers(cb, after, n);
    if (r != FFX_API_RETURN_OK) {
        char why[64];
        snprintf(why, sizeof why, "its dispatch failed (%d)", (int)r);
        fail(why);
        give_up(&t, w, h, ow);
        return -1;   // it may have bound its own pipeline: the caller binds the game's again for this frame's upscale
    }
    vk_fsr3_io_out(cb, G.output.view, ov, ow, oh);   // the caller's full barrier follows
    t.reset = reset;
    vk_temporal_done(&t, w, h, ow);
    vk_fsr3fi_note_picture(cb, out);   // the picture before the game's HUD, for AMD's frame interpolation
    static int first_logged;
    if (!first_logged++)
        rt_log("fsr4: input pass in use: colour RGBA16F, depth 1 - d R32F (inverted%s), motion RG16F, all %ux%u; output through "
               "FSR 3.1's output pass at %ux%u; near %.3g far %.3g, vertical field of view %.1f degrees; exposure %s",
               infinite ? ", far plane infinite" : "", w, h, ow, oh, (double)near_z, (double)far_z, (double)fov_y * 57.29578,
               G.exposure.image ? "1x1 unit image" : "none");
    G.frames++, G.temporal++, G.resets += (uint64_t)d.reset;
    if (G.frames == 1 || G.frames % 1800 == 0)
        rt_log("fsr4: %llu frames, %llu temporal, %llu resets; %ux%u -> %ux%u, jitter %+.3f %+.3f px of %u phases",
               (unsigned long long)G.frames, (unsigned long long)G.temporal, (unsigned long long)G.resets, w, h, ow, oh,
               t.jx, t.jy, t.phases);
    return 1;
}

// Whether there is anything to give back: the context, one waiting in the grave, or any of the images of this file's own
// (the input pass's colour, depth and motion, the output, the unit exposure).
int vk_fsr4_holding(void) {
    if (G.context || G.color.image || G.depth.image || G.motion.image || G.output.image || G.exposure.image) return 1;
    for (int i = 0; i < GRAVE; i++)
        if (G.grave[i].c) return 1;
    return 0;
}

// The setting has chosen another upscaler for a while (vk_dlss.c vk_temporal_idle_tick). Two things go. The images of
// this file's own (the input pass's colour, depth and motion, the output, the exposure) go through drop_image, which is
// vk_defer_image and vk_defer_view: freed once the submissions recorded so far are done. AMD's context, which holds the
// library's own images and pipelines, is retired as one replaced by a new output size is (retire): destroyed in the
// grave once the submission being recorded is done (sweep), the same ffxDestroyContext call a resize already makes
// during play. The library stays loaded, and so do the input pass's pipeline and its layouts, as FSR 3.1's
// pipelines do (a pipeline has no deferred free and holds little). What a long idle adds is that the images and the
// context made again cost what images and make_context log, at the frame that chooses FSR 4 again; that frame's reset
// is set (reset_ctx). The depth and motion that vk_dlss.c's temporal inputs hold are freed by its own tick. A full grave
// leaves the context in place and the next frame asks again; the images go either way.
void vk_fsr4_idle_release(uint64_t fence) {
    sweep();
    if (vk_fsr4_wanted()) return;
    int images = G.color.image || G.depth.image || G.motion.image || G.output.image || G.exposure.image;
    Img *all[] = {&G.color, &G.depth, &G.motion, &G.output, &G.exposure};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) drop_image(all[i]);
    G.exposure_set = 0;
    int context = 0;
    if (G.context) {
        for (int i = 0; i < GRAVE && !context; i++)
            if (!G.grave[i].c) context = retire(fence);   // retire would destroy the oldest entry early when every slot is in flight
    }
    if (images || context)
        rt_log("fsr4: not used for %u frames: %s%s%s let go; the next upscale with FSR 4 makes them again", TEMPORAL_IDLE_FRAMES,
               images ? "its images" : "", images && context ? " and " : "", context ? "AMD's context" : "");
}

void vk_fsr4_release(void) {   // device idle
    VkCtx *c = vk_ctx();
    VkDevice dev = c ? c->device : VK_NULL_HANDLE;
    if (G.state > 0)
        rt_log("fsr4: %llu frames, %llu temporal, %llu resets (%llu left to the game's upscaler)", (unsigned long long)G.frames,
               (unsigned long long)G.temporal, (unsigned long long)G.resets, (unsigned long long)G.spatial);
    for (int i = 0; i < GRAVE; i++) destroy_context(G.grave[i].c), G.grave[i].c = NULL;
    destroy_context(G.context);
    G.context = NULL;
    Img *all[] = {&G.color, &G.depth, &G.motion, &G.output, &G.exposure};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (dev && all[i]->view) vkDestroyImageView(dev, all[i]->view, NULL);
        if (dev && all[i]->image) vmaDestroyImage(c->vma, all[i]->image, all[i]->alloc);
        memset(all[i], 0, sizeof *all[i]);
    }
    if (dev && G.prep) vkDestroyPipeline(dev, G.prep, NULL);
    if (dev && G.prep_layout) vkDestroyPipelineLayout(dev, G.prep_layout, NULL);
    if (dev && G.prep_set) vkDestroyDescriptorSetLayout(dev, G.prep_set, NULL);
    G.prep = VK_NULL_HANDLE, G.prep_layout = VK_NULL_HANDLE, G.prep_set = VK_NULL_HANDLE;
    G.passes = 0, G.exposure_set = 0;
    has_descriptor_buffer = -1;
#ifdef __linux__
    has_linear_derivatives = 0;
#endif
}
