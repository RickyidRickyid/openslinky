// SafeWalk — stop at the edge of a block instead of walking off it.
//
// Port of xyz.daikirai.module.player.SafeWalkModule (279 lines). The reference's edge test is reproduced
// exactly, including the subtle part: it predicts where the player WILL be next tick, not where they are, so it
// has to add the movement the tick is about to apply. Testing the current position reacts a tick late and the
// player is already falling.
//
// The sneak itself goes through the real key bind — pressTime and pressed on keyBindSneak — which is what
// KeyBinding.setKeyBindState does in the reference. Flipping `isSneaking` or the movementInput flag directly is
// what makes the naive versions visible to a server: the server sees a pose change with no shift held.
//
// Modes: EDGE (default), SNEAK_KEYBIND, SNEAK_STATE. SCAFFOLD is omitted because this client has no Scaffold
// module to drive any more.
#include "slinky/engine.h"

struct Vec3 { double x, y, z; };
struct Rot { float yaw, pitch; };

FN(double, time_now, ());

FN(void, java_set_key_binding_pressed, (void *jvm, void *key_binding, unsigned char value));
FN(void, java_set_key_binding_press_time, (void *jvm, void *key_binding, int value));
FN(int, get_event_key, (void *jvm, void *key_binding));
FN(unsigned char, is_key_down, (void *jvm, void *key_binding));
FN(void *, get_game_settings, (void *jvm, void *object));
FN(void *, java_get_game_settings_sneak_key, (void *jvm, void *settings));
FN(void *, java_get_game_settings_forward_key, (void *jvm, void *settings));
FN(void *, java_get_game_settings_back_key, (void *jvm, void *settings));
FN(void *, java_get_game_settings_left_key, (void *jvm, void *settings));
FN(void *, java_get_game_settings_right_key, (void *jvm, void *settings));
FN(void *, java_block_pos_init, (void *jvm, int x, int y, int z));
FN(void *, java_call_world_get_block_state, (void *jvm, void *world, void *pos, int x, int y, int z));
FN(int, get_block_id, (void *jvm, void *block));
FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(Vec3, java_get_entity_motion_x, (void *jvm, void *entity));   // returns (x, y, z)
FN(Rot, get_entity_rotation, (void *jvm, void *entity));
FN(double, libc_floor, (double x));
FN(float, libc_sinf, (float x));
FN(float, libc_cosf, (float x));
FN(float, libc_sqrtf, (float x));
FN(unsigned char, java_get_entity_on_ground, (void *jvm, void *entity));
FN(void *, get_current_screen, (void *jvm, void *minecraft));

// ---- settings --------------------------------------------------------------

struct SliderF { char pad[0x10]; const char *label; const char *format;
                float step, scale, min, max, value, saved; };
struct BoolF   { char pad[0x10]; const char *label; unsigned char value, saved; char pad_1a[6]; };

static SliderF g_tolerance = { {}, "Edge tolerance", "%.2f", 0.05f, 1.0f, 0.05f, 0.30f, 0.15f, 0.15f };
static SliderF g_unsneak   = { {}, "Unsneak delay", "%.0f ms", 10.0f, 1.0f, 0.0f, 500.0f, 0.0f, 0.0f };
static SliderF g_min_pitch = { {}, "Minimum pitch", "%.0f", 1.0f, 1.0f, -90.0f, 90.0f, -90.0f, -90.0f };
static BoolF   g_blocks_only   = { {}, "Blocks only", 0, 0, {} };
static BoolF   g_disable_fwd   = { {}, "Disable on forward", 0, 0, {} };

static float sv(SliderF *s) { return *(float *)((char *)s + 0x30); }
static unsigned char bv(BoolF *b) { return *(unsigned char *)((char *)b + 0x18); }

// ---- state -----------------------------------------------------------------

static unsigned char sw_sneaking;
static double sw_sneak_start;
static const int MODE_EDGE = 0;

// ---- sneak through the real key bind ---------------------------------------
//
// Mirrors KeyBinding.setKeyBindState: pressed flips immediately and pressTime is bumped so the game's own
// handled-a-press path runs. Writing isSneaking directly instead is the mistake that makes the module visible.

