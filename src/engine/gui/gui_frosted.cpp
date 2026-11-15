// Frosted glass backdrop: the pixels behind the panel are sampled from the world, blurred, stretched back.
//
// The 2026-11-14 experimental version captured and uploaded correctly ("frosted: 1920x1009 -> tex 480x252")
// but nothing it drew reached the screen. Two reasons:
//
//  * nvgImagePattern() takes a NanoVG *image handle*, not a GL texture name. glnvg__findTexture() looks the
//    id up in the context's own texture list and returns NULL for a name made by glGenTextures(); nvg__fill()
//    then returns 0 without emitting a vertex, so the whole fill disappears without an error. The call that
//    does work, draw_framebuffer_texture_over_screen(), passes renderer->fb->tex, and FBuf+0x14 is
//    NVGLUframebuffer.image - the handle, not the GL name. Here the image is created and updated through
//    NanoVG itself (nvgCreateImageRGBA / nvgUpdateImage), so the handle and the context always match.
//  * glReadPixels() on a display surface without an alpha channel can return alpha = 0, which would have made
//    the panel transparent even with a valid handle. half_size() forces alpha to 255.
//
// Two smaller ones that would have looked wrong rather than invisible: box_blur() only ran horizontally (streaks
// instead of frost), and the 480x320 caps were applied to the *variable* after downsampling instead of to the
// image, so on a 1440p/4K screen the blur read past the end of its row. Both are fixed below.
//
// The read-back is a GPU sync point, so it runs every CAP_EVERY frames and only while a screen is open; a
// backdrop a few frames old is indistinguishable from a fresh one for a panel that is static most of the time.
#include "slinky/engine.h"
#include "slinky/platform.h"
#include "slinky/gui_frosted.h"

// ---- settings ---------------------------------------------------------------------------------------
// Defined here and referenced from two module pages: the ClickGUI module shows the first three, Mod Overlay
// shows the last two. They are independent objects, so either surface can be switched off without touching
// the other; only `frosted_blur` shapes the shared capture.
extern "C" const char str_frosted_section[] = "Frosted glass";
extern "C" const char str_frosted_gui_on[] = "frosted_gui";
extern "C" const char str_frosted_gui_tint[] = "frosted_tint";
extern "C" const char str_frosted_blur[] = "frosted_blur";
extern "C" const char str_hud_frosted_on[] = "frosted_hud";
extern "C" const char str_hud_frosted_tint[] = "hud_frosted_tint";
extern "C" const char str_frosted_percent_format[] = "%.0f%%";
extern "C" const char str_frosted_radius_format[] = "%.0f px";

FrostBool gui_frosted_gui_on = { {}, str_frosted_gui_on, 1, 1, {} };
FrostSlider gui_frosted_gui_tint = { {}, str_frosted_gui_tint, str_frosted_percent_format,
                                     1.0f, 1.0f, 0.0f, 40.0f, 10.0f, 10.0f };
FrostSlider gui_frosted_blur = { {}, str_frosted_blur, str_frosted_radius_format,
                                 1.0f, 1.0f, 1.0f, 9.0f, 4.0f, 4.0f };
extern "C" const char str_frosted_panel[] = "Opacity";
extern "C" const char str_frosted_hue[] = "Hue";
extern "C" const char str_frosted_hue_format[] = "%.0f";

// Card/panel opacity: the single knob that decides how see-through the glass is. 100 = solid, the
// original look; lower values let the blurred world through under the text.
FrostSlider gui_frosted_panel = { {}, str_frosted_panel, str_frosted_percent_format,
                                 1.0f, 1.0f, 20.0f, 100.0f, 30.0f, 30.0f };

// Hue in degrees, fed through a dark low-saturation HSV conversion - this is the "custom colour":
// pick a hue and the panel tint follows it while staying dark enough for white text.
extern "C" const char str_frosted_panel_blur[] = "Panel frost";
extern "C" const char str_frosted_zoom_format[] = "%.0f%%";

// The panel gets its own frost, separate from the world blur: the blurred frame is sampled with a
// larger extent, which magnifies it and hides more detail without a second blur pass. 100 % is 1:1
// (identical to the background), 150 % reads noticeably softer inside the panel only.
FrostSlider gui_frosted_panel_blur = { {}, str_frosted_panel_blur, str_frosted_zoom_format,
                                      5.0f, 1.0f, 100.0f, 250.0f, 100.0f, 100.0f };

