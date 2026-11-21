// Unit tests for stick radial calibration (s2_cal_apply), input_pad_stick circular sweeps, and high-FPS stick-spin
// ring cadence accumulation (runtime/frame_rate.c spin_step). No game files or GPU required.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "switch2_proto.h"

#define STICK_MAX 32767

static uint64_t g_now_ns = 1000000000ull;
static uint64_t plat_now_ns(void) { return g_now_ns; }

typedef int PlatMutex;
#define PLAT_MUTEX_INIT 0
static void plat_mutex_lock(PlatMutex *m) { (*m)++; }
static void plat_mutex_unlock(PlatMutex *m) { assert(*m > 0); (*m)--; }

#include "stick_spin_cut.c"

static const double PI = 3.14159265358979323846;

static double wrap_deg(double a) {
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return a;
}

static void check_s2_sweep(const s2_cal *cal, float dead, const char *label) {
    float cx = (cal && cal->valid) ? cal->cx : 2048.f;
    float cy = (cal && cal->valid) ? cal->cy : 2048.f;
    float maxx = (cal && cal->valid) ? cal->maxx : 1200.f;
    float minx = (cal && cal->valid) ? cal->minx : 1200.f;
    float maxy = (cal && cal->valid) ? cal->maxy : 1200.f;
    float miny = (cal && cal->valid) ? cal->miny : 1200.f;

    /* 360-degree sweep at 5-degree steps crossing 0, 90, 180, 270 degrees at full and 65% deflection. */
    const float scales[2] = {0.65f, 1.15f};
    for (int si = 0; si < 2; si++) {
        float prev_ang = 0.f;
        for (int deg = 0; deg <= 360; deg += 5) {
            double rad = (double)deg * PI / 180.0;
            double c = cos(rad), s = sin(rad);
            double rx = cx + c * (c >= 0 ? maxx : minx) * scales[si];
            double ry = cy + s * (s >= 0 ? maxy : miny) * scales[si];
            if (rx < 0.0) rx = 0.0;
            if (rx > 4095.0) rx = 4095.0;
            if (ry < 0.0) ry = 0.0;
            if (ry > 4095.0) ry = 4095.0;
            float x = 0.f, y = 0.f;
            s2_cal_apply(cal, (uint16_t)lrint(rx), (uint16_t)lrint(ry), dead, &x, &y);
            float r = hypotf(x, y);
            if (!(r > 0.1f && r <= 1.0001f)) {
                fprintf(stderr, "FAIL %s scale %.2f deg %d: r=%.5f (x=%.5f y=%.5f)\n", label, scales[si], deg, r, x, y);
                abort();
            }
            float ang = atan2f(y, x) * (float)(180.0 / PI);
            if (deg > 0) {
                double d = wrap_deg((double)ang - (double)prev_ang);
                if (!(d > 1.0 && d < 15.0)) {
                    fprintf(stderr, "FAIL %s scale %.2f deg %d: non-monotonic delta %.4f (prev=%.3f now=%.3f)\n",
                            label, scales[si], deg, d, prev_ang, ang);
                    abort();
                }
            }
            prev_ang = ang;
        }
    }
}

