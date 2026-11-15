// TPAura 鈥?ported from RavenB4's TPAuraModule (xyz.daikirai.module.combat.TPAuraModule).
//
// The reference, per tick: pick the nearest player inside Range, teleport the local player onto them with a
// small random offset, then call PlayerUtils.attack() on them. Two substitutions were forced by this build:
//
//   teleport -> writes Entity.posX/posY/posZ through the existing field IDs (java_set_entity_pos_x). There is
//               no Entity.setPosition method ID here, and adding one means editing the rdata string table that
//               jni_ids.cpp indexes by hand. Writing the fields skips setPosition's bounding-box update, so the
//               hitbox stays where the server believes you are 鈥?which is the whole point of the module: swing
//               from the new spot before the next move packet gets you pulled back.
//
//               This is client-side only, exactly like the reference. On a server that validates movement
//               (Hypixel does) you will be rubber-banded; the attack still goes out in that same tick.
//
//   attack   -> silent rotation toward the target + swing, the same path Kill Aura uses. The reference calls
//               PlayerControllerMP.attackEntity, whose ID is not in this build either.
//
// Left out on purpose (the reference has them, this build has no equivalent):
//   AntiBotModule.ignored() / BotCheck.isBot() 鈥?no bot-detection table exists here, so NPCs and bots will be
//   targeted. Run it with Range small, or wait until the ignore list is ported.
//
// Callbacks:
//   slot 0  tick    the loop above
#include "slinky/engine.h"
extern "C" void slinky_logf(const char *fmt, ...);

struct Vec3 { double x, y, z; };
struct Rot { float yaw, pitch; };

FN(void *, java_get_world_player_entities, (void *jvm, void *world));
FN(int, java_call_list_size, (void *jvm, void *list));
FN(void *, java_call_list_get, (void *jvm, void *list, int i));
FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(int, java_set_entity_pos_x, (void *jvm, void *entity, Vec3 value));
FN(unsigned char, java_get_entity_is_dead, (void *jvm, void *entity));
FN(double, vec3_distance, (Vec3 a, Vec3 b));
FN(void, apply_silent_rotation, (Rot rotation));
FN(void *, java_get_minecraft_player_controller, (void *jvm, void *minecraft));
FN(int, java_call_player_controller_mp_attack_entity, (void *jvm, void *pc, void *player, void *target));
FN(void, java_jni_call_nonvirtual_void_method_entity_living_base_swing_item, (void *jvm, void *entity));
FN(void *, item_held_hotbar_slot, (void *jvm, void *ent));
FN(unsigned char, item_sword, (void *jvm, void *item));
FN(double, libc_atan2, (double y, double x));
FN(float, libc_sqrtf, (float x));

// ---- settings ---------------------------------------------------------------
struct TpSlider { char pad[0x10]; const char *label; const char *format;
                  float step, scale, min, max, value, saved; };
struct TpBool   { char pad[0x10]; const char *label; unsigned char value, saved; char pad_1a[6]; };

static TpSlider g_range = { {}, "Range", "%.1f", 0.5f, 1.0f, 1.0f, 30.0f, 8.0f, 8.0f };
static TpBool g_weapon_only = { {}, "Weapon only", 1, 1, {} };
static TpBool g_swing = { {}, "Swing", 1, 1, {} };

// ---- helpers ----------------------------------------------------------------
// Yaw/pitch from the local player to `t`, in the client's convention (yaw 0 = +Z, growing clockwise).
static Rot tp_look_at(Vec3 me, Vec3 t) {
    double dx = t.x - me.x;
    double dy = t.y - me.y;
    double dz = t.z - me.z;
    double horiz = libc_sqrtf((float)(dx * dx + dz * dz));
    Rot r;
    r.yaw = (float)(libc_atan2(dz, dx) * 57.29577951308232) - 90.0f;
    r.pitch = (float)(-(libc_atan2(dy, horiz) * 57.29577951308232));
    return r;
}

// Cheap pseudo-random offset in [-0.9, 0.9], so repeated teleports do not land in the same pixel.
static double tp_jitter(unsigned *seed) {
    *seed = (*seed) * 1103515245u + 12345u;
    return ((double)((*seed >> 16) & 31u) - 15.5) / 17.0;
}

