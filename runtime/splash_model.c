// The start-up screen's model (splash.h): which words, numbers and tip the screen shows for the progress the boot
// reports (boot_progress.h), and how long the step has left. The numbers are the step's own: the percentage and the
// count are its items done of its total, and a step without a count (the device, the game's own start) shows none. The
// time left is the step's alone, said only once the rate can be trusted.
#include "splash.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "ui_text.h"

// ---- time left
// The rate is the slope of a least-squares line through the samples of the last WINDOW_S seconds (four a second; half
// the step's time while that is shorter, FIRST_S at least), not the count since the step began: the shader step can go
// fast through what the cache holds and slowly through what it must translate, so its early rate says nothing of the
// rest, and a line through many samples moves less with each burst of finished items than the difference of two.
// Nothing is said before the step has run FIRST_S seconds and done a twentieth of its items. What is shown counts down
// by itself and moves toward each new estimate with a time constant of PULL_S seconds, so the text does not jump with
// each burst. The selftest holds it to an even rate and to a burst of cache hits followed by a tenth of the pace.
enum { WINDOW_S = 15, FIRST_S = 5 };
static const double PULL_S = 4.0;

void splash_eta_reset(SplashEta *e, int step) {
    memset(e, 0, sizeof *e);
    e->step = step;
    e->last_t = -1;
    e->shown = -1;
}

// Items a second over the samples, by least squares; 0 with fewer than three samples or none spread in time
static double eta_slope(const SplashEta *e) {
    if (e->n < 3) return 0;
    double t0 = e->t[e->head], d0 = (double)e->done[e->head], st = 0, sd = 0;
    for (int i = 0; i < e->n; i++) {
        int k = (e->head + i) % SPLASH_SAMPLES;
        st += e->t[k] - t0, sd += (double)e->done[k] - d0;
    }
    double mt = st / e->n, md = sd / e->n, num = 0, den = 0;
    for (int i = 0; i < e->n; i++) {
        int k = (e->head + i) % SPLASH_SAMPLES;
        double x = e->t[k] - t0 - mt;
        num += x * ((double)e->done[k] - d0 - md), den += x * x;
    }
    return den > 0 ? num / den : 0;
}

double splash_eta_note(SplashEta *e, double now, double since, uint64_t done, uint64_t total) {
    if (!total) return e->shown = -1;
    int newest = (e->head + e->n - 1 + SPLASH_SAMPLES) % SPLASH_SAMPLES;
    if (e->n && done < e->done[newest]) splash_eta_reset(e, e->step);   // the count began again
    if (e->last_t >= 0 && now - e->last_t < 0.25) return e->shown;
    double dt = e->last_t < 0 ? 0 : now - e->last_t;
    e->last_t = now;
    if (e->n == SPLASH_SAMPLES) e->head = (e->head + 1) % SPLASH_SAMPLES, e->n--;   // the oldest makes room
    int at = (e->head + e->n) % SPLASH_SAMPLES;
    e->t[at] = now, e->done[at] = done, e->n++;
    // A young step looks back over half its time (five seconds at least), so a burst at its start leaves the line soon
    double window = fmin((double)WINDOW_S, fmax((double)FIRST_S, 0.5 * (now - since)));
    while (e->n > 3 && now - e->t[e->head] > window) e->head = (e->head + 1) % SPLASH_SAMPLES, e->n--;
    if (done >= total) return e->shown = 0;
    double rate = eta_slope(e);
    if (now - since < FIRST_S || now - e->t[e->head] < FIRST_S - 1 || done * 20 < total) return e->shown = -1;
    if (rate <= 0) return e->shown = e->shown < 0 ? -1 : fmax(0.0, e->shown - dt);   // a pause: the clock runs on
    double raw = (double)(total - done) / rate;
    if (e->shown < 0) return e->shown = raw;
    e->shown = fmax(0.0, e->shown - dt);
    e->shown += (raw - e->shown) * fmin(1.0, dt / PULL_S);
    return e->shown;
}

// ---- texts
// 12345 as "12,345" (English) or "12 345" (Russian, with a no-break space)
static void with_groups(char *out, size_t n, uint64_t v) {
    char digits[32];
    int k = snprintf(digits, sizeof digits, "%llu", (unsigned long long)v);
    int ru = menu_text(UI_MENU_SPLASH_STARTING) == ui_text_in("ru", UI_MENU_SPLASH_STARTING);   // the menu's language
    const char *sep = ru ? "\xc2\xa0" : ",";
    size_t o = 0;
    for (int i = 0; i < k && o + 4 < n; i++) {
        if (i && (k - i) % 3 == 0) for (const char *s = sep; *s && o + 1 < n; s++) out[o++] = *s;
        out[o++] = digits[i];
    }
    out[o] = 0;
}

