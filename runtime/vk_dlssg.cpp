// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA DLSS Frame Generation (DLSS-G) through NGX's D3D12 side, for GPUs whose NGX Vulkan side offers none. NGX's
// Vulkan side offers frame generation only from the RTX 40 series on (NGX decides). So the frames are made on a D3D12
// device of our own, on the Vulkan device's adapter, from images both APIs share.
//
// The order of one 2x frame (vk_framegen.c record, frame_gen_by = dlss):
//
//   the frame generation command buffer (Vulkan): this frame, its picture without the HUD, the scene's depth (through
//     shaders/dlssg_depth.comp: a depth aspect cannot be copied into a colour image) and its motion, into the shared
//     images (vk_dlssg_record)
//   that command buffer's submission, which signals the shared fence at v_in (vk_dlssg_submit)
//   the D3D12 queue: waits for v_in, runs NGX's evaluation into the shared output, signals v_out
//   a second Vulkan submission: waits for v_out, copies the shared output into the made frame
//
// A semaphore wait holds back that submission's commands. The order against what follows comes from the closing
// vk_full_barrier in the copy-back command buffer recorded in vk_dlssg_submit: its second scope covers every later
// submission on the queue, so the present of the made frame comes after the copy. Between the two APIs the
// shared images rest in D3D12's COMMON state and Vulkan's GENERAL layout, owned by VK_QUEUE_FAMILY_EXTERNAL.
//
// Written without NVIDIA's headers: the exports are taken with GetProcAddress and the parameter object is called
// through its MSVC vtable, as vk_dlss.c does. The camera NGX is given (camera, put) is adapted from Hookshot's
// gpu/vk/vk_dlss.cpp (dlss_fg_inputs, fg_put), GPL-3.0-or-later; see THIRD_PARTY_NOTICES.md.
#include "vk_dlssg.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vulkan/vulkan_win32.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "dlssg_depth_spv.h"

extern "C" {
// host.h, cfg.h and vk.h pull in C-only headers: the functions needed from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *cfg_value(const char *name);
void vk_queue_lock(void);
void vk_queue_unlock(void);
}

namespace {

typedef uint32_t NgxResult;
bool ngx_failed(NgxResult r) { return (r & 0xFFF00000u) == 0xBAD00000u; }
constexpr uint32_t kNgxVersionApi = 0x15;   // NVSDK_NGX_Version_API of DLSS SDK 310.9, as vk_dlss.c gives it
constexpr unsigned long long kAppId = 231313132ull;   // vk_dlss.c init: the same identity on both sides
constexpr int kFrameGeneration = 11;   // NVSDK_NGX_Feature_FrameGeneration
constexpr int kSlots = 3;              // D3D12 command lists and Vulkan copy-back buffers in flight

struct NgxParam;    // NVSDK_NGX_Parameter, a C++ class built by MSVC: virtual methods only
struct NgxHandle;   // NVSDK_NGX_Handle, opaque
typedef NgxResult (*PfnInit)(unsigned long long, const wchar_t *, ID3D12Device *, uint32_t, const void *);
typedef NgxResult (*PfnParams)(NgxParam **);
typedef NgxResult (*PfnDestroy)(NgxParam *);
// _nvngx.dll's Shutdown1 takes a second argument that NVIDIA's header leaves out (its static library passes it): an
// out pointer through which the DLL stores how many NGX instances are left, unconditionally. Called with the
// device alone it writes through whatever the second argument register holds (dkb.exe crashed in the D3D12 one
// writing address 0), so every call passes the address of a scratch value.
typedef NgxResult (*PfnShutdown)(ID3D12Device *, unsigned *left);
// CreateFeature and EvaluateFeature as the DLL exports them (four arguments each; the progress callback is NULL)
typedef NgxResult (*PfnCreate)(ID3D12GraphicsCommandList *, int, NgxParam *, NgxHandle **);
typedef NgxResult (*PfnEvaluate)(ID3D12GraphicsCommandList *, const NgxHandle *, const NgxParam *, void *);
typedef NgxResult (*PfnRelease)(NgxHandle *);

// The parameter object's vtable, overloads in reverse declaration order (vk_dlss.c slot): Set(name, T) at slots 0 to 7
// for void *, ID3D12Resource *, ID3D11Resource *, int, unsigned, double, float and unsigned long long, Get(name, T *)
// eight slots later. A name NGX does not know leaves the value as given.
void *slot(const NgxParam *p, int i) {
    return (*(void *const *const *)p)[i];
}

int get_i(NgxParam *p, const char *n) {
    int v = 0;
    ((NgxResult (*)(NgxParam *, const char *, int *))slot(p, 11))(p, n, &v);
    return v;
}

void set_ptr(NgxParam *p, const char *n, void *v) {
    ((NgxResult (*)(NgxParam *, const char *, void *))slot(p, 0))(p, n, v);
}

void set_res(NgxParam *p, const char *n, ID3D12Resource *v) {
    ((NgxResult (*)(NgxParam *, const char *, ID3D12Resource *))slot(p, 1))(p, n, v);
}

void set_u(NgxParam *p, const char *n, unsigned v) {
    ((NgxResult (*)(NgxParam *, const char *, unsigned))slot(p, 4))(p, n, v);
}

void set_f(NgxParam *p, const char *n, float v) {
    ((NgxResult (*)(NgxParam *, const char *, float))slot(p, 6))(p, n, v);
}

void set_ull(NgxParam *p, const char *n, unsigned long long v) {
    ((NgxResult (*)(NgxParam *, const char *, unsigned long long))slot(p, 7))(p, n, v);
}

template <class T> void drop(T *&p) {
    if (p) p->Release(), p = nullptr;
}

// The folder of dkb.exe with a trailing backslash, or false.
bool exe_dir(wchar_t *dir, size_t cap) {
    DWORD n = GetModuleFileNameW(nullptr, dir, (DWORD)cap);
    if (!n || n >= cap) return false;
    wchar_t *cut = wcsrchr(dir, L'\\');
    if (!cut) return false;
    cut[1] = 0;
    return true;
}

// The D3D12 device on the Vulkan device's adapter, found by its LUID. d3d12.dll and dxgi.dll stay loaded: the driver
// keeps references into them. why says what was missing.
ID3D12Device *d3d12_device(const LUID &luid, char *name, size_t name_cap, const char **why) {
    static HMODULE d3d12 = LoadLibraryW(L"d3d12.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    typedef HRESULT(WINAPI * CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    typedef HRESULT(WINAPI * CreateFactory)(REFIID, void **);
    CreateDevice create_device = d3d12 ? (CreateDevice)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    CreateFactory create_factory = dxgi ? (CreateFactory)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!create_device || !create_factory) return *why = "no D3D12 on this system", nullptr;
    IDXGIFactory4 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    if (FAILED(create_factory(IID_PPV_ARGS(&factory)))) return *why = "no DXGI factory", nullptr;
    HRESULT ha = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    drop(factory);
    if (FAILED(ha)) return *why = "DXGI has no adapter with the Vulkan device's LUID", nullptr;
    DXGI_ADAPTER_DESC1 desc;
    if (name && SUCCEEDED(adapter->GetDesc1(&desc)))
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)name_cap, nullptr, nullptr);
    ID3D12Device *device = nullptr;
    HRESULT hd = create_device(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    drop(adapter);
    if (FAILED(hd)) return *why = "no D3D12 device on the Vulkan device's adapter", nullptr;
    return device;
}

