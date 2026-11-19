// What the scenes of the start-up screen share (runtime/vk_splash.c; the scenes are shaders/splash_canyon.comp, the
// canyon at sunrise, shaders/splash_under.comp, the cavern, and shaders/splash_jungle.comp, the dawn over the jungle):
// the parameters, the noise and drawing tools, the layout of a pixel in scene units, the tunnel that shows the progress
// with the bunch of bananas at its end, the bunch beside the name, the text, and main. A scene defines backdrop_pixel (pass 0, what stays: painted again only when the size or the
// dawn changed) and frame_pixel (pass 1, what moves, over the backdrop); main adds the bunch beside the name and the
// text over the scene's frame, and the fade-in. The colours are sRGB code values throughout; the frame is RGBA8 and
// the present treats it as a picture of the game's (HDR10: at the paper white the settings give).
//
// The scene is laid out in units of the window's height from the middle of the window, so it keeps its proportions at
// any size and a wider window shows more of it at the sides. Each scene is a pipeline of its own, so the screen
// compiles only the one it shows.

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D frame;
layout(set = 0, binding = 1, rgba8) uniform image2D backdrop;

struct Box {
    vec4 rect;    // where the text goes: x, y, width, height in window pixels, top-left origin
    vec4 src;     // the atlas pixel at the box's top-left (x, y); shadow offset (pixels); shadow blur (pixels)
    vec4 color;   // sRGB colour and opacity
    vec4 shade;   // shadow colour and opacity
};
layout(std430, set = 0, binding = 2) readonly buffer Params {
    ivec4 size;   // window width, height; pass; number of text boxes
    ivec4 atlas;  // atlas row length in bytes (a multiple of 4), its height; 0; 0
    vec4 t;       // seconds since the screen opened; pixels a unit (1 at 1080 lines); dawn 0 to 1; fade-in 0 to 1
    vec4 bar;     // the tunnel: x from, x to, y; fraction done (negative: the step has no count)
    vec4 mark;    // the bananas beside the name: their crown's x, y, pixels a unit; how close the tunnel is to its end
    vec4 misc;    // where the scene's light comes from: its x as a share of the width; 0; 0; 0
    Box box[16];
} P;
layout(std430, set = 0, binding = 3) readonly buffer Atlas { uint a[]; } A;

