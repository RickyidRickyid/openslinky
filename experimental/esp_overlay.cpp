// ESP overlay in its own top-level window, so screen capture never sees it.
//
// Why this exists at all: the ESP modules currently draw inside the game's own GL context, which means their
// pixels are part of the game's frame. OBS Game Capture reads that frame, and no GL API can hide a subset of
// it. To be invisible to a capture tool the drawing has to live in a different window, and that window has to
// be excluded from capture by the compositor.
//
//   Game Capture      -> sees only the game's window; this overlay is a different window, so nothing is captured
//   Display Capture   -> sees the whole screen, BUT WDA_EXCLUDEFROMCAPTURE makes the window render as empty
//                        in any capture, while staying visible on the physical display
//
// WDA_EXCLUDEFROMCAPTURE is Windows 10 2004 (build 19041) and later. On older builds the call fails and the
// overlay is simply capturable; there is no older equivalent that also keeps it visible on screen.
//
// The overlay is a layered, click-through, always-on-top window covering the game's client area. Boxes are
// projected from world space with the modelview/projection matrices the client already captures each frame,
// and drawn with GDI into a DIB section, then pushed to the window with UpdateLayeredWindow.
#include "slinky/engine.h"

// ---- Win32 -----------------------------------------------------------------

typedef void *HWND_t;
typedef void *HDC_t;
typedef void *HGDIOBJ_t;
typedef void *HINSTANCE_t;
typedef long long LRESULT_t;
typedef unsigned UINT_t;
typedef unsigned long DWORD_t;
typedef int BOOL_t;
typedef unsigned short ATOM_t;
typedef long long LPARAM_t;
typedef unsigned long long WPARAM_t;
typedef unsigned UINT_PTR_t;

struct WNDCLASSW_t {
    UINT_t style;
    LRESULT_t (*lpfnWndProc)(HWND_t, UINT_t, WPARAM_t, LPARAM_t);
    int cbClsExtra, cbWndExtra;
    HINSTANCE_t hInstance, hIcon, hCursor, hbrBackground;
    const wchar_t *lpszMenuName, *lpszClassName;
};
struct POINT_t { int x, y; };
struct MSG_t { HWND_t hwnd; UINT_t message; WPARAM_t wParam; LPARAM_t lParam; DWORD_t time; POINT_t pt; };
struct RECT_t { int left, top, right, bottom; };
struct BITMAPINFOHEADER_t {
    DWORD_t biSize; int biWidth, biHeight; unsigned short biPlanes, biBitCount;
    DWORD_t biCompression, biSizeImage; int biXPelsPerMeter, biYPelsPerMeter;
    DWORD_t biClrUsed, biClrImportant;
};
struct BITMAPINFO_t { BITMAPINFOHEADER_t bmiHeader; DWORD_t bmiColors[3]; };

FN(void, slinky_logf, (const char *fmt, ...));
FN(HWND_t, GetForegroundWindow, ());
FN(HWND_t, FindWindowW, (const wchar_t *cls, const wchar_t *title));
FN(int, GetWindowThreadProcessId, (HWND_t h, DWORD_t *pid));
FN(DWORD_t, GetCurrentProcessId, ());
FN(BOOL_t, GetClientRect, (HWND_t h, RECT_t *r));
FN(BOOL_t, ClientToScreen, (HWND_t h, POINT_t *p));
FN(BOOL_t, GetWindowRect, (HWND_t h, RECT_t *r));

FN(ATOM_t, RegisterClassW, (const WNDCLASSW_t *wc));
FN(HWND_t, CreateWindowExW, (DWORD_t ex, const wchar_t *cls, const wchar_t *title, DWORD_t style,
                            int x, int y, int w, int h, HWND_t parent, void *menu, HINSTANCE_t inst, void *param));
FN(BOOL_t, DestroyWindow, (HWND_t h));
FN(BOOL_t, ShowWindow, (HWND_t h, int cmd));
FN(BOOL_t, SetWindowPos, (HWND_t h, HWND_t after, int x, int y, int cx, int cy, UINT_t flags));
FN(long long, GetWindowLongW, (HWND_t h, int index));
FN(long long, SetWindowLongW, (HWND_t h, int index, long long value));
FN(BOOL_t, SetLayeredWindowAttributes, (HWND_t h, DWORD_t key, unsigned char alpha, DWORD_t flags));
FN(BOOL_t, SetWindowDisplayAffinity, (HWND_t h, DWORD_t affinity));
FN(HDC_t, GetDC, (HWND_t h));
FN(int, ReleaseDC, (HWND_t h, HDC_t dc));
FN(LRESULT_t, DefWindowProcW, (HWND_t h, UINT_t msg, WPARAM_t wp, LPARAM_t lp));
FN(LRESULT_t, CallWindowProcW, (void *prev, HWND_t h, UINT_t msg, WPARAM_t wp, LPARAM_t lp));
FN(int, GetSystemMetrics, (int index));
FN(BOOL_t, PeekMessageW, (MSG_t *msg, HWND_t h, UINT_t lo, UINT_t hi, UINT_t remove));
FN(BOOL_t, TranslateMessage, (const MSG_t *msg));
FN(LRESULT_t, DispatchMessageW, (const MSG_t *msg));
FN(HGDIOBJ_t, LoadCursorW, (HINSTANCE_t inst, const wchar_t *name));
FN(int, UpdateLayeredWindow, (HWND_t h, HDC_t dst, POINT_t *dst_pt, void *size, HDC_t src,
                              POINT_t *src_pt, DWORD_t colour_key, void *blend, DWORD_t flags));