// ---- the route -------------------------------------------------------------------------------------------------------

// An image both APIs see: a D3D12 committed resource, shared, and the same memory imported into a Vulkan image.
struct Shared {
    ID3D12Resource *res = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;   // the depth image's storage view (the depth pass writes it)
    uint32_t w = 0, h = 0;
    bool fresh = true;   // its Vulkan layout still UNDEFINED: GENERAL from the first use
};

struct Ngx {
    PfnInit init;
    PfnParams caps_of, alloc;
    PfnDestroy destroy;
    PfnShutdown shutdown;
    PfnCreate create;
    PfnEvaluate evaluate;
    PfnRelease release;
};

struct State {
    // the Vulkan side, from vk_dlssg_device
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice vk = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem{};
    LUID luid{};
    bool luid_ok = false, interop = false;
    // started at the first set (given up: g_dead below)
    bool started = false, ok = false, ngx_on = false;
    Ngx f{};
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    ID3D12CommandAllocator *alloc[kSlots]{};
    ID3D12GraphicsCommandList *list[kSlots]{};
    uint64_t list_done[kSlots]{};   // the fence value its last run signals
    int slot = 0;
    PFN_vkGetMemoryWin32HandlePropertiesKHR handle_props = nullptr;
    PFN_vkCmdPushDescriptorSetKHR push = nullptr;
    VkSemaphore shared_sem = VK_NULL_HANDLE;   // the D3D12 fence, imported as a timeline semaphore
    VkCommandPool pool[kSlots]{};
    VkCommandBuffer back[kSlots]{};
    VkSemaphore back_sem = VK_NULL_HANDLE;     // the copies back's own timeline
    uint64_t back_value = 0, back_done[kSlots]{};
    // the depth pass (shaders/dlssg_depth.comp)
    VkSampler nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    NgxParam *params = nullptr;
    NgxHandle *feature = nullptr;
    uint32_t fw = 0, fh = 0, frw = 0, frh = 0;
    Shared color, hudless, depth, motion, out;
    // the set vk_dlssg_record recorded, sent by vk_dlssg_submit
    bool pending = false;
    int pending_slot = 0;
    VkImage pending_out = VK_NULL_HANDLE;
    uint64_t last_sequence = 0, sets = 0, resets = 0, with_hudless = 0;
};
State S;

// Given up for the run (one log line says why). Apart from State because two threads read it: the replay thread asks
// vk_dlssg_wanted whether to note the inputs (vk_fsr3fi.c inputs_wanted), while the present thread gives up. It
// guards no other data, so a relaxed load is enough.
std::atomic<bool> g_dead{false};

void give_up(const char *why) {
    if (!g_dead.exchange(true, std::memory_order_relaxed))
        rt_log("dlssg: %s; DLSS Frame Generation is off for this run, the built-in interpolation makes the frames", why);
    S.ok = false;
}

// The D3D12 queue past value (a CPU wait, 5 s at most). False when it is not: nothing it may still use is reset or
// freed then, and the route is given up. The fence is read again after each wake: a wait that timed out before leaves
// the auto-reset event set when its value comes late, so a wake alone does not say this value is reached.
bool d3d12_wait(uint64_t value) {
    if (!value || !S.fence || S.fence->GetCompletedValue() >= value) return true;
    const ULONGLONG until = GetTickCount64() + 5000;
    if (FAILED(S.fence->SetEventOnCompletion(value, S.event))) return give_up("the D3D12 fence takes no event"), false;
    while (S.fence->GetCompletedValue() < value) {
        const ULONGLONG now = GetTickCount64();
        if (now >= until || WaitForSingleObject(S.event, (DWORD)(until - now)) == WAIT_FAILED) {
            if (S.fence->GetCompletedValue() >= value) break;
            return give_up("the D3D12 queue did not finish a frame in 5 s"), false;
        }
    }
    return true;
}

void free_shared(Shared &im) {
    if (im.view) vkDestroyImageView(S.vk, im.view, nullptr);
    if (im.image) vkDestroyImage(S.vk, im.image, nullptr);
    if (im.mem) vkFreeMemory(S.vk, im.mem, nullptr);
    drop(im.res);
    im = Shared{};
}

void free_images() {
    free_shared(S.color), free_shared(S.hudless), free_shared(S.depth), free_shared(S.motion), free_shared(S.out);
}