// ---- small tools
uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd(uint v) { return float(pcg(v) & 0xffffffu) / 16777216.0; }
float rnd2(ivec2 c, uint seed) { return rnd(uint(c.x) * 1973u + uint(c.y) * 9277u + seed * 26699u); }
float n1(float x, uint seed) {   // value noise along a line, 0 to 1
    float i = floor(x), f = x - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(rnd(uint(int(i)) * 7919u + seed), rnd(uint(int(i) + 1) * 7919u + seed), f);
}
float fbm1(float x, uint seed, int oct) {
    float s = 0.0, a = 0.5, n = 0.0;
    for (int o = 0; o < oct; o++) {
        s += a * n1(x, seed + uint(o) * 101u);
        n += a;
        x = x * 2.03 + 3.7;
        a *= 0.5;
    }
    return s / n;
}
float n2(vec2 p, uint seed) {    // value noise over the plane, 0 to 1, with a quintic fade (no visible grid)
    vec2 i = floor(p), f = p - i;
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    ivec2 c = ivec2(i);
    float a = rnd2(c, seed), b = rnd2(c + ivec2(1, 0), seed), d = rnd2(c + ivec2(0, 1), seed), e = rnd2(c + ivec2(1, 1), seed);
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}
float fbm2(vec2 p, uint seed, int oct) {
    float s = 0.0, a = 0.5, n = 0.0;
    const mat2 turn = mat2(0.80, 0.60, -0.60, 0.80);   // each octave turned, so the lattice does not line up
    for (int o = 0; o < oct; o++) {
        s += a * n2(p, seed + uint(o) * 131u);
        n += a;
        p = turn * p * 2.02 + vec2(5.1, 1.7);
        a *= 0.5;
    }
    return s / n;
}
// Gradient noise over the plane, about -0.5 to 0.5: smooth everywhere, without the flat steps value noise shows when it
// is stretched (the clouds)
float gn(vec2 p, uint seed) {
    vec2 i = floor(p), f = p - i;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    ivec2 c = ivec2(i);
    float r00 = 6.28318 * rnd2(c, seed), r10 = 6.28318 * rnd2(c + ivec2(1, 0), seed);
    float r01 = 6.28318 * rnd2(c + ivec2(0, 1), seed), r11 = 6.28318 * rnd2(c + ivec2(1, 1), seed);
    float a = dot(vec2(cos(r00), sin(r00)), f), b = dot(vec2(cos(r10), sin(r10)), f - vec2(1.0, 0.0));
    float d = dot(vec2(cos(r01), sin(r01)), f - vec2(0.0, 1.0)), e = dot(vec2(cos(r11), sin(r11)), f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}
float gfbm(vec2 p, uint seed, int oct) {
    float s = 0.0, a = 0.5;
    const mat2 turn = mat2(0.80, 0.60, -0.60, 0.80);
    for (int o = 0; o < oct; o++) {
        s += a * gn(p, seed + uint(o) * 131u);
        p = turn * p * 2.02 + vec2(5.1, 1.7);
        a *= 0.5;
    }
    return s;
}
vec3 hex(uint v) { return vec3(float((v >> 16) & 255u), float((v >> 8) & 255u), float(v & 255u)) / 255.0; }
float sat(float v) { return clamp(v, 0.0, 1.0); }
float cover(float d) { return sat(0.5 - d); }   // coverage of an edge at signed distance d (pixels, negative inside)
mat2 rot(float a) { return mat2(cos(a), sin(a), -sin(a), cos(a)); }

// ---- the layout every pass shares
float W, H, U, X, Y;   // window size, pixels a unit (1 at 1080 lines), this pixel's centre
vec2 S;                // this pixel in scene units: x from the window's middle, y from its top, both in window heights
float BACK_A = 1.0;    // pass 0: what the scene keeps in the backdrop's alpha at this pixel (backdrop_pixel may set it)
float BACK_IN_A;       // pass 1: that value, read back with the backdrop

// ---- the text
float atlas_at(ivec2 p) {
    if (p.x < 0 || p.y < 0 || p.x >= P.atlas.x || p.y >= P.atlas.y) return 0.0;
    int i = p.y * P.atlas.x + p.x;
    return float((A.a[i >> 2] >> (8 * (i & 3))) & 255u) / 255.0;
}

vec3 text_over(vec3 c) {
    for (int i = 0; i < P.size.w; i++) {
        Box b = P.box[i];
        float off = b.src.z, blur = b.src.w;
        vec2 r0 = b.rect.xy, r1 = b.rect.xy + b.rect.zw;
        if (X < r0.x - blur || Y < r0.y - blur || X > r1.x + off + blur || Y > r1.y + off + blur) continue;
        if (b.color.a <= 0.0) continue;
        ivec2 at = ivec2(floor(vec2(X, Y) - r0)) + ivec2(b.src.xy);
        ivec2 lo = ivec2(b.src.xy), hi = lo + ivec2(b.rect.zw);
        // the shadow: the glyphs moved down and right, blurred over a few texels
        float s = 0.0;
        if (b.shade.a > 0.0) {
            ivec2 sp = at - ivec2(off);
            float k = max(1.0, blur);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    ivec2 tp = sp + ivec2(vec2(dx, dy) * k);
                    if (all(greaterThanEqual(tp, lo)) && all(lessThan(tp, hi))) s += atlas_at(tp);
                }
            s /= 9.0;
            c = mix(c, b.shade.rgb, sat(s * 1.4) * b.shade.a * b.color.a);
        }
        if (all(greaterThanEqual(at, lo)) && all(lessThan(at, hi))) c = mix(c, b.color.rgb, atlas_at(at) * b.color.a);
    }
    return c;
}

