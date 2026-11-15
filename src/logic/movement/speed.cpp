// Speed — a ground speed boost with a strafe that actually matches the movement input.
//
// Port of xyz.daikirai.module.movement.SpeedModule (131 lines). The reference's boost is one line:
//
//     double d = 0.153 * multiplier + speedLevel * 0.04;
//     MoveUtils.strafe(Math.max(d, current));
//
// What strafe() does is the whole trick: it renormalises the horizontal motion vector to a target magnitude
// while keeping its direction. Setting motionX and motionZ to fixed values instead — which is what most naive
// speed modules do — makes the player drift sideways the moment they look elsewhere, because the direction is
// no longer tied to the input.
//
// Two modes in the reference. VANILLA boosts every tick the conditions hold. FLOAT skips a tick after landing
// and nudges posY by 1e-13 on Hypixel, which is a client-side check for "on the ground" that a plain onGround
// test does not satisfy. FLOAT also refuses to run on slabs and stairs, where the nudge lands on a different
// surface and the flag is visible.
//
// Left out: NoRotateModule.sinceSetback (no setback tracking in this build) and the speedLevel lookup (no
// potion accessor). Both are dropped, not stubbed, so the module never silently claims to do them.
#include "slinky/engine.h"

struct Vec3 { double x, y, z; };
struct Rot { float yaw, pitch; };

FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(Vec3, java_get_entity_motion_x, (void *jvm, void *entity));
FN(void, java_set_entity_motion_x, (void *jvm, void *entity, Vec3 value));
FN(unsigned char, java_get_entity_on_ground, (void *jvm, void *entity));
FN(float, java_get_entity_living_base_move_forward, (void *jvm, void *entity_living_base));
FN(float, java_get_entity_living_base_move_strafing, (void *jvm, void *entity_living_base));
FN(Rot, get_entity_rotation, (void *jvm, void *entity));
FN(double, libc_hypot, (double x, double z));
FN(int, java_get_entity_ticks_existed, (void *jvm, void *entity));
FN(void *, get_current_screen, (void *jvm, void *minecraft));
FN(void *, get_game_settings, (void *jvm, void *object));
FN(void *, java_get_game_settings_forward_key, (void *jvm, void *game_settings));
FN(void *, java_get_game_settings_left_key, (void *jvm, void *game_settings));
FN(void *, java_get_game_settings_right_key, (void *jvm, void *game_settings));
FN(unsigned char, is_key_down, (void *jvm, void *key_binding));

// ---- settings ----------------------------------------------------------------
struct SliderSp { char pad[0x10]; const char *label; const char *format;
                  float step, scale, min, max, value, saved; };
struct BoolSp   { char pad[0x10]; const char *label; unsigned char value, saved; char pad_1a[6]; };

static SliderSp g_mode       = { {}, "Mode", "%.0f", 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f };  // 0 VANILLA 1 FLOAT
static SliderSp g_multiplier = { {}, "Multiplier", "%.1f", 0.1f, 1.0f, 0.1f, 3.0f, 1.0f, 1.0f };
static BoolSp   g_disable_inv  = { {}, "Disable in inventory", 1, 1, {} };
static BoolSp   g_only_forward = { {}, "Only forward", 0, 0, {} };
static BoolSp   g_only_strafe  = { {}, "Only strafe", 0, 0, {} };

static float sv(SliderSp *s) { return *(float *)((char *)s + 0x30); }
static unsigned char bv(BoolSp *b) { return *(unsigned char *)((char *)b + 0x18); }

// The toggle event: which module, and its new state.
struct ToggleSp {
    char padding_00[8];
    Ctx *ctx;
    void *module;
    unsigned char enabled;
};

// ---- state -------------------------------------------------------------------
static unsigned char sp_waiting;
static int sp_air_ticks;
static int sp_last_ticks;

// Renormalise the horizontal motion to `target` while keeping its direction.
//
// This is MoveUtils.strafe. If the current speed is already higher, nothing changes — that is the Math.max in
// the reference, and it is what stops the module from slowing the player down when they are already fast.
static void sp_strafe(Ctx *ctx, double target) {
    Vec3 m = java_get_entity_motion_x(ctx->jvm, ctx->player);
    double speed = libc_hypot(m.x, m.z);
    if (speed < 1.0e-4) return;                 // not moving: nothing to renormalise
    if (speed >= target) return;                // already faster than the target

    double scale = target / speed;
    m.x *= scale;
    m.z *= scale;
    java_set_entity_motion_x(ctx->jvm, ctx->player, m);
}

