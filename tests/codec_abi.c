#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "platform/platform.h"

static RtHostEntry entries[64];
static int entry_count;
#undef HOST_OVERRIDE
#define HOST_OVERRIDE(sym, fn) \
    static void __attribute__((constructor)) RT_CAT(register_codec_, __COUNTER__)(void) { \
        entries[entry_count++] = (RtHostEntry){sym, fn}; \
    }
#include "ov_codec.c"

unsigned rt_trace;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint64_t ldr_symbol(const char *name) { (void)name; abort(); }
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) { (void)c; (void)fn; (void)n; abort(); }
void *mem_kernel_alloc(uint64_t size, uint64_t align) { (void)size; (void)align; abort(); }
void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear) { (void)c; (void)ev; (void)autoclear; abort(); }
void kern_signal_system_event(uint64_t ev) { (void)ev; abort(); }

static GuestFn binding(const char *name) {
    for (int i = 0; i < entry_count; i++) {
        if (!strcmp(entries[i].name, name)) return entries[i].fn;
    }
    return NULL;
}

static void properties(int updated) {
    uint16_t sizes[4] = {0};
    struct { int64_t value; uint64_t guard; } pts = {0, 0xabcdef};
    struct { uint8_t value; uint8_t guard[7]; } eos = {0, {1,2,3,4,5,6,7}};
    uint32_t color = 0xdeadbeef;
    uint64_t stack[] = {3};
    Cpu c = {0};
    c.sp = (uintptr_t)stack;
    for (int i = 0; i < 4; i++) c.x[i] = (uintptr_t)&sizes[i];
    c.x[updated ? 4 : 5] = (uintptr_t)&pts.value;
    c.x[updated ? 5 : 6] = (uintptr_t)&eos.value;
    c.x[updated ? 6 : 7] = 0x1234;
    if (updated) c.x[7] = 3;
    else c.x[4] = (uintptr_t)&color;
    GuestFn fn = binding(updated ?
        "_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj" :
        "_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj");
    assert(fn);
    fn(&c);
    assert(c.x[0] == 0);
    assert(sizes[0] == 960 && sizes[1] == 540 && sizes[2] == 960 && sizes[3] == 540);
    assert(pts.value == 123456789 && pts.guard == 0xabcdef);
    assert(eos.value == 1 && eos.guard[0] == 1 && eos.guard[6] == 7);
    assert(color == (updated ? 0xdeadbeef : 0));
    c.x[updated ? 6 : 7] = 0x5678;
    fn(&c);
    assert(c.x[0] == R_NOT_READY);
}

static void initialize(int variant) {
    static const char *symbols[] = {
        "_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
        "_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
        "_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE"
    };
    Cpu c = {0};
    c.x[0] = 0x1234;
    for (int i = 1; i < 8; i++) c.x[i] = 111 + i;
    int width_arg = 4 - variant;
    c.x[width_arg] = 960;
    c.x[width_arg + 1] = 540;
    GuestFn fn = binding(symbols[variant]);
    assert(fn);
    fn(&c);
    assert(c.x[0] == 0 && decs[0].vpx_ok);
    assert(decs[0].vpx.config.dec->w == 960 && decs[0].vpx.config.dec->h == 540);
    c.x[0] = 0x1234;
    ov_vp9_fini(&c);
}

static void try_wait_point(void) {
    GuestFn fn = binding("_ZN2nn6syncpt3sdk16TryWaitSyncPointERKNS1_15SyncPointHandleE");
    assert(fn);
    uint32_t handle[2] = {FENCE_TAG | 3u, 0};   // decoder 0, buffer 3: an acquired output buffer's fence
    Cpu c = {0};
    // x0 carries the handle in and the answer out, so it is reloaded before each call.
    decs[0].slot[3].state = S_WAIT;
    c.x[0] = (uintptr_t)handle;
    fn(&c);
    assert(c.x[0] == 0);   // acquired, no picture decoded into it yet: the player keeps waiting
    decs[0].slot[3].state = S_FILLED;
    c.x[0] = (uintptr_t)handle;
    fn(&c);
    assert(c.x[0] == 1);   // the picture is there
    handle[0] = 0x1234;    // not a decoder fence: the host has no sync point to wait for
    c.x[0] = (uintptr_t)handle;
    fn(&c);
    assert(c.x[0] == 1);
    c.x[0] = 0;            // no handle at all
    fn(&c);
    assert(c.x[0] == 1);
    decs[0].slot[3].state = S_FREE;
}

int main(void) {
    for (int variant = 0; variant < 3; variant++) initialize(variant);
    decs[0].key = 0x1234;
    decs[0].slot[3] = (Slot){.w=960, .h=540, .pts=123456789, .eos=1};
    properties(0);
    properties(1);
    try_wait_point();
    puts("codec ABI: three initializers, base/update properties and the sync point poll passed");
    return 0;
}
