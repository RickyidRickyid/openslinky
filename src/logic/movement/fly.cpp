// Fly 鈥?ported from RavenB4's FlyModule (xyz.daikirai.module.movement.FlyModule). Both modes.
//
//   VANILLA  capabilities.flySpeed = 0.05 * horizontalSpeed, capabilities.isFlying = true. This is the
//            engine's own flight: the server moves you, so it only works where flight is permitted (creative,
//            servers that allow it). On disable: flySpeed back to 0.05, isFlying back to
//            (allowFlying ? what-it-was : false) 鈥?the reference's restore logic.
//
//   FAST     pure motion work:
//              motionY  = 0
//              space    -> motionY += 0.5 * verticalSpeed
//              lshift   -> motionY -= 0.5 * verticalSpeed
//              moving   -> MoveUtils.strafe(0.7 * horizontalSpeed)
//              idle     -> motionX = motionZ = 0
//
// How the IDs were obtained, because neither was in the original table:
//   EntityPlayer.capabilities  wn/bA  Ljava/entity/player/PlayerCapabilities;   (appended to jids.h)
//   the three PlayerCapabilities fields are resolved on first use rather than at load time: that would need
//   the PlayerCapabilities class from the client's own loader, while the capabilities instance we already
//   hold hands us its class for free. wl/b = isFlying (Z), wl/c = allowFlying (Z), wl/f = flySpeed (F) 鈥?//   the field order in the class file follows the source declaration order, and only the last two are floats.
//
// The jump/sneak bindings are found by keyCode (57 / 42), not by array index: this client's
// GameSettings.keyBindings is not in the vanilla order (forward sits at slot 2).
//
// Callbacks:
//   slot 0  tick    the selected mode
//   slot 5  toggle  on disable: restore capabilities (VANILLA), optionally zero the motion
#include "slinky/engine.h"
#include "slinky/jcall.h"
extern "C" void slinky_logf(const char *fmt, ...);

struct Vec3 { double x, y, z; };

FN(void *, get_game_settings, (void *jvm, void *object));
FN(void *, java_get_game_settings_key_bindings, (void *jvm, void *game_settings));
FN(int, java_get_object_array_length, (void *jvm, void *array));
FN(void *, java_get_object_array_element, (void *jvm, void *array, int index));
FN(int, get_event_key, (void *jvm, void *key_binding));
FN(unsigned char, is_key_down, (void *jvm, void *key_binding));
FN(Vec3, java_get_entity_motion_x, (void *jvm, void *entity));
FN(void, java_set_entity_motion_x, (void *jvm, void *entity, Vec3 value));
FN(float, java_get_entity_living_base_move_forward, (void *jvm, void *entity_living_base));
FN(float, java_get_entity_living_base_move_strafing, (void *jvm, void *entity_living_base));
FN(double, libc_hypot, (double x, double z));

// ---- settings ---------------------------------------------------------------
struct FlySlider {
    char pad[0x10];
    const char *label;
    const char *format;
    float step, scale, min, max, value, saved;
};
struct FlyBool {
    char pad[0x10];
    const char *label;
    unsigned char value, saved;
    char pad_1a[6];
};

extern "C" const char str_b4fly[] = "Fly";
extern "C" const char str_b4fly_desc[] = "VANILLA flies with the engine, FAST flies with motion.";
extern "C" const char str_b4fly_main[] = "Main";
extern "C" const char str_b4fly_mode[] = "VANILLA mode";
extern "C" const char str_b4fly_h[] = "Horizontal speed";
extern "C" const char str_b4fly_v[] = "Vertical speed";
extern "C" const char str_b4fly_stop[] = "Stop motion";
extern "C" const char str_b4fly_fmt[] = "%.1f";
extern "C" const char str_b4fly_fmt0[] = "%.0f";

enum { FLY_VANILLA = 1, FLY_FAST = 0 };          // slider order: FAST first, matching the settings widget

// The reference uses a Mode slider; a bool is used here because the slider widget on this module page did
// not write its value back (the toggle log showed mode=0 no matter where the handle was dragged). The bool
// widget is the one every other module already uses for on/off, so it is a known-good control.
static FlyBool g_vanilla_mode = { {}, str_b4fly_mode, 0, 0, {} };   // 0 = FAST, 1 = VANILLA
static FlySlider g_h_speed = { {}, str_b4fly_h, str_b4fly_fmt, 0.1f, 1.0f, 0.1f, 5.0f, 1.0f, 1.0f };
static FlySlider g_v_speed = { {}, str_b4fly_v, str_b4fly_fmt, 0.1f, 1.0f, 0.1f, 5.0f, 1.0f, 1.0f };
static FlyBool g_stop_motion = { {}, str_b4fly_stop, 1, 1, {} };