FrostSlider gui_frosted_hue = { {}, str_frosted_hue, str_frosted_hue_format,
                               5.0f, 1.0f, 0.0f, 360.0f, 220.0f, 220.0f };

FrostBool hud_frosted_on = { {}, str_hud_frosted_on, 1, 1, {} };
FrostSlider hud_frosted_tint = { {}, str_hud_frosted_tint, str_frosted_percent_format,
                                 1.0f, 1.0f, 0.0f, 80.0f, 42.0f, 42.0f };

#if SLINKY_FROSTED_GUI

#define SLINKY_GL_MACROS 1              // glReadPixels expands to the client's own slot table
#include "slinky/gl_slots.h"

struct V4f { float x, y, z, w; };
struct Paint { char b[0x4c]; };         // NVGpaint, 76 bytes, returned by value (hidden pointer on Win64)

// NanoVG entry points (wrappers in src/platform/nvg_names.cpp)
FN(void, nvg_Save, (void *nvg));
FN(void, nvg_Restore, (void *nvg));
FN(void, nvg_GlobalAlpha, (void *nvg, float a));
FN(void, nvg_BeginPath, (void *nvg));
FN(void, nvg_Rect, (void *nvg, float x, float y, float w, float h));
FN(void, nvg_RoundedRect, (void *nvg, float x, float y, float w, float h, float round));
FN(void, nvg_FillColor, (void *nvg, V4f *c));
FN(void, nvg_Fill, (void *nvg));
FN(void, nvg_FillPaint, (void *nvg, Paint p));
FN(Paint, nvg_image_pattern, (void *nvg, float ox, float oy, float ex, float ey, float angle, int image, float alpha));
FN(Paint, nvg_linear_gradient, (void *nvg, float sx, float sy, float ex, float ey, V4f c0, V4f c1));
FN(V4f, nvg_rgba_f, (float r, float g, float b, float a));

// NanoVG images (defined in third_party/nanovg/src/nanovg.c, compiled into this DLL). The returned int is a
// context-local handle, not a GL name - that is the whole point of going through these instead of glGenTextures.
FN(int, nvgCreateImageRGBA, (void *nvg, int w, int h, int imageFlags, const unsigned char *data));
FN(void, nvgUpdateImage, (void *nvg, int image, const unsigned char *data));
FN(void, nvgDeleteImage, (void *nvg, int image));

// libc, declared per file the way the rest of the client does it
FN(void *, libc_malloc, (unsigned long long n));
FN(void, libc_free, (void *ptr));

enum {
    CAP_SCALE = 4,                  // the blurred image is 1/CAP_SCALE of the framebuffer in each axis...
    CAP_DOWNSTEPS = 2,              // ...reached in this many halvings: 2 = 1/4 (softer detail), 3 = 1/8
    CAP_BLUR_RADIUS = 4,            // box radius at that scale; on-screen blur 鈮?CAP_BLUR_RADIUS * 2^CAP_DOWNSTEPS
    CAP_EVERY = 3,                  // refresh one frame in three (CAP_EVERY = 1 refreshes every frame)
    CAP_SATURATE_NUM = 12,          // saturation lift applied after the blur, in tenths:
                                    // 12 = 1.2x. Blur washes colour toward grey, but lift it too far and the
                                    // backdrop turns uniformly vivid - that reads as plastic, not glass. 1.6 was
                                    // the recipe value; 1.2 keeps the colour alive without the moulded look.
    CAP_GRAIN_AMPLITUDE = 2,        // dither, not texture: +-2/255 (~0.8 %). At 10 with a per-frame seed it
                                    // read as TV snow; at 2 with a fixed seed it is invisible as noise but breaks
                                    // up the flat uniformity that made the plate look moulded. Seed is constant,
                                    // so the pattern never crawls. Raise to 4-6 only if banding actually shows.

    CAP_MAX_PIXELS = 3840 * 2160,   // above this the read-back is not worth its cost; backdrop stays off
};

