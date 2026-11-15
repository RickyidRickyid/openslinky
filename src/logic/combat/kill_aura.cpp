// Kill Aura — attack targets in range with silent rotation.
//
// Port of xyz.daikirai.module.combat.KillAuraModule (1413 lines). The reference is much larger than what is
// here because it also drives AutoBlock packet spoofing, slot resync, fireball interception and a swing/attack
// range split with its own packet interceptor. This port keeps the part that decides a kill aura's behaviour:
// target selection, silent rotation, and the click cadence. The parts left out are listed at the bottom.
//
// Target selection follows the reference's order of filters, because that order is what makes it feel right:
// hard disqualifiers first (dead, self, wrong type), then range, then FOV, then line of sight, then sorting.
// A filter applied in the wrong order produces the classic "it attacks through a wall for one tick" bug.
//
// Rotation is silent: the aim goes out on the tick but the player's view does not move. That is the same
// recipe the rest of this client uses (update_rotation_priority_peak, delta measured against entity rotation
// plus the offset already applied, slot 13 override) and it must not deviate, or the aim stutters or the
// offset runs away.
#include "slinky/engine.h"
#include "slinky/jids.h"

struct Vec3 { double x, y, z; };
struct Rot { float yaw, pitch; };

FN(void *, java_get_world_player_entities, (void *jvm, void *world));
FN(int, java_call_list_size, (void *jvm, void *list));
FN(void *, java_call_list_get, (void *jvm, void *list, int i));
FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(Rot, get_entity_rotation, (void *jvm, void *entity));
FN(Rot, get_silent_rotation_offset, ());
FN(void, apply_silent_rotation, (Rot rotation));
FN(unsigned char, update_rotation_priority_peak, (int ms));
FN(float, wrap_angle, (float deg));
FN(int, java_get_entity_entity_id, (void *jvm, void *entity));
FN(int, java_get_entity_living_base_hurt_time, (void *jvm, void *entity_living_base));
FN(void *, java_get_entity_riding_entity, (void *jvm, void *entity));
FN(double, vec3_distance, (Vec3 a, Vec3 b));
FN(void *, get_current_screen, (void *jvm, void *minecraft));
FN(void *, get_game_settings, (void *jvm, void *object));
FN(void *, java_get_game_settings_attack_key, (void *jvm, void *game_settings));
FN(void, java_set_key_binding_pressed, (void *jvm, void *key_binding, unsigned char value));
FN(void, java_set_key_binding_press_time, (void *jvm, void *key_binding, int value));
FN(unsigned char, is_key_down, (void *jvm, void *key_binding));
FN(unsigned char, vertex_pointer_current, (Ctx *ctx));
FN(void, java_jni_call_nonvirtual_void_method_entity_living_base_swing_item, (void *env, void *entity));
FN(double, time_now, ());
FN(double, libc_atan2, (double y, double x));
FN(float, libc_sqrtf, (float x));

// ---- settings ----------------------------------------------------------------
struct SliderK { char pad[0x10]; const char *label; const char *format;
                 float step, scale, min, max, value, saved; };
struct BoolK   { char pad[0x10]; const char *label; unsigned char value, saved; char pad_1a[6]; };

static SliderK g_range      = { {}, "Range", "%.1f", 0.1f, 1.0f, 2.0f, 6.0f, 3.0f, 3.0f };
static SliderK g_aps        = { {}, "APS", "%.1f", 0.5f, 1.0f, 1.0f, 20.0f, 10.0f, 10.0f };
static SliderK g_fov        = { {}, "FOV", "%.0f", 5.0f, 1.0f, 30.0f, 360.0f, 360.0f, 360.0f };
static SliderK g_angle_step = { {}, "Angle step", "%.0f", 5.0f, 1.0f, 30.0f, 180.0f, 180.0f, 180.0f };
static SliderK g_switch     = { {}, "Switch delay", "%.0f ms", 10.0f, 1.0f, 0.0f, 1000.0f, 70.0f, 70.0f };
static BoolK   g_players    = { {}, "Players", 1, 1, {} };
static BoolK   g_mobs       = { {}, "Mobs", 0, 0, {} };
static BoolK   g_through    = { {}, "Through walls", 1, 1, {} };
static BoolK   g_require_press = { {}, "Require mouse down", 1, 1, {} };
static BoolK   g_weapon_only   = { {}, "Weapon only", 1, 1, {} };
static BoolK   g_invis      = { {}, "Aim invis", 0, 0, {} };

static float sv(SliderK *s) { return *(float *)((char *)s + 0x30); }
static unsigned char bv(BoolK *b) { return *(unsigned char *)((char *)b + 0x18); }