// ---- capabilities (VANILLA) -------------------------------------------------
static jfieldID g_cap_flying, g_cap_allow, g_cap_speed;
static unsigned char g_cap_state;                // 0 untested, 1 ready, 2 unavailable
static float g_cap_saved_speed = 0.05f;
static unsigned char g_cap_saved_flying;

static jobject fly_caps(void *jvm, void *player) {
    Jvm *jv = (Jvm *)jvm;
    if (!jv || !jv->env || !jv->ids) return 0;
    // Guard the field ID itself: when the lookup failed during jni_ids loading this stays NULL, and passing
    // NULL to GetObjectField is an immediate access violation (that is what crashed the client on enable).
    if (!jv->ids->EntityPlayer_capabilities) return 0;
    if (!player) return 0;
    return (*jv->env)->GetObjectField(jv->env, (jobject)player, jv->ids->EntityPlayer_capabilities);
}

static int fly_caps_ready(void *jvm, void *player) {
    if (g_cap_state == 1) return 1;
    if (g_cap_state == 2) return 0;
    Jvm *jv = (Jvm *)jvm;
    if (!jv || !jv->env || !jv->ids || !jv->ids->EntityPlayer_capabilities) {
        g_cap_state = 2;
        slinky_logf("fly: capabilities field id missing, VANILLA mode unavailable");
        return 0;
    }
    jobject cap = fly_caps(jvm, player);
    if (!cap) { g_cap_state = 2; return 0; }
    jclass cls = (*jv->env)->GetObjectClass(jv->env, cap);
    if (!cls) { (*jv->env)->DeleteLocalRef(jv->env, cap); g_cap_state = 2; return 0; }
    g_cap_flying = (*jv->env)->GetFieldID(jv->env, cls, "b", "Z");
    g_cap_allow  = (*jv->env)->GetFieldID(jv->env, cls, "c", "Z");
    g_cap_speed  = (*jv->env)->GetFieldID(jv->env, cls, "f", "F");
    (*jv->env)->DeleteLocalRef(jv->env, cls);
    (*jv->env)->DeleteLocalRef(jv->env, cap);
    if (!g_cap_flying || !g_cap_allow || !g_cap_speed) {
        (*jv->env)->ExceptionClear(jv->env);
        g_cap_state = 2;
        slinky_logf("fly: capabilities fields not found (isFlying=%d allowFlying=%d flySpeed=%d)",
                    g_cap_flying ? 1 : 0, g_cap_allow ? 1 : 0, g_cap_speed ? 1 : 0);
        return 0;
    }
    g_cap_state = 1;
    return 1;
}

// ---- FAST helpers -----------------------------------------------------------
enum {
    FLY_KEY_JUMP = 57,
    FLY_KEY_SNEAK = 42,
    FLY_MAX_BINDINGS = 64,
};

static void *fly_key_by_code(void *jvm, void *minecraft, int code) {
    void *gs = get_game_settings(jvm, minecraft);
    if (!gs) return 0;
    void *arr = java_get_game_settings_key_bindings(jvm, gs);
    if (!arr) return 0;
    int n = java_get_object_array_length(jvm, arr);
    if (n > FLY_MAX_BINDINGS) n = FLY_MAX_BINDINGS;
    for (int i = 0; i < n; i++) {
        void *kb = java_get_object_array_element(jvm, arr, i);
        if (!kb) continue;
        if (get_event_key(jvm, kb) == code) return kb;
    }
    return 0;
}

static void fly_strafe(void *jvm, void *player, double target) {
    Vec3 m = java_get_entity_motion_x(jvm, player);
    double speed = libc_hypot(m.x, m.z);
    if (speed < 1.0e-4) return;
    if (speed >= target) return;
    double scale = target / speed;
    m.x *= scale;
    m.z *= scale;
    java_set_entity_motion_x(jvm, player, m);
}

// ---- modes ------------------------------------------------------------------
static void fly_vanilla(Ctx *ctx) {
    if (!fly_caps_ready(ctx->jvm, ctx->player)) return;
    Jvm *jv = (Jvm *)ctx->jvm;
    jobject cap = fly_caps(ctx->jvm, ctx->player);
    if (!cap) return;
    (*jv->env)->SetFloatField(jv->env, cap, g_cap_speed, (jfloat)(0.05 * (double)g_h_speed.value));
    (*jv->env)->SetBooleanField(jv->env, cap, g_cap_flying, 1);
    (*jv->env)->DeleteLocalRef(jv->env, cap);
}