// ---- GDI -------------------------------------------------------------------

FN(HDC_t, CreateCompatibleDC, (HDC_t dc));
FN(HGDIOBJ_t, CreateDIBSection, (HDC_t dc, const BITMAPINFO_t *bmi, UINT_t usage,
                                 void **bits, void *section, DWORD_t offset));
FN(HGDIOBJ_t, SelectObject, (HDC_t dc, HGDIOBJ_t obj));
FN(BOOL_t, DeleteObject, (HGDIOBJ_t obj));
FN(BOOL_t, DeleteDC, (HDC_t dc));
FN(HGDIOBJ_t, GetStockObject, (int index));
FN(HGDIOBJ_t, CreatePen, (int style, int width, DWORD_t colour));
FN(HGDIOBJ_t, CreateSolidBrush, (DWORD_t colour));
FN(BOOL_t, MoveToEx, (HDC_t dc, int x, int y, POINT_t *prev));
FN(BOOL_t, LineTo, (HDC_t dc, int x, int y));
FN(BOOL_t, Rectangle, (HDC_t dc, int l, int t, int r, int b));
FN(BOOL_t, Ellipse, (HDC_t dc, int l, int t, int r, int b));
FN(BOOL_t, SetBkMode, (HDC_t dc, int mode));
FN(DWORD_t, SetTextColor, (HDC_t dc, DWORD_t colour));
FN(BOOL_t, TextOutW, (HDC_t dc, int x, int y, const wchar_t *s, int len));

// ---- state -----------------------------------------------------------------

enum {
    WS_EX_LAYERED_         = 0x00080000,
    WS_EX_TRANSPARENT_     = 0x00000020,
    WS_EX_TOPMOST_         = 0x00000008,
    WS_EX_TOOLWINDOW_      = 0x00000080,
    WS_EX_NOACTIVATE_     = 0x08000000,
    WS_POPUP_              = 0x80000000,
    SW_SHOWNOACTIVATE_     = 4,
    SW_HIDE_               = 0,
    HWND_TOPMOST_          = -1,
    SWP_NOMOVE_            = 0x0002,
    SWP_NOSIZE_            = 0x0001,
    SWP_NOACTIVATE_        = 0x0010,
    SWP_SHOWWINDOW_        = 0x0040,
    GWL_EXSTYLE_           = -20,
    ULW_ALPHA_             = 0x00000002,
    AC_SRC_OVER_           = 0x00,
    AC_SRC_ALPHA_          = 0x01,
    WDA_NONE_              = 0x00000000,
    WDA_EXCLUDEFROMCAPTURE_ = 0x00000011,
    DIB_RGB_COLORS_        = 0,
    BI_RGB_                = 0,
    PS_SOLID_              = 0,
    GDI_NULL_BRUSH_        = 5,     // HOLLOW_BRUSH stock object
    TRANSPARENT_           = 1,
    PM_REMOVE_             = 0x0001,
    CS_HREDRAW_            = 0x0002,
    CS_VREDRAW_            = 0x0001,
};

static HWND_t   g_overlay;
static HDC_t    g_mem_dc;
static HGDIOBJ_t g_bitmap, g_old_bitmap;
static unsigned g_w, g_h;
static unsigned *g_pixels;
static unsigned char g_ready;

// The client already fills these every frame in java_natives_world.cpp, before dispatching render events.
extern "C" double gl_modelview_matrix[16];
extern "C" double gl_projection_matrix[16];

static LRESULT_t overlay_proc(HWND_t h, UINT_t msg, WPARAM_t wp, LPARAM_t lp) {
    // Everything is drawn on demand; the window never needs to handle input or paint.
    return DefWindowProcW(h, msg, wp, lp);
}