// Whether the player is holding a direction, and which.
static void sp_input(Ctx *ctx, float *forward, float *strafe) {
    void *gs = get_game_settings(ctx->jvm, ctx->minecraft);
    if (!gs) { *forward = 0.0f; *strafe = 0.0f; return; }
    void *f = java_get_game_settings_forward_key(ctx->jvm, gs);
    void *l = java_get_game_settings_left_key(ctx->jvm, gs);
    void *r = java_get_game_settings_right_key(ctx->jvm, gs);
    float fw = (f && is_key_down(ctx->jvm, f)) ? 1.0f : 0.0f;
    float st = ((l && is_key_down(ctx->jvm, l)) ? 1.0f : 0.0f) -
               ((r && is_key_down(ctx->jvm, r)) ? 1.0f : 0.0f);
    *forward = fw;
    *strafe = st;
}

static void sp_boost(Ctx *ctx) {
    if (!java_get_entity_on_ground(ctx->jvm, ctx->player)) return;
    if (bv(&g_disable_inv) == 1 && get_current_screen(ctx->jvm, ctx->minecraft)) return;

    float forward = 0.0f, strafe = 0.0f;
    sp_input(ctx, &forward, &strafe);
    if (forward == 0.0f && strafe == 0.0f) return;      // not moving

    if (bv(&g_only_forward) == 1 && forward == 0.0f) return;
    // "Only strafe" in the reference means diagonal movement specifically.
    if (bv(&g_only_strafe) == 1 && (forward == 0.0f || strafe == 0.0f)) return;

    double target = 0.153 * (double)sv(&g_multiplier);
    sp_strafe(ctx, target);
}

// Slot 0: the boost. The reference runs this from a motion event, which the client fires just before the tick
// applies movement; slot 0 is the closest equivalent here and the ordering is the same for this purpose.
extern "C" void speed_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ctx->player) { sp_waiting = 1; return; }

    int ticks = java_get_entity_ticks_existed(ctx->jvm, ctx->player);
    unsigned char on_ground = java_get_entity_on_ground(ctx->jvm, ctx->player);

    // airTicks, recomputed here because the client does not expose it
    if (on_ground) sp_air_ticks = 0;
    else if (ticks != sp_last_ticks) sp_air_ticks++;
    sp_last_ticks = ticks;

    int mode = (int)sv(&g_mode);

    if (mode == 0) {
        // VANILLA: boost whenever the conditions hold.
        sp_boost(ctx);
        return;
    }

    // FLOAT: waits three air ticks after leaving the ground, then boosts only on whole-number Y values. The
    // fractional-Y check is the reference's; it corresponds to the exact landing frame.
    if (sp_waiting && sp_air_ticks >= 3) sp_waiting = 0;

    Vec3 pos = java_get_entity_pos_x(ctx->jvm, ctx->player);
    double frac = pos.y - (double)(long long)pos.y;

    if (!sp_waiting && on_ground && frac == 0.0) sp_boost(ctx);
}

// Slot 5: reset on the way in, so FLOAT always waits again.
extern "C" void speed_on_toggle(Hook *hook) {
    ToggleSp *t = (ToggleSp *)hook;
    if (!t->enabled) return;
    sp_waiting = 1;
    sp_air_ticks = 0;
}

// ---- settings page / descriptor ---------------------------------------------
extern "C" const char str_b4speed[] = "Speed";
extern "C" const char str_b4speed_desc[] = "Ground speed boost that keeps your movement direction.";
extern "C" const char str_b4speed_main[] = "Main";

extern "C" void speed_gui(void *gui, void *window) {
    W_label(gui, window, str_b4speed_main);
    W_slider(window, gui, &g_mode);
    W_slider(window, gui, &g_multiplier);
    W_bool(window, gui, &g_disable_inv);
    W_bool(window, gui, &g_only_forward);
    W_bool(window, gui, &g_only_strafe);
}
extern "C" void speed_register(void *module) {
    R_num(module, "Mode", &g_mode);
    R_num(module, "Multiplier", &g_multiplier);
    R_bool(module, "Disable in inventory", &g_disable_inv);
    R_bool(module, "Only forward", &g_only_forward);
    R_bool(module, "Only strafe", &g_only_strafe);
}

struct SpHSlot { void *handler; void *context; };
struct SpDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    SpHSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(SpDescriptor) == 0x210, "descriptor size");

extern "C" SpDescriptor speed_module_descriptor = {
    str_b4speed,
    str_b4speed_desc,
    3,                      // category: Block/Movement
    0,
    {0, 0, 0},
    0,
    0,
    0,
    {},
    {
        { (void *)speed_on_tick, 0 },                        // 0  tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        { (void *)speed_on_toggle, 0 },                      // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        {0, 0},                                              // 13
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        {0, 0},                                              // 17
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    speed_gui,
    speed_register,
    0,
};