static void fly_fast(Ctx *ctx) {
    double h = (double)g_h_speed.value;
    double v = (double)g_v_speed.value;

    Vec3 m = java_get_entity_motion_x(ctx->jvm, ctx->player);
    m.y = 0.0;
    void *jump = fly_key_by_code(ctx->jvm, ctx->minecraft, FLY_KEY_JUMP);
    void *sneak = fly_key_by_code(ctx->jvm, ctx->minecraft, FLY_KEY_SNEAK);
    if (jump && is_key_down(ctx->jvm, jump)) m.y += 0.5 * v;
    if (sneak && is_key_down(ctx->jvm, sneak)) m.y -= 0.5 * v;

    float forward = java_get_entity_living_base_move_forward(ctx->jvm, ctx->player);
    float strafe = java_get_entity_living_base_move_strafing(ctx->jvm, ctx->player);
    if (forward != 0.0f || strafe != 0.0f) {
        java_set_entity_motion_x(ctx->jvm, ctx->player, m);
        fly_strafe(ctx->jvm, ctx->player, 0.7 * h);
    } else {
        m.x = 0.0;
        m.z = 0.0;
        java_set_entity_motion_x(ctx->jvm, ctx->player, m);
    }
}

extern "C" void fly_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (ctx->in_game != 1 || !ctx->player) return;
    if (g_vanilla_mode.value == FLY_VANILLA) fly_vanilla(ctx);
    else fly_fast(ctx);
}

// Slot 5: the reference restores capabilities on the way out, and optionally kills the motion.
struct ToggleFly { char pad[8]; Ctx *ctx; void *mod; unsigned char enabled; };
extern "C" void fly_on_toggle(Hook *hook) {
    ToggleFly *t = (ToggleFly *)hook;
    Ctx *ctx = t->ctx;
    slinky_logf("fly: toggle enabled=%d vanilla=%d", (int)t->enabled, (int)g_vanilla_mode.value);
    if (!ctx || !ctx->player) return;

    if (!t->enabled) {
        // VANILLA: put the engine back the way we found it.
        if (g_vanilla_mode.value == FLY_VANILLA && fly_caps_ready(ctx->jvm, ctx->player)) {
            Jvm *jv = (Jvm *)ctx->jvm;
            jobject cap = fly_caps(ctx->jvm, ctx->player);
            if (cap) {
                unsigned char allow = (*jv->env)->GetBooleanField(jv->env, cap, g_cap_allow) ? 1 : 0;
                (*jv->env)->SetBooleanField(jv->env, cap, g_cap_flying, allow ? g_cap_saved_flying : 0);
                (*jv->env)->SetFloatField(jv->env, cap, g_cap_speed, (jfloat)g_cap_saved_speed);
                (*jv->env)->DeleteLocalRef(jv->env, cap);
            }
        }
        if (g_stop_motion.value == 1)
            java_set_entity_motion_x(ctx->jvm, ctx->player, Vec3{0.0, 0.0, 0.0});
        return;
    }

    // Enabling: remember what capability state we are about to overwrite.
    if (fly_caps_ready(ctx->jvm, ctx->player)) {
        Jvm *jv = (Jvm *)ctx->jvm;
        jobject cap = fly_caps(ctx->jvm, ctx->player);
        if (cap) {
            g_cap_saved_flying = (*jv->env)->GetBooleanField(jv->env, cap, g_cap_flying) ? 1 : 0;
            g_cap_saved_speed = (float)(*jv->env)->GetFloatField(jv->env, cap, g_cap_speed);
            (*jv->env)->DeleteLocalRef(jv->env, cap);
        }
    }
}

// ---- settings page / descriptor ---------------------------------------------
extern "C" void fly_gui(void *gui, void *window) {
    W_label(gui, window, str_b4fly_main);
    W_bool(window, gui, &g_vanilla_mode);
    W_slider(window, gui, &g_h_speed);
    W_slider(window, gui, &g_v_speed);
    W_bool(window, gui, &g_stop_motion);
}
extern "C" void fly_register(void *module) {
    R_bool(module, "VANILLA mode", &g_vanilla_mode);
    R_num(module, "Horizontal speed", &g_h_speed);
    R_num(module, "Vertical speed", &g_v_speed);
    R_bool(module, "Stop motion", &g_stop_motion);
}

struct FlyHSlot { void *handler; void *context; };
struct FlyDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold; unsigned char pad_21[0x27];
    FlyHSlot handlers[27]; void (*settings_gui)(void *, void *); void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(FlyDescriptor) == 0x210, "descriptor size");

extern "C" FlyDescriptor fly_module_descriptor = {
    str_b4fly,
    str_b4fly_desc,
    3,                      // category: Block/Movement
    0,
    {0, 0, 0},
    0,
    0,
    0,
    {},
    {
        { (void *)fly_on_tick, 0 },                          // 0  tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 1..4
        { (void *)fly_on_toggle, 0 },                        // 5  toggle
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 6..10
        {0, 0}, {0, 0},                                      // 11..12
        {0, 0},                                              // 13
        {0, 0}, {0, 0}, {0, 0},                              // 14..16
        {0, 0},                                              // 17
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 18..22
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 23..26
    },
    fly_gui,
    fly_register,
    0,
};