static void release_surface(void) {
    if (g_mem_dc) {
        if (g_old_bitmap) SelectObject(g_mem_dc, g_old_bitmap);
        if (g_bitmap) DeleteObject(g_bitmap);
        DeleteDC(g_mem_dc);
    }
    g_mem_dc = 0;
    g_bitmap = 0;
    g_old_bitmap = 0;
    g_pixels = 0;
    g_w = 0;
    g_h = 0;
}

static int ensure_surface(HWND_t game, unsigned w, unsigned h) {
    if (g_mem_dc && g_w == w && g_h == h && g_pixels) return 1;
    release_surface();

    HDC_t screen = GetDC(0);
    if (!screen) return 0;
    g_mem_dc = CreateCompatibleDC(screen);
    ReleaseDC(0, screen);
    if (!g_mem_dc) return 0;

    BITMAPINFO_t bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER_t);
    bi.bmiHeader.biWidth = (int)w;
    // Negative height: top-down rows, matching screen coordinates. Without this every box is mirrored.
    bi.bmiHeader.biHeight = -(int)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB_;

    void *bits = 0;
    g_bitmap = CreateDIBSection(g_mem_dc, &bi, DIB_RGB_COLORS_, &bits, 0, 0);
    if (!g_bitmap || !bits) { release_surface(); return 0; }
    g_old_bitmap = SelectObject(g_mem_dc, g_bitmap);
    g_pixels = (unsigned *)bits;
    g_w = w;
    g_h = h;
    (void)game;
    return 1;
}

extern "C" int overlay_ensure(void) {
    if (g_ready) return 1;

    DWORD_t self = GetCurrentProcessId();
    HWND_t game = GetForegroundWindow();
    if (!game) return 0;
    DWORD_t owner = 0;
    GetWindowThreadProcessId(game, &owner);
    if (owner != self) return 0;   // the game window is not in front: nothing to overlay onto

    static const wchar_t *cls = L"SlkyEspOverlay";
    WNDCLASSW_t wc = {};
    wc.style = CS_HREDRAW_ | CS_VREDRAW_;
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = 0;
    wc.hCursor = LoadCursorW(0, (const wchar_t *)32512);   // IDC_ARROW
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    DWORD_t ex = WS_EX_LAYERED_ | WS_EX_TRANSPARENT_ | WS_EX_TOPMOST_ | WS_EX_TOOLWINDOW_ | WS_EX_NOACTIVATE_;
    g_overlay = CreateWindowExW(ex, cls, L"", WS_POPUP_, 0, 0, 1, 1, 0, 0, 0, 0);
    if (!g_overlay) return 0;

    // Click-through over the whole window, not just transparent pixels.
    SetWindowLongW(g_overlay, GWL_EXSTYLE_,
                   GetWindowLongW(g_overlay, GWL_EXSTYLE_) | WS_EX_LAYERED_ | WS_EX_TRANSPARENT_);

    // The point of the exercise: keep the window out of any screen capture while it stays visible on screen.
    // This fails on Windows builds before 10 2004; the overlay still works, it is just capturable.
    DWORD_t affinity_ok = SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE_);
    if (!affinity_ok) SetWindowDisplayAffinity(g_overlay, WDA_NONE_);

    ShowWindow(g_overlay, SW_SHOWNOACTIVATE_);
    g_ready = 1;
    slinky_logf("overlay: hwnd=%p display_affinity=%s", (void *)g_overlay,
                affinity_ok ? "EXCLUDED_FROM_CAPTURE" : "FAILED_NEEDS_WIN10_2004");
    return affinity_ok ? 2 : 1;   // 2 = excluded from capture, 1 = visible but capturable
}

// Put the overlay over the game's client area, matching its size.
extern "C" void overlay_track(int client_w, int client_h) {
    if (!g_overlay) return;
    HWND_t game = GetForegroundWindow();
    if (!game) { ShowWindow(g_overlay, SW_HIDE_); return; }
    DWORD_t owner = 0;
    GetWindowThreadProcessId(game, &owner);
    if (owner != GetCurrentProcessId()) { ShowWindow(g_overlay, SW_HIDE_); return; }
    if (game == g_overlay) return;

    POINT_t origin = {0, 0};
    ClientToScreen(game, &origin);
    SetWindowPos(g_overlay, (HWND_t)HWND_TOPMOST_, origin.x, origin.y, client_w, client_h,
                 SWP_NOACTIVATE_ | SWP_SHOWWINDOW_);
    static int last_w, last_h;
    if (client_w != last_w || client_h != last_h) {
        last_w = client_w;
        last_h = client_h;
        slinky_logf("overlay: tracking %dx%d", client_w, client_h);
    }
    ensure_surface(game, (unsigned)client_w, (unsigned)client_h);
}