static void test_s2_cal_apply(void) {
    /* 1. Default calibration (NULL and invalid). */
    check_s2_sweep(NULL, S2_DEADZONE, "default-null");
    s2_cal inv = {0};
    check_s2_sweep(&inv, S2_DEADZONE, "default-invalid");

    /* 2. Symmetric calibration. */
    s2_cal sym = {.cx = 2048.f, .cy = 2048.f, .maxx = 1300.f, .minx = 1300.f, .maxy = 1300.f, .miny = 1300.f, .valid = 1};
    check_s2_sweep(&sym, S2_DEADZONE, "symmetric");

    /* 3. Asymmetric calibration. */
    s2_cal asym = {.cx = 1980.f, .cy = 2110.f, .maxx = 1420.f, .minx = 1150.f, .maxy = 1180.f, .miny = 1360.f, .valid = 1};
    check_s2_sweep(&asym, S2_DEADZONE, "asymmetric");

    /* 4. Deadzone interior returns exact (0, 0), full travel reaches 1.0, overtravel clamps circularly to 1.0. */
    float x, y;
    s2_cal_apply(&sym, 2048, 2048, S2_DEADZONE, &x, &y);
    assert(x == 0.f && y == 0.f);
    s2_cal_apply(&sym, 2048 + 25, 2048 - 25, S2_DEADZONE, &x, &y);   // hypot(25, 25) = 35.35 < 40
    assert(x == 0.f && y == 0.f);

    s2_cal_apply(&sym, 2048 + 1300, 2048, S2_DEADZONE, &x, &y);
    assert(fabsf(x - 1.f) < 1e-5f && fabsf(y) < 1e-5f);
    s2_cal_apply(&sym, 2048, 2048 - 1300, S2_DEADZONE, &x, &y);
    assert(fabsf(x) < 1e-5f && fabsf(y + 1.f) < 1e-5f);

    /* Diagonal square corner (overtravel): circularly clamped to radius 1.0, not sqrt(2). */
    s2_cal_apply(&sym, 2048 + 1300, 2048 + 1300, S2_DEADZONE, &x, &y);
    assert(fabsf(hypotf(x, y) - 1.f) < 1e-5f && fabsf(x - y) < 1e-5f);

    /* 5. Edge cases: zero span / huge deadzone / negative deadzone never produce NaN or Inf. */
    s2_cal zspan = {.cx = 2048.f, .cy = 2048.f, .maxx = 0.f, .minx = 1000.f, .maxy = 1000.f, .miny = 0.f, .valid = 1};
    s2_cal_apply(&zspan, 3000, 3000, S2_DEADZONE, &x, &y);
    assert(isfinite(x) && isfinite(y) && hypotf(x, y) <= 1.0001f);
    s2_cal_apply(&sym, 3000, 2048, 5000.f, &x, &y);   // dead > span -> d0 clamped to 0.99
    assert(isfinite(x) && isfinite(y) && x == 0.f && y == 0.f);
    s2_cal_apply(&sym, 2048 + 1300, 2048, 5000.f, &x, &y);   // full travel at d0=0.99 -> 1.0
    assert(isfinite(x) && fabsf(x - 1.f) < 1e-4f);
    s2_cal_apply(&sym, 2048 + 650, 2048, -10.f, &x, &y);
    assert(isfinite(x) && fabsf(x - 0.5f) < 1e-5f && fabsf(y) < 1e-5f);
}

static void test_input_pad_stick_sweeps(void) {
    const float deads[2] = {0.08f, 0.15f};
    for (int di = 0; di < 2; di++) {
        float dead = deads[di];
        int32_t out[2];
        input_pad_stick(0, 0, dead, out);
        assert(out[0] == 0 && out[1] == 0);

        double prev_ang = 0.0;
        for (int deg = 0; deg <= 360; deg += 5) {
            double rad = (double)deg * PI / 180.0;
            int32_t rx = (int32_t)lrint(cos(rad) * STICK_MAX);
            int32_t ry = (int32_t)lrint(sin(rad) * STICK_MAX);
            input_pad_stick(rx, ry, dead, out);
            double r = hypot((double)out[0], (double)out[1]) / STICK_MAX;
            assert(r > 0.99 && r <= 1.001);
            double ang = atan2((double)out[1], (double)out[0]) * (180.0 / PI);
            if (deg > 0) {
                double d = wrap_deg(ang - prev_ang);
                assert(d > 3.5 && d < 6.5);
            }
            prev_ang = ang;
        }
    }
}

/* Faithful model of Bananza 3.2.0 f_70002409d0 (main_0021.c:40171-42032):
 * computes signed angle delta / dt, optionally filters via spin_step(), pushes into 64-entry ring ceil(s0) times,
 * then walks back up to 15 entries stopping on sign reversal or >2 consecutive |v| < 1e-5f samples.
 * Spin trigger (f_700146872c / f_70003bf174): streak >= 15 and |sum / (min(streak, 15) + 1)| >= 15.0.
 */
typedef struct {
    float ring[64];
    int head, count;
    float prev_x, prev_y;
    int has_prev;
    float out_avg, out_streak;
} SpinSim;