bool make_shared(Shared &im, DXGI_FORMAT dx, VkFormat vkf, uint32_t w, uint32_t h, VkImageUsageFlags extra) {
    free_shared(im);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w, rd.Height = h;
    rd.DepthOrArraySize = 1, rd.MipLevels = 1;
    rd.Format = dx;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&im.res))))
        return false;
    HANDLE handle = nullptr;
    if (FAILED(S.device->CreateSharedHandle(im.res, nullptr, GENERIC_ALL, nullptr, &handle))) return free_shared(im), false;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = vkf;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(S.vk, &ci, nullptr, &im.image) != VK_SUCCESS) {
        CloseHandle(handle);
        return free_shared(im), false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(S.vk, im.image, &req);
    // the types the handle's memory can be imported as, the driver's answer: device-local first among them
    uint32_t bits = req.memoryTypeBits;
    if (S.handle_props) {
        VkMemoryWin32HandlePropertiesKHR wp = {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (S.handle_props(S.vk, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &wp) == VK_SUCCESS &&
            (bits & wp.memoryTypeBits))
            bits &= wp.memoryTypeBits;
    }
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < S.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((bits & (1u << i)) && (S.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    for (uint32_t i = 0; i < S.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if (bits & (1u << i)) type = i;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = im.image;
    VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    imp.handle = handle;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkResult r = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(S.vk, &ai, nullptr, &im.mem);
    CloseHandle(handle);   // an import from a Windows handle does not take it over
    if (r != VK_SUCCESS || vkBindImageMemory(S.vk, im.image, im.mem, 0) != VK_SUCCESS) return free_shared(im), false;
    if (extra & VK_IMAGE_USAGE_STORAGE_BIT) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vkf;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(S.vk, &vi, nullptr, &im.view) != VK_SUCCESS) return free_shared(im), false;
    }
    im.w = w, im.h = h, im.fresh = true;
    return true;
}

// The shared images at these sizes. Made again (both queues idle first) when a size changes.
bool images(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh) {
    if (S.color.res && S.color.w == w && S.color.h == h && S.depth.w == rw && S.depth.h == rh) return true;
    if (S.color.res) {
        vk_queue_lock();   // vkDeviceWaitIdle needs every queue left alone
        vkDeviceWaitIdle(S.vk);
        vk_queue_unlock();
    }
    if (!d3d12_wait(S.fence_value)) return false;
    if (S.feature) S.f.release(S.feature), S.feature = nullptr;
    const VkImageUsageFlags none = 0;
    bool ok = make_shared(S.color, DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, w, h, none) &&
              make_shared(S.hudless, DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, w, h, none) &&
              make_shared(S.depth, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, rw, rh, VK_IMAGE_USAGE_STORAGE_BIT) &&
              make_shared(S.motion, DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, rw, rh, none) &&
              make_shared(S.out, DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, w, h, none);
    if (!ok) {
        free_images();
        char line[96];
        snprintf(line, sizeof line, "the images shared with D3D12 were not made at %ux%u", w, h);
        give_up(line);
    }
    return ok;
}

void d3d12_barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER *b,
                   UINT &n) {
    D3D12_RESOURCE_BARRIER &x = b[n++];
    x = D3D12_RESOURCE_BARRIER{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = res;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = from;
    x.Transition.StateAfter = to;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cb, &di);
}

// How a shared image changes hands in a Vulkan command buffer. Between the two APIs each rests in GENERAL, owned by
// the external API: Vulkan acquires it before its writes or reads and releases it after them, so what one API wrote
// is defined to the other. A fresh image (UNDEFINED, never shared yet) is only made GENERAL on an acquire. RELEASE
// follows writes, which it makes available; RELEASE_READ follows reads alone, which leave nothing to make available,
// so it orders them before D3D12's next use by its stage only.
enum Hand { ACQUIRE, RELEASE, RELEASE_READ };
void vk_hand(VkCommandBuffer cb, Shared *const *ims, int n, Hand hand) {
    VkImageMemoryBarrier2 b[5];
    for (int i = 0; i < n; i++) {
        VkImageMemoryBarrier2 &x = b[i];
        x = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        x.oldLayout = x.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        x.srcStageMask = x.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (hand == ACQUIRE) {
            x.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            if (ims[i]->fresh) {
                x.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ims[i]->fresh = false;
            } else {
                x.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL, x.dstQueueFamilyIndex = S.family;
            }
        } else {
            x.srcAccessMask = hand == RELEASE ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_NONE;
            x.srcQueueFamilyIndex = S.family, x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        x.image = ims[i]->image;
        x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    if (!n) return;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = (uint32_t)n, di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cb, &di);
}

void copy(VkCommandBuffer cb, VkImage from, VkImage to, uint32_t w, uint32_t h) {
    const VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

// A command list of the ring, free again (its last run done), reset for recording. Null when its last run did not end.
ID3D12GraphicsCommandList *next_list(int &slot) {
    slot = S.slot;
    S.slot = (S.slot + 1) % kSlots;
    if (!d3d12_wait(S.list_done[slot])) return nullptr;
    if (FAILED(S.alloc[slot]->Reset()) || FAILED(S.list[slot]->Reset(S.alloc[slot], nullptr)))
        return give_up("a D3D12 command list could not be reset"), nullptr;
    return S.list[slot];
}

// The depth pass: a compute pipeline over one sampled depth (binding 0) and one storage image (binding 1), pushed
// with push descriptors.
bool make_depth_pass() {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(S.vk, &si, nullptr, &S.nearest) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0, b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].binding = 1, b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    for (auto &x : b) x.descriptorCount = 1, x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = 2, dci.pBindings = b;
    if (vkCreateDescriptorSetLayout(S.vk, &dci, nullptr, &S.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1, li.pSetLayouts = &S.dsl, li.pushConstantRangeCount = 1, li.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(S.vk, &li, nullptr, &S.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof dlssg_depth_spv, mi.pCode = dlssg_depth_spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(S.vk, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = m, ci.stage.pName = "main";
    ci.layout = S.layout;
    VkResult r = vkCreateComputePipelines(S.vk, VK_NULL_HANDLE, 1, &ci, nullptr, &S.pipe);
    vkDestroyShaderModule(S.vk, m, nullptr);
    return r == VK_SUCCESS;
}

// Everything the route made let go: at the release, and when the start fails part way. The Vulkan device is idle.
void teardown() {
    if (S.feature) S.f.release(S.feature), S.feature = nullptr;
    if (S.params) S.f.destroy(S.params), S.params = nullptr;
    unsigned left;
    if (S.ngx_on && S.device) S.f.shutdown(S.device, &left);
    S.ngx_on = false;
    free_images();
    for (int i = 0; i < kSlots; i++) {
        if (S.pool[i]) vkDestroyCommandPool(S.vk, S.pool[i], nullptr), S.pool[i] = VK_NULL_HANDLE;
        S.back[i] = VK_NULL_HANDLE, S.back_done[i] = 0, S.list_done[i] = 0;
        drop(S.list[i]), drop(S.alloc[i]);
    }
    if (S.shared_sem) vkDestroySemaphore(S.vk, S.shared_sem, nullptr), S.shared_sem = VK_NULL_HANDLE;
    if (S.back_sem) vkDestroySemaphore(S.vk, S.back_sem, nullptr), S.back_sem = VK_NULL_HANDLE;
    if (S.pipe) vkDestroyPipeline(S.vk, S.pipe, nullptr), S.pipe = VK_NULL_HANDLE;
    if (S.layout) vkDestroyPipelineLayout(S.vk, S.layout, nullptr), S.layout = VK_NULL_HANDLE;
    if (S.dsl) vkDestroyDescriptorSetLayout(S.vk, S.dsl, nullptr), S.dsl = VK_NULL_HANDLE;
    if (S.nearest) vkDestroySampler(S.vk, S.nearest, nullptr), S.nearest = VK_NULL_HANDLE;
    drop(S.fence), drop(S.queue), drop(S.device);
    if (S.event) CloseHandle(S.event), S.event = nullptr;
    S.fence_value = S.back_value = 0, S.slot = 0;
    S.fw = S.fh = S.frw = S.frh = 0;
    S.ok = false, S.pending = false;
}

// The D3D12 device, its queue, the shared fence and its Vulkan semaphore, the command lists and buffers, the depth
// pass, NGX's D3D12 side and its parameter block. why: the reason when not.
bool start_on(const char **why) {
    static char line[160];
    if (!S.interop) return *why = "the Vulkan device was made without external memory and semaphores from Win32 handles", false;
    if (!S.luid_ok) return *why = "the Vulkan device has no LUID to find its adapter by", false;
    HMODULE core = (HMODULE)vk_ngx_core(why);
    if (!core) return false;
    S.f.init = (PfnInit)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
    S.f.caps_of = (PfnParams)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    S.f.alloc = (PfnParams)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters");
    S.f.destroy = (PfnDestroy)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters");
    S.f.shutdown = (PfnShutdown)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown1");
    S.f.create = (PfnCreate)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature");
    S.f.evaluate = (PfnEvaluate)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    S.f.release = (PfnRelease)(void *)GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!S.f.init || !S.f.caps_of || !S.f.alloc || !S.f.destroy || !S.f.shutdown || !S.f.create || !S.f.evaluate ||
        !S.f.release)
        return *why = "_nvngx.dll lacks a D3D12 export frame generation needs", false;
    char name[128] = "?";
    if (!(S.device = d3d12_device(S.luid, name, sizeof name, why))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(S.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&S.queue)))) return *why = "no D3D12 queue", false;
    if (FAILED(S.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&S.fence))))
        return *why = "no shared D3D12 fence", false;
    if (!(S.event = CreateEventW(nullptr, FALSE, FALSE, nullptr))) return *why = "no event for the fence", false;
    for (int i = 0; i < kSlots; i++) {
        if (FAILED(S.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&S.alloc[i]))) ||
            FAILED(S.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.alloc[i], nullptr,
                                               IID_PPV_ARGS(&S.list[i]))))
            return *why = "no D3D12 command lists", false;
        S.list[i]->Close();
    }
    // the fence as a Vulkan timeline semaphore
    PFN_vkImportSemaphoreWin32HandleKHR import_sem =
        (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(S.vk, "vkImportSemaphoreWin32HandleKHR");
    S.handle_props = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(S.vk, "vkGetMemoryWin32HandlePropertiesKHR");
    S.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(S.vk, "vkCmdPushDescriptorSetKHR");
    if (!import_sem || !S.push) return *why = "no vkImportSemaphoreWin32HandleKHR or vkCmdPushDescriptorSetKHR", false;
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tci;
    if (vkCreateSemaphore(S.vk, &sci, nullptr, &S.shared_sem) != VK_SUCCESS ||
        vkCreateSemaphore(S.vk, &sci, nullptr, &S.back_sem) != VK_SUCCESS)
        return *why = "no Vulkan timeline semaphores", false;
    HANDLE fh = nullptr;
    if (FAILED(S.device->CreateSharedHandle(S.fence, nullptr, GENERIC_ALL, nullptr, &fh)))
        return *why = "the D3D12 fence could not be shared", false;
    VkImportSemaphoreWin32HandleInfoKHR isi = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    isi.semaphore = S.shared_sem;
    isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    isi.handle = fh;
    VkResult ri = import_sem(S.vk, &isi);
    CloseHandle(fh);
    if (ri != VK_SUCCESS) {
        snprintf(line, sizeof line, "the D3D12 fence was not imported into Vulkan (%d)", (int)ri);
        return *why = line, false;
    }
    for (int i = 0; i < kSlots; i++) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = S.family;
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkCreateCommandPool(S.vk, &pci, nullptr, &S.pool[i]) != VK_SUCCESS)
            return *why = "no Vulkan command pools", false;
        cai.commandPool = S.pool[i];
        if (vkAllocateCommandBuffers(S.vk, &cai, &S.back[i]) != VK_SUCCESS) return *why = "no Vulkan command buffers", false;
    }
    if (!make_depth_pass()) return *why = "the depth pass was not made", false;
    // NGX's D3D12 side on that device, with the Vulkan side's application id; NGX looks for nvngx_dlssg.dll in the
    // data folder first (dkb.exe's) and writes its logs there
    wchar_t dir[MAX_PATH + 1];
    if (!exe_dir(dir, MAX_PATH + 1)) return *why = "the exe's folder is not known", false;
    NgxResult r = S.f.init(kAppId, dir, S.device, kNgxVersionApi, nullptr);
    if (ngx_failed(r)) {
        snprintf(line, sizeof line, "NGX's D3D12 side did not start (%#x)", r);
        return *why = line, false;
    }
    S.ngx_on = true;
    NgxParam *caps = nullptr;
    r = S.f.caps_of(&caps);
    if (ngx_failed(r) || !caps) return *why = "NGX's D3D12 side has no capability parameters", false;
    int available = get_i(caps, "FrameGeneration.Available");
    unsigned init_result = (unsigned)get_i(caps, "FrameGeneration.FeatureInitResult");
    S.f.destroy(caps);
    if (!available) {
        snprintf(line, sizeof line, "NGX's D3D12 side offers no frame generation here (%#x): it needs an RTX 40 or newer", init_result);
        return *why = line, false;
    }
    if (ngx_failed(S.f.alloc(&S.params)) || !S.params) return *why = "no NGX parameter block", false;
    rt_log("dlssg: a D3D12 device on %s (LUID %08lx:%08lx) makes the frames: NGX's D3D12 frame generation is available",
           name, (unsigned long)S.luid.HighPart, (unsigned long)S.luid.LowPart);
    return true;
}

