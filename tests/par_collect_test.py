"""Unit tests for parallel slice collection: compact snapshots, inline binds, DLSS pass latch, and par_collect_walk.

Extracts ParPassSnap, ParSnap, par_pass_take/load, par_snap_take/load, state_op, par_run_op, par_bind_op, par_take,
and par_collect_walk verbatim from runtime/vk_exec.c, plus vk_dlss_scene_draw from runtime/vk_dlss.c, and verifies them
at -O0 and -O2.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def extract_typedef(source, name):
    end = source.index(f'}} {name};') + len(f'}} {name};')
    start = source.rindex('typedef struct {', 0, end)
    return source[start:end]


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def extract_macro(source, name):
    m = re.search(rf'^#define {re.escape(name)}\b.*?(?:\\\n.*?)*$', source, re.M)
    if not m:
        raise ValueError(f'macro {name} not found')
    return m.group(0)


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_exec.h"
#include "vk_spec.h"

/* ---- stubs for par_bind_op fallback and par_collect_walk ---- */
static int g_fallback_calls;
static uint32_t g_fallback_last_op;
static int g_trace_draw_ops;

static void record_direct_op(uint32_t op, const void *payload, int depth);

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex;
    g_fallback_calls++;
    g_fallback_last_op = op;
    if (g_trace_draw_ops) record_direct_op(op, payload, -1);
}

/* ---- extracted vk_exec.c types and snapshot/bind functions ---- */
/* VK_EXEC_SNAP_EXTRACT */

/* ---- stubs and state for vk_dlss_scene_draw ---- */
#define CAM_BLOCK 0x100u
typedef struct {
    int have;
    uint32_t serial;
    float cam[64];
    VkTexImage z;
    VkFormat zfmt;
    uint32_t zlevel, zlayer;
} Scene;

static int state = 1;
static struct { uint32_t w, h; uint64_t frame; } J;
static Scene S, SH[VK_SPEC_HELPERS];
static int g_fsr_wanted = 0;
static int g_dlss_wanted = 1;
static int g_tex_locks = 0;
static const void *g_usnap_ptr = NULL;
static uint64_t g_usnap_addr = 0;
static int g_usnap_calls = 0;

int vk_fsr3_wanted(void) { return g_fsr_wanted; }
int vk_fsr4_wanted(void) { return 0; }
int vk_dlss_wanted(void) { return g_dlss_wanted; }
static uint64_t frames_now(void) { return 100; }
const char *cfg_value(const char *k) { (void)k; return "dlss"; }
void rt_log(const char *fmt, ...) { (void)fmt; }
const void *vk_usnap_bytes(uint64_t addr, size_t n) {
    (void)n;
    g_usnap_calls++;
    return addr == g_usnap_addr ? g_usnap_ptr : NULL;
}
void vk_texture_lock(void) { g_tex_locks++; }
void vk_texture_unlock(void) { g_tex_locks--; }
_Thread_local int vk_spec_here = 0;

/* ---- extracted vk_dlss.c function ---- */
/* VK_DLSS_EXTRACT */

/* ---- harness state and stubs for par_take / par_collect_walk differential tests ---- */
#define CALL_DEPTH 16
#define PAR_MAX_SLICES 32
static int g_par_max_ops = 256;
#define PAR_MAX_OPS g_par_max_ops

static VkExec X;
VkExecFrame vk_exec_frame;
static uint64_t draws_total;
static uint32_t pass_draws;
int vk_dump_armed;
static uint32_t wb_tag_walk;
static int pd_n;
static int split_draws, split_n = 100000;
static uint64_t par_end_op[OP_COUNT_ + 1];
static _Thread_local const op_hdr *walk_at;
static _Thread_local const op_hdr *walk_ret[CALL_DEPTH];
static _Thread_local int walk_depth;
static uint16_t pss0[PAR_MAX_SLICES];
static volatile uint8_t pdone[PAR_MAX_SLICES];
static _Thread_local int par_skip_start;

static struct {
    const op_hdr *next;
} P;

static struct {
    int on;
    const op_hdr **ops;
    int nops;
    ParSnap *snap;
    int start[PAR_MAX_SLICES], end[PAR_MAX_SLICES];
    int nslices, s0, upd;
    uint8_t soq[PAR_MAX_SLICES], supd[PAR_MAX_SLICES], snote[PAR_MAX_SLICES];
    uint32_t stag[PAR_MAX_SLICES];
    VkQueryControlFlags soqf[PAR_MAX_SLICES];
    int draws, slice_draws, slice_size;
    int oq;
    VkQueryControlFlags oq_flags;
    uint8_t dlss_done, inline_binds;
} PC;

static int g_par_indirect_on = 1;
static int g_par_slice_size = 2;
static int g_sync_noting = 1;
static int g_par_inline_binds = 1;
static int g_use_fast = 0;
static int g_par_start_allow = 1;

typedef struct {
    int nops, nslices, s0, draws, publish_calls;
    const op_hdr *ops[256];
    int start[PAR_MAX_SLICES], end[PAR_MAX_SLICES];
    ParSnap snap[PAR_MAX_SLICES];
    uint8_t soq[PAR_MAX_SLICES], supd[PAR_MAX_SLICES], snote[PAR_MAX_SLICES], pdone_v[PAR_MAX_SLICES];
    uint32_t stag[PAR_MAX_SLICES];
    VkQueryControlFlags soqf[PAR_MAX_SLICES];
    uint16_t pss0_v[PAR_MAX_SLICES];
} FlushRec;

typedef struct {
    uint32_t op;
    const void *payload;
    int depth;
    const op_hdr *w_at;
    int w_depth;
    const op_hdr *w_ret[CALL_DEPTH];
} DirectRec;

typedef struct {
    int nflush;
    FlushRec flushes[16];
    int ndirect;
    DirectRec direct[512];
    int par_start_calls, par_flush_calls, par_publish_calls;
    int settle_calls, split_calls;
    VkExec ex_final;
    uint64_t draws_total, draws_seen;
    uint32_t pass_draws;
    uint64_t par_end_op[OP_COUNT_ + 1];
    int pc_on, pc_nops, pc_nslices, pc_draws, pc_slice_draws, pc_slice_size;
    uint8_t pc_dlss_done;
    const op_hdr *pc_ops[256];
    const op_hdr *final_walk_at;
    int final_walk_depth;
} RunTrace;

static RunTrace *g_trace;

static void record_direct_op(uint32_t op, const void *payload, int depth) {
    if (!g_trace) return;
    assert(g_trace->ndirect < 512);
    DirectRec *r = &g_trace->direct[g_trace->ndirect++];
    memset(r, 0, sizeof *r);
    r->op = op;
    r->payload = payload;
    r->depth = depth;
    r->w_at = walk_at;
    r->w_depth = walk_depth;
    for (int i = 0; i < walk_depth && i < CALL_DEPTH; i++) r->w_ret[i] = walk_ret[i];
}

void rt_fatal(const char *fmt, ...) {
    (void)fmt;
    abort();
}

static int par_indirect(void) { return g_par_indirect_on; }
static int par_size(void) { return g_par_slice_size; }
int vk_sync_noting(void) { return g_sync_noting; }

static void par_publish(void) {
    PC.end[PC.nslices - 1] = PC.nops;
    if (g_trace) g_trace->par_publish_calls++;
}

static int par_settle_ready(VkExec *ex) {
    (void)ex;
    if (g_trace) g_trace->settle_calls++;
    return 1;
}

static void maybe_split(VkExec *ex) {
    (void)ex;
    if (g_trace) g_trace->split_calls++;
}

static int par_start(VkExec *ex) {
    if (g_trace) g_trace->par_start_calls++;
    if (!g_par_start_allow || ex->in_pass) return 0;
    ex->in_pass = 1;
    PC.nops = 0;
    PC.nslices = 0;
    PC.s0 = 0;
    PC.on = 1;
    PC.draws = 0;
    PC.dlss_done = 0;
    PC.inline_binds = (uint8_t)g_par_inline_binds;
    PC.slice_size = PC.slice_draws = par_size();
    PC.oq = 1;
    PC.oq_flags = VK_QUERY_CONTROL_PRECISE_BIT;
    PC.upd = 2;
    return 1;
}

static void par_flush(VkExec *ex) {
    if (!PC.on) return;
    if (g_trace) g_trace->par_flush_calls++;
    PC.on = 0;
    if (PC.nslices) par_publish();
    if (g_trace) {
        assert(g_trace->nflush < 16);
        FlushRec *f = &g_trace->flushes[g_trace->nflush++];
        memset(f, 0, sizeof *f);
        f->nops = PC.nops;
        f->nslices = PC.nslices;
        f->s0 = PC.s0;
        f->draws = PC.draws;
        f->publish_calls = g_trace->par_publish_calls;
        assert(PC.nops <= 256);
        for (int i = 0; i < PC.nops; i++) f->ops[i] = PC.ops[i];
        for (int i = 0; i < PC.nslices; i++) {
            f->start[i] = PC.start[i];
            f->end[i] = PC.end[i];
            f->snap[i] = PC.snap[i];
            f->soq[i] = PC.soq[i];
            f->supd[i] = PC.supd[i];
            f->snote[i] = PC.snote[i];
            f->stag[i] = PC.stag[i];
            f->soqf[i] = PC.soqf[i];
            f->pss0_v[i] = pss0[i];
            f->pdone_v[i] = pdone[i];
        }
    }
    ex->tex_serial++;
    ex->in_pass = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
}

static void walk(VkExec *ex, const op_hdr *h, int depth);

/* ---- extracted vk_exec.c walk/collection functions ---- */
/* VK_EXEC_WALK_EXTRACT */

static void walk(VkExec *ex, const op_hdr *h, int depth) {
    for (;;) {
        if (ex == &X) walk_at = h, walk_depth = depth;
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) abort();
        const void *a = nvn_op_payload(h);
        int skip_par = par_skip_start;
        par_skip_start = 0;
        if (g_use_fast) {
            if (PC.on && !P.next && h->op != OP_END) {
                int res = 0;
                h = par_collect_walk(ex, h, depth, &res);
                if (res) continue;
                a = nvn_op_payload(h);
            } else if (h->op != OP_END && h->op != OP_BEGIN && h->op != OP_JUMP && h->op != OP_CALL) {
                if (PC.on) {
                    if (par_take(ex, h, a)) {
                        if (par_run_op(h->op) == 2) draws_total++, pass_draws++;
                        h = nvn_op_next(h);
                        continue;
                    }
                    par_end_op[h->op]++;
                    par_flush(ex);
                } else if (!skip_par && par_run_op(h->op) == 2 && par_start(ex)) {
                    if (!P.next) {
                        int res = 0;
                        h = par_collect_walk(ex, h, depth, &res);
                        if (res) continue;
                        a = nvn_op_payload(h);
                    } else if (par_take(ex, h, a)) {
                        draws_total++, pass_draws++;
                        h = nvn_op_next(h);
                        continue;
                    } else {
                        par_end_op[h->op]++;
                        par_flush(ex);
                    }
                }
            }
        } else {
            if (h->op != OP_END && h->op != OP_BEGIN && h->op != OP_JUMP && h->op != OP_CALL) {
                if (PC.on) {
                    if (par_take(ex, h, a)) {
                        if (par_run_op(h->op) == 2) draws_total++, pass_draws++;
                        h = nvn_op_next(h);
                        continue;
                    }
                    par_end_op[h->op]++;
                    par_flush(ex);
                } else if (par_run_op(h->op) == 2 && par_start(ex)) {
                    if (par_take(ex, h, a)) {
                        draws_total++, pass_draws++;
                        h = nvn_op_next(h);
                        continue;
                    }
                    par_end_op[h->op]++;
                    par_flush(ex);
                }
            }
        }
        switch (h->op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) abort();
            if (ex == &X) walk_ret[depth] = nvn_op_next(h);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        case OP_SET_RT:
            record_direct_op(h->op, a, depth);
            if (memcmp(&ex->rt, a, sizeof ex->rt)) {
                ex->in_pass = 0;
                ex->rt = *(const p_set_rt *)a;
                ex->rt_resolved = 0;
            }
            break;
        case OP_BARRIER:
            record_direct_op(h->op, a, depth);
            ex->game_barrier = 1;
            break;
        case OP_WAIT_SYNC:
            record_direct_op(h->op, a, depth);
            break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DISPATCH:
            record_direct_op(h->op, a, depth);
            if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND) {
                vk_exec_frame.draws_seen++;
                split_draws++;
            } else {
                vk_exec_frame.dispatches++;
                split_draws++;
            }
            draws_total++;
            break;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CONST:
            record_direct_op(h->op, a, depth);
            par_bind_op(ex, h->op, a);
            break;
        default:
            record_direct_op(h->op, a, depth);
            assert(state_op(ex, h->op, a));
            break;
        }
        if ((h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND) && ex->in_pass) pass_draws++;
        if (pd_n) par_settle_ready(ex);
        if (split_draws >= split_n) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

static void test_pass_snap(void) {
    VkExec ex, w;
    memset(&ex, 0, sizeof ex);
    memset(&w, 0x5a, sizeof w);

    ex.fence = 42;
    ex.submitted_ticks = 9999;
    ex.in_pass = 1;
    ex.rt_resolved = 1;
    ex.render_disabled = 0;
    ex.has_render_cond = 1;
    ex.pass_w = 1920;
    ex.pass_h = 1080;
    ex.pass_layers = 2;
    ex.pass_noatt = 0;
    ex.pass_ncolor = 2;
    ex.pass_samples = 1;
    ex.pass_color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    ex.pass_color[1] = VK_FORMAT_R16G16B16A16_SFLOAT;
    ex.pass_depth = VK_FORMAT_D32_SFLOAT;
    ex.pass_stencil = VK_FORMAT_UNDEFINED;
    ex.color[0].image = (VkImage)(uintptr_t)0x1111;
    ex.color[0].width = 1920;
    ex.color[1].image = (VkImage)(uintptr_t)0x2222;
    ex.depth.image = (VkImage)(uintptr_t)0x3333;
    ex.depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    ex.fbl_img[0] = (VkImage)(uintptr_t)0x1111;
    ex.fbl_aspect[0] = VK_IMAGE_ASPECT_COLOR_BIT;
    ex.nfbl = 1;
    ex.fbl_mask = 1;
    ex.pass_serial = 77;

    ParPassSnap ps;
    memset(&ps, 0xcd, sizeof ps);
    par_pass_take(&ps, &ex);
    par_pass_load(&w, &ps);

    assert(w.fence == 42);
    assert(w.submitted_ticks == 9999);
    assert(w.in_pass == 1 && w.rt_resolved == 1);
    assert(w.render_disabled == 0 && w.has_render_cond == 1);
    assert(w.pass_w == 1920 && w.pass_h == 1080 && w.pass_layers == 2);
    assert(w.pass_ncolor == 2 && w.pass_samples == 1);
    assert(w.pass_color[0] == VK_FORMAT_R8G8B8A8_UNORM);
    assert(w.pass_color[1] == VK_FORMAT_R16G16B16A16_SFLOAT);
    assert(w.pass_depth == VK_FORMAT_D32_SFLOAT);
    assert(w.color[0].image == (VkImage)(uintptr_t)0x1111);
    assert(w.color[1].image == (VkImage)(uintptr_t)0x2222);
    assert(w.depth.image == (VkImage)(uintptr_t)0x3333);
    assert(w.fbl_img[0] == (VkImage)(uintptr_t)0x1111);
    assert(w.nfbl == 1 && w.fbl_mask == 1 && w.pass_serial == 77);
}

static void test_slice_snap_roundtrip_and_shrink(void) {
    VkExec ex, w;
    ParSnap s1, s2, s3;
    memset(&ex, 0, sizeof ex);
    memset(&w, 0, sizeof w);

    /* Slice 1: populated slots across all 3 graphics stages, viewports, vbufs, push constants. */
    ex.tex_pool.addr = 0x10000;
    ex.samp_pool.addr = 0x20000;
    ex.has_vp = 1;
    ex.has_scissor = 1;
    ex.scissor.x = 10; ex.scissor.y = 20; ex.scissor.w = 640; ex.scissor.h = 360;
    ex.depth_range[0] = 0.0f; ex.depth_range[1] = 1.0f;
    ex.point_size = 4.0f;
    ex.blend_color[0] = 0.25f; ex.blend_color[3] = 0.75f;
    ex.st_vmask[0] = 0xaa; ex.st_wmask[1] = 0x55; ex.st_ref[0] = 7;
    ex.poly_offset.factor = 1.5f;
    ex.prim_restart.enable = 1; ex.prim_restart.index = 0xffffu;
    ex.pipe.depth_clamp = 1;
    ex.prog = (const prog_t *)(uintptr_t)0xfeedface;
    ex.prog_stages = 0x3;
    ex.push_set[0][0] = 0x5ull;
    ex.push[0][0][0] = 0x12345678u;
    ex.push[0][0][2] = 0x9abcdef0u;

    ex.vp_hi = 4;
    for (int i = 0; i < 4; i++) ex.vp[i][2] = (float)(100 + i);
    ex.vbuf_hi = 5;
    for (int i = 0; i < 5; i++) ex.vbuf[i].address = 0x5000 + (uint64_t)i * 64, ex.vbuf[i].size = 64;
    for (int st = 0; st < 3; st++) {
        ex.ubo_hi[st] = (uint8_t)(6 - st);
        for (int i = 0; i < ex.ubo_hi[st]; i++) ex.ubo[st][i].addr = 0x1000 + (uint64_t)st * 100 + (uint64_t)i;
        ex.ssbo_hi[st] = (uint8_t)(4 - st);
        for (int i = 0; i < ex.ssbo_hi[st]; i++) ex.ssbo[st][i].addr = 0x2000 + (uint64_t)st * 100 + (uint64_t)i;
        ex.tex_hi[st] = (uint8_t)(10 - st * 2);
        for (int i = 0; i < ex.tex_hi[st]; i++) ex.tex[st][i] = 0x3000 + (uint64_t)st * 100 + (uint64_t)i;
        ex.samp_hi[st] = (uint8_t)(8 - st * 2);
        for (int i = 0; i < ex.samp_hi[st]; i++) ex.samp[st][i] = 0x4000 + (uint64_t)st * 100 + (uint64_t)i;
        ex.img_hi[st] = (uint8_t)(3 - st);
        for (int i = 0; i < ex.img_hi[st]; i++) ex.img[st][i] = 0x5000 + (uint64_t)st * 100 + (uint64_t)i;
    }

    memset(&s1, 0xdd, sizeof s1);
    par_snap_take(&s1, &ex);
    par_snap_load(&w, &s1);

    assert(w.tex_pool.addr == 0x10000 && w.samp_pool.addr == 0x20000);
    assert(w.has_vp == 1 && w.has_scissor == 1 && w.scissor.w == 640);
    assert(w.point_size == 4.0f && w.blend_color[3] == 0.75f);
    assert(w.st_vmask[0] == 0xaa && w.st_wmask[1] == 0x55 && w.st_ref[0] == 7);
    assert(w.pipe.depth_clamp == 1 && w.prog == ex.prog && w.prog_stages == 0x3);
    assert(w.push_set[0][0] == 0x5ull && w.push[0][0][0] == 0x12345678u && w.push[0][0][2] == 0x9abcdef0u);
    assert(w.vp_hi == 4 && w.vp[3][2] == 103.0f);
    assert(w.vbuf_hi == 5 && w.vbuf[4].address == 0x5000 + 4 * 64);
    for (int st = 0; st < 3; st++) {
        assert(w.ubo_hi[st] == ex.ubo_hi[st] && w.ssbo_hi[st] == ex.ssbo_hi[st]);
        assert(w.tex_hi[st] == ex.tex_hi[st] && w.samp_hi[st] == ex.samp_hi[st] && w.img_hi[st] == ex.img_hi[st]);
        for (int i = 0; i < w.ubo_hi[st]; i++) assert(w.ubo[st][i].addr == ex.ubo[st][i].addr);
        for (int i = 0; i < w.tex_hi[st]; i++) assert(w.tex[st][i] == ex.tex[st][i]);
    }

    /* Simulate slice 1 replay raising worker high-water marks even further. */
    w.vp[9][2] = 999.0f; w.vp_hi = 10;
    w.vbuf[11].address = 0xdeadbeef; w.vbuf_hi = 12;
    w.ubo[0][13].addr = 0xbeef; w.ubo_hi[0] = 14;
    w.ssbo[1][9].addr = 0xcafe; w.ssbo_hi[1] = 10;
    w.tex[1][40] = 0x7777; w.tex_hi[1] = 41;
    w.samp[2][25] = 0x8888; w.samp_hi[2] = 26;
    w.img[0][7] = 0x9999; w.img_hi[0] = 8;

    /* Slice 2: much smaller high-water marks (e.g. after state_reset), push_set0 == 0. */
    memset(&ex, 0, sizeof ex);
    ex.vp_hi = 1; ex.vp[0][2] = 50.0f;
    ex.vbuf_hi = 2; ex.vbuf[1].address = 0x7000;
    ex.ubo_hi[0] = 2; ex.ubo[0][1].addr = 0x8001;
    ex.ssbo_hi[1] = 1; ex.ssbo[1][0].addr = 0x8002;
    ex.tex_hi[1] = 3; ex.tex[1][2] = 0x8003;
    ex.samp_hi[2] = 1; ex.samp[2][0] = 0x8004;
    ex.img_hi[0] = 1; ex.img[0][0] = 0x8005;

    par_snap_take(&s2, &ex);
    par_snap_load(&w, &s2);

    assert(w.push_set[0][0] == 0);
    assert(w.vp_hi == 1 && w.vp[0][2] == 50.0f && w.vp[1][2] == 0.0f && w.vp[9][2] == 0.0f);
    assert(w.vbuf_hi == 2 && w.vbuf[1].address == 0x7000 && w.vbuf[2].address == 0 && w.vbuf[11].address == 0);
    assert(w.ubo_hi[0] == 2 && w.ubo[0][1].addr == 0x8001 && w.ubo[0][2].addr == 0 && w.ubo[0][13].addr == 0);
    assert(w.ubo_hi[1] == 0 && w.ubo[1][0].addr == 0);
    assert(w.ssbo_hi[1] == 1 && w.ssbo[1][0].addr == 0x8002 && w.ssbo[1][9].addr == 0);
    assert(w.tex_hi[1] == 3 && w.tex[1][2] == 0x8003 && w.tex[1][3] == 0 && w.tex[1][40] == 0);
    assert(w.samp_hi[2] == 1 && w.samp[2][0] == 0x8004 && w.samp[2][25] == 0);
    assert(w.img_hi[0] == 1 && w.img[0][0] == 0x8005 && w.img[0][7] == 0);

    /* Slice 3: completely empty high-water marks clear the remaining active prefix. */
    memset(&ex, 0, sizeof ex);
    par_snap_take(&s3, &ex);
    par_snap_load(&w, &s3);
    assert(w.vp_hi == 0 && w.vp[0][2] == 0.0f);
    assert(w.vbuf_hi == 0 && w.vbuf[1].address == 0);
    assert(w.ubo_hi[0] == 0 && w.ubo[0][1].addr == 0);
    assert(w.tex_hi[1] == 0 && w.tex[1][2] == 0);
    assert(w.img_hi[0] == 0 && w.img[0][0] == 0);
}

static void test_par_bind_op(void) {
    VkExec ex;
    memset(&ex, 0, sizeof ex);
    g_fallback_calls = 0;

    p_bind_program bp = {.prog = (const prog_t *)(uintptr_t)0x1234, .stages = 0x7};
    par_bind_op(&ex, OP_BIND_PROGRAM, &bp);
    assert(ex.prog == bp.prog && ex.prog_stages == 0x7);

    p_bind_state bs = {0};
    blend_st bl = {.target = 2, .src_rgb = 5};
    bs.kind = 0; memcpy(bs.bytes, &bl, sizeof bl);
    par_bind_op(&ex, OP_BIND_STATE, &bs);
    assert(ex.pipe.blend[2].src_rgb == 5);
    float ps = 3.5f;
    bs.kind = 6; memcpy(bs.bytes, &ps, sizeof ps);
    par_bind_op(&ex, OP_BIND_STATE, &bs);
    assert(ex.point_size == 3.5f);

    p_bind_vattr va = {.first = 1, .n = 2, .st = {0x11, 0x22}};
    ex.pipe.vattr[5] = 0x99;
    par_bind_op(&ex, OP_BIND_VATTR, &va);
    assert(ex.pipe.vattr[1] == 0x11 && ex.pipe.vattr[2] == 0x22 && ex.pipe.vattr[5] == 0);

    p_bind_vstream vs = {.first = 0, .n = 1};
    vs.st[0].stride = 32;
    ex.pipe.vstream[3].stride = 64;
    par_bind_op(&ex, OP_BIND_VSTREAM, &vs);
    assert(ex.pipe.vstream[0].stride == 32 && ex.pipe.vstream[3].stride == 0);

    p_bind_vbuf vb = {.first = 2, .n = 2};
    vb.r[0].address = 0xa000; vb.r[1].address = 0xb000;
    par_bind_op(&ex, OP_BIND_VBUF, &vb);
    assert(ex.vbuf_hi == 4 && ex.vbuf[2].address == 0xa000 && ex.vbuf[3].address == 0xb000);

    p_bind_buf bu = {.stage = 1, .index = 4, .addr = 0xc000, .size = 256};
    par_bind_op(&ex, OP_BIND_UBO, &bu);
    assert(ex.ubo_hi[1] == 5 && ex.ubo[1][4].addr == 0xc000);

    /* Stage 5 (compute) updates slot without touching graphics high-water marks [0..2]. */
    p_bind_buf bu_cs = {.stage = 5, .index = 10, .addr = 0xc500, .size = 64};
    par_bind_op(&ex, OP_BIND_UBO, &bu_cs);
    assert(ex.ubo[5][10].addr == 0xc500);
    assert(ex.ubo_hi[0] == 0 && ex.ubo_hi[1] == 5 && ex.ubo_hi[2] == 0);

    p_bind_buf bsbo = {.stage = 2, .index = 3, .addr = 0xd000, .size = 128};
    par_bind_op(&ex, OP_BIND_SSBO, &bsbo);
    assert(ex.ssbo_hi[2] == 4 && ex.ssbo[2][3].addr == 0xd000);

    p_bind_handles bh = {.stage = 0, .first = 4, .n = 3, .h = {11, 22, 33}};
    par_bind_op(&ex, OP_BIND_TEX, &bh);
    assert(ex.tex_hi[0] == 7 && ex.tex[0][4] == 11 && ex.tex[0][6] == 33);

    bh.stage = 1; bh.first = 2; bh.n = 2;
    par_bind_op(&ex, OP_BIND_SAMP, &bh);
    assert(ex.samp_hi[1] == 4 && ex.samp[1][2] == 11 && ex.samp[1][3] == 22);

    bh.stage = 2; bh.first = 1; bh.n = 2;
    par_bind_op(&ex, OP_BIND_IMG, &bh);
    assert(ex.img_hi[2] == 3 && ex.img[2][1] == 11 && ex.img[2][2] == 22);

    p_push_const pc = {.compute = 0, .table = 0, .offset = 8, .bytes = 8, .w = {0xaaaau, 0xbbbbu}};
    par_bind_op(&ex, OP_PUSH_CONST, &pc);
    assert(ex.push_set[0][0] == (3ull << 2) && ex.push[0][0][2] == 0xaaaau && ex.push[0][0][3] == 0xbbbbu);

    assert(g_fallback_calls == 0);

    /* Out-of-range binds must fall back to vk_exec_op_draw so warnings fire. */
    p_bind_buf bad_ssbo = {.stage = 0, .index = 16, .addr = 0x100, .size = 16};
    par_bind_op(&ex, OP_BIND_SSBO, &bad_ssbo);
    assert(g_fallback_calls == 1 && g_fallback_last_op == OP_BIND_SSBO);

    p_bind_handles bad_tex = {.stage = 0, .first = 120, .n = 16};
    par_bind_op(&ex, OP_BIND_TEX, &bad_tex);
    assert(g_fallback_calls == 2 && g_fallback_last_op == OP_BIND_TEX);

    p_bind_handles bad_img = {.stage = 0, .first = 30, .n = 4};
    par_bind_op(&ex, OP_BIND_IMG, &bad_img);
    assert(g_fallback_calls == 3 && g_fallback_last_op == OP_BIND_IMG);

    p_push_const bad_pc = {.compute = 0, .table = 1, .offset = 0, .bytes = 4};
    par_bind_op(&ex, OP_PUSH_CONST, &bad_pc);
    assert(g_fallback_calls == 4 && g_fallback_last_op == OP_PUSH_CONST);
}

static void test_dlss_latch(void) {
    VkExec ex;
    VkTexImage zimg;
    float cam_data[64];
    for (int i = 0; i < 64; i++) cam_data[i] = (float)(i + 1);

    memset(&ex, 0, sizeof ex);
    memset(&zimg, 0, sizeof zimg);
    memset(&S, 0, sizeof S);
    state = 1; g_fsr_wanted = 0; g_dlss_wanted = 1;
    J.w = 1920; J.h = 1080; J.frame = 99;

    zimg.image = (VkImage)(uintptr_t)0x9000;
    ex.color[0].image = (VkImage)(uintptr_t)0x8000;
    ex.depth.image = zimg.image;
    ex.depth.img = &zimg;
    ex.depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    ex.depth.width = 1920; ex.depth.height = 1080; ex.depth.samples = 1;
    ex.pass_serial = 11;

    /* Ineligible target (shadow map size mismatch): returns 1 immediately so caller latches. */
    ex.depth.width = 512; ex.depth.height = 512;
    g_usnap_calls = 0;
    assert(vk_dlss_scene_draw(&ex) == 1);
    assert(!S.have && g_usnap_calls == 0);
    ex.depth.width = 1920; ex.depth.height = 1080;

    /* Eligible target, draw 0 has wrong c3 size: returns 0 so draw 1 retries. */
    ex.ubo[0][0].addr = 0x50000;
    ex.ubo[0][0].size = 64;
    assert(vk_dlss_scene_draw(&ex) == 0);
    assert(!S.have);

    /* Eligible target, right size, but not in usnap: returns 0 so later draw retries. */
    ex.ubo[0][0].size = CAM_BLOCK;
    g_usnap_addr = 0x60000;
    g_usnap_ptr = cam_data;
    assert(vk_dlss_scene_draw(&ex) == 0);
    assert(!S.have && g_usnap_calls == 1);

    /* Draw 2 binds snapshotted CAM_BLOCK: captures scene and returns 1. */
    ex.ubo[0][0].addr = 0x60000;
    assert(vk_dlss_scene_draw(&ex) == 1);
    assert(S.have == 1 && S.serial == 11 && S.cam[0] == 1.0f && S.cam[63] == 64.0f);
    assert(g_tex_locks == 0);

    /* Same pass_serial again: returns 1 immediately without querying usnap. */
    g_usnap_calls = 0;
    assert(vk_dlss_scene_draw(&ex) == 1);
    assert(g_usnap_calls == 0);

    /* Temporal upscaler disabled: returns 1 immediately. */
    state = -1; g_fsr_wanted = 0;
    ex.pass_serial = 12;
    assert(vk_dlss_scene_draw(&ex) == 1);
    assert(S.serial == 11);
}

typedef struct {
    _Alignas(8) uint8_t mem[8192];
    size_t off;
} OpBuf;

static const op_hdr *opbuf_first(const OpBuf *b) {
    return (const op_hdr *)(const void *)b->mem;
}

static const op_hdr *emit_op(OpBuf *b, uint32_t op, const void *payload, size_t plen) {
    size_t sz = ((sizeof(op_hdr) + plen) + 7u) & ~(size_t)7u;
    assert(b->off + sz <= sizeof b->mem);
    op_hdr *h = (op_hdr *)(void *)(b->mem + b->off);
    memset(h, 0, sz);
    h->op = op;
    h->size = (uint32_t)sz;
    if (payload && plen) memcpy(h + 1, payload, plen);
    b->off += sz;
    return h;
}

static const op_hdr *emit_jump(OpBuf *b, const op_hdr *target) {
    p_jump j = {.target = (uint64_t)(uintptr_t)target};
    return emit_op(b, OP_JUMP, &j, sizeof j);
}

static const op_hdr *emit_call(OpBuf *b, const op_hdr *callee) {
    p_jump j = {.target = (uint64_t)(uintptr_t)callee};
    return emit_op(b, OP_CALL, &j, sizeof j);
}

static void run_streams(RunTrace *out, const op_hdr *const *streams, int nstreams, int use_fast,
                        const VkExec *init_ex, int use_X, int flush_end) {
    static const op_hdr *ops_buf[256];
    static ParSnap snap_buf[PAR_MAX_SLICES];
    memset(out, 0, sizeof *out);
    memset(&PC, 0, sizeof PC);
    memset(snap_buf, 0x5a, sizeof snap_buf);
    PC.ops = ops_buf;
    PC.snap = snap_buf;
    memset(&P, 0, sizeof P);
    memset(&vk_exec_frame, 0, sizeof vk_exec_frame);
    memset(par_end_op, 0, sizeof par_end_op);
    memset(pss0, 0, sizeof pss0);
    memset((void *)pdone, 0xaa, sizeof pdone);
    draws_total = 0;
    pass_draws = 0;
    wb_tag_walk = 7;
    split_draws = 0;
    par_skip_start = 0;
    walk_at = NULL;
    walk_depth = 0;
    memset(walk_ret, 0, sizeof walk_ret);
    g_use_fast = use_fast;
    g_trace = out;
    g_trace_draw_ops = 1;

    VkExec local_ex;
    VkExec *ex = use_X ? &X : &local_ex;
    if (init_ex) *ex = *init_ex;
    else memset(ex, 0, sizeof *ex);

    for (int i = 0; i < nstreams; i++) walk(ex, streams[i], 0);
    if (flush_end && PC.on) {
        par_end_op[OP_COUNT_]++;
        par_flush(ex);
    }

    out->ex_final = *ex;
    out->draws_total = draws_total;
    out->draws_seen = vk_exec_frame.draws_seen;
    out->pass_draws = pass_draws;
    memcpy(out->par_end_op, par_end_op, sizeof par_end_op);
    out->pc_on = PC.on;
    out->pc_nops = PC.nops;
    out->pc_nslices = PC.nslices;
    out->pc_draws = PC.draws;
    out->pc_slice_draws = PC.slice_draws;
    out->pc_slice_size = PC.slice_size;
    out->pc_dlss_done = PC.dlss_done;
    for (int i = 0; i < PC.nops && i < 256; i++) out->pc_ops[i] = PC.ops[i];
    out->final_walk_at = walk_at;
    out->final_walk_depth = walk_depth;

    g_trace_draw_ops = 0;
    g_trace = NULL;
}

static void assert_traces_equal(const RunTrace *ref, const RunTrace *fast) {
    assert(ref->nflush == fast->nflush);
    assert(ref->par_start_calls == fast->par_start_calls);
    assert(ref->par_flush_calls == fast->par_flush_calls);
    assert(ref->par_publish_calls == fast->par_publish_calls);
    assert(ref->ndirect == fast->ndirect);
    assert(ref->draws_total == fast->draws_total);
    assert(ref->draws_seen == fast->draws_seen);
    assert(ref->pass_draws == fast->pass_draws);
    assert(memcmp(ref->par_end_op, fast->par_end_op, sizeof ref->par_end_op) == 0);
    assert(memcmp(&ref->ex_final, &fast->ex_final, sizeof ref->ex_final) == 0);
    assert(ref->pc_on == fast->pc_on);
    assert(ref->pc_nops == fast->pc_nops);
    assert(ref->pc_nslices == fast->pc_nslices);
    assert(ref->pc_draws == fast->pc_draws);
    assert(ref->pc_slice_draws == fast->pc_slice_draws);
    assert(ref->pc_slice_size == fast->pc_slice_size);
    assert(ref->pc_dlss_done == fast->pc_dlss_done);
    assert(ref->final_walk_at == fast->final_walk_at);
    assert(ref->final_walk_depth == fast->final_walk_depth);
    for (int i = 0; i < ref->pc_nops; i++) assert(ref->pc_ops[i] == fast->pc_ops[i]);

    for (int f = 0; f < ref->nflush; f++) {
        const FlushRec *a = &ref->flushes[f], *b = &fast->flushes[f];
        assert(a->nops == b->nops);
        assert(a->nslices == b->nslices);
        assert(a->s0 == b->s0);
        assert(a->draws == b->draws);
        assert(a->publish_calls == b->publish_calls);
        for (int i = 0; i < a->nops; i++) assert(a->ops[i] == b->ops[i]);
        for (int i = 0; i < a->nslices; i++) {
            assert(a->start[i] == b->start[i]);
            assert(a->end[i] == b->end[i]);
            assert(a->soq[i] == b->soq[i]);
            assert(a->supd[i] == b->supd[i]);
            assert(a->snote[i] == b->snote[i]);
            assert(a->stag[i] == b->stag[i]);
            assert(a->soqf[i] == b->soqf[i]);
            assert(a->pss0_v[i] == b->pss0_v[i]);
            assert(a->pdone_v[i] == b->pdone_v[i]);
            VkExec wa, wb;
            memset(&wa, 0, sizeof wa);
            memset(&wb, 0, sizeof wb);
            par_snap_load(&wa, &a->snap[i]);
            par_snap_load(&wb, &b->snap[i]);
            assert(memcmp(&wa, &wb, sizeof wa) == 0);
        }
    }

    for (int d = 0; d < ref->ndirect; d++) {
        const DirectRec *a = &ref->direct[d], *b = &fast->direct[d];
        assert(a->op == b->op);
        assert(a->payload == b->payload);
        assert(a->depth == b->depth);
        assert(a->w_at == b->w_at);
        assert(a->w_depth == b->w_depth);
        for (int k = 0; k < a->w_depth && k < CALL_DEPTH; k++) assert(a->w_ret[k] == b->w_ret[k]);
    }
}

static void compare_scenario(const op_hdr *const *streams, int nstreams, const VkExec *init_ex, int use_X, int flush_end,
                             RunTrace *out_fast) {
    static RunTrace ref, fast;
    run_streams(&ref, streams, nstreams, 0, init_ex, use_X, flush_end);
    run_streams(&fast, streams, nstreams, 1, init_ex, use_X, flush_end);
    assert_traces_equal(&ref, &fast);
    if (out_fast) *out_fast = fast;
}

static void test_par_collect_walk_nested_calls_jumps_and_slices(void) {
    static OpBuf top, c1, c1_tail, c2;
    memset(&top, 0, sizeof top);
    memset(&c1, 0, sizeof c1);
    memset(&c1_tail, 0, sizeof c1_tail);
    memset(&c2, 0, sizeof c2);

    p_set_rt rt_a = {.n = 1};
    rt_a.color[0] = (const tex_t *)(uintptr_t)0x1000;
    p_set_rt rt_b = {.n = 1};
    rt_b.color[0] = (const tex_t *)(uintptr_t)0x2000;
    p_viewports vp = {.first = 0, .n = 1, .v = {{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f}}};
    p_bind_buf ubo1 = {.stage = 0, .index = 2, .addr = 0x40000, .size = 64};
    p_bind_buf ubo2 = {.stage = 1, .index = 3, .addr = 0x50000, .size = 128};
    p_bind_vbuf vb = {.first = 1, .n = 1, .r = {{.address = 0x60000, .size = 256}}};
    p_push_const pc = {.compute = 0, .table = 0, .offset = 4, .bytes = 4, .w = {0x12345678u}};
    p_wait_sync ws = {.sync = NULL};
    uint64_t draw_stub[4] = {1, 2, 3, 4};

    /* c2 (depth 2): OP_BEGIN, matching OP_SET_RT, OP_WAIT_SYNC, bind, 3 draws (forces slice cut), OP_END */
    emit_op(&c2, OP_BEGIN, NULL, 0);
    emit_op(&c2, OP_SET_RT, &rt_a, sizeof rt_a);
    emit_op(&c2, OP_WAIT_SYNC, &ws, sizeof ws);
    emit_op(&c2, OP_BIND_UBO, &ubo2, sizeof ubo2);
    emit_op(&c2, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&c2, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&c2, OP_DRAW_IDX_IND, draw_stub, sizeof draw_stub);
    emit_op(&c2, OP_END, NULL, 0);

    /* c1_tail: call c2, push_const, draw, OP_END */
    emit_op(&c1_tail, OP_BEGIN, NULL, 0);
    emit_call(&c1_tail, opbuf_first(&c2));
    emit_op(&c1_tail, OP_PUSH_CONST, &pc, sizeof pc);
    emit_op(&c1_tail, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&c1_tail, OP_END, NULL, 0);

    /* c1 (depth 1): OP_BEGIN, vbuf bind, OP_JUMP -> c1_tail */
    emit_op(&c1, OP_BEGIN, NULL, 0);
    emit_op(&c1, OP_BIND_VBUF, &vb, sizeof vb);
    emit_jump(&c1, opbuf_first(&c1_tail));

    /* top (depth 0): set rt_a, draw (starts pass), vp, ubo1, call c1, draw, set rt_b (flushes), OP_END */
    emit_op(&top, OP_BEGIN, NULL, 0);
    emit_op(&top, OP_SET_RT, &rt_a, sizeof rt_a);
    emit_op(&top, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&top, OP_VIEWPORTS, &vp, sizeof vp);
    emit_op(&top, OP_BIND_UBO, &ubo1, sizeof ubo1);
    emit_call(&top, opbuf_first(&c1));
    emit_op(&top, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&top, OP_SET_RT, &rt_b, sizeof rt_b);
    emit_op(&top, OP_END, NULL, 0);

    g_par_max_ops = 256;
    g_par_slice_size = 2;
    g_par_indirect_on = 1;
    g_par_inline_binds = 1;
    g_dlss_wanted = 0;
    vk_dump_armed = 0;
    pd_n = 0;

    static RunTrace fast;
    const op_hdr *streams[1] = {opbuf_first(&top)};
    compare_scenario(streams, 1, NULL, 1, 1, &fast);

    assert(fast.nflush == 1);
    assert(fast.flushes[0].draws == 6);
    assert(fast.flushes[0].nslices == 3);
    assert(fast.par_end_op[OP_SET_RT] == 1);
    /* Verify neither matching OP_SET_RT nor OP_WAIT_SYNC was pushed into PC.ops. */
    for (int i = 0; i < fast.flushes[0].nops; i++) {
        assert(fast.flushes[0].ops[i]->op != OP_SET_RT);
        assert(fast.flushes[0].ops[i]->op != OP_WAIT_SYNC);
    }
    /* Verify slices' snapshots saw the binds preceding each slice cut. */
    assert(fast.flushes[0].snap[0].vbuf_hi == 0 && fast.flushes[0].snap[0].ubo_hi[0] == 0);
    assert(fast.flushes[0].snap[1].vbuf_hi == 2 && fast.flushes[0].snap[1].ubo_hi[0] == 3);
    assert(fast.flushes[0].snap[2].ubo_hi[1] == 4);
}

static void test_par_collect_walk_mid_call_exits_and_unwind(void) {
    static OpBuf top, c1, c2;
    memset(&top, 0, sizeof top);
    memset(&c1, 0, sizeof c1);
    memset(&c2, 0, sizeof c2);

    p_bind_buf ubo = {.stage = 0, .index = 1, .addr = 0x71000, .size = 64};
    p_barrier bar = {.bits = 1};
    uint64_t draw_stub[4] = {10, 20, 30, 40};

    /* c2 (depth 2): draw, OP_BARRIER (mid-call exit!), OP_DISPATCH, draw (starts 2nd sliced pass in callee!), OP_END */
    emit_op(&c2, OP_BEGIN, NULL, 0);
    emit_op(&c2, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&c2, OP_BARRIER, &bar, sizeof bar);
    emit_op(&c2, OP_DISPATCH, draw_stub, sizeof draw_stub);
    emit_op(&c2, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&c2, OP_END, NULL, 0);

    /* c1 (depth 1): bind ubo, call c2, draw (continues 2nd pass after c2's OP_END!), OP_END */
    emit_op(&c1, OP_BEGIN, NULL, 0);
    emit_op(&c1, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_call(&c1, opbuf_first(&c2));
    emit_op(&c1, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&c1, OP_END, NULL, 0);

    /* top (depth 0): draw (starts 1st pass), call c1, draw (continues 2nd pass at depth 0!), OP_BARRIER (flushes 2nd), OP_END */
    emit_op(&top, OP_BEGIN, NULL, 0);
    emit_op(&top, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_call(&top, opbuf_first(&c1));
    emit_op(&top, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&top, OP_BARRIER, &bar, sizeof bar);
    emit_op(&top, OP_END, NULL, 0);

    g_par_max_ops = 256;
    g_par_slice_size = 2;
    g_par_indirect_on = 1;
    g_par_inline_binds = 1;
    vk_dump_armed = 0;
    pd_n = 1;
    split_n = 1;

    static RunTrace fast;
    const op_hdr *streams[1] = {opbuf_first(&top)};
    compare_scenario(streams, 1, NULL, 1, 1, &fast);

    assert(fast.nflush == 2);
    assert(fast.flushes[0].draws == 2);
    assert(fast.flushes[1].draws == 3);
    assert(fast.par_end_op[OP_BARRIER] == 2);
    assert(fast.ndirect == 3);
    assert(fast.direct[0].op == OP_BARRIER && fast.direct[0].depth == 2);
    assert(fast.direct[1].op == OP_DISPATCH && fast.direct[1].depth == 2);
    assert(fast.direct[2].op == OP_BARRIER && fast.direct[2].depth == 0);
    /* Verify post-call settle/split calls fire during unwind. */
    assert(fast.settle_calls > 0 && fast.split_calls > 0);
    pd_n = 0;
    split_n = 100000;
}

static void test_par_collect_walk_max_ops_and_indirect_stop(void) {
    static OpBuf top, c1;
    memset(&top, 0, sizeof top);
    memset(&c1, 0, sizeof c1);

    p_bind_buf ubo = {.stage = 0, .index = 0, .addr = 0x80000, .size = 64};
    uint64_t draw_stub[4] = {1, 2, 3, 4};

    /* c1 (depth 1): ubo, draw (op 3 -> hits PAR_MAX_OPS=3!), draw (starts 2nd pass), OP_DRAW_IDX_IND (stops when !par_indirect), OP_END */
    emit_op(&c1, OP_BEGIN, NULL, 0);
    emit_op(&c1, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_op(&c1, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&c1, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&c1, OP_DRAW_IDX_IND, draw_stub, sizeof draw_stub);
    emit_op(&c1, OP_END, NULL, 0);

    /* top (depth 0): draw (op 0), ubo (op 1), call c1, draw, OP_END */
    emit_op(&top, OP_BEGIN, NULL, 0);
    emit_op(&top, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&top, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_call(&top, opbuf_first(&c1));
    emit_op(&top, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&top, OP_END, NULL, 0);

    /* Case A: PAR_MAX_OPS=3 hit on OP_DRAW_IDX mid-OP_CALL, plus !par_indirect mid-OP_CALL. */
    g_par_max_ops = 3;
    g_par_slice_size = 4;
    g_par_indirect_on = 0;
    g_par_inline_binds = 1;
    vk_dump_armed = 0;
    pd_n = 0;

    static RunTrace fast;
    const op_hdr *streams[1] = {opbuf_first(&top)};
    compare_scenario(streams, 1, NULL, 1, 1, &fast);
    assert(fast.nflush == 3);
    assert(fast.par_end_op[OP_DRAW_IDX] == 1);
    assert(fast.par_end_op[OP_DRAW_IDX_IND] == 1);
    assert(fast.par_end_op[OP_COUNT_] == 1);

    /* Case B: PAR_MAX_OPS=2 hit on OP_WAIT_SYNC and matching OP_SET_RT at depth 0. */
    static OpBuf top2;
    memset(&top2, 0, sizeof top2);
    p_wait_sync ws = {.sync = NULL};
    p_set_rt rt0;
    memset(&rt0, 0, sizeof rt0);
    emit_op(&top2, OP_BEGIN, NULL, 0);
    emit_op(&top2, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&top2, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_op(&top2, OP_WAIT_SYNC, &ws, sizeof ws);
    emit_op(&top2, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&top2, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_op(&top2, OP_SET_RT, &rt0, sizeof rt0);
    emit_op(&top2, OP_END, NULL, 0);

    g_par_max_ops = 2;
    g_par_indirect_on = 1;
    const op_hdr *streams2[1] = {opbuf_first(&top2)};
    compare_scenario(streams2, 1, NULL, 1, 1, &fast);
    assert(fast.nflush == 2);
    assert(fast.par_end_op[OP_WAIT_SYNC] == 1);
    assert(fast.par_end_op[OP_SET_RT] == 1);
}

static void test_par_collect_walk_op_end_across_cmdbufs_and_modes(void) {
    static OpBuf cb1, cb2, sub;
    memset(&cb1, 0, sizeof cb1);
    memset(&cb2, 0, sizeof cb2);
    memset(&sub, 0, sizeof sub);

    p_bind_buf ubo = {.stage = 0, .index = 0, .addr = 0x60000, .size = CAM_BLOCK};
    uint64_t draw_stub[4] = {7, 8, 9, 10};

    /* sub (depth 1): OP_DRAW, OP_END (pops back to cb1, stays in par_collect_walk). */
    emit_op(&sub, OP_BEGIN, NULL, 0);
    emit_op(&sub, OP_DRAW, draw_stub, sizeof draw_stub);
    emit_op(&sub, OP_END, NULL, 0);

    /* cb1 (depth 0): bind ubo, draw (starts pass), call sub, OP_END (depth == d0: returns with PC.on == 1!). */
    emit_op(&cb1, OP_BEGIN, NULL, 0);
    emit_op(&cb1, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_op(&cb1, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_call(&cb1, opbuf_first(&sub));
    emit_op(&cb1, OP_END, NULL, 0);

    /* cb2 (depth 0, walked while PC.on == 1): bind ubo, draw, OP_END (still PC.on == 1 until caller flushes!). */
    emit_op(&cb2, OP_BEGIN, NULL, 0);
    emit_op(&cb2, OP_BIND_UBO, &ubo, sizeof ubo);
    emit_op(&cb2, OP_DRAW_IDX, draw_stub, sizeof draw_stub);
    emit_op(&cb2, OP_END, NULL, 0);

    /* Test without flush_end first to verify PC.on == 1 and synced PC.nops on OP_END at depth == d0. */
    g_par_max_ops = 256;
    g_par_slice_size = 2;
    g_par_indirect_on = 1;
    g_par_inline_binds = 1;
    vk_dump_armed = 0;

    VkExec init_ex;
    VkTexImage zimg;
    float cam_data[64];
    for (int i = 0; i < 64; i++) cam_data[i] = (float)(i + 1);
    memset(&init_ex, 0, sizeof init_ex);
    memset(&zimg, 0, sizeof zimg);
    memset(&S, 0, sizeof S);
    state = 1; g_fsr_wanted = 0; g_dlss_wanted = 1;
    J.w = 1920; J.h = 1080; J.frame = 99;
    zimg.image = (VkImage)(uintptr_t)0x9000;
    init_ex.color[0].image = (VkImage)(uintptr_t)0x8000;
    init_ex.depth.image = zimg.image;
    init_ex.depth.img = &zimg;
    init_ex.depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    init_ex.depth.width = 1920; init_ex.depth.height = 1080; init_ex.depth.samples = 1;
    init_ex.pass_serial = 55;
    g_usnap_addr = 0x60000;
    g_usnap_ptr = cam_data;

    static RunTrace fast;
    const op_hdr *streams[2] = {opbuf_first(&cb1), opbuf_first(&cb2)};
    compare_scenario(streams, 2, &init_ex, 1, 0, &fast);
    assert(fast.pc_on == 1);
    assert(fast.pc_draws == 3);
    assert(fast.pc_nops == 4);
    assert(fast.pc_dlss_done == 1);

    /* Also test non-X context (use_X=0), inline_binds=0, and vk_dump_armed=1. */
    g_par_inline_binds = 0;
    g_dlss_wanted = 0;
    compare_scenario(streams, 2, &init_ex, 0, 1, &fast);
    assert(fast.nflush == 1 && fast.flushes[0].draws == 3);

    vk_dump_armed = 1;
    g_par_inline_binds = 1;
    compare_scenario(streams, 1, &init_ex, 1, 1, &fast);
    assert(fast.nflush == 2 && fast.flushes[0].draws == 0 && fast.flushes[1].draws == 0);
    vk_dump_armed = 0;
}

/* Deferred OP_BIND_VATTR/OP_BIND_VSTREAM (both full 16-slot and partial binds) across slice boundaries and OP_END,
 * plus multiple runs of consecutive OP_CALLs separated by a non-call op (prefetch re-arming and OP_END safety). */
static void test_par_collect_walk_deferred_vstate_and_call_runs(void) {
    VkExec init_ex;
    memset(&init_ex, 0, sizeof init_ex);
    init_ex.color[0].image = (VkImage)(uintptr_t)0x8000;
    init_ex.pipe.vattr[0] = 0xdeadbeefu;
    init_ex.pipe.vstream[0] = (vs_st){99, 1};

    static OpBuf leaf[8], main_b;
    uint64_t draw_stub[4] = {1, 2, 3, 4};
    for (int i = 0; i < 8; i++) {
        memset(&leaf[i], 0, sizeof leaf[i]);
        emit_op(&leaf[i], OP_BEGIN, NULL, 0);
        if (i % 2 == 0) {
            p_bind_vattr va = {.first = 0, .n = 16};
            p_bind_vstream vs = {.first = 0, .n = 16};
            for (int k = 0; k < 16; k++)
                va.st[k] = (uint32_t)(0x1000 + i * 32 + k),
                vs.st[k] = (vs_st){(uint16_t)(i + k + 1), (uint8_t)(k & 3)};
            emit_op(&leaf[i], OP_BIND_VATTR, &va, sizeof va);
            emit_op(&leaf[i], OP_BIND_VSTREAM, &vs, sizeof vs);
        } else {
            p_bind_vattr va = {.first = 3, .n = 2, .st = {0x5000u + (uint32_t)i, 0x6000u + (uint32_t)i}};
            p_bind_vstream vs = {.first = 2, .n = 2, .st = {{(uint16_t)(20 + i), 1}, {(uint16_t)(30 + i), 2}}};
            emit_op(&leaf[i], OP_BIND_VATTR, &va, sizeof va);
            emit_op(&leaf[i], OP_BIND_VSTREAM, &vs, sizeof vs);
        }
        p_bind_vbuf vb = {.first = 0, .n = 4};
        for (int k = 0; k < 4; k++) vb.r[k] = (NVNbufferRange){0x80000000ull + (uint64_t)(i * 16 + k) * 64u, 256u};
        emit_op(&leaf[i], OP_BIND_VBUF, &vb, sizeof vb);
        emit_op(&leaf[i], OP_DRAW, draw_stub, sizeof draw_stub);
        emit_op(&leaf[i], OP_END, NULL, 0);
    }

    memset(&main_b, 0, sizeof main_b);
    emit_op(&main_b, OP_BEGIN, NULL, 0);
    for (int i = 0; i < 4; i++) emit_call(&main_b, opbuf_first(&leaf[i]));
    p_bind_buf ub = {.stage = 0, .index = 2, .addr = 0x90001000ull, .size = 256};
    emit_op(&main_b, OP_BIND_UBO, &ub, sizeof ub);
    for (int i = 4; i < 8; i++) emit_call(&main_b, opbuf_first(&leaf[i]));
    emit_op(&main_b, OP_END, NULL, 0);

    const op_hdr *streams[1] = {opbuf_first(&main_b)};
    g_par_max_ops = 256;
    g_par_slice_size = 3;
    g_par_inline_binds = 1;
    g_par_indirect_on = 1;
    g_dlss_wanted = 0;
    vk_dump_armed = 0;
    static RunTrace fast;
    compare_scenario(streams, 1, &init_ex, 1, 1, &fast);
    assert(fast.nflush == 1 && fast.flushes[0].draws == 8 && fast.flushes[0].nslices == 3);
}

int main(void) {
    test_pass_snap();
    test_slice_snap_roundtrip_and_shrink();
    test_par_bind_op();
    test_dlss_latch();
    test_par_collect_walk_nested_calls_jumps_and_slices();
    test_par_collect_walk_mid_call_exits_and_unwind();
    test_par_collect_walk_max_ops_and_indirect_stop();
    test_par_collect_walk_op_end_across_cmdbufs_and_modes();
    test_par_collect_walk_deferred_vstate_and_call_runs();
    puts("par_collect_test: ok");
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=shutil.which('clang') or 'clang')
    ap.add_argument('--deps', type=Path, required=True)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    args = ap.parse_args()

    vk_exec_src = (args.source_root / 'runtime/vk_exec.c').read_text()
    vk_dlss_src = (args.source_root / 'runtime/vk_dlss.c').read_text()

    vk_exec_snap_parts = [
        extract_typedef(vk_exec_src, 'ParPassSnap'),
        extract_typedef(vk_exec_src, 'ParSnap'),
        extract_function(vk_exec_src, 'static void par_pass_take('),
        extract_function(vk_exec_src, 'static void par_pass_load('),
        extract_macro(vk_exec_src, 'PAR_SLICE_LOAD'),
        extract_function(vk_exec_src, 'static void par_snap_take('),
        extract_function(vk_exec_src, 'static void par_snap_load('),
        extract_function(vk_exec_src, 'static inline void par_bind_op('),
    ]
    vk_exec_walk_parts = [
        extract_function(vk_exec_src, 'static int state_op('),
        extract_function(vk_exec_src, 'static int par_run_op('),
        extract_function(vk_exec_src, 'static int par_take('),
        extract_function(vk_exec_src, 'static inline void apply_vattr_inline('),
        extract_function(vk_exec_src, 'static inline void apply_vstream_inline('),
        extract_function(vk_exec_src, 'static const op_hdr *par_collect_walk('),
    ]
    vk_dlss_part = extract_function(vk_dlss_src, 'int vk_dlss_scene_draw(')

    code = (HARNESS
            .replace('/* VK_EXEC_SNAP_EXTRACT */', '\n\n'.join(vk_exec_snap_parts))
            .replace('/* VK_EXEC_WALK_EXTRACT */', '\n\n'.join(vk_exec_walk_parts))
            .replace('/* VK_DLSS_EXTRACT */', vk_dlss_part))

    inc = [
        '-I' + str(args.source_root / 'runtime'),
        '-I' + str(args.deps / 'include'),
        '-I' + str(args.deps / 'include' / 'vma'),
    ]
    with tempfile.TemporaryDirectory(prefix='dkb-par-collect-') as tmp:
        src = Path(tmp) / 'test.c'
        src.write_text(code)
        for opt in ('-O0', '-O2'):
            exe = Path(tmp) / ('par_collect' + opt[2:] + '.exe')
            subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Werror', *inc, str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