static unsigned char *g_full;       // full-resolution read-back, reused
static unsigned g_full_cap;
static unsigned char *g_half;       // 1/2
static unsigned g_half_cap;
static unsigned char *g_quarter;    // 1/4
static unsigned g_quarter_cap;
static unsigned char *g_eighth;     // 1/8 鈥?the blur source. Halving once more quarters the pixel count and
static unsigned g_eighth_cap;       // buys the real frosted look; go back to blurring at 1/4 by dropping the
                                    // third pass in capture_frosted_backdrop.
static unsigned char *g_blur_a;     // scratch for the two blur passes
static unsigned g_blur_a_cap;
static unsigned char *g_blur_b;     // the finished (flipped) image
static unsigned g_blur_b_cap;

static unsigned g_cap_w, g_cap_h;   // size of the last capture (the 1/8 image)
static unsigned g_fb_w, g_fb_h;     // framebuffer size it was captured at; the HUD aligns its pattern to this
static unsigned g_serial;           // bumped on every new capture
static unsigned g_frame;            // frame counter for CAP_EVERY
static unsigned g_active;           // frames left in which something wants the backdrop (see note_active)
static unsigned char g_logged;      // one line per session
static unsigned char g_draw_logged;
static unsigned char g_black_logged;

// A NanoVG image handle belongs to the context that created it, and the GUI and the HUD (mod overlay) run on
// two different NanoVG contexts. One slot per context, from the same pixel buffer.
struct ImageSlot { void *ctx; int img; unsigned w, h, uploaded; };
static ImageSlot g_slots[2];

static unsigned char *ensure(unsigned char **buf, unsigned *cap, unsigned long long need) {
    if (need <= *cap && *buf) return *buf;
    if (*buf) libc_free(*buf);
    *buf = (unsigned char *)libc_malloc(need);
    *cap = *buf ? (unsigned)need : 0;
    return *buf;
}

// Halve an RGBA image by averaging 2x2 blocks. Alpha is forced opaque here: the panel is drawn into an FBO
// that is composited over the world, and an alpha of 0 from the display surface would erase it.
static void half_size(const unsigned char *src, unsigned char *dst, unsigned w, unsigned h) {
    unsigned dw = w / 2, dh = h / 2;
    for (unsigned y = 0; y < dh; y++) {
        const unsigned char *p0 = src + (unsigned long long)(y * 2) * w * 4;
        const unsigned char *p1 = p0 + (unsigned long long)w * 4;
        unsigned char *out = dst + (unsigned long long)y * dw * 4;
        for (unsigned x = 0; x < dw; x++) {
            const unsigned char *a = p0 + (unsigned long long)x * 8;
            const unsigned char *b = p1 + (unsigned long long)x * 8;
            out[x * 4 + 0] = (unsigned char)((a[0] + a[4] + b[0] + b[4]) >> 2);
            out[x * 4 + 1] = (unsigned char)((a[1] + a[5] + b[1] + b[5]) >> 2);
            out[x * 4 + 2] = (unsigned char)((a[2] + a[6] + b[2] + b[6]) >> 2);
            out[x * 4 + 3] = 255;
        }
    }
}

// One separable box blur step over RGBA bytes, src -> dst, along one axis.
static void box_blur_h(const unsigned char *src, unsigned char *dst, unsigned w, unsigned h, int radius) {
    const int span = radius * 2 + 1;
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *row = src + (unsigned long long)y * w * 4;
        unsigned char *out = dst + (unsigned long long)y * w * 4;
        int acc[3] = {0, 0, 0};
        for (int k = -radius; k <= radius; k++) {
            int xx = k < 0 ? 0 : (k >= (int)w ? (int)w - 1 : k);
            for (int c = 0; c < 3; c++) acc[c] += row[xx * 4 + c];
        }
        for (unsigned x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) out[x * 4 + c] = (unsigned char)(acc[c] / span);
            out[x * 4 + 3] = 255;
            int add = (int)x + radius + 1;
            int sub = (int)x - radius;
            if (add >= (int)w) add = (int)w - 1;
            if (sub < 0) sub = 0;
            for (int c = 0; c < 3; c++) acc[c] += row[add * 4 + c] - row[sub * 4 + c];
        }
    }
}