bool start() {
    if (S.started) return S.ok;
    S.started = true;
    const char *why = nullptr;
    if (start_on(&why)) return S.ok = true;
    teardown();   // what it made before the no: nothing of it is used
    give_up(why ? why : "the D3D12 route did not start");
    return false;
}

// The feature for these sizes, made on a list of its own, run and waited for.
bool feature(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, bool *made) {
    *made = false;
    if (S.feature && S.fw == w && S.fh == h && S.frw == rw && S.frh == rh) return true;
    if (S.feature) {
        if (!d3d12_wait(S.fence_value)) return false;
        S.f.release(S.feature), S.feature = nullptr;
    }
    int slot = 0;
    ID3D12GraphicsCommandList *l = next_list(slot);
    if (!l) return false;
    NgxParam *p = S.params;
    set_u(p, "CreationNodeMask", 1), set_u(p, "VisibilityNodeMask", 1);
    set_u(p, "Width", w), set_u(p, "Height", h);
    set_u(p, "DLSSG.Width", w), set_u(p, "DLSSG.Height", h);
    set_u(p, "DLSSG.BackbufferFormat", DXGI_FORMAT_R8G8B8A8_UNORM);
    set_u(p, "DLSSG.InternalWidth", rw), set_u(p, "DLSSG.InternalHeight", rh);
    set_u(p, "DLSSG.DynamicResolution", 0);
    NgxResult r = S.f.create(l, kFrameGeneration, p, &S.feature);
    l->Close();
    ID3D12CommandList *one = l;
    S.queue->ExecuteCommandLists(1, &one);
    S.queue->Signal(S.fence, ++S.fence_value);
    S.list_done[slot] = S.fence_value;
    if (!d3d12_wait(S.fence_value)) return false;
    if (ngx_failed(r) || !S.feature) {
        S.feature = nullptr;
        char line[96];
        snprintf(line, sizeof line, "NGX made no frame generation feature for %ux%u (%#x)", w, h, r);
        give_up(line);
        return false;
    }
    S.fw = w, S.fh = h, S.frw = rw, S.frh = rh;
    *made = true;
    rt_log("dlssg: frame generation feature made for %ux%u from a %ux%u scene", w, h, rw, rh);
    return true;
}