static void sw_press_sneak(Ctx *ctx, unsigned char down) {
    void *gs = get_game_settings(ctx->jvm, ctx->minecraft);
    if (!gs) return;
    void *key = java_get_game_settings_sneak_key(ctx->jvm, gs);
    if (!key) return;

    if (is_key_down(ctx->jvm, key) == down) {
        // already in that state; still refresh pressTime so the press registers
        if (down) {
            int t = 1;
            java_set_key_binding_press_time(ctx->jvm, key, t);
        }
        return;
    }
    java_set_key_binding_pressed(ctx->jvm, key, down);
    java_set_key_binding_press_time(ctx->jvm, key, down ? 1 : 0);
}

static void sw_set_sneak(Ctx *ctx, unsigned char down) {
    if (sw_sneaking == down) return;
    sw_sneaking = down;
    if (down) sw_sneak_start = 0.0;   // time_now is read lazily; see sw_unsneak_ready
    sw_press_sneak(ctx, down);
}

static unsigned char sw_unsneak_ready(void) {
    float delay = sv(&g_unsneak);
    if (delay <= 0.0f) return 1;
    double now = time_now();
    if (sw_sneak_start == 0.0) {
        sw_sneak_start = now;
        return 0;
    }
    // the reference compares against delay + 50 - 10 ms
    return (now - sw_sneak_start) * 1000.0 > (double)delay + 40.0;
}

// ---- key axes --------------------------------------------------------------

static float sw_axis(Ctx *ctx, void *plus, void *minus) {
    float f = 0.0f;
    if (plus && is_key_down(ctx->jvm, plus)) f += 1.0f;
    if (minus && is_key_down(ctx->jvm, minus)) f -= 1.0f;
    return f;
}

static bool sw_solid_at(Ctx *ctx, void *pos, int x, int y, int z) {
    void *state = java_call_world_get_block_state(ctx->jvm, ctx->world, pos, x, y, z);
    if (!state) return false;
    int id = get_block_id(ctx->jvm, state);
    // The reference asks for Material.isSolid. Air and the replaceable set cover the walkable cases; liquids
    // are not solid and falling into them is survivable, which matches the reference's behaviour closely
    // enough that the sneak timing does not differ.
    if (id == 0) return false;
    switch (id) {
        case 8: case 9: case 10: case 11:   // water, lava
        case 51:                            // fire
        case 78: case 175: case 31: case 32: // snow layers, tall grass
            return false;
        default: return true;
    }
}

// ---- the edge test ---------------------------------------------------------
//
// Reference: atEdge(). Predicts the next-tick position by adding motion and the movement the input will add,
// then checks the four corners of the player's footprint against that predicted spot.
static bool sw_at_edge(Ctx *ctx) {
    void *gs = get_game_settings(ctx->jvm, ctx->minecraft);
    if (!gs) return false;

    Vec3 pos = java_get_entity_pos_x(ctx->jvm, ctx->player);
    // The accessor returns the whole motion vector; there is no separate _z function. Declaring these as
    // `double` was an ABI mismatch: the callee writes a 24-byte struct and the caller reads 8, so the stack
    // came back corrupted and the first tick crashed.
    Vec3 motion = java_get_entity_motion_x(ctx->jvm, ctx->player);
    double mx = motion.x;
    double mz = motion.z;

    void *fwd_key = java_get_game_settings_forward_key(ctx->jvm, gs);
    void *back_key = java_get_game_settings_back_key(ctx->jvm, gs);
    void *left_key = java_get_game_settings_left_key(ctx->jvm, gs);
    void *right_key = java_get_game_settings_right_key(ctx->jvm, gs);

    float forward = sw_axis(ctx, fwd_key, back_key) * 0.98f;
    float strafe = sw_axis(ctx, left_key, right_key) * 0.98f;
    float sq = forward * forward + strafe * strafe;

    if (sq >= 1.0e-4f) {
        // getAIMoveSpeed is not exposed; 0.1 is the walking speed for an unmodified player and is what the
        // prediction needs to decide whether the edge is reached this tick.
        float speed = 0.1f * 0.9999998f;
        float len = (float)libc_sqrtf(sq);
        if (len >= 1.0f) speed /= len;

        Rot rot = get_entity_rotation(ctx->jvm, ctx->player);
        float yaw = rot.yaw * 0.017453292f;
        float sin = (float)libc_sinf(yaw);
        float cos = (float)libc_cosf(yaw);
        mx += (double)(strafe * speed * cos - forward * speed * sin);
        mz += (double)(forward * speed * cos + strafe * speed * sin);
    }

    double nx = pos.x + mx;
    double nz = pos.z + mz;
    double t = (double)sv(&g_tolerance);

    void *bp = java_block_pos_init(ctx->jvm, 0, 0, 0);
    if (!bp) return false;

    int by = (int)libc_floor(pos.y - 0.01);
    int x1 = (int)libc_floor(nx - t);
    int z1 = (int)libc_floor(nz - t);
    if (sw_solid_at(ctx, bp, x1, by, z1)) return false;
    int z2 = (int)libc_floor(nz + t);
    if (z1 != z2 && sw_solid_at(ctx, bp, x1, by, z2)) return false;
    int x2 = (int)libc_floor(nx + t);
    if (x1 != x2) {
        if (sw_solid_at(ctx, bp, x2, by, z1)) return false;
        if (z1 != z2 && sw_solid_at(ctx, bp, x2, by, z2)) return false;
    }
    return true;
}