// ---- the bananas
// One banana in its own frame: the fruit lies along an arc about (0, -1) of radius 1, from angle -0.95 (the stem) to
// 0.95 (the tip) off straight down, so it bends up at both ends. Signed distance in frame units; t along it (0 the
// stem, 1 the tip), u across it (-1 the inner, upper edge, 1 the outer belly).
float banana_sd(vec2 p, out float t, out float u) {
    const float AA = 0.95;
    vec2 q = p - vec2(0.0, -1.0);
    float phi = atan(q.x, q.y), r = length(q);
    float tt = clamp(phi / AA, -1.0, 1.0);
    t = tt * 0.5 + 0.5;
    float w = 0.20 * pow(max(0.0, 1.0 - tt * tt), 0.42) + 0.015;
    float rc = 1.0 + 0.045 * (1.0 - tt * tt);   // the belly rounder than the inner edge
    u = (r - rc) / w;
    if (abs(phi) <= AA) return abs(r - rc) - w;
    vec2 e = vec2(0.0, -1.0) + 1.0 * vec2(sin(sign(phi) * AA), cos(AA));
    return length(p - e) - 0.015;
}

// A hand of three bananas fanned out from their crown at o (pixels), s pixels a frame unit; lit 0 to 1 brightens it.
// The fruit reach about 1.75 units to the right of the crown, the back one 1.2 units above it, the belly of the front
// one 0.5 under it; the stalk rises 0.4 units up and left of the crown.
float capsule(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * sat(dot(pa, ba) / dot(ba, ba))) - r;
}
vec3 bunch(vec3 c, vec2 o, float s, float lit) {
    vec2 q = (vec2(X, Y) - o) / s;
    if (q.x < -0.6 || q.x > 2.1 || q.y < -1.5 || q.y > 0.9) return c;
    const float ang[3] = float[3](0.72, 0.36, 0.02);   // the back one first, the front one last
    const float sc[3] = float[3](0.92, 1.0, 0.96);
    const uint base[3] = uint[3](0xd99e1cu, 0xf6c52bu, 0xffd650u);
    vec2 e0 = vec2(sin(-0.95), cos(0.95) - 1.0);   // the stem end in the banana's frame
    for (int i = 0; i < 3; i++) {
        vec2 p = rot(ang[i]) * q / sc[i] + e0;
        float t, u;
        float dd = banana_sd(p, t, u) * s * sc[i];
        if (dd > 1.5) continue;
        vec3 col = hex(base[i]);
        col *= mix(1.18, 0.66, sat(u * 0.5 + 0.5));                    // lit from above
        col += vec3(0.28, 0.22, 0.10) * exp(-pow((u + 0.45) / 0.18, 2.0)) * (1.0 - abs(t - 0.5) * 1.6);   // its sheen
        col = mix(col, hex(0x7a6a1eu), smoothstep(0.10, 0.02, t));     // green at the stem
        col = mix(col, hex(0x3a2210u), smoothstep(0.93, 0.99, t));     // the dark tip
        col *= 0.72 + 0.38 * lit;
        col = mix(hex(0x6e4410u), col, smoothstep(0.0, 1.4, -dd));    // the outline
        c = mix(c, col, cover(dd));
        c *= 1.0 - 0.25 * cover(dd - 2.5) * (1.0 - cover(dd)) * float(i < 2);   // a soft contact shadow on the next
    }
    // the crown where the fruit meet, and the stalk out of it, tapering, lit on its upper side
    float sd = min(capsule(q, vec2(0.02, 0.02), vec2(-0.20, -0.30), 0.075), length(q - vec2(0.03, 0.04)) - 0.12) * s;
    vec3 stalk = mix(hex(0x8a6a2cu), hex(0x4a3416u), sat(0.5 + (q.x + q.y) * 2.2));
    stalk = mix(stalk, hex(0x2a1c0cu), smoothstep(-0.18, -0.30, q.y));   // its cut end
    stalk = mix(hex(0x3a2610u), stalk, smoothstep(0.0, 1.4, -sd));   // the outline, as the fruit's
    c = mix(c, stalk, cover(sd));
    return c;
}