// ---- the camera ------------------------------------------------------------------------------------------------------

// 4x4 row-major products and inverse, in double.
void mul4(const double a[16], const double b[16], double o[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0;
            for (int i = 0; i < 4; i++) v += a[r * 4 + i] * b[i * 4 + c];
            o[r * 4 + c] = v;
        }
}

bool inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c)
            for (int k = 0; k < 8; k++) std::swap(a[c][k], a[piv][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return true;
}

// NGX's matrices are rows of a row-vector convention (position * M, as Streamline's float4x4): the game's column-vector
// matrices transposed (Hookshot vk_dlss.cpp fg_put). Handed to NGX as a pointer to float[4][4].
void put(float out[16], const double m[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)m[c * 4 + r];
}

// The camera inputs of a set, from the scene's projection and view (vk_dlssg.h VkDlssgSet). The game's clip space keeps
// OpenGL's z in [-1, 1] and the depth buffer holds (ndc z + 1) / 2 (vk_fsr3.c vk_fsr3_camera_factors), while NGX reads
// the depth it is given as the clip space's z / w. So every clip space here is the game's with z taken to [0, 1] by
// T = [1 0 0 0; 0 1 0 0; 0 0 .5 .5; 0 0 0 1]: viewToClip = T P, and this clip to the last = T M T^-1.
struct Camera {
    float view_to_clip[16], clip_to_view[16], to_prev[16], from_prev[16];
    float pos[3], up[3], right[3], fwd[3];
    float near_z, far_z, fov, aspect;
};

bool camera(const VkDlssgSet *s, Camera &cam) {
    const float *p = s->proj;
    if (p[12] != 0.0f || p[13] != 0.0f || p[14] != -1.0f || p[15] != 0.0f || !(p[0] > 0.0f) || !(p[5] > 0.0f))
        return false;   // not the perspective projection the depth above assumes
    static const double T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5, 0.5, 0, 0, 0, 1};
    static const double Ti[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, -1, 0, 0, 0, 1};
    double P[16], TP[16], TPi[16], M[16], tmp[16], TM[16], TMi[16];
    for (int i = 0; i < 16; i++) P[i] = p[i];
    mul4(T, P, TP);
    if (!inv4(TP, TPi)) return false;
    if (s->to_prev) {
        for (int i = 0; i < 16; i++) M[i] = s->to_prev[i];
    } else {
        for (int i = 0; i < 16; i++) M[i] = (i % 5) == 0;
    }
    mul4(T, M, tmp);
    mul4(tmp, Ti, TM);
    if (!inv4(TM, TMi)) return false;
    put(cam.view_to_clip, TP), put(cam.clip_to_view, TPi), put(cam.to_prev, TM), put(cam.from_prev, TMi);
    // the view [R | t]: the camera at -R^T t, its axes the rows of R; OpenGL's camera looks down -z
    const float *v = s->view;
    for (int i = 0; i < 3; i++) {
        cam.pos[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
        cam.right[i] = v[i], cam.up[i] = v[4 + i], cam.fwd[i] = -v[8 + i];
    }
    // OpenGL's p22 = -(f + n) / (f - n), p23 = -2 f n / (f - n)
    cam.near_z = (float)(P[11] / (P[10] - 1.0)), cam.far_z = (float)(P[11] / (P[10] + 1.0));
    if (!(cam.near_z > 0.0f) || !(cam.far_z > cam.near_z)) cam.near_z = 1.0f, cam.far_z = 5000.0f;
    cam.fov = (float)(2.0 * std::atan(1.0 / P[5]));
    cam.aspect = (float)(P[5] / P[0]);
    return true;
}

}  // namespace

