// Stasis — freeze in place, then release.
//
// Port of xyz.daikirai.module.movement.StasisModule (108 lines). The idea is to hold the player perfectly still
// for a moment: motion is zeroed every tick while frozen, and the saved motion is handed back on release, so the
// player continues from the halt instead of losing the movement entirely. Pulsed: freeze for N ticks, release
// for one, repeat. That rhythm is what makes it useful — a permanent freeze is just standing still.
//
// Why the delay on release: without it the module freezes again on the very next tick, the motion never gets a
// chance to be applied, and the player is stuck without moving at all. The reference uses `delay = 1`, one tick
// of freedom between pulses.
//
// Omitted from the reference: onSendPacketAlways. It replaces the outgoing C03PacketPlayer with a look-only
// packet while frozen, which needs packet construction and a send hook this build does not expose. Without it
// the freeze still works; the server simply receives the position packets the game would send anyway, which for
// a motion-zeroed player are identical positions.
#include "slinky/engine.h"

struct Vec3 { double x, y, z; };

// ---- primitives, each checked against its real definition ---------------------
FN(Vec3, java_get_entity_motion_x, (void *jvm, void *entity));
FN(void, java_set_entity_motion_x, (void *jvm, void *entity, Vec3 value));
FN(int, java_get_entity_ticks_existed, (void *jvm, void *entity));

// ---- settings ----------------------------------------------------------------
struct SliderS { char pad[0x10]; const char *label; const char *format;
                 float step, scale, min, max, value, saved; };
struct BoolS   { char pad[0x10]; const char *label; unsigned char value, saved; char pad_1a[6]; };

// Pulse = how many ticks to stay frozen. 0 disables pulsing: one freeze until the module is turned off.
static SliderS g_pulse = { {}, "Pulse", "%.0f ticks", 1.0f, 1.0f, 0.0f, 40.0f, 4.0f, 4.0f };
static BoolS   g_zero_input = { {}, "Hold input", 1, 1, {} };

static float sv(SliderS *s) { return *(float *)((char *)s + 0x30); }
static unsigned char bv(BoolS *b) { return *(unsigned char *)((char *)b + 0x18); }

// The toggle event: which module, and its new state.
struct ToggleE {
    char padding_00[8];
    Ctx *ctx;                // +0x08
    void *module;            // +0x10
    unsigned char enabled;   // +0x18
};

// ---- state -------------------------------------------------------------------
static unsigned char st_frozen;
static Vec3 st_saved;
static int st_frozen_at;
static int st_delay;

static void st_freeze(Ctx *ctx, unsigned char on) {
    if (on) {
        // Remember what the player was doing before the freeze; release puts it back.
        st_saved = java_get_entity_motion_x(ctx->jvm, ctx->player);
        st_frozen_at = java_get_entity_ticks_existed(ctx->jvm, ctx->player);
    } else {
        java_set_entity_motion_x(ctx->jvm, ctx->player, st_saved);
    }
    st_frozen = on;
}

// Slot 0: the freeze itself.
extern "C" void stasis_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ctx->player) {
        st_frozen = 0;
        st_delay = 0;
        return;
    }

    // Countdown to the next freeze. Releasing for one tick before freezing again is what lets the restored
    // motion actually be applied; without the gap the player never moves.
    if (st_delay > 0) {
        st_delay--;
        if (st_delay == 0) st_freeze(ctx, 1);
    }

    if (!st_frozen) return;

    Vec3 zero = {0.0, 0.0, 0.0};
    java_set_entity_motion_x(ctx->jvm, ctx->player, zero);

    int pulse = (int)sv(&g_pulse);
    if (pulse > 0) {
        int elapsed = java_get_entity_ticks_existed(ctx->jvm, ctx->player) - st_frozen_at;
        if (elapsed >= pulse) {
            st_freeze(ctx, 0);
            st_delay = 1;
        }
    }
}

// Slot 17: hold the movement input at zero while frozen, so the player does not walk out of the freeze. The
// client has no setter for movementInput, so the effect is produced by zeroing motion again after the tick has
// had its chance to apply input. This runs late for that reason.
extern "C" void stasis_on_late_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (!st_frozen || ctx->in_game != 1 || !ctx->player) return;
    if (bv(&g_zero_input) != 1) return;
    Vec3 zero = {0.0, 0.0, 0.0};
    java_set_entity_motion_x(ctx->jvm, ctx->player, zero);
}

// Slot 5: leaving the module must not leave the player frozen with no motion.
extern "C" void stasis_on_toggle(Hook *hook) {
    ToggleE *t = (ToggleE *)hook;
    if (t->enabled) {
        // Turning on: two ticks of grace before the first freeze, the same as the reference's onEnable.
        st_delay = 2;
        st_frozen = 0;
        return;
    }
    if (st_frozen && t->ctx && t->ctx->player)
        java_set_entity_motion_x(t->ctx->jvm, t->ctx->player, st_saved);
    st_frozen = 0;
    st_delay = 0;
}

// ---- settings page / descriptor ---------------------------------------------
extern "C" const char str_stasis[] = "Stasis";
extern "C" const char str_stasis_desc[] = "Freezes you in place and releases on a pulse.";
extern "C" const char str_stasis_main[] = "Main";

extern "C" void stasis_gui(void *gui, void *window) {
    W_label(gui, window, str_stasis_main);
    W_slider(window, gui, &g_pulse);
    W_bool(window, gui, &g_zero_input);
}
extern "C" void stasis_register(void *module) {
    R_num(module, "Pulse", &g_pulse);
    R_bool(module, "Hold input", &g_zero_input);
}

struct StHSlot { void *handler; void *context; };
struct StDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    StHSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(StDescriptor) == 0x210, "descriptor size");

extern "C" StDescriptor stasis_module_descriptor = {
    str_stasis,
    str_stasis_desc,
    3,                      // category: Block/Movement, same group the engine's own SafeWalk uses
    0,                      // starts off
    {0, 0, 0},
    0,                      // ordinary module
    0,                      // no default bind
    0,                      // no hold mode
    {},
    {
        { (void *)stasis_on_tick, 0 },                       // 0  tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        { (void *)stasis_on_toggle, 0 },                     // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        {0, 0},                                              // 13
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        { (void *)stasis_on_late_tick, 0 },                  // 17 late tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    stasis_gui,
    stasis_register,
    0,
};