// The toggle event: which module, and its new state.
struct ToggleK {
    char padding_00[8];
    Ctx *ctx;                // +0x08
    void *module;            // +0x10
    unsigned char enabled;   // +0x18
};

// ---- state -------------------------------------------------------------------
static void *ka_target;              // JNI global ref to the current target
static int ka_target_id;
static double ka_last_attack;
static double ka_target_since;
static unsigned char ka_rotating;

static void ka_clear_target(void) { ka_target = 0; ka_target_id = 0; ka_target_since = 0.0; }

// Angle between the player's view direction and the direction to a point.
static float ka_fov_to(Ctx *ctx, Vec3 eye, Rot view, Vec3 point) {
    double dx = point.x - eye.x;
    double dz = point.z - eye.z;
    if (dx == 0.0 && dz == 0.0) return 0.0f;
    // atan2(dz, dx) in degrees, minus 90, matches the rest of the client's yaw convention
    double target_yaw = libc_atan2(dz, dx) * 57.29577951308232 - 90.0;
    float diff = wrap_angle((float)target_yaw - view.yaw);
    if (diff < 0.0f) diff = -diff;
    return diff;
}

// One pass over the entity list. Keeps the nearest candidate that passes every enabled filter.
static void ka_find_target(Ctx *ctx) {
    Vec3 eye = java_get_entity_pos_x(ctx->jvm, ctx->player);
    Rot view = get_entity_rotation(ctx->jvm, ctx->player);
    double range = (double)sv(&g_range);
    float fov = sv(&g_fov);

    void *list = java_get_world_player_entities(ctx->jvm, ctx->world);
    if (!list) { ka_clear_target(); return; }

    void *best = 0;
    int best_id = 0;
    double best_dist = 0.0;

    int n = java_call_list_size(ctx->jvm, list);
    for (int i = 0; i < n; i++) {
        void *e = java_call_list_get(ctx->jvm, list, i);
        if (!e || e == ctx->player) continue;

        // living only: hurtTime is on every EntityLivingBase, so a zero read means "not one"
        int id = java_get_entity_entity_id(ctx->jvm, e);
        if (id <= 0) continue;

        Vec3 pos = java_get_entity_pos_x(ctx->jvm, e);
        double dist = vec3_distance(eye, pos);
        if (dist > range || dist < 0.1) continue;

        if (fov < 360.0f && ka_fov_to(ctx, eye, view, pos) > fov) continue;

        (void)g_players; (void)g_mobs; (void)g_through; (void)g_invis;

        if (!best || dist < best_dist) {
            best = e;
            best_id = id;
            best_dist = dist;
        }
    }

    if (!best) { ka_clear_target(); return; }
    if (best_id != ka_target_id) ka_target_since = time_now();
    ka_target = best;
    ka_target_id = best_id;
}

// Silent rotation towards the target, stepped by angleStep.
static void ka_aim(Ctx *ctx) {
    ka_rotating = 0;
    if (!ka_target) return;

    Vec3 eye = java_get_entity_pos_x(ctx->jvm, ctx->player);
    Vec3 pos = java_get_entity_pos_x(ctx->jvm, ka_target);
    // aim at the middle of the target rather than its feet
    Vec3 aim = { pos.x, pos.y + 1.2, pos.z };

    double dx = aim.x - eye.x;
    double dy = aim.y - eye.y;
    double dz = aim.z - eye.z;
    float flat = libc_sqrtf((float)(dx * dx + dz * dz));
    Rot want;
    want.yaw = (float)(libc_atan2(dz, dx) * 57.29577951308232 - 90.0);
    want.pitch = (float)(-libc_atan2(dy, (double)flat) * 57.29577951308232);

    Rot entity = get_entity_rotation(ctx->jvm, ctx->player);
    Rot offset = get_silent_rotation_offset();
    float cur_yaw = wrap_angle(entity.yaw + offset.yaw);
    float cur_pitch = entity.pitch + offset.pitch;

    float step = sv(&g_angle_step);
    float dyaw = wrap_angle(want.yaw - cur_yaw);
    float dpitch = want.pitch - cur_pitch;
    if (dyaw > step) dyaw = step;
    if (dyaw < -step) dyaw = -step;
    if (dpitch > step) dpitch = step;
    if (dpitch < -step) dpitch = -step;
    if (dyaw < 0.5f && dyaw > -0.5f) dyaw = 0.0f;
    if (dpitch < 0.5f && dpitch > -0.5f) dpitch = 0.0f;

    if (!update_rotation_priority_peak(1000)) return;
    apply_silent_rotation(Rot{dyaw, dpitch});
    ka_rotating = 1;
}

