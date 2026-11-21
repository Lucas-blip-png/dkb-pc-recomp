"""Unit tests for in-place dirty-bitmask vertex buffer binding, pruned dynamic state, and shared pipeline lock.

Extracts bind_vertex_buffers verbatim from runtime/vk_exec_draw.c, and vertex_input, vertex_input_same,
vk_pipeline_set_dynamic_from, graphics, and vk_pipeline_stats verbatim from runtime/vk_pipeline.c, verifying
exact parity and skip behavior at -O0 and -O2 with -Wall -Werror.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def extract_struct(source, name):
    end = source.index(f'}} {name};') + len(f'}} {name};')
    start = source.rindex('typedef struct', 0, end)
    return source[start:end]


VB_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_exec.h"

#define ZERO_BYTES 256u
#undef VK_MEM_USE
#define VK_MEM_USE(sp) ((void)(sp))

typedef struct DrawCtx {
    struct {
        VkBuffer vb[17];
        VkDeviceSize vo[17], vsz[17], vstr[17];
        int vcls[17];
        void *vpool[17];
        NVNbufferRange vin[16];
        uint64_t vgen;
        int vb_valid;
        uint16_t vb_dirty, vstr_dirty, vcls_mask, vpool_mask;
    } B;
} DrawCtx;

#define DC_OF(ex) ((DrawCtx *)(ex)->draw)

static struct { VkBuffer zbuf; } standins;
uint32_t rt_trace;
static int g_dyn_vi = 1;
static uint64_t g_mem_gen = 1;

typedef struct {
    uint64_t addr;
    VkMemSpan sp;
    int ok;
} MemEntry;

static MemEntry g_mem[32];
static int g_nmem;
static int g_resolve_calls;

int vk_pipeline_vertex_input_dynamic(void) { return g_dyn_vi; }
uint64_t vk_mem_gen(void) { return g_mem_gen; }

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *sp) {
    (void)size;
    g_resolve_calls++;
    for (int i = 0; i < g_nmem; i++) {
        if (g_mem[i].addr == addr) {
            if (!g_mem[i].ok) return 0;
            *sp = g_mem[i].sp;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int nbind;
    uint32_t first, count;
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int has_str;
    int ncount;
    int count_cls[32];
    uint64_t count_sz[32];
    int nnote;
    void *note_pool[32];
} VbTrace;

static VbTrace g_tr;

static void mock_bind_vb2(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *vb,
                          const VkDeviceSize *vo, const VkDeviceSize *vsz, const VkDeviceSize *vstr) {
    (void)cb;
    g_tr.nbind++;
    g_tr.first = first;
    g_tr.count = count;
    g_tr.has_str = vstr != NULL;
    for (uint32_t i = 0; i < count; i++) {
        g_tr.vb[i] = vb[i];
        g_tr.vo[i] = vo[i];
        g_tr.vsz[i] = vsz[i];
        g_tr.vstr[i] = vstr ? vstr[i] : 0;
    }
}
#undef vkCmdBindVertexBuffers2
#define vkCmdBindVertexBuffers2 mock_bind_vb2

void vk_mem_count(int cls, int use, uint64_t bytes) {
    assert(use == VK_MUSE_VB);
    assert(g_tr.ncount < 32);
    g_tr.count_cls[g_tr.ncount] = cls;
    g_tr.count_sz[g_tr.ncount] = bytes;
    g_tr.ncount++;
}

void vk_mem_bound_note(void *pool) {
    assert(g_tr.nnote < 32);
    g_tr.note_pool[g_tr.nnote++] = pool;
}

/* BIND_VB_EXTRACT */

/* Reference implementation (original full-scan bind_vertex_buffers). */
static void bind_vertex_buffers_ref(VkExec *ex, DrawCtx *dc) {
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17] = {0};
    void *vpool[17] = {0};
    int dyn = vk_pipeline_vertex_input_dynamic();
    uint64_t gen = vk_mem_gen();
    int same_gen = dc->B.vb_valid && dc->B.vgen == gen;
    if (same_gen && !memcmp(dc->B.vin, ex->vbuf, sizeof dc->B.vin)) {
        int same_str = 1;
        for (int b = 0; b < 16 && !dyn; b++) same_str &= dc->B.vstr[b] == ex->pipe.vstream[b].stride;
        if (same_str) return;
    }
    for (int b = 0; b < 17; b++) {
        vb[b] = standins.zbuf, vo[b] = 0, vsz[b] = ZERO_BYTES, vstr[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        if (same_gen && dc->B.vin[b].address == r->address && dc->B.vin[b].size == r->size) {
            vb[b] = dc->B.vb[b], vo[b] = dc->B.vo[b], vsz[b] = dc->B.vsz[b], vcls[b] = dc->B.vcls[b], vpool[b] = dc->B.vpool[b];
            continue;
        }
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        if (!sp.buffer) continue;
        uint64_t n = r->size && r->size < sp.avail ? r->size : sp.avail;
        vb[b] = sp.buffer, vo[b] = sp.offset, vsz[b] = n, vcls[b] = sp.cls, vpool[b] = sp.pool;
    }
    memcpy(dc->B.vcls, vcls, sizeof vcls), memcpy(dc->B.vpool, vpool, sizeof vpool);
    memcpy(dc->B.vin, ex->vbuf, sizeof dc->B.vin);
    dc->B.vgen = gen;
    int lo = 0, hi = 16;
    if (dc->B.vb_valid) {
#define VB_DIFF(b) (dc->B.vb[b] != vb[b] || dc->B.vo[b] != vo[b] || dc->B.vsz[b] != vsz[b] || (!dyn && dc->B.vstr[b] != vstr[b]))
        while (lo < 17 && !VB_DIFF(lo)) lo++;
        if (lo == 17) return;
        while (!VB_DIFF(hi)) hi--;
#undef VB_DIFF
    }
    mock_bind_vb2(ex->cb, (uint32_t)lo, (uint32_t)(hi - lo + 1), vb + lo, vo + lo, vsz + lo, dyn ? NULL : vstr + lo);
    ex->recorded++;
    for (int b = 0; b < 17; b++)
        if (vcls[b]) vk_mem_count(vcls[b], VK_MUSE_VB, vsz[b]);
    if (rt_trace & RT_TRACE_PERF)
        for (int b = 0; b < 17; b++)
            if (vpool[b]) vk_mem_bound_note(vpool[b]);
    memcpy(dc->B.vb, vb, sizeof vb), memcpy(dc->B.vo, vo, sizeof vo), memcpy(dc->B.vsz, vsz, sizeof vsz), memcpy(dc->B.vstr, vstr, sizeof vstr);
    dc->B.vb_valid = 1;
}

static void set_vbuf(VkExec *ex, DrawCtx *dc, int first, int n, const NVNbufferRange *r) {
    memcpy(&ex->vbuf[first], r, sizeof(NVNbufferRange) * (size_t)n);
    if (n) dc->B.vb_dirty |= (uint16_t)(((1u << n) - 1u) << first);
}

static void set_vstream(VkExec *ex, DrawCtx *dc, int first, int n, const vs_st *st) {
    memset(ex->pipe.vstream, 0, sizeof ex->pipe.vstream);
    memcpy(&ex->pipe.vstream[first], st, sizeof(vs_st) * (size_t)n);
    dc->B.vstr_dirty = 0xFFFFu;
}

static void check_step(VkExec *ex_f, DrawCtx *dc_f, VkExec *ex_r, DrawCtx *dc_r, int *out_resolves) {
    VbTrace tf, tr;
    memset(&g_tr, 0, sizeof g_tr);
    g_resolve_calls = 0;
    bind_vertex_buffers(ex_f);
    tf = g_tr;
    if (out_resolves) *out_resolves = g_resolve_calls;

    memset(&g_tr, 0, sizeof g_tr);
    bind_vertex_buffers_ref(ex_r, dc_r);
    tr = g_tr;

    assert(ex_f->recorded == ex_r->recorded);
    assert(memcmp(&tf, &tr, sizeof tf) == 0);
    assert(dc_f->B.vb_valid == dc_r->B.vb_valid);
    assert(dc_f->B.vgen == dc_r->B.vgen);
    assert(memcmp(dc_f->B.vb, dc_r->B.vb, sizeof dc_f->B.vb) == 0);
    assert(memcmp(dc_f->B.vo, dc_r->B.vo, sizeof dc_f->B.vo) == 0);
    assert(memcmp(dc_f->B.vsz, dc_r->B.vsz, sizeof dc_f->B.vsz) == 0);
    if (!g_dyn_vi) assert(memcmp(dc_f->B.vstr, dc_r->B.vstr, sizeof dc_f->B.vstr) == 0);
    assert(memcmp(dc_f->B.vcls, dc_r->B.vcls, sizeof dc_f->B.vcls) == 0);
    assert(memcmp(dc_f->B.vpool, dc_r->B.vpool, sizeof dc_f->B.vpool) == 0);
    assert(memcmp(dc_f->B.vin, dc_r->B.vin, sizeof dc_f->B.vin) == 0);
}

static void run_vb_suite(int dyn_mode) {
    g_dyn_vi = dyn_mode;
    g_mem_gen = 10;
    rt_trace = RT_TRACE_PERF;
    standins.zbuf = (VkBuffer)(uintptr_t)0x9999;

    g_nmem = 4;
    g_mem[0] = (MemEntry){0x1000, {.buffer = (VkBuffer)(uintptr_t)0x101, .offset = 64, .avail = 4096, .cls = 1, .pool = (void *)0xaaa1}, 1};
    g_mem[1] = (MemEntry){0x2000, {.buffer = (VkBuffer)(uintptr_t)0x102, .offset = 128, .avail = 2048, .cls = 2, .pool = (void *)0xaaa2}, 1};
    g_mem[2] = (MemEntry){0x3000, {.buffer = VK_NULL_HANDLE, .offset = 0, .avail = 512, .cls = 3, .pool = (void *)0xaaa3}, 1};   // unbacked staged
    g_mem[3] = (MemEntry){0x4000, {.buffer = (VkBuffer)(uintptr_t)0x104, .offset = 256, .avail = 1024, .cls = 0, .pool = (void *)0xaaa4}, 1};

    VkExec ex_f, ex_r;
    DrawCtx dc_f, dc_r;
    memset(&ex_f, 0, sizeof ex_f);
    memset(&ex_r, 0, sizeof ex_r);
    memset(&dc_f, 0, sizeof dc_f);
    memset(&dc_r, 0, sizeof dc_r);
    ex_f.draw = &dc_f;
    ex_r.draw = &dc_r;

    NVNbufferRange r0[2] = {{0x1000, 512}, {0x2000, 0}};
    vs_st s0[3] = {{.stride = 16}, {.stride = 32}, {.stride = 48}};
    set_vbuf(&ex_f, &dc_f, 0, 2, r0);
    set_vbuf(&ex_r, &dc_r, 0, 2, r0);
    set_vstream(&ex_f, &dc_f, 0, 3, s0);
    set_vstream(&ex_r, &dc_r, 0, 3, s0);

    /* 1. Initial bind on fresh CB: binds 0..16, resolves only non-zero slots 0 and 1. */
    int res = -1;
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(res == 2 && ex_f.recorded == 1 && dc_f.B.vb[16] == standins.zbuf);

    /* 2. Repeat draw with no dirty bits: 0 resolves, 0 binds. */
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(res == 0 && ex_f.recorded == 1);

    /* 3. Re-bind identical range on slot 0: dirty bit set, vin matches -> 0 resolves, 0 binds. */
    set_vbuf(&ex_f, &dc_f, 0, 1, &r0[0]);
    set_vbuf(&ex_r, &dc_r, 0, 1, &r0[0]);
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(res == 0 && ex_f.recorded == 1);

    /* 4. Update two non-adjacent slots (2 = unbacked, 7 = valid): only slot 7 differs from standins.zbuf -> lo=hi=7! */
    NVNbufferRange r_unb = {0x3000, 64}, r_v4 = {0x4000, 200};
    set_vbuf(&ex_f, &dc_f, 2, 1, &r_unb);
    set_vbuf(&ex_r, &dc_r, 2, 1, &r_unb);
    set_vbuf(&ex_f, &dc_f, 7, 1, &r_v4);
    set_vbuf(&ex_r, &dc_r, 7, 1, &r_v4);
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(res == 2 && ex_f.recorded == 2 && g_tr.first == 7 && g_tr.count == 1);

    /* 5. OP_BIND_VSTREAM replacing [0..2] with [0..0] (clears stride on 1, 2!): ignored if dyn=1, binds [1..2] if dyn=0. */
    vs_st s1 = {.stride = 16};
    set_vstream(&ex_f, &dc_f, 0, 1, &s1);
    set_vstream(&ex_r, &dc_r, 0, 1, &s1);
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(res == 0);
    assert(ex_f.recorded == (dyn_mode ? 2 : 3));

    /* 6. Clear slot 0 to address 0 -> back to standins.zbuf, vcls_mask bit 0 cleared. */
    NVNbufferRange r_zero = {0, 0};
    set_vbuf(&ex_f, &dc_f, 0, 1, &r_zero);
    set_vbuf(&ex_r, &dc_r, 0, 1, &r_zero);
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(res == 0 && dc_f.B.vb[0] == standins.zbuf && !(dc_f.B.vcls_mask & 1u));

    /* 7. vk_mem_gen bump without backing change -> re-resolves non-zero slots, 0 new bind! */
    int rec_before = ex_f.recorded;
    g_mem_gen++;
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(ex_f.recorded == rec_before);

    /* 8. vk_mem_gen bump WITH backing offset change on 0x2000 (slot 1) -> binds only slot 1! */
    g_mem[1].sp.offset = 512;
    g_mem_gen++;
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(ex_f.recorded == rec_before + 1 && g_tr.first == 1 && g_tr.count == 1);

    /* 9. New CB reset (memset B = 0) -> binds 0..16 again including VK_PIPE_ZERO_BINDING. */
    memset(&dc_f.B, 0, sizeof dc_f.B);
    memset(&dc_r.B, 0, sizeof dc_r.B);
    check_step(&ex_f, &dc_f, &ex_r, &dc_r, &res);
    assert(g_tr.first == 0 && g_tr.count == 17 && dc_f.B.vb[16] == standins.zbuf);
}

int main(void) {
    run_vb_suite(1);
    run_vb_suite(0);
    puts("draw_vb_test: ok");
    return 0;
}
'''

PIPE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"

#define LOG_ONCE(...) ((void)0)

struct GfxShader {
    uint32_t in_mask;
    GfxShaderIo in_io[32];
    uint32_t out_mask;
};

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo out[32]) {
    if (out) memcpy(out, s->in_io, sizeof s->in_io);
    return s->in_mask;
}

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo out[32]) {
    if (out) memset(out, 0, sizeof(GfxShaderIo) * 32);
    return s->out_mask;
}

VkFormat vk_vertex_format(int nvn_fmt, int *unsupported) {
    if (unsupported) *unsupported = 0;
    if (nvn_fmt == 1) return VK_FORMAT_R32G32B32_SFLOAT;
    if (nvn_fmt == 2) return VK_FORMAT_R32G32_SFLOAT;
    if (nvn_fmt == 3) return VK_FORMAT_R8G8B8A8_UNORM;
    return VK_FORMAT_UNDEFINED;
}

static VkFormat target_format(int32_t nvn_fmt, int *s, int *u) {
    if (s) *s = 0;
    if (u) *u = 0;
    if (nvn_fmt == 1) return VK_FORMAT_R8G8B8A8_UNORM;
    if (nvn_fmt == 2) return VK_FORMAT_R32_UINT;   // non-blendable
    if (nvn_fmt == 3) return VK_FORMAT_R16G16B16A16_SFLOAT;
    return VK_FORMAT_UNDEFINED;
}

static int blendable(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

VkPrimitiveTopology vk_pipeline_topology(int prim) { return (VkPrimitiveTopology)prim; }
static VkCompareOp compare_op(int v) { return (VkCompareOp)(v & 7); }
static VkStencilOp stencil_op(int v) { return (VkStencilOp)(v & 7); }
static VkPolygonMode polygon_mode(int v) { return (VkPolygonMode)(v & 3); }
static VkBlendFactor blend_factor(int v) { return (VkBlendFactor)(v & 31); }
static VkBlendOp blend_op(int v) { return (VkBlendOp)(v & 7); }

/* Recorded Vulkan dynamic state on the mock command buffer. */
typedef struct {
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    VkBool32 dt, dw, st, bias, rdisc, prestart, dclamp, a2c;
    VkCompareOp df;
    VkStencilOp fail_f, zpass_f, zfail_f, fail_b, zpass_b, zfail_b;
    VkCompareOp sf_f, sf_b;
    VkPolygonMode poly_mode;
    uint32_t n_bind, n_attr;
    VkVertexInputBindingDescription2EXT vi_b[17];
    VkVertexInputAttributeDescription2EXT vi_a[32];
    uint32_t en_n, eq_n, wm_n;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
} DynHw;

typedef struct {
    int c_cull, c_front, c_topo, c_dt, c_dw, c_df, c_st, c_sf, c_sb, c_bias, c_rdisc, c_prestart;
    int c_vi, c_clamp, c_poly, c_a2c, c_en, c_eq, c_wm;
} DynCalls;

static DynHw g_hw;
static DynCalls g_calls;

static void m_cull(VkCommandBuffer cb, VkCullModeFlags m) { (void)cb; g_hw.cull = m; g_calls.c_cull++; }
static void m_front(VkCommandBuffer cb, VkFrontFace f) { (void)cb; g_hw.front = f; g_calls.c_front++; }
static void m_topo(VkCommandBuffer cb, VkPrimitiveTopology t) { (void)cb; g_hw.topo = t; g_calls.c_topo++; }
static void m_dt(VkCommandBuffer cb, VkBool32 v) { (void)cb; g_hw.dt = v; g_calls.c_dt++; }
static void m_dw(VkCommandBuffer cb, VkBool32 v) { (void)cb; g_hw.dw = v; g_calls.c_dw++; }
static void m_df(VkCommandBuffer cb, VkCompareOp v) { (void)cb; g_hw.df = v; g_calls.c_df++; }
static void m_st(VkCommandBuffer cb, VkBool32 v) { (void)cb; g_hw.st = v; g_calls.c_st++; }
static void m_sop(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d) {
    (void)cb;
    if (f == VK_STENCIL_FACE_FRONT_BIT) { g_hw.fail_f = a, g_hw.zpass_f = b, g_hw.zfail_f = c, g_hw.sf_f = d; g_calls.c_sf++; }
    else { g_hw.fail_b = a, g_hw.zpass_b = b, g_hw.zfail_b = c, g_hw.sf_b = d; g_calls.c_sb++; }
}
static void m_bias(VkCommandBuffer cb, VkBool32 v) { (void)cb; g_hw.bias = v; g_calls.c_bias++; }
static void m_rdisc(VkCommandBuffer cb, VkBool32 v) { (void)cb; g_hw.rdisc = v; g_calls.c_rdisc++; }
static void m_prestart(VkCommandBuffer cb, VkBool32 v) { (void)cb; g_hw.prestart = v; g_calls.c_prestart++; }

#undef vkCmdSetCullMode
#undef vkCmdSetFrontFace
#undef vkCmdSetPrimitiveTopology
#undef vkCmdSetDepthTestEnable
#undef vkCmdSetDepthWriteEnable
#undef vkCmdSetDepthCompareOp
#undef vkCmdSetStencilTestEnable
#undef vkCmdSetStencilOp
#undef vkCmdSetDepthBiasEnable
#undef vkCmdSetRasterizerDiscardEnable
#undef vkCmdSetPrimitiveRestartEnable
#define vkCmdSetCullMode m_cull
#define vkCmdSetFrontFace m_front
#define vkCmdSetPrimitiveTopology m_topo
#define vkCmdSetDepthTestEnable m_dt
#define vkCmdSetDepthWriteEnable m_dw
#define vkCmdSetDepthCompareOp m_df
#define vkCmdSetStencilTestEnable m_st
#define vkCmdSetStencilOp m_sop
#define vkCmdSetDepthBiasEnable m_bias
#define vkCmdSetRasterizerDiscardEnable m_rdisc
#define vkCmdSetPrimitiveRestartEnable m_prestart

void rec_vertex_input(PFN_vkCmdSetVertexInputEXT fn, VkCommandBuffer cb, uint32_t nb,
                      const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                      const VkVertexInputAttributeDescription2EXT *a) {
    (void)fn; (void)cb;
    g_hw.n_bind = nb; g_hw.n_attr = na;
    memset(g_hw.vi_b, 0, sizeof g_hw.vi_b);
    memset(g_hw.vi_a, 0, sizeof g_hw.vi_a);
    if (nb) memcpy(g_hw.vi_b, b, sizeof b[0] * nb);
    if (na) memcpy(g_hw.vi_a, a, sizeof a[0] * na);
    g_calls.c_vi++;
}

void rec_bool(PFN_vkCmdSetDepthClampEnableEXT fn, VkCommandBuffer cb, VkBool32 v) {
    (void)cb;
    if ((uintptr_t)fn == 1) { g_hw.dclamp = v; g_calls.c_clamp++; }
    else { g_hw.a2c = v; g_calls.c_a2c++; }
}

void rec_polygon_mode(PFN_vkCmdSetPolygonModeEXT fn, VkCommandBuffer cb, VkPolygonMode m) {
    (void)fn; (void)cb;
    g_hw.poly_mode = m;
    g_calls.c_poly++;
}

void rec_blend_enable(PFN_vkCmdSetColorBlendEnableEXT fn, VkCommandBuffer cb, uint32_t first, uint32_t n,
                      const VkBool32 *en) {
    (void)fn; (void)cb; assert(first == 0);
    g_hw.en_n = n;
    for (uint32_t i = 0; i < n; i++) g_hw.en[i] = en[i];
    g_calls.c_en++;
}

void rec_blend_equation(PFN_vkCmdSetColorBlendEquationEXT fn, VkCommandBuffer cb, uint32_t first, uint32_t n,
                        const VkColorBlendEquationEXT *eq) {
    (void)fn; (void)cb; assert(first == 0);
    g_hw.eq_n = n;
    for (uint32_t i = 0; i < n; i++) g_hw.eq[i] = eq[i];
    g_calls.c_eq++;
}

void rec_write_mask(PFN_vkCmdSetColorWriteMaskEXT fn, VkCommandBuffer cb, uint32_t first, uint32_t n,
                    const VkColorComponentFlags *wm) {
    (void)fn; (void)cb; assert(first == 0);
    g_hw.wm_n = n;
    for (uint32_t i = 0; i < n; i++) g_hw.wm[i] = wm[i];
    g_calls.c_wm++;
}

/* Shared lock & graphics() fast-path stubs. */
typedef struct {
    int shared_locks, excl_locks, shared_held, excl_held;
} MockLock;

static void plat_mutex_lock_shared(MockLock *l) { l->shared_locks++; l->shared_held++; }
static void plat_mutex_unlock_shared(MockLock *l) { assert(l->shared_held > 0); l->shared_held--; }
static void plat_mutex_lock(MockLock *l) { l->excl_locks++; l->excl_held++; }
static void plat_mutex_unlock(MockLock *l) { assert(l->excl_held > 0); l->excl_held--; }

typedef struct { uint64_t key; } GKey;
typedef struct Layout Layout;
typedef struct {
    int building, guess, fast_tried;
    void *lay, *job;
    VkPipeline fast;
    VkPipeInfo info;
} Pipe;

static struct {
    MockLock lock;
    VkDevice device;
    int dyn_vi, dyn_clamp, dyn_poly, dyn_a2c, dyn_blend_en, dyn_blend_eq, dyn_wmask, f_dual_src;
    PFN_vkCmdSetVertexInputEXT set_vi;
    PFN_vkCmdSetPolygonModeEXT set_poly;
    PFN_vkCmdSetColorBlendEnableEXT set_blend_en;
    PFN_vkCmdSetColorBlendEquationEXT set_blend_eq;
    PFN_vkCmdSetColorWriteMaskEXT set_wmask;
    PFN_vkCmdSetDepthClampEnableEXT set_clamp;
    void *set_a2c;
    int guessing, gpl;
    struct { uint64_t la_used, dlinks, slow; double dlink_ms, dlink_max; } g;
    VkPipeStats stats;
} P;

static struct { int on; } U;
static int done_cv;
static Pipe *g_found_pipe;
static int g_miss_calls;
static int ensure_init(void) { return 1; }
static void gkey_fill(const VkPipeState *s, GKey *k) { k->key = (uintptr_t)s->vs; }
static uint64_t key_hash(const void *k, size_t n) { (void)n; return ((const GKey *)k)->key; }
static Pipe *pipe_find(const void *k, size_t n, uint64_t h) { (void)k; (void)n; (void)h; return g_found_pipe; }
static void keys_note(const VkPipeState *s) { (void)s; }
static Pipe *pipe_miss(const VkPipeState *s, const GKey *k, uint64_t h, int async, int *queued) {
    (void)s; (void)k; (void)h; (void)async; *queued = 0; g_miss_calls++; return NULL;
}

static uint64_t qpc(void) { return 0; }
static double ms_since(uint64_t t) { (void)t; return 0.0; }
static double now_ms(void) { return 0.0; }
static void job_lower(void *j) { (void)j; }
static void lib_wait(const VkPipeState *s, const GKey *k, Layout *l, double e) { (void)s; (void)k; (void)l; (void)e; }
static VkPipeline gpl_link(const VkPipeState *s, const GKey *k, void *l, int b, int *f) {
    (void)s; (void)k; (void)l; (void)b; *f = 1; return VK_NULL_HANDLE;
}
static void promote(Pipe *p) { (void)p; }
static void job_steal(Pipe *p) { (void)p; }
static void plat_cond_broadcast(int *c) { (void)c; }
static void plat_cond_wait_ms(int *c, MockLock *l, uint32_t ms) { (void)c; (void)l; (void)ms; }
static void mock_destroy_pipe(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) { (void)d; (void)p; (void)a; }
#undef vkDestroyPipeline
#define vkDestroyPipeline mock_destroy_pipe
uint64_t perf_begin(void) { return 0; }
void perf_end(int k, uint64_t t) { (void)k; (void)t; }
void perf_count(int k) { (void)k; }
const char *cfg_get(const char *k) { (void)k; return "0"; }
void rt_log(const char *fmt, ...) { (void)fmt; }

/* PIPE_EXTRACT */

static void vertex_input_ref(const VkPipeState *s, VtxInput *v) {
    GfxShaderIo io[32];
    uint32_t used = gfx_shader_inputs(s->vs, io), streams = 0;
    int zero = 0;
    memset(v, 0, sizeof *v);
    for (int l = 0; l < 32; l++) {
        if (!(used >> l & 1)) continue;
        VkVertexInputAttributeDescription2EXT *a = &v->a[v->n_attr++];
        *a = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                     .location = (uint32_t)l};
        va_st va = {0};
        if (l < 16) memcpy(&va, &s->vattr[l], sizeof va);
        VkFormat f = va.fmt ? vk_vertex_format((int)va.fmt, NULL) : VK_FORMAT_UNDEFINED;
        if (f != VK_FORMAT_UNDEFINED && va.stream < 16) {
            a->binding = va.stream, a->format = f, a->offset = va.offset;
            streams |= 1u << va.stream;
        } else {
            a->binding = VK_PIPE_ZERO_BINDING, a->format = zero_format(io[l].type);
            zero = 1;
        }
    }
    for (uint32_t b = 0; b < 16; b++) {
        if (!(streams >> b & 1)) continue;
        uint32_t div = s->vstream[b].divisor;
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = b, .stride = s->vstream[b].stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
    }
    if (zero)
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = VK_PIPE_ZERO_BINDING,
            .stride = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
}

static void assert_hw_matches_full(const VkPipeState *s) {
    DynHw actual = g_hw;
    DynCalls saved = g_calls;
    memset(&g_hw, 0, sizeof g_hw);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, s, NULL);
    DynHw expected = g_hw;
    g_hw = actual;
    g_calls = saved;

    assert(actual.cull == expected.cull && actual.front == expected.front && actual.topo == expected.topo);
    assert(actual.dt == expected.dt && actual.dw == expected.dw && actual.df == expected.df);
    assert(actual.st == expected.st && actual.bias == expected.bias);
    assert(actual.fail_f == expected.fail_f && actual.zpass_f == expected.zpass_f &&
           actual.zfail_f == expected.zfail_f && actual.sf_f == expected.sf_f);
    assert(actual.fail_b == expected.fail_b && actual.zpass_b == expected.zpass_b &&
           actual.zfail_b == expected.zfail_b && actual.sf_b == expected.sf_b);
    assert(actual.rdisc == expected.rdisc && actual.prestart == expected.prestart);
    assert(actual.dclamp == expected.dclamp && actual.poly_mode == expected.poly_mode && actual.a2c == expected.a2c);
    if (P.dyn_vi && s->vs) {
        assert(actual.n_bind == expected.n_bind && actual.n_attr == expected.n_attr);
        assert(memcmp(actual.vi_b, expected.vi_b, sizeof actual.vi_b[0] * expected.n_bind) == 0);
        assert(memcmp(actual.vi_a, expected.vi_a, sizeof actual.vi_a[0] * expected.n_attr) == 0);
    }
    int n = target_count(s);
    if (n) {
        assert(actual.en_n == (uint32_t)n && actual.eq_n == (uint32_t)n && actual.wm_n == (uint32_t)n);
        for (int i = 0; i < n; i++) {
            assert(actual.en[i] == expected.en[i]);
            assert(memcmp(&actual.eq[i], &expected.eq[i], sizeof actual.eq[i]) == 0);
            assert(actual.wm[i] == expected.wm[i]);
        }
    }
}

static void test_vertex_input(void) {
    GfxShader vs = {0};
    vs.in_mask = (1u << 0) | (1u << 3) | (1u << 5) | (1u << 18);
    vs.in_io[0].type = GFX_TYPE_FLOAT;
    vs.in_io[3].type = GFX_TYPE_UINT;
    vs.in_io[5].type = GFX_TYPE_SINT;
    vs.in_io[18].type = GFX_TYPE_FLOAT;

    VkPipeState s;
    memset(&s, 0, sizeof s);
    s.vs = &vs;
    va_st a0 = {.fmt = 1, .stream = 2, .offset = 8};
    va_st a3 = {.fmt = 0, .stream = 0, .offset = 0};    // zero fallback (UINT)
    va_st a5 = {.fmt = 2, .stream = 17, .offset = 4};   // stream >= 16 -> zero fallback (SINT)
    memcpy(&s.vattr[0], &a0, sizeof a0);
    memcpy(&s.vattr[3], &a3, sizeof a3);
    memcpy(&s.vattr[5], &a5, sizeof a5);
    s.vstream[2].stride = 32;
    s.vstream[2].divisor = 2;

    VtxInput v_opt, v_ref;
    memset(&v_opt, 0xaa, sizeof v_opt);   // poison stack buffer to prove every written field is initialized!
    vertex_input(&s, &v_opt);
    vertex_input_ref(&s, &v_ref);

    assert(v_opt.n_attr == v_ref.n_attr && v_opt.n_bind == v_ref.n_bind);
    for (uint32_t i = 0; i < v_ref.n_attr; i++)
        assert(memcmp(&v_opt.a[i], &v_ref.a[i], sizeof v_ref.a[i]) == 0);
    for (uint32_t i = 0; i < v_ref.n_bind; i++)
        assert(memcmp(&v_opt.b[i], &v_ref.b[i], sizeof v_ref.b[i]) == 0);
}

static void test_dynamic_state_pruning(void) {
    memset(&P, 0, sizeof P);
    P.dyn_vi = P.dyn_clamp = P.dyn_poly = P.dyn_a2c = P.dyn_blend_en = P.dyn_blend_eq = P.dyn_wmask = 1;
    P.f_dual_src = 1;
    P.set_clamp = (PFN_vkCmdSetDepthClampEnableEXT)(uintptr_t)1;
    P.set_a2c = (void *)(uintptr_t)2;

    GfxShader vs1 = {.in_mask = (1u << 0) | (1u << 2)};
    vs1.in_io[0].type = GFX_TYPE_FLOAT; vs1.in_io[2].type = GFX_TYPE_FLOAT;

    /* vs2: same mask, different type on mapped attr 0 (not zero-fallback) -> vertex_input identical! */
    GfxShader vs2 = {.in_mask = (1u << 0) | (1u << 2)};
    vs2.in_io[0].type = GFX_TYPE_UINT; vs2.in_io[2].type = GFX_TYPE_FLOAT;

    /* vs3: same mask, different type on zero-fallback attr 2 -> MUST re-emit vertex_input! */
    GfxShader vs3 = {.in_mask = (1u << 0) | (1u << 2)};
    vs3.in_io[0].type = GFX_TYPE_FLOAT; vs3.in_io[2].type = GFX_TYPE_UINT;

    /* vs4: different mask -> MUST re-emit vertex_input! */
    GfxShader vs4 = {.in_mask = (1u << 0)};
    vs4.in_io[0].type = GFX_TYPE_FLOAT;

    GfxShader fs1 = {.out_mask = 0x3u};
    GfxShader fs2 = {.out_mask = 0x3u};   // same outputs as fs1 -> skip all blend calls!
    GfxShader fs3 = {.out_mask = 0x1u};   // different outputs -> emit ONLY wm!
    GfxShader fs_all = {.out_mask = 0xffu};

    VkPipeState p, s;
    memset(&p, 0, sizeof p);
    p.vs = &vs1; p.fs = &fs1;
    va_st a0 = {.fmt = 1, .stream = 0, .offset = 0};
    memcpy(&p.vattr[0], &a0, sizeof a0);
    p.vstream[0].stride = 12;
    p.poly.cull = 1; p.poly.front = NVN_FRONT_FACE_CW; p.poly.mode = NVN_POLYGON_MODE_FILL;
    p.ds.dt = 1; p.ds.dw = 1; p.ds.df = 2;
    p.color_fmt[0] = 1; p.color_fmt[1] = 3;
    p.color.blend_en = 0x1;
    p.chmask.mask = 0xff;
    p.blend[0] = (blend_st){.src_rgb = 1, .dst_rgb = 2, .eq_rgb = 0, .src_a = 1, .dst_a = 2, .eq_a = 0};

    /* Initial full emit (p == NULL). */
    memset(&g_hw, 0, sizeof g_hw);
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &p, NULL);
    assert_hw_matches_full(&p);
    assert(g_calls.c_vi == 1 && g_calls.c_en == 1 && g_calls.c_eq == 1 && g_calls.c_wm == 1);

    /* 1. Switch vs1 -> vs2 (same IO + same zero-attr type) and fs1 -> fs2 (same output mask): 0 calls emitted! */
    s = p; s.vs = &vs2; s.fs = &fs2;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_vi == 0 && g_calls.c_en == 0 && g_calls.c_eq == 0 && g_calls.c_wm == 0);
    assert(g_calls.c_cull == 0 && g_calls.c_dt == 0);
    p = s;

    /* 2. Switch vs2 -> vs3 (zero-attr 2 type FLOAT -> UINT): MUST emit ONLY vertex_input! */
    s = p; s.vs = &vs3;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_vi == 1 && g_calls.c_en == 0 && g_calls.c_eq == 0 && g_calls.c_wm == 0);
    p = s;

    /* 3. Switch vs3 -> vs4 (different mask): emits vertex_input. */
    s = p; s.vs = &vs4;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_vi == 1);
    p = s;

    /* 4. Switch fs2 -> fs3 (output mask 0x3 -> 0x1): skips en & eq, emits ONLY wm! */
    s = p; s.fs = &fs3;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_en == 0 && g_calls.c_eq == 0 && g_calls.c_wm == 1);
    p = s;

    /* 5. Switch fs3 -> NULL -> fs_all (fs != NULL parity): emits wm cleanly! */
    s = p; s.fs = NULL;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_wm == 1 && g_calls.c_en == 0 && g_calls.c_eq == 0);
    p = s;

    s = p; s.fs = &fs_all;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_wm == 1 && g_calls.c_en == 0 && g_calls.c_eq == 0);
    p = s;

    /* 6. Dual-source SRC1 toggle with same fs & same chmask: emits eq AND wm (clears target 1 wmask)! */
    s = p; s.blend[0].src_rgb = NVN_BLEND_FUNC_SRC1_COLOR;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_en == 0 && g_calls.c_eq == 1 && g_calls.c_wm == 1 && g_hw.wm[1] == 0);
    p = s;

    /* 7. Toggle SRC1 off via blend_en=0 (same blend[], same chmask, same fs): emits en AND wm (restores target 1 wmask)! */
    s = p; s.color.blend_en = 0;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_en == 1 && g_calls.c_eq == 0 && g_calls.c_wm == 1 && g_hw.wm[1] == 0xf);
    p = s;

    /* 8. Only blend_en changes (non-SRC1): emits ONLY en! */
    s = p; s.blend[0].src_rgb = 1;
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    p = s;
    s = p; s.color.blend_en = 0x2;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_en == 1 && g_calls.c_eq == 0 && g_calls.c_wm == 0);
    p = s;

    /* 9. Target count changes (n = 2 -> 0 -> 3 -> 1): emits all 3 blend states whenever n != prev_n! */
    s = p; s.color_fmt[0] = 0; s.color_fmt[1] = 0;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert(g_calls.c_en == 0 && g_calls.c_eq == 0 && g_calls.c_wm == 0);
    p = s;

    s = p; s.color_fmt[0] = 1; s.color_fmt[1] = 2; s.color_fmt[2] = 3;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_en == 1 && g_calls.c_eq == 1 && g_calls.c_wm == 1);
    p = s;

    s = p; s.color_fmt[1] = 0; s.color_fmt[2] = 0;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_en == 1 && g_calls.c_eq == 1 && g_calls.c_wm == 1);
    p = s;

    /* 10. poly and ds diffs only fire when their structs differ. */
    s = p; s.poly.cull = 2; s.ds.dw = 0;
    memset(&g_calls, 0, sizeof g_calls);
    vk_pipeline_set_dynamic_from(VK_NULL_HANDLE, &s, &p);
    assert_hw_matches_full(&s);
    assert(g_calls.c_cull == 1 && g_calls.c_front == 0 && g_calls.c_bias == 0 && g_calls.c_poly == 0);
    assert(g_calls.c_dw == 1 && g_calls.c_dt == 0 && g_calls.c_df == 0 && g_calls.c_st == 0);
}

static void test_shared_pipe_lock(void) {
    memset(&P, 0, sizeof P);
    GfxShader vs = {0};
    VkPipeState s;
    memset(&s, 0, sizeof s);
    s.vs = &vs;

    Pipe ready = {.building = 0, .guess = 0, .info = {.pipeline = (VkPipeline)(uintptr_t)0x5555}};
    g_found_pipe = &ready;

    /* 1. Ready hit (!guess): takes ONLY shared lock (0 exclusive locks!), increments hits atomically. */
    const VkPipeInfo *pi = NULL;
    int pending = 99;
    VkPipeline got = graphics(&s, &pi, 1, &pending, 0, 0);
    assert(got == ready.info.pipeline && pi == &ready.info && pending == 0);
    assert(P.lock.shared_locks == 1 && P.lock.excl_locks == 0);
    assert(P.lock.shared_held == 0 && P.lock.excl_held == 0);
    VkPipeStats st;
    vk_pipeline_stats(&st);
    assert(st.hits == 1 && P.lock.shared_locks == 2 && P.lock.excl_locks == 0);

    /* 2. Building entry: shared fast path declines, falls back to exclusive lock. */
    Pipe building = {.building = 1, .guess = 0, .info = {.pipeline = VK_NULL_HANDLE}};
    g_found_pipe = &building;
    got = graphics(&s, &pi, 1, &pending, 0, 0);
    assert(got == VK_NULL_HANDLE && pending == 1);
    assert(P.lock.shared_locks == 3 && P.lock.excl_locks == 1);
    assert(P.lock.shared_held == 0 && P.lock.excl_held == 0);

    /* 3. Lookahead guess entry (p->guess == 1): takes exclusive lock to clear p->guess and bump la_used. */
    Pipe guessed = {.building = 0, .guess = 1, .info = {.pipeline = (VkPipeline)(uintptr_t)0x6666}};
    g_found_pipe = &guessed;
    got = graphics(&s, &pi, 1, &pending, 0, 0);
    assert(got == guessed.info.pipeline && guessed.guess == 0 && P.g.la_used == 1);
    assert(P.lock.excl_locks == 2);
    vk_pipeline_stats(&st);
    assert(st.hits == 2);

    /* 4. Lookahead caller (guess == 1): skips shared fast path, takes exclusive lock. */
    int sl_before = P.lock.shared_locks, el_before = P.lock.excl_locks;
    got = graphics(&s, &pi, 1, &pending, 0, 1);
    assert(got == VK_NULL_HANDLE && P.lock.shared_locks == sl_before && P.lock.excl_locks == el_before + 1);

    /* 5. Cache miss (p == NULL): shared lookup misses, exclusive path invokes pipe_miss. */
    g_found_pipe = NULL;
    g_miss_calls = 0;
    got = graphics(&s, &pi, 1, &pending, 0, 0);
    assert(got == VK_NULL_HANDLE && g_miss_calls == 1);
    assert(P.lock.shared_held == 0 && P.lock.excl_held == 0);
}

int main(void) {
    test_vertex_input();
    test_dynamic_state_pruning();
    test_shared_pipe_lock();
    puts("draw_pipe_test: ok");
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=shutil.which('clang') or 'clang')
    ap.add_argument('--deps', type=Path, required=True)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    args = ap.parse_args()

    draw_src = (args.source_root / 'runtime/vk_exec_draw.c').read_text(encoding='utf-8')
    pipe_src = (args.source_root / 'runtime/vk_pipeline.c').read_text(encoding='utf-8')

    vb_code = VB_HARNESS.replace('/* BIND_VB_EXTRACT */',
                                 extract_function(draw_src, 'static void bind_vertex_buffers('))
    pipe_parts = [
        extract_struct(pipe_src, 'VtxInput'),
        extract_function(pipe_src, 'static VkFormat zero_format('),
        extract_function(pipe_src, 'static void vertex_input('),
        extract_function(pipe_src, 'static int target_count('),
        extract_function(pipe_src, 'static int is_src1('),
        extract_function(pipe_src, 'static int uses_src1('),
        extract_function(pipe_src, 'static int vertex_input_same('),
        extract_function(pipe_src, 'void vk_pipeline_set_dynamic_from('),
        extract_function(pipe_src, 'static VkPipeline graphics('),
        extract_function(pipe_src, 'void vk_pipeline_stats('),
    ]
    pipe_code = PIPE_HARNESS.replace('/* PIPE_EXTRACT */', '\n\n'.join(pipe_parts))

    inc = [
        '-I' + str(args.source_root / 'runtime'),
        '-I' + str(args.deps / 'include'),
        '-I' + str(args.deps / 'include' / 'vma'),
    ]
    with tempfile.TemporaryDirectory(prefix='dkb-draw-vb-dynstate-') as tmp:
        for name, code in (('vb', vb_code), ('pipe', pipe_code)):
            src = Path(tmp) / f'{name}.c'
            src.write_text(code, encoding='utf-8')
            for opt in ('-O0', '-O2'):
                exe = Path(tmp) / f'{name}{opt[2:]}.exe'
                subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Werror', *inc, str(src), '-o', str(exe)], check=True)
                res = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
                if res.stdout:
                    print(opt, res.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
