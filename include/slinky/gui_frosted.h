// Frosted-glass backdrop for the ClickGUI panel and the Mod Overlay HUD.
//
// One capture feeds both (a single glReadPixels, one blur, one NanoVG image per context), but the two
// surfaces are switched and tinted independently, each from its own module settings page:
//
//   ClickGUI module     "Frosted glass"  on/off   鈫?gui_frosted_gui_on
//                       "frosted_tint"   percent  鈫?gui_frosted_gui_tint
//                       "frosted_blur"   px       鈫?gui_frosted_blur        (shared: it shapes the capture)
//   Mod Overlay module  "Frosted glass"  on/off   鈫?hud_frosted_on
//                       "hud_frosted_tint" percent 鈫?hud_frosted_tint        (row plate opacity)
//
// Turning both off means nothing asks for the backdrop, so capture_frosted_backdrop() returns on its first
// check and the read-back never runs.
//
// SLINKY_FROSTED_GUI 0 compiles the pipeline out (the GUI falls back to its flat plate, the HUD to solid
// rows). The settings objects still exist in that build so both module pages keep their shape and their
// saved values survive a rollback.
#pragma once

#define SLINKY_FROSTED_GUI 1

#ifdef __cplusplus
// Setting storage, laid out the way the engine's visitors cast it - same shapes as bed_esp.cpp.
struct FrostBool {
    char pad[0x10];
    const char *label;              // +0x10
    unsigned char value, saved;     // +0x18 / +0x19
    char pad_1a[6];
};
struct FrostSlider {
    char pad[0x10];
    const char *label;              // +0x10
    const char *format;             // +0x18
    float step, scale, min, max, value, saved;
};

extern "C" {
#endif

// Read the current default framebuffer and refresh the blurred backdrop. Must be called while the finished
// world frame is still on the default framebuffer: in java_natives_frame.cpp's frame hook, right before
// begin_frame_framebuffer(). Returns immediately when nothing has claimed the backdrop this frame.
void capture_frosted_backdrop(int framebuffer_w, int framebuffer_h);

// Draw the backdrop under a panel: flat plate, blurred world, flat tint. `round` is the corner radius.
// Does nothing when the ClickGUI's frosted switch is off, so the caller can fall back on its own plate.
void gui_draw_frosted(void *nvg, float x, float y, float w, float h, float round, float alpha);

// Release the images and the scratch buffers.
void gui_frosted_shutdown(void);

// Image handle for this NanoVG context (0 before the first capture). A handle only exists in the context that
// created it, so the GUI (window->ig) and the HUD (window->dl) each ask for their own.
int gui_frosted_image(void *nvg);

// Size of the blurred image, and of the framebuffer it came from. Passing the latter as the pattern extent
// lines the image up with the screen, so a small shape samples the blur that sits behind it.
void gui_frosted_size(int *w, int *h);
void gui_frosted_screen_size(int *w, int *h);

// Claim the backdrop for this frame. The GUI calls it from its own draw path; the HUD calls it while it has
// rows on screen. Without a claim the capture stops as soon as nothing is drawing.
void gui_frosted_note_active(void);

#ifdef __cplusplus
}

// Settings objects: defined in gui_frosted.cpp, shown on two different module pages.
extern FrostBool gui_frosted_gui_on;        // ClickGUI: frosted backdrop on/off
extern FrostSlider gui_frosted_gui_tint;    // ClickGUI: backdrop tint, percent
extern FrostSlider gui_frosted_blur;        // shared: blur radius at the downsampled scale
extern FrostSlider gui_frosted_panel;       // ClickGUI: card/panel opacity, percent (the "how see-through" knob)
extern FrostSlider gui_frosted_hue;         // ClickGUI: backdrop hue, degrees (custom colour)
extern FrostSlider gui_frosted_panel_blur;  // ClickGUI: the panel's own frost, independent of the world blur
extern FrostBool hud_frosted_on;            // Mod Overlay: frosted rows on/off
extern FrostSlider hud_frosted_tint;        // Mod Overlay: row plate opacity, percent

extern "C" const char str_frosted_section[];
extern "C" const char str_frosted_gui_on[];
extern "C" const char str_frosted_gui_tint[];
extern "C" const char str_frosted_blur[];
extern "C" const char str_hud_frosted_on[];
extern "C" const char str_hud_frosted_tint[];
extern "C" const char str_frosted_percent_format[];
extern "C" const char str_frosted_radius_format[];
extern "C" const char str_frosted_panel[];
extern "C" const char str_frosted_hue[];
extern "C" const char str_frosted_hue_format[];
extern "C" const char str_frosted_panel_blur[];
extern "C" const char str_frosted_zoom_format[];
#endif