// ---- plants
// A palm, as coverage 0 to 1 at q (scene units from its foot, y up): a leaning trunk with rings, and a crown of
// fronds that arch and droop, each a tapered leaf cut into leaflets
float palm(vec2 q, float h, float lean, float seed) {
    if (q.y < -0.01 || q.y > h * 1.45 || abs(q.x - lean * h) > h * 0.95) return 0.0;
    float px = 1.0 / H, cov = 0.0;
    float ty = sat(q.y / h);
    float tx = lean * h * (ty * ty * 0.7 + ty * 0.3);
    float tw = h * (0.020 - 0.008 * ty) * (1.0 + 0.10 * step(0.5, fract(q.y / (h * 0.045))));
    if (q.y < h) cov = cover((abs(q.x - tx) - tw) / px);
    vec2 c = q - vec2(lean * h, h);
    for (int i = 0; i < 8; i++) {
        float a = 3.14159 * (0.06 + 0.88 * float(i) / 7.0) + 0.15 * sin(seed * 3.0 + float(i) * 1.7);
        vec2 dir = vec2(cos(a), sin(a) * 0.70 + 0.12);
        float len = h * (0.50 + 0.14 * fract(seed * 7.1 + float(i) * 0.37));
        float along = sat(dot(c, dir) / len);
        vec2 pt = dir * along * len - vec2(0.0, 0.42 * len * along * along * (0.6 + 0.4 * abs(dir.x)));
        float lw = h * 0.055 * pow(sin(3.14159 * sat(along * 0.92 + 0.04)), 0.8);
        lw *= 0.55 + 0.45 * abs(sin(along * 46.0 + float(i)));   // the leaflets
        cov = max(cov, cover((length(c - pt) - lw) / px));
    }
    return cov;
}