static void left_text(char *out, size_t n, double secs) {
    out[0] = 0;
    if (secs < 20) return;   // a step this close to its end needs no clock
    // under a minute in tens of seconds, rounded up; then whole minutes, up from a fifth of one past the last
    if (secs < 60) snprintf(out, n, menu_text(UI_MENU_SPLASH_LEFT_SECONDS), 10 * (int)ceil(secs / 10));
    else snprintf(out, n, menu_text(UI_MENU_SPLASH_LEFT_MINUTES), (int)ceil(secs / 60 - 0.2));
}

static const UiTextId kTips[] = {
    UI_MENU_SPLASH_TIP_FULLSCREEN, UI_MENU_SPLASH_TIP_SETTINGS, UI_MENU_SPLASH_TIP_KEPT, UI_MENU_SPLASH_TIP_GRAPHICS,
    UI_MENU_SPLASH_TIP_OVERLAY, UI_MENU_SPLASH_TIP_FRAME_RATE, UI_MENU_SPLASH_TIP_REMAP, UI_MENU_SPLASH_TIP_TABS,
};
enum { TIP_FROM_S = 2, TIP_EVERY_S = 7 };
static const double TIP_FADE_S = 0.8;

// The tip for the seconds since the screen opened, and its opacity: each one stays TIP_EVERY_S seconds and eases in and
// out over TIP_FADE_S. The frame-rate tip names Bananza's Display tab, which a TotK build lays out otherwise.
static const char *tip_at(double secs, float *alpha) {
    *alpha = 0;
    if (secs < TIP_FROM_S) return "";
    double at = secs - TIP_FROM_S, in = fmod(at, TIP_EVERY_S);
    int n = 0, which[sizeof kTips / sizeof *kTips];
    for (size_t i = 0; i < sizeof kTips / sizeof *kTips; i++)
        if (kTips[i] != UI_MENU_SPLASH_TIP_FRAME_RATE || !rt_game_version) which[n++] = (int)i;
    double a = fmin(1.0, fmin(in, TIP_EVERY_S - in) / TIP_FADE_S);
    a = a < 0 ? 0 : a * a * (3 - 2 * a);
    *alpha = (float)a;
    return menu_text(kTips[which[(int)(at / TIP_EVERY_S) % n]]);
}

void splash_view(SplashView *v, const BootProgress *p, double now, double step_secs, SplashEta *eta) {
    memset(v, 0, sizeof *v);
    v->fraction = -1;
    const char *tip = tip_at(now, &v->tip_alpha);
    snprintf(v->tip, sizeof v->tip, "%s", tip);
    int step = p->step;
    UiTextId label = step == BOOT_STEP_SHADERS ? UI_MENU_SPLASH_SHADERS : step == BOOT_STEP_PIPELINES ? UI_MENU_SPLASH_PIPELINES
                   : step == BOOT_STEP_GAME ? UI_MENU_SPLASH_GAME : UI_MENU_SPLASH_STARTING;
    snprintf(v->label, sizeof v->label, "%s", menu_text(label));
    // the sky by the steps: a decoration, never read as a number
    float in_step = p->total ? (float)p->done / (float)p->total : 0.5f;
    v->dawn = step == BOOT_STEP_SHADERS ? 0.20f + 0.40f * in_step : step == BOOT_STEP_PIPELINES ? 0.60f + 0.25f * in_step
            : step == BOOT_STEP_GAME ? 0.92f : 0.15f;
    if (eta->step != step) splash_eta_reset(eta, step);
    int counted = (step == BOOT_STEP_SHADERS || step == BOOT_STEP_PIPELINES) && p->total;
    if (!counted || step_secs < 0.4) {   // a step over in less than that would only flash its numbers
        if (counted) splash_eta_note(eta, now, now - step_secs, p->done, p->total);
        return;
    }
    uint64_t pc = p->done >= p->total ? 100 : p->done * 100 / p->total;
    snprintf(v->pct, sizeof v->pct, "%llu", (unsigned long long)pc);
    v->fraction = (float)p->done / (float)p->total;
    char a[32], b[32];
    with_groups(a, sizeof a, p->done);
    with_groups(b, sizeof b, p->total);
    snprintf(v->count, sizeof v->count, menu_text(step == BOOT_STEP_SHADERS ? UI_MENU_SPLASH_COUNT_SHADERS : UI_MENU_SPLASH_COUNT_PIPELINES),
             a, b);
    left_text(v->left, sizeof v->left, splash_eta_note(eta, now, now - step_secs, p->done, p->total));
}

const char *splash_project_name(void) { return rt_game_version ? "TotK Recomp" : "DKB Recomp"; }

void splash_version_text(char *out, size_t n) {
    snprintf(out, n, menu_text(UI_MENU_SPLASH_VERSION), rt_port_version, rt_game_display);
}

