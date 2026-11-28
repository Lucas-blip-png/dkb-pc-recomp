// NVIDIA DLSS Frame Generation through NGX's D3D12 side (vk_dlssg.cpp), and the NGX core it shares with DLSS Super
// Resolution's Vulkan side (vk_dlss.c). Included from C and C++, so it takes raw Vulkan handles rather than vk.h's types.
#pragma once

#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// The driver's NGX core (_nvngx.dll, an HMODULE), loaded on the first call and kept for the process. NULL on a system
// without it (no NVIDIA driver, not Windows), with *why, when given, saying what was missing. vk_dlss.c.
void *vk_ngx_core(const char **why);

// The device extensions the shared images and the shared fence need (external memory and semaphores from Win32
// handles), added to names[*n] (cap entries at most) where the device has them, on Windows with an NVIDIA GPU; ext is
// the device's list of n_have. Before the device is made (vk_device.c create_device). Elsewhere it adds nothing.
void vk_dlssg_device_exts(const VkExtensionProperties *ext, uint32_t n_have, const char **names, uint32_t *n,
                          uint32_t cap);
// After the device is made: what the route works with. interop: both extensions above were enabled.
void vk_dlssg_device(VkPhysicalDevice phys, VkDevice device, uint32_t family, const uint8_t luid[8], int luid_valid,
                     int interop);
// frame_gen_by = dlss on Windows, and the route has not been given up. Cheap: read each frame.
int vk_dlssg_wanted(void);

// One 2x frame, the made one between the frame before (whose inputs the feature keeps) and this one. Every image is in
// GENERAL. color: this frame as shown, RGBA8 UNORM, w x h (TRANSFER_SRC). hudless: the same frame without the game's
// HUD (RGBA8 UNORM, w x h, TRANSFER_SRC), or VK_NULL_HANDLE: then color is given as both, and NGX finds no HUD.
// depth: a sampled view of the scene's depth at rw x rh (0 at the near plane). motion: the camera motion vectors in
// render pixels (R16G16_SFLOAT, previous position minus this one), valid in its top-left rw x rh. proj: the scene's
// projection, row-major, clip = proj * view position, OpenGL style (w row 0 0 -1 0, depth = (ndc z + 1) / 2); view: the
// rows of the view matrix [R | t]; to_prev: this frame's clip space to the frame before's (row-major), or NULL when
// there is no frame before. out: where the made frame goes (RGBA8 UNORM, w x h, TRANSFER_DST).
typedef struct {
    VkImage color, hudless;
    uint32_t w, h;
    VkImageView depth;
    VkImage motion;
    uint32_t rw, rh;
    float proj[16], view[12];
    const double *to_prev;
    float jitter[2];   // pixels, the viewport offset the scene was drawn with
    int reset;         // the upscaler's own history started again
    uint64_t sequence; // the count of the frames noted (VkFiInputs.sequence)
    VkImage out;
} VkDlssgSet;
// Records the set into cb (the frame generation command buffer, after a full barrier): the inputs into the images both
// APIs share, NGX's evaluation recorded on the D3D12 side. 1 = recorded: the caller submits cb through
// vk_dlssg_submit, which sends the D3D12 work and the copy of the made frame into set->out behind it. 0 = nothing
// recorded (why logged once): the caller makes the frame another way.
int vk_dlssg_record(VkCommandBuffer cb, const VkDlssgSet *set);
// Under vk_queue_lock, in place of the caller's vkQueueSubmit2(queue, 1, si, fence) for the command buffer the set was
// recorded into: that submission (fence signalled after it, as the caller's own) signals the shared fence too, the
// D3D12 queue waits for it, evaluates and signals it again, and a second Vulkan submission waits for that and copies
// the made frame into set->out. Returns the first submission's result. *made: 1 when the copy into set->out was sent.
// A failure on the way gives the route up and leaves *made 0 (out keeps what it held), so the caller shows the game's
// frame alone. Without a recorded set it is the caller's submission alone.
VkResult vk_dlssg_submit(VkQueue queue, const VkSubmitInfo2 *si, VkFence fence, int *made);
// The feature, the shared images and the D3D12 device let go: Vulkan device idle, before it goes (vk_present.c).
void vk_dlssg_release(void);

#ifdef __cplusplus
}
#endif