// Present the DIB. `alpha` is folded into each pixel so the whole layer fades with the GUI.
extern "C" void overlay_present(unsigned char alpha) {
    if (!g_overlay || !g_mem_dc || !g_w || !g_h) return;

    POINT_t dst = {0, 0};
    POINT_t src = {0, 0};
    // the game window's top-left in screen space
    HWND_t game = GetForegroundWindow();
    if (game) ClientToScreen(game, &dst);

    unsigned long long size = ((unsigned long long)g_w << 32) | g_h;
    unsigned long long blend = ((unsigned long long)alpha << 56) | ((unsigned long long)AC_SRC_ALPHA_ << 24);
    UpdateLayeredWindow(g_overlay, 0, &dst, &size, g_mem_dc, &src, 0, (void *)&blend, ULW_ALPHA_);
}

// World -> screen. Returns 0 when the point is behind the camera.
extern "C" int overlay_project(double wx, double wy, double wz, int *out_x, int *out_y) {
    double mv[16], pj[16];
    for (int i = 0; i < 16; i++) { mv[i] = gl_modelview_matrix[i]; pj[i] = gl_projection_matrix[i]; }

    // eye = modelview * world
    double ex = mv[0] * wx + mv[4] * wy + mv[8] * wz + mv[12];
    double ey = mv[1] * wx + mv[5] * wy + mv[9] * wz + mv[13];
    double ez = mv[2] * wx + mv[6] * wy + mv[10] * wz + mv[14];
    double ew = mv[3] * wx + mv[7] * wy + mv[11] * wz + mv[15];
    if (ew != 0.0) { ex /= ew; ey /= ew; ez /= ew; }
    if (ez > -0.05) return 0;   // behind the near plane

    // clip = projection * eye
    double cx = pj[0] * ex + pj[4] * ey + pj[8] * ez + pj[12];
    double cy = pj[1] * ex + pj[5] * ey + pj[9] * ez + pj[13];
    double cw = pj[3] * ex + pj[7] * ey + pj[11] * ez + pj[15];
    if (cw == 0.0) return 0;

    double ndc_x = cx / cw;
    double ndc_y = cy / cw;
    if (ndc_x < -1.3 || ndc_x > 1.3 || ndc_y < -1.3 || ndc_y > 1.3) return 0;

    *out_x = (int)((ndc_x * 0.5 + 0.5) * (double)g_w);
    *out_y = (int)((0.5 - ndc_y * 0.5) * (double)g_h);
    return 1;
}

// Clear the DIB to fully transparent. Required every frame: the previous frame's pixels stay in it otherwise.
extern "C" void overlay_clear(void) {
    if (!g_pixels) return;
    unsigned long long n = (unsigned long long)g_w * g_h;
    for (unsigned long long i = 0; i < n; i++) g_pixels[i] = 0;
}

// Axis-aligned world box -> filled/outlined rect on screen.
extern "C" void overlay_draw_box(double x0, double y0, double z0, double x1, double y1, double z1,
                                 unsigned colour_abgr, int filled) {
    if (!g_mem_dc) return;
    int sx0, sy0, sx1, sy1;
    if (!overlay_project(x0, y0, z0, &sx0, &sy0)) return;
    if (!overlay_project(x1, y1, z1, &sx1, &sy1)) return;

    // GDI takes COLORREF: 0x00BBGGRR
    HGDIOBJ_t pen = CreatePen(PS_SOLID_, 1, colour_abgr & 0x00FFFFFF);
    HGDIOBJ_t old_pen = SelectObject(g_mem_dc, pen);
    HGDIOBJ_t old_brush = 0;
    if (filled) {
        HGDIOBJ_t brush = CreateSolidBrush(colour_abgr & 0x00FFFFFF);
        old_brush = SelectObject(g_mem_dc, brush);
    } else {
        old_brush = SelectObject(g_mem_dc, GetStockObject(GDI_NULL_BRUSH_));
    }
    SetBkMode(g_mem_dc, TRANSPARENT_);

    if (sx0 > sx1) { int t = sx0; sx0 = sx1; sx1 = t; }
    if (sy0 > sy1) { int t = sy0; sy0 = sy1; sy1 = t; }
    Rectangle(g_mem_dc, sx0, sy0, sx1, sy1);

    if (old_brush) SelectObject(g_mem_dc, old_brush);
    SelectObject(g_mem_dc, old_pen);
    DeleteObject(pen);
    if (filled) { /* brush freed below by owner */ }
}

extern "C" void overlay_shutdown(void) {
    if (g_overlay) {
        DestroyWindow(g_overlay);
        g_overlay = 0;
    }
    release_surface();
    g_ready = 0;
}

extern "C" int overlay_active(void) { return g_ready; }