// ---- the progress: a tunnel dug from the left to the lamp, its course ahead marked in the rock, and the bunch at its
// far end, which glows more as the tunnel nears it; with no count, a light travels the course
vec3 tunnel(vec3 c, float time) {
    float x0 = P.bar.x, x1 = P.bar.y, ly = P.bar.z, f = P.bar.w;
    float hh = 9.0 * U;   // half the tunnel's height
    if (abs(Y - ly) < 110.0 * U && X > x0 - 140.0 * U && X < x1 + 160.0 * U) {
        float hx = x0 + (x1 - x0) * sat(f);
        if (X > (f >= 0.0 ? hx + 10.0 * U : x0) && X < x1) {   // the course ahead: small marks every 10 units
            float k = mod(X - x0, 10.0 * U) - 5.0 * U;
            float m = length(vec2(k, Y - ly)) - 1.0 * max(U, 0.8);
            c = mix(c, hex(0xe9d6b0u), cover(m) * 0.28);
        }
        if (f >= 0.0) {
            // the dug part: a hollow with a ragged edge, dark at the back, lit warm toward the lamp
            float rough = hh * (1.0 + 0.16 * (n1(X / (16.0 * U), 241u) - 0.5) + 0.06 * (n1(X / (5.0 * U), 243u) - 0.5));
            if (X > x0 - hh && X < hx + hh) {
                vec2 e = vec2(max(x0 - X, 0.0) + max(X - hx, 0.0), Y - ly);
                float hd = length(e) - rough;
                float near_lamp = smoothstep(hx - 520.0 * U, hx, X);
                if (hd < 1.5) {
                    float v = (Y - ly) / rough;   // -1 the roof, 1 the floor
                    vec3 hollow = mix(hex(0x0a0603u), hex(0x2a1608u), sat(v * 0.5 + 0.5));
                    hollow += hex(0xffa040u) * (0.06 + 0.42 * near_lamp * near_lamp) * (0.5 + 0.5 * v);
                    c = mix(c, hollow, cover(hd));
                    // the thread of gold along the floor that shows how far the tunnel goes
                    float thread = cover(abs(Y - (ly + 0.45 * rough)) - 0.8 * max(U, 0.75));
                    c = mix(c, mix(hex(0xa8781cu), hex(0xffe08au), near_lamp), thread * (0.55 + 0.45 * near_lamp) * cover(hd + 1.0));
                }
                // the rim of the hollow: chipped rock catching the light, all along and most near the lamp
                float rim = abs(hd - 1.2 * U);
                c += hex(0xffb050u) * (0.07 + 0.25 * near_lamp) * (1.0 - smoothstep(0.0, 1.8 * U, rim)) * step(0.0, hd);
            }
            // the lamp: a bright core in a warm glow that breathes, its light on the rock around
            vec2 hv = vec2(X - hx, Y - ly) / U;
            float pulse = 0.88 + 0.12 * sin(time * 2.7) + 0.04 * sin(time * 7.3);
            float r2 = dot(hv, hv);
            c *= 1.0 + 0.60 * pulse / (1.0 + r2 / 2600.0);
            c += hex(0xff9a36u) * 0.40 * pulse / (1.0 + r2 / 420.0);
            c += hex(0xfff2c8u) * 0.95 * pulse / (1.0 + r2 / 7.0);
            // sparks thrown back and up from where it digs, falling as they fade
            for (int s = 0; s < 16; s++) {
                float life = 0.8 + 0.6 * rnd(uint(s) * 13u + 239u);
                float tt = time + rnd(uint(s) * 13u + 241u) * life;
                float age = mod(tt, life) / life;
                uint gen = uint(floor(tt / life));
                float ang = 2.0 + 1.6 * rnd(uint(s) * 13u + 251u + gen * 7u);
                vec2 v0 = vec2(cos(ang), -abs(sin(ang))) * (45.0 + 70.0 * rnd(uint(s) * 13u + 257u + gen * 5u));
                vec2 sp = v0 * age * life + vec2(0.0, 95.0) * age * age * life * life;
                vec2 dv = hv - sp;
                float glow = (1.0 - age) * (1.0 - age);
                c += hex(0xffc860u) * glow * (0.85 / (1.0 + dot(dv, dv) * 1.1));
            }
        } else {
            // no count: a light moves to and fro along the course
            float at = x0 + (x1 - x0) * (0.5 - 0.5 * cos(time * 0.8));
            float k = 1.0 - abs(X - at) / (180.0 * U);
            if (k > 0.0) c = mix(c, hex(0xffe08au), cover(abs(Y - ly) - 1.0 * max(U, 0.75)) * k * k * 0.9);
            vec2 hv = vec2(X - at, Y - ly) / U;
            c += hex(0xffb347u) * 0.32 / (1.0 + dot(hv, hv) / 320.0);
        }
    }
    // the bunch at the tunnel's end, glowing more as the tunnel nears it
    float g = P.mark.w;
    vec2 bo = vec2(x1 + 40.0 * U, ly - 30.0 * U);
    vec2 bv = (vec2(X, Y) - bo - vec2(25.0, -4.0) * U) / U;   // the middle of the hand
    c += hex(0xf6c52bu) * (0.06 + 0.30 * g) * (0.92 + 0.08 * sin(time * 2.0)) / (1.0 + dot(bv, bv) / 650.0);
    c = bunch(c, bo, 30.0 * U, 0.55 + 0.45 * g);
    return c;
}

// ---- the scene's two passes, and main
vec3 backdrop_pixel();      // pass 0: what stays, at the current dawn (P.t.z)
vec3 frame_pixel(vec3 c);   // pass 1: what moves, over the backdrop c

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    W = float(P.size.x), H = float(P.size.y), U = P.t.y;
    if (p.x >= P.size.x || p.y >= P.size.y) return;
    X = float(p.x) + 0.5, Y = float(p.y) + 0.5;
    S = vec2((X - 0.5 * W) / H, Y / H);
    float dither = (rnd(uint(p.x) * 4801u + uint(p.y) * 6343u + uint(P.size.z) * 97u) - 0.5) / 255.0;
    if (P.size.z == 0) {
        vec3 b = backdrop_pixel();
        imageStore(backdrop, p, vec4(clamp(b + dither, 0.0, 1.0), BACK_A));
        return;
    }
    vec4 b = imageLoad(backdrop, p);
    BACK_IN_A = b.a;
    vec3 c = frame_pixel(b.rgb);
    // the bunch beside the name, and the text
    if (P.mark.z > 0.0) c = bunch(c, P.mark.xy, P.mark.z, 0.85);
    c = text_over(c);
    c = mix(hex(0x1d1109u), c, P.t.w);   // the fade-in from the launcher's darkest brown
    imageStore(frame, p, vec4(clamp(c + dither, 0.0, 1.0), 1.0));
}
