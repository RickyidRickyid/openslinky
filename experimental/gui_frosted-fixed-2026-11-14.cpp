// Frosted glass backdrop: the pixels behind the panel are actually sampled and blurred.
//
//  1. read the finished world frame back from the default framebuffer (the only moment in the frame where it
//     is still intact - once the GUI framebuffer is bound and cleared the world is gone),
//  2. box-downsample it twice in software (1/1 -> 1/2 -> 1/4) and blur the smallest octave,
//  3. upload that as an RGBA texture and draw it back through nvgImagePattern, stretched over the panel,
//     with a tint and a rim highlight on top so text still reads.
//
// Differences from the version in _experimental/gui_frosted.cpp (2026-11-13), all of them bugs that would
// have made this crash or draw nothing:
//
//  * the read-back buffer was sized for the 1/4 scale image (w/4 * h/4 * 4 bytes) but glReadPixels was told
//    to read the whole framebuffer - at 1080p that writes 8.2 MB into a 518 KB allocation. The full-size
//    buffer is allocated here and the downsampling happens afterwards.
//  * nvg_image_pattern was declared as returning void*: the real wrapper returns NVGpaint by value (72
//    bytes, so the Win64 ABI returns it through a hidden pointer). Calling it through the wrong signature
//    corrupts the stack. It is declared with a 0x4c-byte placeholder, like gui_layout.cpp does.
//  * nvg_FillColor was called between nvg_FillPaint and nvg_Fill, so the blurred image was replaced by a
//    flat tint before it was ever filled - the texture was uploaded and never drawn.
//
// The read-back is a GPU sync point, so it runs on a cadence (CAP_EVERY frames) and only while a screen is
// open. The panel is static most of the time and a four-times-old frame of the blurred backdrop is not
// distinguishable from a fresh one.
#include "slinky/engine.h"
#define SLINKY_GL_MACROS 1              // glReadPixels / glGenTextures / ... expand to the client's own slots
#include "slinky/gl_slots.h"

struct V4f { float x, y, z, w; };
struct Paint { char b[0x4c]; };          // NVGpaint, returned by value

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
FN(V4f, nvg_rgba_f, (float r, float g, float b, float a));

// libc, declared per file the way the rest of the client does it
FN(void *, libc_malloc, (unsigned long long n));
FN(void, libc_free, (void *ptr));
FN(void *, libc_memcpy_alt, (void *dest, const void *src, unsigned long long n));
FN(void, slinky_logf, (const char *fmt, ...));

enum {
    CAP_SCALE = 4,            // the blurred image is 1/4 of the framebuffer in each axis
    CAP_MAX_W = 480,          // hard caps so a 4K window does not smear everything into one colour
    CAP_MAX_H = 320,
    CAP_BLUR_RADIUS = 3,
    CAP_EVERY = 3,            // capture one frame in three
    CAP_MAX_PIXELS = 3840 * 2160,
};

static unsigned char *g_full;         // full-resolution read-back, reused
static unsigned g_full_cap;
static unsigned char *g_half;         // 1/2
static unsigned g_half_cap;
static unsigned char *g_quarter;      // 1/4, the blur source
static unsigned g_quarter_cap;
static unsigned char *g_blurred;      // 1/4, the blur result
static unsigned g_blurred_cap;
static unsigned char *g_row;          // flip scratch
static unsigned g_row_cap;

static unsigned g_tex;                // GL texture name
static unsigned g_tex_w, g_tex_h;
static unsigned g_frame;
static unsigned char g_logged;        // one line per session, so the log says whether this actually ran
static unsigned g_draw_count;

static unsigned char *ensure(unsigned char **buf, unsigned *cap, unsigned need) {
    if (need <= *cap && *buf) return *buf;
    if (*buf) libc_free(*buf);
    *buf = (unsigned char *)libc_malloc(need);
    *cap = *buf ? need : 0;
    return *buf;
}

// One separable box blur over RGBA bytes, horizontal then vertical, src -> dst.
static void box_blur(const unsigned char *src, unsigned char *dst, unsigned w, unsigned h, int radius) {
    if (radius < 1) {
        libc_memcpy_alt(dst, src, (unsigned long long)w * h * 4);
        return;
    }
    const int span = radius * 2 + 1;
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *row = src + (unsigned long long)y * w * 4;
        unsigned char *out = dst + (unsigned long long)y * w * 4;
        int acc[4] = {0, 0, 0, 0};
        for (int k = -radius; k <= radius; k++) {
            int xx = k < 0 ? 0 : (k >= (int)w ? (int)w - 1 : k);
            for (int c = 0; c < 4; c++) acc[c] += row[xx * 4 + c];
        }
        for (unsigned x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) out[x * 4 + c] = (unsigned char)(acc[c] / span);
            int add = (int)x + radius + 1;
            int sub = (int)x - radius;
            if (add >= (int)w) add = (int)w - 1;
            if (sub < 0) sub = 0;
            for (int c = 0; c < 4; c++) acc[c] += row[add * 4 + c] - row[sub * 4 + c];
        }
    }
}