static void spin_sim_step(SpinSim *sim, float sx, float sy, float dt, uint64_t det_key, int use_hook) {
    float rate = 0.0f;
    if (sim->has_prev) {
        float r0 = sim->prev_x * sim->prev_x + sim->prev_y * sim->prev_y;
        float r1 = sx * sx + sy * sy;
        if (r0 >= 1e-10f && r1 >= 1e-10f) {
            float cross = sim->prev_x * sy - sim->prev_y * sx;
            float dot = sim->prev_x * sx + sim->prev_y * sy;
            float ang = atan2f(cross, dot) * 57.295776f;
            rate = ang / dt;
        }
    }
    sim->prev_x = sx;
    sim->prev_y = sy;
    sim->has_prev = 1;

    float eff_dt = dt;
    float eff_rate = rate;
    if (use_hook) {
        float dt_mem = dt;
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[0] = (uint64_t)(uintptr_t)&dt_mem;
        c.x[19] = det_key;
        set_f32(&c, 8, rate);
        spin_step(&c);
        eff_dt = reg_f32(&c, 0);
        eff_rate = reg_f32(&c, 8);
    }

    int pushes = (int)ceilf(eff_dt);
    for (int p = 0; p < pushes; p++) {
        sim->ring[sim->head] = eff_rate;
        sim->head = (sim->head + 1) & 63;
        if (sim->count < 64) sim->count++;
    }

    int streak = 0, zeros = 0;
    float sum = 0.0f, dir = 0.0f;
    int avail = sim->count < 15 ? sim->count : 15;
    for (int k = 0; k < avail; k++) {
        float v = sim->ring[(sim->head - 1 - k) & 63];
        if (fabsf(v) < 1e-5f) {
            if (++zeros > 2) break;
            streak++;
            continue;
        }
        zeros = 0;
        if (dir == 0.0f) dir = v;
        else if (dir * v <= 0.0f) break;
        sum += v;
        streak++;
    }
    sim->out_streak = (float)streak;
    sim->out_avg = sum / (float)((streak < 15 ? streak : 15) + 1);
}

static int simulate_spin_triggers(double fps, double poll_hz, double spin_dps, int use_hook, uint64_t key) {
    SpinSim sim;
    memset(&sim, 0, sizeof sim);
    double frame_s = 1.0 / fps;
    double poll_s = 1.0 / poll_hz;
    float dt = (float)(60.0 / fps);
    int total_frames = (int)(1.0 * fps);   // 1 second of continuous stick rotation
    int triggered = 0;

    g_now_ns += 1000000000ull;   // fresh window (>250ms after any prior test)
    for (int f = 0; f < total_frames; f++) {
        double t = f * frame_s;
        /* Quantize stick position to latest HID report time (models polled USB/BT controller). */
        double sample_t = floor(t / poll_s) * poll_s;
        double rad = sample_t * spin_dps * (PI / 180.0);
        float sx = (float)cos(rad), sy = (float)sin(rad);
        g_now_ns += (uint64_t)llround(frame_s * 1e9);
        spin_sim_step(&sim, sx, sy, dt, key, use_hook);
        if (sim.out_streak >= 15.0f && fabsf(sim.out_avg) >= 15.0f) triggered++;
    }
    return triggered;
}