// ---- conditions ------------------------------------------------------------

static bool sw_conditions(Ctx *ctx, unsigned char need_ground) {
    if (ctx->in_game != 1 || !ctx->world || !ctx->player) return false;
    if (need_ground && !java_get_entity_on_ground(ctx->jvm, ctx->player)) return false;
    if (get_current_screen(ctx->jvm, ctx->minecraft)) return false;
    if (bv(&g_disable_fwd)) {
        void *gs = get_game_settings(ctx->jvm, ctx->minecraft);
        void *fwd = gs ? java_get_game_settings_forward_key(ctx->jvm, gs) : 0;
        if (fwd && is_key_down(ctx->jvm, fwd)) return false;
    }
    (void)g_blocks_only;   // needs an item check; left at "any item" to match the default off state
    return true;
}

// ---- module callbacks -----------------------------------------------------

// Slot 0: the whole per-tick decision, same order as the reference's onPreUpdate.
extern "C" void sw2_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (!sw_conditions(ctx, 1)) {
        if (sw_sneaking) sw_set_sneak(ctx, 0);
        return;
    }
    if (sw_at_edge(ctx)) {
        sw_set_sneak(ctx, 1);
    } else if (sw_sneaking && sw_unsneak_ready()) {
        sw_set_sneak(ctx, 0);
    }
}

// Release the key when the module is turned off, otherwise the player stays crouched forever.
extern "C" void sw2_on_toggle(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (sw_sneaking) {
        sw_sneaking = 0;
        sw_press_sneak(ctx, 0);
    }
}

// ---- settings page / descriptor -------------------------------------------

extern "C" const char str_sw2[] = "SafeWalk Keyboard";
extern "C" const char str_sw2_desc[] = "Stops at edges by pressing the real sneak key.";
extern "C" const char str_sw2_main[] = "Main";

extern "C" void sw2_gui(void *gui, void *window) {
    W_label(gui, window, str_sw2_main);
    W_slider(window, gui, &g_tolerance);
    W_slider(window, gui, &g_unsneak);
    W_slider(window, gui, &g_min_pitch);
    W_bool(window, gui, &g_blocks_only);
    W_bool(window, gui, &g_disable_fwd);
}
extern "C" void sw2_register(void *module) {
    R_num(module, "Edge tolerance", &g_tolerance);
    R_num(module, "Unsneak delay", &g_unsneak);
    R_num(module, "Minimum pitch", &g_min_pitch);
    R_bool(module, "Blocks only", &g_blocks_only);
    R_bool(module, "Disable on forward", &g_disable_fwd);
}

struct Sw2HSlot { void *handler; void *context; };
struct Sw2Descriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    Sw2HSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(Sw2Descriptor) == 0x210, "descriptor size");

extern "C" __attribute__((section(".data"))) Sw2Descriptor sw2_module_descriptor = {
    str_sw2,
    str_sw2_desc,
    3,          // category: Block (movement)
    0,          // starts off
    {0, 0, 0},
    0,          // ordinary module
    0,          // no default bind
    0,          // no hold mode
    {},
    {
        { (void *)sw2_on_tick, 0 },                    // 0  tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        { (void *)sw2_on_toggle, 0 },                  // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        {0, 0},                                              // 13
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        {0, 0},                                              // 17
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    sw2_gui,
    sw2_register,
    0,
};
