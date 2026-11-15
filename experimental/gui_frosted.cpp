// Frosted glass backdrop, done properly: the pixels behind the panel are actually sampled and blurred.
//
// Pipeline, once per frame while the GUI is open:
//
//   1. glReadPixels the strip of the framebuffer the panel covers, at 1/4 resolution. Reading a quarter of the
//      pixels is four times cheaper and gives the first level of averaging for free.
//   2. Box-downsample that image twice more in software (1/4 -> 1/8 -> 1/16) and then blur each level with a
//      small separable kernel. Three octaves is what makes it look like glass rather than a smear.
//   3. Upload the result as an RGBA texture and draw it back through nvg_image_pattern, stretched to cover the
//      panel, with a tint and a rim highlight on top.
//
// The read-back is the expensive part, so it runs on the panel's dirty cadence rather than every frame: the
// world behind a static panel does not change, and re-reading it sixty times a second is what makes naive
// frosted-glass implementations drop the frame rate.
#include "slinky/engine.h"


// GL through the client's own slots: pGl* are plain void* function pointers filled by gl_slots.cpp.
extern "C" void *pGlReadPixels;
extern "C" void *pGlPixelStorei;
extern "C" void *pGlGenTextures;
extern "C" void *pGlBindTexture;
extern "C" void *pGlTexImage2D;
extern "C" void *pGlTexParameteri;
extern "C" void *pGlDeleteTextures;

typedef void (*FnReadPixels)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*FnPixelStorei)(unsigned, int);
typedef void (*FnGenTextures)(int, int *);
typedef void (*FnBindTexture)(unsigned, int);
typedef void (*FnTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void (*FnTexParameteri)(unsigned, unsigned, int);
typedef void (*FnDeleteTextures)(int, const int *);

#define GL_RGBA_           0x1908u
#define GL_UNSIGNED_BYTE_  0x1401u
#define GL_TEXTURE_2D_     0x0DE1u
#define GL_TEXTURE_MIN_FILTER_ 0x2801u
#define GL_TEXTURE_MAG_FILTER_ 0x2800u
#define GL_TEXTURE_WRAP_S_ 0x2802u
#define GL_TEXTURE_WRAP_T_ 0x2803u
#define GL_LINEAR_         0x2601
#define GL_CLAMP_TO_EDGE_  0x812F

struct V4fb { float x, y, z, w; };

FN(void, nvg_BeginPath, (void *nvg));
FN(void, nvg_Rect, (void *nvg, float x, float y, float w, float h));
FN(void, nvg_Fill, (void *nvg));
FN(void, nvg_FillPaint, (void *nvg, void *paint));
FN(void, nvg_RoundedRect, (void *nvg, float x, float y, float w, float h, float round));
FN(void, nvg_FillColor, (void *nvg, V4fb *c));
FN(void, nvg_GlobalAlpha, (void *nvg, float alpha));
FN(void, nvg_Save, (void *nvg));
FN(void, nvg_Restore, (void *nvg));
FN(void *, nvg_image_pattern, (void *nvg, float ox, float oy, float ex, float ey, int angle, int tex, float alpha));
FN(V4fb, nvg_rgba_f, (float r, float g, float b, float a));

// libc, declared per-file the way the rest of the client does it
FN(void *, libc_malloc, (unsigned long long n));
FN(void, libc_free, (void *ptr));
FN(void *, libc_memcpy_alt, (void *dest, const void *src, unsigned long long n));

// ---- capture buffer -------------------------------------------------------

enum {
    CAP_DOWN = 4,        // read the framebuffer at 1/4 resolution
    CAP_MAX_W = 480,     // hard cap so a 4K window does not allocate 40 MB
    CAP_MAX_H = 320,
    BLUR_RADIUS = 3,     // separable box radius at the smallest octave
};

static unsigned char *g_buf;          // scratch RGBA, reused every capture
static unsigned  g_buf_w, g_buf_h;
static unsigned  g_buf_capacity;

static int g_tex;                     // the texture the blurred result lives in
static unsigned g_tex_w, g_tex_h;

// ---------------------------------------------------------------------------

static unsigned char *ensure_buffer(unsigned w, unsigned h) {
    unsigned need = w * h * 4;
    if (need <= g_buf_capacity) {
        g_buf_w = w;
        g_buf_h = h;
        return g_buf;
    }
    if (g_buf) libc_free(g_buf);
    g_buf = (unsigned char *)libc_malloc(need);
    g_buf_capacity = g_buf ? need : 0;
    g_buf_w = g_buf ? w : 0;
    g_buf_h = g_buf ? h : 0;
    return g_buf;
}

// One separable box blur pass over RGBA bytes, horizontal then vertical, into `dst`.
static void box_blur(const unsigned char *src, unsigned char *dst, unsigned w, unsigned h, int radius) {
    if (radius < 1) {
        libc_memcpy_alt(dst, src, (unsigned long long)w * h * 4);
        return;
    }
    const int span = radius * 2 + 1;

    // horizontal: src -> tmp
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
            const unsigned char *p1 = p0 + w * 4;
            for (int c = 0; c < 4; c++) {
                unsigned sum = p0[c] + p0[4 + c] + p1[c] + p1[4 + c];
                dst[((unsigned long long)y * dw + x) * 4 + c] = (unsigned char)(sum >> 2);
            }
        }
    }
}