static void test_spin_step_cadence_and_invariants(void) {
    /* 1. Across 60/90/120/144/180/240 FPS and 60/66.7/125/1000 Hz controllers at 1080 deg/s (3 rev/s):
     *    spin_step MUST reliably trigger spin attacks in every combination. */
    const double fps_list[] = {60.0, 90.0, 120.0, 144.0, 180.0, 240.0};
    const double poll_list[] = {60.0, 66.7, 125.0, 1000.0};
    uint64_t next_key = 0x10000;

    for (size_t fi = 0; fi < sizeof fps_list / sizeof *fps_list; fi++) {
        for (size_t pi = 0; pi < sizeof poll_list / sizeof *poll_list; pi++) {
            int hits = simulate_spin_triggers(fps_list[fi], poll_list[pi], 1080.0, 1, next_key++);
            if (hits <= 0) {
                fprintf(stderr, "FAIL spin_step did not trigger at %.1f fps, %.1f Hz poll\n",
                        fps_list[fi], poll_list[pi]);
                abort();
            }
        }
    }

    /* Confirm the unpatched game fails completely at 240 FPS on 60/66.7 Hz pads (>2 consecutive zeros break streak). */
    assert(simulate_spin_triggers(240.0, 60.0, 1080.0, 0, 0) == 0);
    assert(simulate_spin_triggers(240.0, 66.7, 1080.0, 0, 0) == 0);

    /* 2. dt >= 1.0 (60 FPS or slower) is a strict no-op: loads s0 = [x0] and leaves s8 and every other reg intact. */
    const float slow_dts[] = {1.0f, 1.5f, 2.0f, 0.0f, -1.0f};
    for (size_t i = 0; i < sizeof slow_dts / sizeof *slow_dts; i++) {
        float dt_mem = slow_dts[i];
        Cpu c, want;
        memset(&c, 0, sizeof c);
        for (int r = 0; r < 32; r++) {
            c.x[r] = 0x5555000011110000ull + (uint64_t)r;
            c.v[r] = ((__uint128_t)c.x[r] << 64) | ~c.x[r];
        }
        c.sp = 0x70001000ull;
        c.nzcv = 0x60000000u;
        c.x[0] = (uint64_t)(uintptr_t)&dt_mem;
        set_f32(&c, 8, 19.25f);
        want = c;
        set_f32(&want, 0, dt_mem);
        spin_step(&c);
        assert(memcmp(&c, &want, sizeof c) == 0);
    }

    /* 3. Register preservation at 0 < dt < 1: ONLY v[0] (and v[8] on emit) may change. */
    {
        float dt_mem = 0.5f;
        uint64_t key = 0x9990001;
        g_now_ns += 500000000ull;
        Cpu c, want;
        memset(&c, 0, sizeof c);
        for (int r = 0; r < 32; r++) {
            c.x[r] = 0xaaaa000022220000ull + (uint64_t)r;
            c.v[r] = ((__uint128_t)c.x[r] << 64) | ~c.x[r];
        }
        c.sp = 0x70002000ull;
        c.nzcv = 0x90000000u;
        c.x[0] = (uint64_t)(uintptr_t)&dt_mem;
        c.x[19] = key;
        set_f32(&c, 8, 36.0f);
        want = c;
        set_f32(&want, 0, 0.0f);   // first 0.5-tick frame: skip push, v[8] untouched
        spin_step(&c);
        assert(memcmp(&c, &want, sizeof c) == 0);

        g_now_ns += 8333333ull;
        set_f32(&c, 8, 0.0f);      // second 0.5-tick frame: emits (36*0.5 + 0*0.5)/1.0 = 18.0
        want = c;
        set_f32(&want, 0, 1.0f);
        set_f32(&want, 8, 18.0f);
        spin_step(&c);
        assert(memcmp(&c, &want, sizeof c) == 0);
    }

    /* 4. >250 ms gap resets accumulated partial dt and angle cleanly. */
    {
        float dt_mem = 0.6f;
        uint64_t key = 0x9990002;
        g_now_ns += 500000000ull;
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[0] = (uint64_t)(uintptr_t)&dt_mem;
        c.x[19] = key;
        set_f32(&c, 8, 50.0f);
        spin_step(&c);   // acc.dt = 0.6 < 1.0 -> s0 = 0.0
        assert(reg_f32(&c, 0) == 0.0f);

        g_now_ns += 300000000ull;   // 300 ms gap > 250 ms -> must reset acc.dt to 0!
        set_f32(&c, 8, 10.0f);
        spin_step(&c);   // if not reset, 0.6 + 0.6 = 1.2 would emit; after reset, 0.6 < 1.0 -> s0 = 0.0!
        assert(reg_f32(&c, 0) == 0.0f);

        g_now_ns += 10000000ull;
        set_f32(&c, 8, 10.0f);
        spin_step(&c);   // 0.6 + 0.6 = 1.2 >= 1.0 -> emits 10.0 (NOT contaminated by old 50.0!)
        assert(reg_f32(&c, 0) == 1.0f);
        assert(fabsf(reg_f32(&c, 8) - 10.0f) < 1e-4f);
    }
}

int main(void) {
    test_s2_cal_apply();
    test_input_pad_stick_sweeps();
    test_spin_step_cadence_and_invariants();
    puts("stick_spin_test: ok");
    return 0;
}