static void box_blur_v(const unsigned char *src, unsigned char *dst, unsigned w, unsigned h, int radius) {
    const int span = radius * 2 + 1;
    const unsigned long long stride = (unsigned long long)w * 4;
    for (unsigned x = 0; x < w; x++) {
        int acc[3] = {0, 0, 0};
        for (int k = -radius; k <= radius; k++) {
            int yy = k < 0 ? 0 : (k >= (int)h ? (int)h - 1 : k);
            const unsigned char *p = src + (unsigned long long)yy * stride + (unsigned long long)x * 4;
            for (int c = 0; c < 3; c++) acc[c] += p[c];
        }
        for (unsigned y = 0; y < h; y++) {
            unsigned char *out = dst + (unsigned long long)y * stride + (unsigned long long)x * 4;
            for (int c = 0; c < 3; c++) out[c] = (unsigned char)(acc[c] / span);
            out[3] = 255;
            int add = (int)y + radius + 1;
            int sub = (int)y - radius;
            if (add >= (int)h) add = (int)h - 1;
            if (sub < 0) sub = 0;
            const unsigned char *pa = src + (unsigned long long)add * stride + (unsigned long long)x * 4;
            const unsigned char *ps = src + (unsigned long long)sub * stride + (unsigned long long)x * 4;
            for (int c = 0; c < 3; c++) acc[c] += pa[c] - ps[c];
        }
    }
}

// Mean of the red/green/blue channels over a sparse sample, so the log can say whether the read-back actually
// contains the world or just black (a black capture looks exactly like "the backdrop does not draw" on screen).
static float mean_brightness(const unsigned char *px, unsigned w, unsigned h) {
    unsigned long long n = (unsigned long long)w * h;
    unsigned long long step = n / 4096 ? n / 4096 : 1;
    unsigned long long sum = 0, count = 0;
    for (unsigned long long i = 0; i < n; i += step) {
        sum += px[i * 4 + 0] + px[i * 4 + 1] + px[i * 4 + 2];
        count += 3;
    }
    return count ? (float)((double)sum / (double)count) : 0.0f;
}