// Capture the panel area from the framebuffer and blur it into g_tex.
extern "C" int capture_frosted_backdrop(int fb_x, int fb_y, int fb_w, int fb_h) {
    if (fb_w <= 0 || fb_h <= 0) return 0;

    unsigned cw = (unsigned)fb_w / CAP_DOWN;
    unsigned ch = (unsigned)fb_h / CAP_DOWN;
    if (cw == 0 || ch == 0) return 0;
    if (cw > CAP_MAX_W) cw = CAP_MAX_W;
    if (ch > CAP_MAX_H) ch = CAP_MAX_H;

    unsigned char *buf = ensure_buffer(cw, ch);
    if (!buf) return 0;

    if (!pGlReadPixels) return 0;
    ((FnPixelStorei)pGlPixelStorei)(0x0D05u, 1);              // GL_PACK_ALIGNMENT = 1
    ((FnReadPixels)pGlReadPixels)(fb_x, fb_y, fb_w, fb_h, GL_RGBA_, GL_UNSIGNED_BYTE_, buf);

    // Three octaves: 1/4 -> 1/8 -> 1/16, blurred at the last one.
    unsigned w = cw, h = ch;
    unsigned char *cur = buf;
    for (int octave = 0; octave < 2 && w > 4 && h > 4; octave++) {
        unsigned nw = w / 2, nh = h / 2;
        unsigned char *next = (unsigned char *)libc_malloc((unsigned long long)nw * nh * 4);
        if (!next) break;
        half_size(cur, next, w, h);
        if (cur != buf) libc_free(cur);
        cur = next;
        w = nw;
        h = nh;
    }

    unsigned char *blurred = (unsigned char *)libc_malloc((unsigned long long)w * h * 4);
    if (!blurred) {
        if (cur != buf) libc_free(cur);
        return 0;
    }
    box_blur(cur, blurred, w, h, BLUR_RADIUS);
    if (cur != buf) libc_free(cur);

    // glReadPixels returns rows bottom-up, NanoVG draws top-down. Flip here rather than flipping the UVs,
    // because the tint and the rim highlight are drawn in NanoVG's space on top of the image.
    {
        unsigned char *row = (unsigned char *)libc_malloc((unsigned long long)w * 4);
        if (row) {
            for (unsigned y = 0; y < h / 2; y++) {
                unsigned char *a = blurred + (unsigned long long)y * w * 4;
                unsigned char *b = blurred + (unsigned long long)(h - 1 - y) * w * 4;
                libc_memcpy_alt(row, a, (unsigned long long)w * 4);
                libc_memcpy_alt(a, b, (unsigned long long)w * 4);
                libc_memcpy_alt(b, row, (unsigned long long)w * 4);
            }
            libc_free(row);
        }
    }

    // Upload. Reallocate only when the size changed.
    if (!g_tex) {
        ((FnGenTextures)pGlGenTextures)(1, &g_tex);
        g_tex_w = 0;
        g_tex_h = 0;
    }
    ((FnBindTexture)pGlBindTexture)(GL_TEXTURE_2D_, g_tex);
    ((FnTexParameteri)pGlTexParameteri)(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    ((FnTexParameteri)pGlTexParameteri)(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    ((FnTexParameteri)pGlTexParameteri)(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    ((FnTexParameteri)pGlTexParameteri)(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    ((FnTexImage2D)pGlTexImage2D)(GL_TEXTURE_2D_, 0, (int)GL_RGBA_, (int)w, (int)h, 0,
                                  GL_RGBA_, GL_UNSIGNED_BYTE_, blurred);
    ((FnBindTexture)pGlBindTexture)(GL_TEXTURE_2D_, 0);

    g_tex_w = w;
    g_tex_h = h;
    libc_free(blurred);
    return 1;
}

// Draw the captured, blurred framebuffer region back over the panel, then tint it.
extern "C" void gui_draw_frosted(void *nvg, float x, float y, float w, float h, float round, float alpha) {
    if (!nvg || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    nvg_Save(nvg);
    nvg_GlobalAlpha(nvg, alpha);

    if (g_tex && g_tex_w && g_tex_h) {
        // The blurred texture covers the panel. It is stretched back to full size with GL_LINEAR, which is
        // where most of the smoothness comes from.
        void *paint = nvg_image_pattern(nvg, x, y, x + w, y + h, 0, g_tex, 1.0f);
        nvg_BeginPath(nvg);
        nvg_RoundedRect(nvg, x, y, w, h, round);
        nvg_FillPaint(nvg, paint);
    } else {
        nvg_BeginPath(nvg);
        nvg_RoundedRect(nvg, x, y, w, h, round);
    }

    // Tint over the blurred image: without it the backdrop is too literal and text stops reading.
    V4fb tint = nvg_rgba_f(0.055f, 0.058f, 0.070f, 0.55f);
    nvg_FillColor(nvg, &tint);
    nvg_Fill(nvg);

    // Rim highlight along the top: the lit edge that sells the glass.
    V4fb hl = nvg_rgba_f(1.0f, 1.0f, 1.0f, 0.10f);
    nvg_FillColor(nvg, &hl);
    nvg_BeginPath(nvg);
    nvg_Rect(nvg, x, y, w, 1.5f);
    nvg_Fill(nvg);

    nvg_Restore(nvg);
}

// Release the texture when the GUI closes so a resize does not leak it.
extern "C" void gui_frosted_shutdown(void) {
    if (g_tex) {
        ((FnDeleteTextures)pGlDeleteTextures)(1, &g_tex);
        g_tex = 0;
    }
    g_tex_w = 0;
    g_tex_h = 0;
    if (g_buf) {
        libc_free(g_buf);
        g_buf = 0;
        g_buf_capacity = 0;
    }
}