// ---- the route's entry points ----------------------------------------------------------------------------------------

extern "C" void vk_dlssg_device_exts(const VkExtensionProperties *ext, uint32_t n_have, const char **names, uint32_t *n,
                                     uint32_t cap) {
    static const char *const want[2] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
    bool have[2] = {false, false};
    for (int k = 0; k < 2; k++)
        for (uint32_t i = 0; i < n_have; i++) have[k] |= !strcmp(ext[i].extensionName, want[k]);
    if (!have[0] || !have[1]) return;
    for (int k = 0; k < 2; k++) {
        bool listed = false;
        for (uint32_t i = 0; i < *n; i++) listed |= !strcmp(names[i], want[k]);
        if (!listed && *n < cap) names[(*n)++] = want[k];
    }
}

extern "C" void vk_dlssg_device(VkPhysicalDevice phys, VkDevice device, uint32_t family, const uint8_t luid[8],
                                int luid_valid, int interop) {
    S = State{};
    S.phys = phys, S.vk = device, S.family = family;
    vkGetPhysicalDeviceMemoryProperties(phys, &S.mem);
    static_assert(sizeof S.luid == 8, "VK_LUID_SIZE");
    memcpy(&S.luid, luid, sizeof S.luid);
    S.luid_ok = luid_valid != 0, S.interop = interop != 0;
}

extern "C" int vk_dlssg_wanted(void) {
    return S.vk && !g_dead.load(std::memory_order_relaxed) && !strcmp(cfg_value("DKB_FRAME_GEN_BY"), "dlss");
}