// Halve an RGBA image by averaging 2x2 blocks.
static void half_size(const unsigned char *src, unsigned char *dst, unsigned w, unsigned h) {
    unsigned dw = w / 2, dh = h / 2;
    for (unsigned y = 0; y < dh; y++) {
        for (unsigned x = 0; x < dw; x++) {
            const unsigned char *p0 = src + ((unsigned long long)(y * 2) * w + x * 2) * 4;
            const unsigned char *p1 = p0 + (unsigned long long)w * 4;
            for (int c = 0; c < 4; c++) {
                unsigned sum = p0[c] + p0[4 + c] + p1[c] + p1[4 + c];
                dst[((unsigned long long)y * dw + x) * 4 + c] = (unsigned char)(sum >> 2);
            }
        }
    }
}

// Capture the finished frame and blur it into the texture. Returns 1 when the texture was refreshed.
extern "C" int capture_frosted_backdrop(int fb_w, int fb_h) {
    if (fb_w < 32 || fb_h < 32) return 0;
    if ((long long)fb_w * fb_h > CAP_MAX_PIXELS) {
        if (!g_logged) { g_logged = 1; slinky_logf("frosted: %dx%d is over the pixel cap, backdrop disabled", fb_w, fb_h); }
        return 0;
    }
    if (!pGlReadPixels) {
        if (!g_logged) { g_logged = 1; slinky_logf("frosted: glReadPixels slot is null"); }
        return 0;
    }

    if (CAP_EVERY > 1 && (g_frame++ % CAP_EVERY) != 0 && g_tex) return 1;

    unsigned fw = (unsigned)fb_w, fh = (unsigned)fb_h;
    unsigned cw = fw / CAP_SCALE, ch = fh / CAP_SCALE;
    if (cw == 0 || ch == 0) return 0;
    if (cw > CAP_MAX_W) cw = CAP_MAX_W;
    if (ch > CAP_MAX_H) ch = CAP_MAX_H;

    unsigned char *full = ensure(&g_full, &g_full_cap, fw * fh * 4);
    if (!full) return 0;
    glPixelStorei(0x0D05u, 1);                       // GL_PACK_ALIGNMENT = 1
    glReadPixels(0, 0, (int)fw, (int)fh, 0x1908u, 0x1401u, full);   // GL_RGBA, GL_UNSIGNED_BYTE

    // 1/1 -> 1/2 -> 1/4. Sizes follow the framebuffer, then the caps above trim the texture.
    unsigned sw = fw, sh = fh;
    unsigned char *src = full;
    for (int step = 0; step < 2; step++) {
        unsigned dw = sw / 2, dh = sh / 2;
        if (!dw || !dh) break;
        unsigned char *dst = ensure(step == 0 ? &g_half : &g_quarter,
                                    step == 0 ? &g_half_cap : &g_quarter_cap,
                                    dw * dh * 4);
        if (!dst) break;
        half_size(src, dst, sw, sh);
        src = dst;
        sw = dw;
        sh = dh;
    }
    if (sw > cw) sw = cw;
    if (sh > ch) sh = ch;
    if (!g_quarter && src != full) return 0;
    if (src == full) return 0;                       // a framebuffer too small to downsample twice

    unsigned char *blurred = ensure(&g_blurred, &g_blurred_cap, sw * sh * 4);
    if (!blurred) return 0;
    box_blur(g_quarter, blurred, sw, sh, CAP_BLUR_RADIUS);

    // glReadPixels returns rows bottom-up, NanoVG draws top-down: flip instead of flipping the UVs, because
    // the tint and the rim are drawn in NanoVG space on top of the image.
    unsigned char *row = ensure(&g_row, &g_row_cap, sw * 4);
    if (row) {
        for (unsigned y = 0; y < sh / 2; y++) {
            unsigned char *a = blurred + (unsigned long long)y * sw * 4;
            unsigned char *b = blurred + (unsigned long long)(sh - 1 - y) * sw * 4;
            libc_memcpy_alt(row, a, (unsigned long long)sw * 4);
            libc_memcpy_alt(a, b, (unsigned long long)sw * 4);
            libc_memcpy_alt(b, row, (unsigned long long)sw * 4);
        }
    }

    if (!g_tex) {
        glGenTextures(1, &g_tex);
        g_tex_w = 0;
        g_tex_h = 0;
    }
    glBindTexture(0x0DE1u, g_tex);
    glTexParameteri(0x0DE1u, 0x2801u, 0x2601);       // MIN_FILTER = LINEAR
    glTexParameteri(0x0DE1u, 0x2800u, 0x2601);       // MAG_FILTER = LINEAR
    glTexParameteri(0x0DE1u, 0x2802u, 0x812F);       // WRAP_S = CLAMP_TO_EDGE
    glTexParameteri(0x0DE1u, 0x2803u, 0x812F);       // WRAP_T = CLAMP_TO_EDGE
    glTexImage2D(0x0DE1u, 0, 0x1908, (int)sw, (int)sh, 0, 0x1908u, 0x1401u, blurred);
    glBindTexture(0x0DE1u, 0);
    g_tex_w = sw;
    g_tex_h = sh;
    if (!g_logged) {
        g_logged = 1;
        slinky_logf("frosted: %dx%d -> tex %ux%u (id %u), blur r=%d", fb_w, fb_h, sw, sh, g_tex, CAP_BLUR_RADIUS);
    }
    return 1;
}