// ---- the test sequence (DKB_SPLASH_TEST)
// Shares of the test's length and the counts of a first start after a translator update on the development PC: the
// device a second or two, then 33,936 shaders whose first fifth the cache holds (fast) and whose rest is translated
// (slow, unevenly), then 3,550 pipelines at an even pace.
int splash_test_progress(double t, double total, BootProgress *out) {
    memset(out, 0, sizeof *out);
    if (t >= total) return 0;
    const double dev = 0.03, sh = 0.78;   // ends of the device and shader parts, as shares of the test
    double f = t / total;
    if (f < dev) {
        out->step = BOOT_STEP_DEVICE;
        return 1;
    }
    if (f < sh) {
        double g = (f - dev) / (sh - dev);   // 0 to 1 through the shader part
        // the first 4% of the time does a fifth (cache hits), the rest the remainder, with a wobble in the rate
        double k = g < 0.04 ? g / 0.04 * 0.2 : 0.2 + 0.8 * ((g - 0.04) / 0.96 + 0.015 * sin((g - 0.04) * 40.0));
        out->step = BOOT_STEP_SHADERS, out->total = 33936;
        out->done = (uint64_t)(fmin(fmax(k, 0.0), 1.0) * 33936);
        return 1;
    }
    out->step = BOOT_STEP_PIPELINES, out->total = 3550;
    out->done = (uint64_t)(fmin((f - sh) / (1.0 - sh), 1.0) * 3550);
    return 1;
}

// ---- --selftest
int splash_selftest(void) {
    int fails = 0;
#define EXPECT(c, what) do { if (!(c)) { printf("splash selftest: %s\n", what); fails++; } } while (0)
    SplashEta e;
    splash_eta_reset(&e, BOOT_STEP_SHADERS);
    // an even 100 items a second of 30,000: no word before five seconds, then about 270 s left at 30 s
    double left = -1;
    for (int i = 0; i <= 120; i++) {
        double t = i * 0.25;
        left = splash_eta_note(&e, t, 0, (uint64_t)(t * 100), 30000);
        if (t < 5) EXPECT(left < 0, "an estimate before five seconds");
    }
    EXPECT(left > 255 && left < 285, "an even rate's estimate");
    // a burst of cache hits, then a tenth of the rate: the estimate follows the recent rate
    splash_eta_reset(&e, BOOT_STEP_SHADERS);
    for (int i = 0; i <= 240; i++) {
        double t = i * 0.25;
        uint64_t d = t < 2 ? (uint64_t)(t * 3000) : 6000 + (uint64_t)((t - 2) * 300);
        left = splash_eta_note(&e, t, 0, d, 33936);
        // ten seconds after the burst the line no longer leans on it: within a fifth of the truth when said at all
        if (t == 12.0) EXPECT(left < 0 || fabs(left - (33936 - d) / 300.0) < 0.2 * (33936 - d) / 300.0, "the estimate soon after a burst");
    }
    EXPECT(left > 0 && fabs(left - (33936 - (6000 + 58 * 300.0)) / 300.0) < 12, "the estimate after a burst");
    EXPECT(splash_eta_note(&e, 61, 0, 33936, 33936) == 0, "a finished step");
    // the texts
    char s[96];
    left_text(s, sizeof s, 12);
    EXPECT(!s[0], "a clock for the last seconds");
    left_text(s, sizeof s, 41);
    EXPECT(strstr(s, "50") != NULL || strstr(s, "50 ") != NULL, "41 s rounded up to 50 s");
    left_text(s, sizeof s, 150);
    EXPECT(strstr(s, "3") != NULL, "150 s as 3 min");
    with_groups(s, sizeof s, 1234567);
    EXPECT(strlen(s) > 7 && s[0] == '1' && s[strlen(s) - 1] == '7', "digit groups");
    // the view: no numbers in a step without a count, nor in the first moments of a counted one
    SplashView v;
    BootProgress p = {.step = BOOT_STEP_GAME};
    splash_eta_reset(&e, 0);
    splash_view(&v, &p, 10, 3, &e);
    EXPECT(!v.pct[0] && !v.count[0] && v.fraction < 0 && v.label[0], "the game step shows no number");
    p = (BootProgress){.step = BOOT_STEP_SHADERS, .done = 999, .total = 1000};
    splash_view(&v, &p, 10, 0.2, &e);
    EXPECT(!v.pct[0], "numbers in a step's first moments");
    splash_view(&v, &p, 10, 3, &e);
    EXPECT(!strcmp(v.pct, "99") && v.fraction > 0.99f, "999 of 1000 shows 99");
    p.done = 1000;
    splash_view(&v, &p, 11, 4, &e);
    EXPECT(!strcmp(v.pct, "100"), "a finished step shows 100");
    splash_view(&v, &p, 0.5, 4, &e);
    EXPECT(!v.tip[0], "a tip in the first seconds");
    splash_view(&v, &p, 30, 4, &e);
    EXPECT(v.tip[0] && v.tip_alpha >= 0 && v.tip_alpha <= 1, "a tip later on");
    // the test sequence runs through the three counted steps and ends
    BootProgress t;
    int steps = 0, last = -1;
    for (double x = 0; splash_test_progress(x, 60, &t); x += 0.1) {
        if (t.step != last) steps++, last = t.step;
        EXPECT(!t.total || t.done <= t.total, "the test's count past its total");
    }
    EXPECT(steps == 3, "the test sequence's steps");
#undef EXPECT
    printf("splash: estimate, texts and test sequence: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
