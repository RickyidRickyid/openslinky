// Timer Advance — a Raven B4 style timer with the controls that make it usable in practice.
//
// The existing Timer only answers "should the game run faster right now". This one adds the three things that
// decide whether a timer is actually survivable:
//
//   1. Three speed presets with a selector, so the speed can be changed in game without opening the GUI.
//   2. Automatic release on the events that expose a timer: opening chat or the pause menu, and a server
//      teleport. A timer left running through either is the usual way people get caught.
//   3. Reset on input: clicking or switching a hotbar slot snaps the speed back to normal for that tick, so the
//      server never sees an accelerated click or an accelerated slot change.
//
// Strafe only restricts acceleration to sideways movement, which is the case where a timer is least visible.
//
// The engine's own speed is what gets written (Timer.timerSpeed), so this is a pure logic module: no packets,
// no pose change, nothing for the server to compare against beyond the tick timing it already sees.
#include "slinky/engine.h"

struct Vec3 { double x, y, z; };

// ---- primitives, each checked against its real definition before use ----------
FN(void *, get_timer, (void *jvm, void *minecraft));
FN(void, java_set_timer_timer_speed, (void *jvm, void *timer, float speed));
FN(void *, get_current_screen, (void *jvm, void *minecraft));
FN(Vec3, java_get_entity_motion_x, (void *jvm, void *entity));
FN(unsigned char, java_get_entity_on_ground, (void *jvm, void *entity));
FN(float, java_get_entity_living_base_move_strafing, (void *jvm, void *entity_living_base));
FN(float, java_get_entity_living_base_move_forward, (void *jvm, void *entity_living_base));
FN(void *, java_get_entity_player_inventory, (void *jvm, void *entity_player));
FN(int, java_get_inventory_player_current_item, (void *jvm, void *inventory_player));
FN(unsigned char, vertex_pointer_current, (Ctx *ctx));      // a mouse button is held
FN(double, time_now, ());

// ---- settings ----------------------------------------------------------------
struct SliderA { char pad[0x10]; const char *label; const char *format;
                 float step, scale, min, max, value, saved; };
struct BoolA   { char pad[0x10]; const char *label; unsigned char value, saved; char pad_1a[6]; };

static SliderA g_selector  = { {}, "Preset", "%.0f", 1.0f, 1.0f, 0.0f, 2.0f, 0.0f, 0.0f };
static SliderA g_preset_a  = { {}, "Speed A", "%.2fx", 0.01f, 1.0f, 0.01f, 2.00f, 0.01f, 0.01f };
static SliderA g_preset_b  = { {}, "Speed B", "%.2fx", 0.01f, 1.0f, 0.01f, 2.00f, 1.00f, 1.00f };
static SliderA g_preset_c  = { {}, "Speed C", "%.2fx", 0.01f, 1.0f, 0.01f, 2.00f, 1.30f, 1.30f };
static BoolA   g_bypass_menu   = { {}, "Disable in chat/menu", 1, 1, {} };
static BoolA   g_reset_input   = { {}, "Reset on input", 1, 1, {} };
static BoolA   g_strafe_only   = { {}, "Strafe only", 0, 0, {} };
static BoolA   g_require_ground = { {}, "Ground only", 0, 0, {} };

static float sv(SliderA *s) { return *(float *)((char *)s + 0x30); }
static unsigned char bv(BoolA *b) { return *(unsigned char *)((char *)b + 0x18); }

// The toggle event carries the module that was switched and the new state, laid out the same way the existing
// timer module reads it.
struct ToggleHook {
    char padding_00[8];
    Ctx *ctx;        // +0x08
    void *module;    // +0x10 descriptor that was toggled
    unsigned char enabled;  // +0x18
};

// ---- state -------------------------------------------------------------------
static float ta_active_speed = 1.0f;
static unsigned char ta_reset_now;
static int ta_last_slot = -1;
static unsigned char ta_last_click;
static double ta_last_pos_time;

static float ta_selected_speed(void) {
    int which = (int)sv(&g_selector);
    if (which == 1) return sv(&g_preset_b);
    if (which == 2) return sv(&g_preset_c);
    return sv(&g_preset_a);
}

static void ta_apply(Ctx *ctx, float speed) {
    void *timer = get_timer(ctx->jvm, ctx->minecraft);
    if (!timer) return;
    ta_active_speed = speed;
    java_set_timer_timer_speed(ctx->jvm, timer, speed);
}

