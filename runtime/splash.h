// The start-up screen's model (splash_model.c) and its text (splash_text.c); vk_splash.c draws and presents it. Plain
// C without Vulkan, so the selftest checks the model on any machine.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "boot_progress.h"

// What the screen shows at one moment. Empty strings are not drawn.
typedef struct {
    char label[96];        // the step, in capitals ("PREPARING SHADERS")
    char pct[8];           // the step's percentage without the sign ("86"); empty when the step has no count
    char count[96];        // "12,345 of 33,936 shaders"; empty when the step has no count
    char left[96];         // "about 2 min left for this step"; empty until the estimate can be trusted
    char tip[256];         // the tip shown now; empty in the first seconds
    float tip_alpha;       // 0 to 1: the tip fading in and out
    float fraction;        // the tunnel, 0 to 1; negative when the step has no count (a light travels the path)
    float dawn;            // the sky, 0 (night) to 1 (sunrise): how far the start is, by its steps
} SplashView;

// The time-left estimate of a counted step (splash_model.c "time left"): the recent rate, from a line through the
// samples of the last seconds, and the seconds shown, which count down. Kept by the caller.
enum { SPLASH_SAMPLES = 64 };
typedef struct {
    int step;                          // the step the samples belong to
    double t[SPLASH_SAMPLES];          // seconds
    uint64_t done[SPLASH_SAMPLES];
    int n, head;
    double last_t;                     // the newest sample's time; negative before the first
    double shown;                      // the estimate shown, seconds; negative while there is none
} SplashEta;

void splash_eta_reset(SplashEta *e, int step);
// A sample at now seconds of a step that began at since seconds; returns the seconds left to show, or a negative
// number while no estimate can be trusted.
double splash_eta_note(SplashEta *e, double now, double since, uint64_t done, uint64_t total);

// The view for the progress p at now seconds since the screen opened (step_secs: since the step began); eta is the
// caller's estimate state. The texts are in the menu language (menu_text).
void splash_view(SplashView *v, const BootProgress *p, double now, double step_secs, SplashEta *eta);

// "v0.4.1 · game 3.2.0" from the build's version texts
void splash_version_text(char *out, size_t n);
const char *splash_project_name(void);   // "DKB Recomp" (TotK Recomp for a TotK build)

// A simulated start for DKB_SPLASH_TEST=<seconds>: the progress a long first start would report at t seconds of a test
// lasting total seconds (a device step, the shaders at a translator's uneven pace, the pipelines). 1 while it runs,
// 0 once it is over.
int splash_test_progress(double t, double total, BootProgress *out);

// The checks of --selftest: the estimate, the texts, the test sequence. Returns the number of failures.
int splash_selftest(void);

// ---- text (splash_text.c): strings rendered with stb_truetype from the menu's font into an 8-bit coverage atlas
typedef struct {
    float x, y, w, h;      // where the box goes in the window (pixels, top-left), its padding included
    int ax, ay;            // the box's top-left in the atlas
} SplashGlyphBox;

typedef struct SplashText SplashText;
SplashText *splash_text_new(void);   // NULL when the font cannot be read
void splash_text_free(SplashText *t);
// Starts a new atlas (the old boxes become invalid)
void splash_text_begin(SplashText *t);
// Adds a string of size px (the em in pixels) with extra spacing between letters (pixels); align 0 left, 1 right of x;
// baseline y. Returns its box; w 0 when nothing was added.
SplashGlyphBox splash_text_add(SplashText *t, const char *utf8, float px, float spacing, float x, float baseline, int align);
float splash_text_width(SplashText *t, const char *utf8, float px, float spacing);
float splash_text_ascent(SplashText *t, float px);   // pixels above the baseline of the font at px
// The atlas: width (a multiple of 4) and height in bytes, row after row
const uint8_t *splash_text_atlas(SplashText *t, int *w, int *h);