// Draw the blurred backdrop over the panel, then the tint and the lit top edge.
//
// The dark plate is drawn unconditionally and the blurred image goes on top of it. That ordering is on
// purpose: if the image pattern ever fails to draw, the result is a dark panel rather than whatever the GUI
// framebuffer happened to be cleared to, so "the backdrop is missing" and "the backdrop is broken" look
// different on screen instead of identical.
extern "C" void gui_draw_frosted(void *nvg, float x, float y, float w, float h, float round, float alpha) {
    if (!nvg || alpha <= 0.0f || w <= 0.0f || h <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    if (g_draw_count < 4) {
        g_draw_count++;
        slinky_logf("frosted: draw #%u alpha=%.2f panel=%.0fx%.0f tex=%u (%ux%u)", g_draw_count, alpha, w, h,
                    g_tex, g_tex_w, g_tex_h);
    }

    nvg_Save(nvg);
    nvg_GlobalAlpha(nvg, alpha);

    V4f plate = nvg_rgba_f(0.055f, 0.058f, 0.070f, 1.0f);
    nvg_BeginPath(nvg);
    nvg_RoundedRect(nvg, x, y, w, h, round);
    nvg_FillColor(nvg, &plate);
    nvg_Fill(nvg);

    if (g_tex && g_tex_w && g_tex_h) {
        Paint paint = nvg_image_pattern(nvg, x, y, x + w, y + h, 0.0f, (int)g_tex, 1.0f);
        nvg_BeginPath(nvg);
        nvg_RoundedRect(nvg, x, y, w, h, round);
        nvg_FillPaint(nvg, paint);
        nvg_Fill(nvg);
    }

    // Tint over the blurred image: without it the backdrop is too literal and text stops reading.
    V4f tint = nvg_rgba_f(0.055f, 0.058f, 0.070f, 0.55f);
    nvg_BeginPath(nvg);
    nvg_RoundedRect(nvg, x, y, w, h, round);
    nvg_FillColor(nvg, &tint);
    nvg_Fill(nvg);

    // Rim highlight along the top: the lit edge that sells the glass.
    V4f hl = nvg_rgba_f(1.0f, 1.0f, 1.0f, 0.10f);
    nvg_BeginPath(nvg);
    nvg_Rect(nvg, x, y, w, 1.5f);
    nvg_FillColor(nvg, &hl);
    nvg_Fill(nvg);

    nvg_Restore(nvg);
}

// Drop the texture and the scratch buffers.
extern "C" void gui_frosted_shutdown(void) {
    if (g_tex && pGlDeleteTextures) {
        glDeleteTextures(1, &g_tex);
        g_tex = 0;
    }
    g_tex_w = 0;
    g_tex_h = 0;
    if (g_full) { libc_free(g_full); g_full = 0; g_full_cap = 0; }
    if (g_half) { libc_free(g_half); g_half = 0; g_half_cap = 0; }
    if (g_quarter) { libc_free(g_quarter); g_quarter = 0; g_quarter_cap = 0; }
    if (g_blurred) { libc_free(g_blurred); g_blurred = 0; g_blurred_cap = 0; }
    if (g_row) { libc_free(g_row); g_row = 0; g_row_cap = 0; }
}
