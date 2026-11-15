// GUI 鈥?Graphical interface for the cheat.
// category: visual   descriptor: 0x2eb740   settings block: 0x2eb938
// functions: gui gui_gui, register gui_register
// implementation: src/logic/visual/gui.cpp
#pragma once
#include "slinky/api.h"
#include "slinky/gui_frosted.h"

S(str_main_in_gui); // "Main"
S(str_layout); // "Layout"
S(str_scale_in_gui); // "scale"
S(str_accent_color_in_gui); // "accent_color"
S(str_allow_input); // "allow_input"
S(str_compact_in_gui); // "compact"
S(str_wide_in_gui); // "wide"

D(gui_accent_color_setting); // setting key "accent_color"
D(gui_scale_setting); // setting enum "scale"
D(gui_allow_input_setting); // setting bool "allow_input"
D(gui_compact_setting); // setting bool "compact"
D(gui_wide_setting); // setting bool "wide"

extern "C" void gui_gui(void *gui, void *window) {
    W_label(gui, window, str_main_in_gui);
    W_enumw(window, gui, gui_scale_setting);
    W_color(window, gui, gui_accent_color_setting);
    W_bool(window, gui, gui_allow_input_setting);
    W_label(gui, window, str_layout);
    W_bool(window, gui, gui_compact_setting);
    W_bool(window, gui, gui_wide_setting);
    W_bool(window, gui, &gui_frosted_gui_on);     // frosted backdrop under the panel (off = stock plate)
    W_slider(window, gui, &gui_frosted_panel);    // how see-through
    W_slider(window, gui, &gui_frosted_hue);      // custom colour
    W_slider(window, gui, &gui_frosted_gui_tint); // tint strength
    W_slider(window, gui, &gui_frosted_panel_blur); // panel-only frost
    W_slider(window, gui, &gui_frosted_blur);     // world blur radius
}

extern "C" void gui_register(void *module) {
    R_enum(module, str_scale_in_gui, gui_scale_setting);
    R_key(module, str_accent_color_in_gui, gui_accent_color_setting);
    R_bool(module, str_allow_input, gui_allow_input_setting);
    R_bool(module, str_compact_in_gui, gui_compact_setting);
    R_bool(module, str_wide_in_gui, gui_wide_setting);
    R_bool(module, str_frosted_gui_on, &gui_frosted_gui_on);
    R_num(module, str_frosted_panel, &gui_frosted_panel);
    R_num(module, str_frosted_hue, &gui_frosted_hue);
    R_num(module, str_frosted_gui_tint, &gui_frosted_gui_tint);
    R_num(module, str_frosted_panel_blur, &gui_frosted_panel_blur);
    R_num(module, str_frosted_blur, &gui_frosted_blur);
}