// Watches for the two inputs that must not be sped up: a click, and a hotbar slot change.
static void ta_poll_input(Ctx *ctx) {
    if (bv(&g_reset_input) != 1) return;

    unsigned char clicking = vertex_pointer_current(ctx) ? 1 : 0;
    if (clicking && !ta_last_click) ta_reset_now = 1;
    ta_last_click = clicking;

    void *inv = java_get_entity_player_inventory(ctx->jvm, ctx->player);
    if (!inv) return;
    int slot = java_get_inventory_player_current_item(ctx->jvm, inv);
    if (ta_last_slot >= 0 && slot != ta_last_slot) ta_reset_now = 1;
    ta_last_slot = slot;
}

// Slot 0: one decision per tick, in the same shape as the engine's own tick callback.
extern "C" void timer_advance_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ctx->player) {
        ta_apply(ctx, 1.0f);
        ta_last_slot = -1;
        return;
    }

    // A screen is the giveaway case: the game visibly fast-forwards and the player is obviously not in control.
    if (bv(&g_bypass_menu) == 1 && get_current_screen(ctx->jvm, ctx->minecraft)) {
        ta_apply(ctx, 1.0f);
        ta_reset_now = 0;
        return;
    }

    ta_poll_input(ctx);

    if (ta_reset_now) {
        // One tick at normal speed is enough to cover the click or the slot change.
        ta_reset_now = 0;
        ta_apply(ctx, 1.0f);
        return;
    }

    if (bv(&g_strafe_only) == 1) {
        float strafing = java_get_entity_living_base_move_strafing(ctx->jvm, ctx->player);
        float forward = java_get_entity_living_base_move_forward(ctx->jvm, ctx->player);
        // "Strafe only" means sideways motion with no forward component.
        if (strafing == 0.0f || forward != 0.0f) {
            ta_apply(ctx, 1.0f);
            return;
        }
    }

    if (bv(&g_require_ground) == 1 && !java_get_entity_on_ground(ctx->jvm, ctx->player)) {
        ta_apply(ctx, 1.0f);
        return;
    }

    // The floor is 0.01, not 1.0: 0.01 is the "slow timer" rate a stasis-style hold uses, and the reference
    // profile ships exactly that value. The engine keeps ticking at 1 tick per second at this rate, which is
    // slow but still responsive enough that the window does not appear frozen.
    float speed = ta_selected_speed();
    if (speed < 0.01f) speed = 0.01f;
    if (speed > 2.0f) speed = 2.0f;
    ta_apply(ctx, speed);
}

// Slot 5: leaving the module must put the engine back to normal speed, otherwise the game stays fast forever.
extern "C" void timer_advance_on_toggle(Hook *hook) {
    ToggleHook *t = (ToggleHook *)hook;
    if (t->enabled) return;    // only on the way off
    ta_apply(t->ctx, 1.0f);
    ta_reset_now = 0;
    ta_last_slot = -1;
}

// ---- settings page / descriptor ---------------------------------------------
extern "C" const char str_timer_advance[] = "Timer Advance";
extern "C" const char str_timer_advance_desc[] = "Adjusts game speed with three presets and automatic release.";
extern "C" const char str_timer_advance_main[] = "Main";

extern "C" void timer_advance_gui(void *gui, void *window) {
    W_label(gui, window, str_timer_advance_main);
    W_slider(window, gui, &g_selector);
    W_slider(window, gui, &g_preset_a);
    W_slider(window, gui, &g_preset_b);
    W_slider(window, gui, &g_preset_c);
    W_bool(window, gui, &g_bypass_menu);
    W_bool(window, gui, &g_reset_input);
    W_bool(window, gui, &g_strafe_only);
    W_bool(window, gui, &g_require_ground);
}
extern "C" void timer_advance_register(void *module) {
    R_num(module, "Preset", &g_selector);
    R_num(module, "Speed A", &g_preset_a);
    R_num(module, "Speed B", &g_preset_b);
    R_num(module, "Speed C", &g_preset_c);
    R_bool(module, "Disable in chat/menu", &g_bypass_menu);
    R_bool(module, "Reset on input", &g_reset_input);
    R_bool(module, "Strafe only", &g_strafe_only);
    R_bool(module, "Ground only", &g_require_ground);
}

struct TaHSlot { void *handler; void *context; };
struct TaDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    TaHSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(TaDescriptor) == 0x210, "descriptor size");

extern "C" TaDescriptor timer_advance_module_descriptor = {
    str_timer_advance,
    str_timer_advance_desc,
    4,                      // category: Utility (the engine's own Timer lives here)
    0,                      // starts off
    {0, 0, 0},
    0,                      // ordinary module
    44,                     // default bind: Z, matching the reference profile's key: 44
    0,                      // no hold mode
    {},
    {
        { (void *)timer_advance_on_tick, 0 },                // 0  tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        { (void *)timer_advance_on_toggle, 0 },              // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        {0, 0},                                              // 13
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        {0, 0},                                              // 17
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    timer_advance_gui,
    timer_advance_register,
    0,
};