extern "C" int vk_dlssg_record(VkCommandBuffer cb, const VkDlssgSet *set) {
    if (!vk_dlssg_wanted() || !start()) return 0;
    S.pending = false;   // a set recorded and never sent is dropped: its list is reset below
    if (!set->color || !set->depth || !set->motion || !set->out || !set->w || !set->h || !set->rw || !set->rh) return 0;
    Camera cam;
    if (!camera(set, cam)) {   // a frame without a usable projection: made another way
        static bool said = false;
        if (!said) said = true, rt_log("dlssg: a frame's projection is not usable for DLSS Frame Generation; frames like it get the built-in one");
        return 0;
    }
    if (!images(set->w, set->h, set->rw, set->rh)) return 0;
    bool made = false;
    if (!feature(set->w, set->h, set->rw, set->rh, &made)) return 0;

    // the D3D12 list: the inputs to shader reads, the output to unordered access, NGX's evaluation, and back to COMMON
    int slot = 0;
    ID3D12GraphicsCommandList *l = next_list(slot);
    if (!l) return 0;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER bs[5];
    UINT nb = 0;
    for (Shared *im : {&S.color, &S.hudless, &S.depth, &S.motion}) d3d12_barrier(im->res, D3D12_RESOURCE_STATE_COMMON, read, bs, nb);
    d3d12_barrier(S.out.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, bs, nb);
    l->ResourceBarrier(nb, bs);
    NgxParam *p = S.params;
    set_res(p, "DLSSG.Backbuffer", S.color.res);
    set_res(p, "DLSSG.MVecs", S.motion.res);
    set_res(p, "DLSSG.Depth", S.depth.res);
    // the scene's colour before the HUD, the same as the backbuffer wherever there is none: NGX makes the frame from it
    // and finds the HUD as what the backbuffer has over it
    set_res(p, "DLSSG.HUDLess", S.hudless.res);
    set_res(p, "DLSSG.UI", nullptr);
    set_res(p, "DLSSG.UIAlpha", nullptr);
    set_res(p, "DLSSG.BidirectionalDistortionField", nullptr);
    set_res(p, "DLSSG.OutputInterpolated", S.out.res);
    set_res(p, "DLSSG.OutputReal", nullptr);
    set_res(p, "DLSSG.OutputDisableInterpolation", nullptr);
    set_u(p, "DLSSG.MultiFrameCount", 1), set_u(p, "DLSSG.MultiFrameIndex", 1);
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // NGX reads the matrices at the evaluation, which runs below: the pointers stay valid until then
    set_ptr(p, "DLSSG.CameraViewToClip", cam.view_to_clip);
    set_ptr(p, "DLSSG.ClipToCameraView", cam.clip_to_view);
    set_ptr(p, "DLSSG.ClipToLensClip", (void *)id);
    set_ptr(p, "DLSSG.ClipToPrevClip", cam.to_prev);
    set_ptr(p, "DLSSG.PrevClipToClip", cam.from_prev);
    set_f(p, "DLSSG.JitterOffsetX", set->jitter[0]), set_f(p, "DLSSG.JitterOffsetY", set->jitter[1]);
    // the motion as it is, render pixels: scale 1, as DLSS's own evaluation takes it. NVIDIA's header says the scale
    // normalizes to [-1, 1]; with 1 / render size the frames come out as cross-fades with no motion
    set_f(p, "DLSSG.MvecScaleX", 1.0f), set_f(p, "DLSSG.MvecScaleY", 1.0f);
    set_f(p, "DLSSG.CameraPinholeOffsetX", 0.0f), set_f(p, "DLSSG.CameraPinholeOffsetY", 0.0f);
    set_f(p, "DLSSG.CameraPosX", cam.pos[0]), set_f(p, "DLSSG.CameraPosY", cam.pos[1]), set_f(p, "DLSSG.CameraPosZ", cam.pos[2]);
    set_f(p, "DLSSG.CameraUpX", cam.up[0]), set_f(p, "DLSSG.CameraUpY", cam.up[1]), set_f(p, "DLSSG.CameraUpZ", cam.up[2]);
    set_f(p, "DLSSG.CameraRightX", cam.right[0]), set_f(p, "DLSSG.CameraRightY", cam.right[1]);
    set_f(p, "DLSSG.CameraRightZ", cam.right[2]);
    set_f(p, "DLSSG.CameraFwdX", cam.fwd[0]), set_f(p, "DLSSG.CameraFwdY", cam.fwd[1]), set_f(p, "DLSSG.CameraFwdZ", cam.fwd[2]);
    set_f(p, "DLSSG.CameraNear", cam.near_z), set_f(p, "DLSSG.CameraFar", cam.far_z);
    set_f(p, "DLSSG.CameraFOV", cam.fov), set_f(p, "DLSSG.CameraAspectRatio", cam.aspect);
    set_u(p, "DLSSG.ColorBuffersHDR", 0);
    set_u(p, "DLSSG.DepthInverted", 0);
    set_u(p, "DLSSG.CameraMotionIncluded", 1);
    // a new history after a new feature, a frame not noted in between, or the upscaler's own reset
    const bool reset = made || set->reset || !S.last_sequence || set->sequence != S.last_sequence + 1;
    S.resets += reset;
    set_u(p, "DLSSG.Reset", reset ? 1 : 0);
    set_u(p, "DLSSG.AutomodeOverrideReset", 0);
    set_u(p, "DLSSG.NotRenderingGameFrames", 0);
    set_u(p, "DLSSG.OrthoProjection", 0);
    set_f(p, "DLSSG.MvecInvalidValue", FLT_MAX);
    set_u(p, "DLSSG.MvecDilated", 0);
    set_u(p, "DLSSG.MenuDetectionEnabled", 0);
    set_ull(p, "DLSSG.BackbufferFrameID", set->sequence);
    NgxResult r = S.f.evaluate(l, S.feature, p, nullptr);
    if (ngx_failed(r)) {
        l->Close();   // never run: the slot's last run is still what list_done says
        char line[64];
        snprintf(line, sizeof line, "NGX refused a frame (%#x)", r);
        give_up(line);
        return 0;
    }
    for (UINT i = 0; i < nb; i++) std::swap(bs[i].Transition.StateBefore, bs[i].Transition.StateAfter);
    l->ResourceBarrier(nb, bs);
    if (FAILED(l->Close())) return give_up("the D3D12 command list did not close"), 0;

    // the Vulkan side: the inputs into the shared images (the output too, the first time: GENERAL and handed to D3D12
    // before its first write)
    Shared *ins[5] = {&S.color, &S.hudless, &S.depth, &S.motion, &S.out};
    const int n_in = S.out.fresh ? 5 : 4;
    vk_hand(cb, ins, n_in, ACQUIRE);
    copy(cb, set->color, S.color.image, set->w, set->h);
    copy(cb, set->hudless ? set->hudless : set->color, S.hudless.image, set->w, set->h);
    copy(cb, set->motion, S.motion.image, set->rw, set->rh);
    VkDescriptorImageInfo di[2] = {{S.nearest, set->depth, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, S.depth.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet ws[2] = {};
    for (int i = 0; i < 2; i++) {
        ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[i].dstBinding = (uint32_t)i, ws[i].descriptorCount = 1, ws[i].pImageInfo = &di[i];
        ws[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe);
    S.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.layout, 0, 2, ws);
    const uint32_t size[2] = {set->rw, set->rh};
    vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, size);
    vkCmdDispatch(cb, (set->rw + 7) / 8, (set->rh + 7) / 8, 1);
    vk_hand(cb, ins, n_in, RELEASE);
    vk_full_barrier(cb);
    S.pending = true;
    S.pending_slot = slot;
    S.pending_out = set->out;
    S.last_sequence = set->sequence;
    S.with_hudless += set->hudless != VK_NULL_HANDLE;
    if (!S.sets++)
        rt_log("dlssg: the first frame made by DLSS Frame Generation (%ux%u from a %ux%u scene%s)", set->w, set->h,
               set->rw, set->rh, set->hudless ? ", the picture before the HUD given with it" : "");
    if (S.sets % 3000 == 0) {
        rt_log("dlssg: of the last 3000 frames made, %llu had the picture before the HUD; %llu history resets",
               (unsigned long long)S.with_hudless, (unsigned long long)S.resets);
        S.with_hudless = S.resets = 0;
    }
    return 1;
}

extern "C" VkResult vk_dlssg_submit(VkQueue queue, const VkSubmitInfo2 *si, VkFence fence, int *made) {
    *made = 0;
    if (!S.pending) return vkQueueSubmit2(queue, 1, si, fence);
    S.pending = false;
    const int slot = S.pending_slot;
    // The frame goes out without the shared fence's signal when the copy back cannot follow it: D3D12 would wait for
    // that signal for ever. The made frame is then not written, and the caller shows the game's frame alone.
    VkSemaphoreSubmitInfo sigs[8];   // the caller's signals and the shared fence's
    if (si->signalSemaphoreInfoCount >= sizeof sigs / sizeof *sigs) {
        give_up("the frame's submission signals more semaphores than the route has room for");
        return vkQueueSubmit2(queue, 1, si, fence);
    }
    // the copy back, recorded first (its buffer free again: its last run done)
    VkCommandBuffer cb = S.back[slot];
    if (S.back_done[slot]) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1, wi.pSemaphores = &S.back_sem, wi.pValues = &S.back_done[slot];
        if (vkWaitSemaphores(S.vk, &wi, 5000000000ull) != VK_SUCCESS) {
            give_up("the copy back of an earlier frame did not finish in 5 s");
            return vkQueueSubmit2(queue, 1, si, fence);
        }
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandPool(S.vk, S.pool[slot], 0) != VK_SUCCESS || vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        give_up("the copy back's command buffer could not be begun");
        return vkQueueSubmit2(queue, 1, si, fence);
    }
    Shared *outs[1] = {&S.out};
    vk_full_barrier(cb);   // the made frame's earlier reads (its last present) before the copy writes it
    vk_hand(cb, outs, 1, ACQUIRE);
    copy(cb, S.out.image, S.pending_out, S.out.w, S.out.h);
    vk_hand(cb, outs, 1, RELEASE_READ);   // the copy only read the shared output
    vk_full_barrier(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        give_up("the copy back's command buffer could not be ended");
        return vkQueueSubmit2(queue, 1, si, fence);
    }
    // the frame's own submission, which signals the shared fence at v_in after it
    const uint64_t v_in = S.fence_value + 1;
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = S.shared_sem, sig.value = v_in, sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 own = *si;
    uint32_t ns = own.signalSemaphoreInfoCount;
    for (uint32_t i = 0; i < ns; i++) sigs[i] = own.pSignalSemaphoreInfos[i];
    sigs[ns++] = sig;
    own.signalSemaphoreInfoCount = ns, own.pSignalSemaphoreInfos = sigs;
    VkResult r = vkQueueSubmit2(queue, 1, &own, fence);
    if (r != VK_SUCCESS) return r;   // the caller reports it and turns frame generation off
    S.fence_value = v_in;
    // The D3D12 side. A refused wait leaves the list unrun (the slot's last run is still what list_done says). A
    // refused signal leaves v_out never reached: the copy back, which would wait on it, is not sent, and
    // vk_dlssg_release waits for v_out, then leaves the objects the list may still use to the process's end.
    if (FAILED(S.queue->Wait(S.fence, v_in))) return give_up("the D3D12 queue refused to wait for the frame"), VK_SUCCESS;
    ID3D12CommandList *l = S.list[slot];
    S.queue->ExecuteCommandLists(1, &l);
    const uint64_t v_out = ++S.fence_value;
    S.list_done[slot] = v_out;
    if (FAILED(S.queue->Signal(S.fence, v_out))) return give_up("the D3D12 queue refused to signal the made frame"), VK_SUCCESS;
    // the copy back, behind the D3D12 work
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = S.shared_sem, wait.value = v_out, wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo done = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    done.semaphore = S.back_sem, done.value = S.back_value + 1, done.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb;
    VkSubmitInfo2 back = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    back.waitSemaphoreInfoCount = 1, back.pWaitSemaphoreInfos = &wait;
    back.commandBufferInfoCount = 1, back.pCommandBufferInfos = &cbi;
    back.signalSemaphoreInfoCount = 1, back.pSignalSemaphoreInfos = &done;
    VkResult rb = vkQueueSubmit2(queue, 1, &back, VK_NULL_HANDLE);
    if (rb != VK_SUCCESS) {
        char line[64];
        snprintf(line, sizeof line, "the copy of the made frame was not submitted (%d)", (int)rb);
        give_up(line);
    } else {
        S.back_done[slot] = ++S.back_value;
        *made = 1;
    }
    return VK_SUCCESS;
}

