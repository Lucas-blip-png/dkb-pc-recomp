// Bananza manual mouse orbit (recomp/patches-dkb100.txt for 1.0.0, recomp/patches-dkb458752.txt for 3.2.0, mousecamera). Callbacks surround
// CameraPoserFollowLimit's yaw update and CameraAngleVerticalCtrl::exeUserCtrl. The native
// poser still computes the orbit, target and collision; other camera modes keep stick input.
#include <math.h>
#include <stdint.h>
#include "input.h"
#include "host.h"
#include "dispatch.h"
#include "platform/platform.h"

#define FIELD(type, p, offset) (*(type *)((uint8_t *)(uintptr_t)(p) + (offset)))
#define FLOAT(p, offset) FIELD(float, p, offset)
#define PTR(p, offset) FIELD(uint64_t, p, offset)
#define BYTE(p, offset) FIELD(uint8_t, p, offset)

// kernel.c fiber_switch_to preserves a fiber's Cpu while it can move between host threads.
// Pair hooks by that owner, camera object and guest stack frame, not the present-thread clock.
// A new yaw begin replaces the sample; each axis consumes it once. Lock only during a hook,
// never across guest execution or a fiber switch.
static PlatMutex mouse_lock = PLAT_MUTEX_INIT;
static struct {
    Cpu *owner;
    uint64_t follow, vertical;
    uint64_t yaw_sp, pitch_sp;
    float yaw, pitch, pitch_before;
    int enabled, yaw_used, pitch_used, pitch_ready;
} mouse;

enum { YAW_BEGIN, YAW_END, PITCH_BEGIN, PITCH_END };

static int pitch_limit(uint64_t vertical, float angle, float *out) {
    if (!vertical) return 0;
    uint64_t limits = PTR(vertical, 0xd0), defaults = PTR(vertical, 0xc8);
    if (!limits || !defaults) return 0;
    float low = -70, high = 85;
    if (BYTE(limits, 0)) {
        low = FLOAT(limits, 4); high = FLOAT(limits, 8);
    } else if (BYTE(defaults, 0x10)) {
        low = FLOAT(defaults, 4); high = FLOAT(defaults, 8);
    }
    low = fmaxf(-87.5f, low);
    high = fminf(87.5f, high);
    *out = fmaxf(low, fminf(high, angle));
    return 1;
}

static void camera_update(Cpu *c, unsigned site, uint64_t obj) {
    if (site == YAW_BEGIN) {
        mouse.owner = c;
        mouse.enabled = 0;
        mouse.yaw_used = mouse.pitch_used = mouse.pitch_ready = 0;
        mouse.follow = obj;
        if (!obj) return;
        uint64_t service = PTR(obj, 0xb0);
        if (!service) return;
        uint64_t timing = PTR(service, 0x10), options = PTR(service, 0x60);
        mouse.vertical = PTR(obj, 0x168);
        if (!timing || !options || !mouse.vertical || !(FLOAT(timing, 4) > 0)) return;
        // CameraPoserFollowLimit exits its manual path at main+0x41b370 (1.0.0; main+0x438610 in 3.2.0) for either flag.
        // Leave the raw counts for stick fallback and do not arm pitch activity in that mode.
        if (BYTE(options, 0xb) || BYTE(obj, 0x2d4)) return;
        float unused;
        if (!pitch_limit(mouse.vertical, 0, &unused)) return;
        float delta[2];
        mouse.enabled = input_mouse_direct(delta);
        mouse.yaw_sp = c->sp;
        // Use the game's invert settings, shared with its stick camera.
        // Positive right-stick X becomes a negative orbit angle in the native follow camera.
        mouse.yaw = BYTE(options, 8) ? delta[0] : -delta[0];
        mouse.pitch = BYTE(options, 9) ? -delta[1] : delta[1];
    } else if (!obj || c != mouse.owner || !mouse.enabled) {
        return;
    } else if (site == YAW_END && obj == mouse.follow && !mouse.yaw_used && c->sp == mouse.yaw_sp) {
        mouse.yaw_used = 1;
        if (mouse.yaw != 0) {
            // +0x204 is this update's yaw step; +0x208 is the stick's smoothing accumulator.
            FLOAT(obj, 0x204) = mouse.yaw;
            FLOAT(obj, 0x208) = 0;
            // Manual input cancels the automatic follow turn, as the native stick path does.
            FLOAT(obj, 0x2d8) = 0;
            FLOAT(obj, 0x2fc) = 0;
            BYTE(obj, 0x300) = 0;
        }
    } else if (site == PITCH_BEGIN && obj == mouse.vertical && !mouse.pitch_used) {
        mouse.pitch_before = FLOAT(obj, 0x104);
        mouse.pitch_sp = c->sp;
        mouse.pitch_ready = 1;
    } else if (site == PITCH_END && obj == mouse.vertical && mouse.pitch_ready && !mouse.pitch_used &&
               c->sp == mouse.pitch_sp) {
        mouse.pitch_used = 1;
        float angle;
        if (mouse.pitch != 0 && pitch_limit(obj, mouse.pitch_before + mouse.pitch, &angle)) {
            // Keep the current and target angles together so stick interpolation adds no delay.
            FLOAT(obj, 0x104) = FLOAT(obj, 0x108) = angle;
            FLOAT(obj, 0x130) = 0;
        }
    }
}

static void camera_hook(Cpu *c, unsigned site, uint64_t obj) {
    plat_mutex_lock(&mouse_lock);
    camera_update(c, site, obj);
    plat_mutex_unlock(&mouse_lock);
}

static void yaw_begin(Cpu *c) { camera_hook(c, YAW_BEGIN, c->x[0]); }
static void yaw_end(Cpu *c) { camera_hook(c, YAW_END, c->x[19]); }
static void pitch_begin(Cpu *c) { camera_hook(c, PITCH_BEGIN, c->x[19]); }
static void pitch_end(Cpu *c) { camera_hook(c, PITCH_END, c->x[19]); }

// Report vertical mouse activity to the automatic pitch states' stick checks so the
// game chooses the next manual state and retains control of scripted cameras.
static void pitch_activity(Cpu *c) {
    plat_mutex_lock(&mouse_lock);
    if (c == mouse.owner && c->x[19] == mouse.vertical && mouse.enabled && mouse.pitch != 0 && !mouse.pitch_used &&
        input_mouse_direct_recent())
        c->v[0] = 0x3f800000u; // s0 = 1.0, an activity signal rather than an angular step
    plat_mutex_unlock(&mouse_lock);
}

void dkb_camera_init(void) {
    if (rt_patch_set("mousecamera") < 0) return;   // another game or generated code predating the patch
    uint64_t page = (uint64_t)(uintptr_t)rt_patch_page();
    rt_register_host(page + 0x200, yaw_begin);
    rt_register_host(page + 0x210, yaw_end);
    rt_register_host(page + 0x220, pitch_begin);
    rt_register_host(page + 0x230, pitch_end);
    rt_register_host(page + 0x240, pitch_activity);
}