struct ToggleTp { char pad[8]; Ctx *ctx; void *mod; unsigned char enabled; };

extern "C" void tpaura_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ctx->player) return;

    if (g_weapon_only.value == 1) {
        void *item = item_held_hotbar_slot(ctx->jvm, ctx->player);
        if (!item || !item_sword(ctx->jvm, item)) return;
    }

    void *world = ctx->world;
    if (!world) return;
    void *list = java_get_world_player_entities(ctx->jvm, world);
    if (!list) return;
    int n = java_call_list_size(ctx->jvm, list);
    if (n > 256) n = 256;

    double range = (double)g_range.value;
    Vec3 me = java_get_entity_pos_x(ctx->jvm, ctx->player);

    for (int i = 0; i < n; i++) {
        void *p = java_call_list_get(ctx->jvm, list, i);
        if (!p || p == ctx->player) continue;
        if (java_get_entity_is_dead(ctx->jvm, p)) continue;

        Vec3 t = java_get_entity_pos_x(ctx->jvm, p);
        if (vec3_distance(me, t) > range) continue;

        // Teleport next to the target (the reference adds a random offset around them), then swing from there
        // with the rotation already pointing at them - the same tick, before the server hears about the move.
        static unsigned seed = 0x9e3779b9u;
        Vec3 spot;
        spot.x = t.x + tp_jitter(&seed);
        spot.y = t.y + 0.2;
        spot.z = t.z + tp_jitter(&seed);
        if (!java_set_entity_pos_x(ctx->jvm, ctx->player, spot)) {
            slinky_logf("tpaura: position write unavailable, stopping");
            return;
        }

        apply_silent_rotation(tp_look_at(spot, t));
        if (g_swing.value == 1) {
            // The reference calls PlayerUtils.attack(p, false) = playerController.attackEntity(player, p),
            // which lands the hit on the named entity. The accessor returns 0 when that method ID did not
            // resolve, and then the swing (what Kill Aura does) is used instead of calling through NULL.
            void *pc = java_get_minecraft_player_controller(ctx->jvm, ctx->minecraft);
            int hit = pc ? java_call_player_controller_mp_attack_entity(ctx->jvm, pc, ctx->player, p) : 0;
            if (!hit) {
                static unsigned char warned;
                if (!warned) {
                    warned = 1;
                    slinky_logf("tpaura: attackEntity unavailable, falling back to swing");
                }
                java_jni_call_nonvirtual_void_method_entity_living_base_swing_item(ctx->jvm, ctx->player);
            }
        }
        break;                              // one target per tick, like the reference
    }
}

// ---- settings page / descriptor ---------------------------------------------
extern "C" const char str_b4tpaura[] = "TPAura";
extern "C" const char str_b4tpaura_desc[] = "Teleports onto the nearest player and swings.";
extern "C" const char str_b4tpaura_main[] = "Main";

extern "C" void tpaura_gui(void *gui, void *window) {
    W_label(gui, window, str_b4tpaura_main);
    W_slider(window, gui, &g_range);
    W_bool(window, gui, &g_weapon_only);
    W_bool(window, gui, &g_swing);
}
extern "C" void tpaura_register(void *module) {
    R_num(module, "Range", &g_range);
    R_bool(module, "Weapon only", &g_weapon_only);
    R_bool(module, "Swing", &g_swing);
}

struct TpHSlot { void *handler; void *context; };
struct TpDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    TpHSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(TpDescriptor) == 0x210, "descriptor size");

extern "C" TpDescriptor tpaura_module_descriptor = {
    str_b4tpaura,
    str_b4tpaura_desc,
    0,                      // category: Combat
    0,
    {0, 0, 0},
    0,
    0,
    0,
    {},
    {
        { (void *)tpaura_on_tick, 0 },                       // 0  tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        {0, 0},                                              // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        {0, 0},                                              // 13
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        {0, 0},                                              // 17
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    tpaura_gui,
    tpaura_register,
    0,
};