extern "C" void vk_dlssg_release(void) {
    if (!S.vk) return;
    if (S.started && !d3d12_wait(S.fence_value)) {
        rt_log("dlssg: the D3D12 queue stayed busy at the release: its D3D12 objects are left to the process's end");
        // the Vulkan device is idle, so the Vulkan side goes; forgetting the D3D12 pointers leaks them on purpose, since
        // the queue may still use them (teardown skips what is null)
        S.feature = nullptr, S.params = nullptr, S.ngx_on = false;
        Shared *const ims[5] = {&S.color, &S.hudless, &S.depth, &S.motion, &S.out};
        for (Shared *im : ims) im->res = nullptr;
        for (int i = 0; i < kSlots; i++) S.list[i] = nullptr, S.alloc[i] = nullptr;
        S.fence = nullptr, S.queue = nullptr, S.device = nullptr;
    }
    teardown();
    if (S.started) rt_log("dlssg: let go");
    S = State{};
    g_dead.store(false, std::memory_order_relaxed);
}

#else

extern "C" void vk_dlssg_device_exts(const VkExtensionProperties *ext, uint32_t n_have, const char **names, uint32_t *n,
                                     uint32_t cap) {
    (void)ext, (void)n_have, (void)names, (void)n, (void)cap;
}
extern "C" void vk_dlssg_device(VkPhysicalDevice phys, VkDevice device, uint32_t family, const uint8_t luid[8],
                                int luid_valid, int interop) {
    (void)phys, (void)device, (void)family, (void)luid, (void)luid_valid, (void)interop;
}
extern "C" int vk_dlssg_wanted(void) { return 0; }
extern "C" int vk_dlssg_record(VkCommandBuffer cb, const VkDlssgSet *set) {
    (void)cb, (void)set;
    return 0;
}
extern "C" VkResult vk_dlssg_submit(VkQueue queue, const VkSubmitInfo2 *si, VkFence fence, int *made) {
    *made = 0;
    return vkQueueSubmit2(queue, 1, si, fence);
}
extern "C" void vk_dlssg_release(void) {}

#endif