// Colour has to survive the frost. Blurring averages neighbouring pixels, which pulls every hue toward the
// mean grey - that is what makes a plain gaussian backdrop read as muddy instead of "glassy". Every recipe in
// the references pairs the blur with a saturation lift (blur(16px) saturate(160%)); Windows Acrylic does the
// same internally. Runs on the 1/4-size image (~120k pixels), once per capture.
// Grain, and not as a texture: a heavy blur over a smooth gradient produces visible contour steps (banding),
// and a tiny noise floor dithers them away. 4 % amplitude (~10/255) is the ceiling - above that it stops being
// dither and starts being film grain, which is a different look and not the one we want.
static void grain_rgb(unsigned char *px, unsigned count, int amplitude, unsigned seed) {
    unsigned s = seed | 1u;
    for (unsigned i = 0; i < count; i += 4) {
        s = s * 1103515245u + 12345u;
        int n = (int)((s >> 16) & 0x1Fu) - 16;               // -16..+15
        n = n * amplitude / 16;
        for (int c = 0; c < 3; c++) {
            int v = px[i + c] + n;
            px[i + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}
// Dark, low-saturation HSV from a hue. The tint has to stay dark (0.05-0.08) or white text stops working,
// so only the hue moves; saturation is fixed at 0.35 and value at 0.075, which at 220 degrees reproduces the
// cold grey the panel used to have hard-coded.
static void frost_hue_colour(float hue, float *r, float *g, float *b) {
    float h = hue;
    while (h < 0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    float sec = h / 60.0f;
    int i = (int)sec;
    float f = sec - (float)i;
    float v = 0.075f, s = 0.35f;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}
static void saturate_rgb(unsigned char *px, unsigned count, float amount) {
    for (unsigned i = 0; i < count; i += 4) {
        int r = px[i], g = px[i + 1], b = px[i + 2];
        int lum = (r * 77 + g * 150 + b * 29) >> 8;          // Rec.601 luma, integer
        int nr = lum + (int)((float)(r - lum) * amount);
        int ng = lum + (int)((float)(g - lum) * amount);
        int nb = lum + (int)((float)(b - lum) * amount);
        px[i]     = (unsigned char)(nr < 0 ? 0 : nr > 255 ? 255 : nr);
        px[i + 1] = (unsigned char)(ng < 0 ? 0 : ng > 255 ? 255 : ng);
        px[i + 2] = (unsigned char)(nb < 0 ? 0 : nb > 255 ? 255 : nb);
    }
}

extern "C" void capture_frosted_backdrop(int fb_w, int fb_h) {
    if (fb_w < 32 || fb_h < 32) return;
    if (!pGlReadPixels) return;
    // The caller's guard (the GUI module's own flag) is the primary condition; this one makes sure the
    // read-back also stops the moment the panel stops being drawn, even if that flag stays set.
    if (!g_active) return;
    g_active--;
    if ((long long)fb_w * fb_h > CAP_MAX_PIXELS) {
        if (!g_logged) {
            g_logged = 1;
            slinky_logf("frosted: %dx%d is over the pixel cap, backdrop disabled", fb_w, fb_h);
        }
        return;
    }
    // Reuse the previous capture on the frames in between; the panel does not need a fresh world every frame.
    if ((g_frame++ % CAP_EVERY) != 0 && g_serial) return;

    unsigned fw = (unsigned)fb_w, fh = (unsigned)fb_h;
    unsigned char *full = ensure(&g_full, &g_full_cap, (unsigned long long)fw * fh * 4);
    if (!full) return;
    glPixelStorei(0x0D05u, 1);                                        // GL_PACK_ALIGNMENT = 1
    glReadPixels(0, 0, (int)fw, (int)fh, 0x1908u, 0x1401u, full);     // GL_RGBA, GL_UNSIGNED_BYTE

    // 1/1 -> 1/2 -> ... -> 1/2^CAP_DOWNSTEPS
    unsigned sw = fw, sh = fh;
    unsigned char *src = full;
    for (int step = 0; step < CAP_DOWNSTEPS; step++) {
        unsigned dw = sw / 2, dh = sh / 2;
        if (!dw || !dh) return;
        unsigned char **slot = (step == 0) ? &g_half : (step == 1) ? &g_quarter : &g_eighth;
        unsigned *cap = (step == 0) ? &g_half_cap : (step == 1) ? &g_quarter_cap : &g_eighth_cap;
        unsigned char *dst = ensure(slot, cap, (unsigned long long)dw * dh * 4);
        if (!dst) return;
        half_size(src, dst, sw, sh);
        src = dst;
        sw = dw;
        sh = dh;
    }
    unsigned char *blur_src = (CAP_DOWNSTEPS >= 3) ? g_eighth : g_quarter;
    if (src != blur_src) return;                                      // too small to reach that scale

    unsigned char *a = ensure(&g_blur_a, &g_blur_a_cap, (unsigned long long)sw * sh * 4);
    unsigned char *b = ensure(&g_blur_b, &g_blur_b_cap, (unsigned long long)sw * sh * 4);
    if (!a || !b) return;
    int radius = (int)gui_frosted_blur.value;
    if (radius < 1) radius = 1;
    if (radius > 9) radius = 9;
    // Two box passes, not one. A single box blur has a rectangular kernel, and the result keeps faint
    // square structure - that is the "moulded plastic" look, it is not the tint or the tint opacity.
    // Convolving two boxes gives a triangular kernel, which is already a close approximation of a
    // gaussian; the falloff becomes smooth and the blocky structure goes away. A true gaussian kernel
    // would be another small step up for about 4x the cost, and at 1/4 scale nobody can tell them apart.
    box_blur_h(blur_src, a, sw, sh, radius);
    box_blur_v(a, b, sw, sh, radius);
    box_blur_h(b, a, sw, sh, radius);      // second pass, on the first result
    box_blur_v(a, b, sw, sh, radius);
    saturate_rgb(b, sw * sh, (float)CAP_SATURATE_NUM * 0.1f);
    if (CAP_GRAIN_AMPLITUDE > 0) grain_rgb(b, sw * sh, CAP_GRAIN_AMPLITUDE, 0x5eedu);

    // glReadPixels returns rows bottom-up and NanoVG's pattern samples v = 0 at the top of the image, so flip
    // the finished image once here instead of building the flip into every draw.
    unsigned char *row = a;                                           // reuse the scratch as one row
    for (unsigned y = 0; y < sh / 2; y++) {
        unsigned char *r0 = b + (unsigned long long)y * sw * 4;
        unsigned char *r1 = b + (unsigned long long)(sh - 1 - y) * sw * 4;
        for (unsigned i = 0; i < sw * 4; i++) row[i] = r0[i];
        for (unsigned i = 0; i < sw * 4; i++) r0[i] = r1[i];
        for (unsigned i = 0; i < sw * 4; i++) r1[i] = row[i];
    }

    // No gain, no grain. Both were tried and removed: the backdrop is meant to be the world, blurred, and
    // nothing else. A brightness lift makes lit ground read as a permanent highlight stripe across the panel,
    // and the grain reads as noise over what should be a clean surface.

    g_cap_w = sw;
    g_cap_h = sh;
    g_fb_w = fw;
    g_fb_h = fh;
    g_serial++;

    if (!g_logged) {
        g_logged = 1;
        slinky_logf("frosted: %dx%d -> %ux%u, blur r=%d, mean=%.1f", fb_w, fb_h, sw, sh, radius,
                    mean_brightness(b, sw, sh));
    } else if (!g_black_logged && mean_brightness(b, sw, sh) < 2.0f) {
        g_black_logged = 1;
        slinky_logf("frosted: capture is black (%ux%u), the world was not in the default framebuffer here", sw, sh);
    }
}

// Make sure this context has an image holding the latest capture, and refresh it when a newer capture exists.
// The handle is only valid in the context that created it, so each context gets its own slot.
static ImageSlot *sync_slot(void *nvg) {
    if (!nvg || !g_serial || !g_cap_w || !g_cap_h || !g_blur_b) return 0;

    ImageSlot *s = 0;
    for (int i = 0; i < 2; i++)
        if (g_slots[i].ctx == nvg) { s = &g_slots[i]; break; }
    if (!s)
        for (int i = 0; i < 2; i++)
            if (!g_slots[i].ctx) { s = &g_slots[i]; break; }
    if (!s) { s = &g_slots[1]; s->img = 0; }      // both taken: the HUD slot is the cheaper one to rebuild
    s->ctx = nvg;

    if (s->img && (s->w != g_cap_w || s->h != g_cap_h)) {
        nvgDeleteImage(nvg, s->img);
        s->img = 0;
    }
    if (!s->img) {
        s->img = nvgCreateImageRGBA(nvg, (int)g_cap_w, (int)g_cap_h, 0, g_blur_b);
        if (!s->img) {
            s->ctx = 0;
            if (!g_logged) {
                g_logged = 1;
                slinky_logf("frosted: nvgCreateImageRGBA returned 0");
            }
            return 0;
        }
        s->w = g_cap_w;
        s->h = g_cap_h;
        s->uploaded = g_serial;
        slinky_logf("frosted: image %d created (%ux%u)", s->img, g_cap_w, g_cap_h);
        return s;
    }
    if (s->uploaded != g_serial) {
        nvgUpdateImage(nvg, s->img, g_blur_b);
        s->uploaded = g_serial;
    }
    return s;
}

// Image handle for `nvg`, or 0 before the first capture. Used by the HUD, which draws its own backdrop.
extern "C" int gui_frosted_image(void *nvg) {
    ImageSlot *s = sync_slot(nvg);
    return s ? s->img : 0;
}

// Size of the blurred image, for code that wants to map it itself.
extern "C" void gui_frosted_size(int *w, int *h) {
    if (w) *w = (int)g_cap_w;
    if (h) *h = (int)g_cap_h;
}

// Size of the framebuffer the capture was taken at. A pattern drawn with (0,0,fb_w,fb_h) as its extent lines
// the image up with the screen, so a small shape samples the part of the blur that sits behind it.
extern "C" void gui_frosted_screen_size(int *w, int *h) {
    if (w) *w = (int)g_fb_w;
    if (h) *h = (int)g_fb_h;
}

// Claim the backdrop for this frame: keeps the capture running even when the GUI is closed (HUD only).
extern "C" void gui_frosted_note_active(void) {
    g_active = 3;
}

extern "C" void gui_draw_frosted(void *nvg, float x, float y, float w, float h, float round, float alpha) {
    if (!gui_frosted_gui_on.value) return;      // switched off: the caller draws its own plate
    if (!nvg || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    g_active = 3;               // the capture for the next frames is worth taking now that the panel is up
    ImageSlot *slot = sync_slot(nvg);

    if (g_draw_logged < 3) {
        g_draw_logged++;
        slinky_logf("frosted: draw alpha=%.2f panel=%.0fx%.0f image=%d (%ux%u)", alpha, w, h,
                    slot ? slot->img : 0, g_cap_w, g_cap_h);
    }

    nvg_Save(nvg);
    nvg_GlobalAlpha(nvg, alpha);

    // 1. Opaque plate. Drawn unconditionally and under the image on purpose: if the image path ever fails the
    //    panel is still readable, and "no backdrop" looks different from "the whole panel vanished".
    V4f plate = nvg_rgba_f(0.055f, 0.058f, 0.070f, 1.0f);
    nvg_BeginPath(nvg);
    nvg_RoundedRect(nvg, x, y, w, h, round);
    nvg_FillColor(nvg, &plate);
    nvg_Fill(nvg);

    // 2. The blurred world. nvgImagePattern() takes (offset, size) in this bundled NanoVG, so the extent is
    //    (w, h) - passing (x + w, y + h) would stretch the image by 2x for a panel that does not start at 0,0.
    if (slot && slot->img) {
        // Panel frost: magnify the sampled region about the panel centre. 100 % maps 1:1 like before;
        // higher values zoom the same image, so the panel looks softer than the world around it without
        // costing another blur pass.
        float zoom = gui_frosted_panel_blur.value * 0.01f;
        if (zoom < 1.0f) zoom = 1.0f;
        float ew = w * zoom, eh = h * zoom;
        float eox = x + (w - ew) * 0.5f, eoy = y + (h - eh) * 0.5f;
        Paint paint = nvg_image_pattern(nvg, eox, eoy, ew, eh, 0.0f, slot->img, 1.0f);
        nvg_BeginPath(nvg);
        nvg_RoundedRect(nvg, x, y, w, h, round);
        nvg_FillPaint(nvg, paint);
        nvg_Fill(nvg);
    }

    // 3. Tint: enough to keep text readable over a busy world, not enough to hide it. Was 0.45, which
    //    darkened everything behind the panel by nearly half and read as a grey film rather than glass.
    float tr = 0.055f, tg = 0.058f, tb = 0.070f;
    frost_hue_colour(gui_frosted_hue.value, &tr, &tg, &tb);
    float tint_a = gui_frosted_gui_tint.value * 0.01f;
    if (tint_a < 0.0f) tint_a = 0.0f;
    if (tint_a > 1.0f) tint_a = 1.0f;
    V4f tint = nvg_rgba_f(tr, tg, tb, tint_a);
    nvg_BeginPath(nvg);
    nvg_RoundedRect(nvg, x, y, w, h, round);
    nvg_FillColor(nvg, &tint);
    nvg_Fill(nvg);

    // That is the whole backdrop: blurred world, one flat tint, nothing else. A specular band or a lit top
    // edge belongs on the panes that sit ON the glass (the cards), not on the glass itself - drawing it here
    // put a white gradient across the top of the whole screen, which is not what a window looks like.

    nvg_Restore(nvg);
}

extern "C" void gui_frosted_shutdown(void) {
    for (int i = 0; i < 2; i++) {
        if (g_slots[i].img && g_slots[i].ctx) nvgDeleteImage(g_slots[i].ctx, g_slots[i].img);
        g_slots[i].img = 0;
        g_slots[i].ctx = 0;
        g_slots[i].w = g_slots[i].h = 0;
    }
    if (g_full) { libc_free(g_full); g_full = 0; g_full_cap = 0; }
    if (g_half) { libc_free(g_half); g_half = 0; g_half_cap = 0; }
    if (g_quarter) { libc_free(g_quarter); g_quarter = 0; g_quarter_cap = 0; }
    if (g_eighth) { libc_free(g_eighth); g_eighth = 0; g_eighth_cap = 0; }
    if (g_blur_a) { libc_free(g_blur_a); g_blur_a = 0; g_blur_a_cap = 0; }
    if (g_blur_b) { libc_free(g_blur_b); g_blur_b = 0; g_blur_b_cap = 0; }
}

#else   // SLINKY_FROSTED_GUI == 0: no GL slots, no NanoVG images, nothing to go wrong.

extern "C" void capture_frosted_backdrop(int, int) {}
extern "C" void gui_draw_frosted(void *, float, float, float, float, float, float) {}
extern "C" void gui_frosted_shutdown(void) {}
extern "C" int gui_frosted_image(void *) { return 0; }
extern "C" void gui_frosted_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
extern "C" void gui_frosted_screen_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
extern "C" void gui_frosted_note_active(void) {}

#endif