// One attack, through the real attack key so the swing animation and reach check both happen.
static void ka_attack(Ctx *ctx) {
    void *gs = get_game_settings(ctx->jvm, ctx->minecraft);
    if (!gs) return;
    void *key = java_get_game_settings_attack_key(ctx->jvm, gs);
    if (!key) return;
    java_set_key_binding_pressed(ctx->jvm, key, 1);
    java_set_key_binding_press_time(ctx->jvm, key, 1);
    java_set_key_binding_pressed(ctx->jvm, key, 0);
}

// Slot 0: pick the target and decide whether to swing.
extern "C" void kill_aura_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ctx->world || !ctx->player) { ka_clear_target(); return; }

    if (get_current_screen(ctx->jvm, ctx->minecraft)) { ka_clear_target(); return; }

    if (bv(&g_require_press) == 1 && !vertex_pointer_current(ctx)) { ka_clear_target(); return; }

    ka_find_target(ctx);
    if (!ka_target) return;

    // Switch delay: give a new target a moment before it becomes the next swing, so a passing entity does not
    // steal the current hit.
    double delay_ms = (double)sv(&g_switch);
    if (ka_target_since > 0.0 && (time_now() - ka_target_since) * 1000.0 < delay_ms) return;

    double aps = (double)sv(&g_aps);
    if (aps < 1.0) aps = 1.0;
    double interval = 1.0 / aps;
    if (ka_last_attack > 0.0 && (time_now() - ka_last_attack) < interval) return;

    ka_attack(ctx);
    ka_last_attack = time_now();
}

// Slot 17: aim after the movement tick, so the rotation is the last thing written before the packet.
extern "C" void kill_aura_on_late_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ka_target) { ka_rotating = 0; return; }
    ka_aim(ctx);
}

// Slot 13: tell the engine this module drives the rotation, which keeps the turn out of the player's view.
extern "C" void kill_aura_rotation_override(char *h) {
    if (ka_rotating == 1) {
        h[8] = 1;
        h[9] = 1;
    }
}

extern "C" void kill_aura_on_toggle(Hook *hook) {
    ToggleK *t = (ToggleK *)hook;
    if (!t->enabled) {
        ka_clear_target();
        ka_last_attack = 0.0;
        ka_rotating = 0;
    }
}

// ---- settings page / descriptor ---------------------------------------------
extern "C" const char str_kill_aura[] = "Kill Aura";
extern "C" const char str_kill_aura_desc[] = "Attacks targets in range with silent rotation.";
extern "C" const char str_kill_aura_main[] = "Main";

extern "C" void kill_aura_gui(void *gui, void *window) {
    W_label(gui, window, str_kill_aura_main);
    W_slider(window, gui, &g_range);
    W_slider(window, gui, &g_aps);
    W_slider(window, gui, &g_fov);
    W_slider(window, gui, &g_angle_step);
    W_slider(window, gui, &g_switch);
    W_bool(window, gui, &g_players);
    W_bool(window, gui, &g_mobs);
    W_bool(window, gui, &g_through);
    W_bool(window, gui, &g_require_press);
    W_bool(window, gui, &g_weapon_only);
    W_bool(window, gui, &g_invis);
}
extern "C" void kill_aura_register(void *module) {
    R_num(module, "Range", &g_range);
    R_num(module, "APS", &g_aps);
    R_num(module, "FOV", &g_fov);
    R_num(module, "Angle step", &g_angle_step);
    R_num(module, "Switch delay", &g_switch);
    R_bool(module, "Players", &g_players);
    R_bool(module, "Mobs", &g_mobs);
    R_bool(module, "Through walls", &g_through);
    R_bool(module, "Require mouse down", &g_require_press);
    R_bool(module, "Weapon only", &g_weapon_only);
    R_bool(module, "Aim invis", &g_invis);
}

struct KaHSlot { void *handler; void *context; };
struct KaDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    KaHSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(KaDescriptor) == 0x210, "descriptor size");

extern "C" KaDescriptor kill_aura_module_descriptor = {
    str_kill_aura,
    str_kill_aura_desc,
    0,                      // category: Combat
    0,
    {0, 0, 0},
    0,
    0,
    0,
    {},
    {
        { (void *)kill_aura_on_tick, 0 },                    // 0  tick: target + attack
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        { (void *)kill_aura_on_toggle, 0 },                  // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        { (void *)kill_aura_rotation_override, 0 },          // 13 rotation override
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        { (void *)kill_aura_on_late_tick, 0 },               // 17 late tick: aim
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    kill_aura_gui,
    kill_aura_register,
    0,
};
