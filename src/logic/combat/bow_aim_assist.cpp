// Bow Aim Assist — see bow_aim_assist.h for why the aiming behaves the way it does.
//
// Flow, once per tick:
//   1. Are we holding a bow and drawing it (or is a fixed draw time configured)?
//   2. Pick the nearest valid target, Driplite-style: scan the list, keep the entry that beats
//      the current best distance, and shrink the bound to it as we go so only the nearest wins.
//   3. Solve the launch pitch against the real trajectory (collision-free, so no JNI per step)
//      and take the yaw straight from the look-at rotation.
//   4. Store the chosen solution; the rotation hook and the renderer both read it.
//
// Flow, once per frame:
//   - rotation hook: move the crosshair toward the stored solution by the cubic speed curve
//   - world render: draw the collision-checked trajectory, the landing marker and the link to
//     the target, so the prediction can be compared against where the target actually is
#include "slinky/engine.h"
#include "slinky/jids.h"
#include "slinky/jcall.h"
#include "../../engine/modlib/trajectory_predict.h"
#include "bow_aim_assist.h"
#include <math.h>
extern "C" void slinky_logf(const char *fmt, ...);

struct Point { float x, y, z; };
struct Range { float lo, hi; };
// Event 1 is "frame", event 6 is "world render" - two different payloads. This used to declare the
// world-render layout (camera vector before the context) and use it for the frame callback, so
// h->ctx read past the real pointer into partial/dt: a garbage Ctx*, the in-game test failed on
// every frame, and the module could solve rotations but never apply one. Layout below is the one
// the engine dispatches, cross-checked against aim_assist.cpp's FrameEvt.
struct FrameEvt {
    char padding_00[8];
    Ctx *ctx;            // +0x08
    float partial;       // +0x10
    int padding_14;
    double dt;           // +0x18
};
struct EvHook { char padding_00[8]; Ctx *ctx; void *ev; unsigned char on; };

// ---- engine entry points ----------------------------------------------------
FN(void *, get_game_settings, (void *jvm, void *object));
FN(void *, item_held_hotbar_slot, (void *jvm, void *slot));
FN(int, java_call_static_item_get_id_from_item, (void *jvm, void *item));
FN(unsigned char, java_call_entity_player_is_using_item, (void *jvm, void *entity_player));
FN(int, java_get_entity_player_item_in_use_count, (void *jvm, void *entity_player));
FN(Rot, get_entity_rotation, (void *jvm, void *entity));
// Direct rotation write. Every open-source bow aimbot (Wurst's BowAimbotHack, LiquidBounce's
// ProjectileAimbot) sets the player's yaw/pitch outright; none of them queue an offset. This client
// exposes the same thing, and the version command below is the one that actually moves the body.
FN(void, set_entity_rotation, (void *jvm, void *entity, Rot rotation));
FN(void, set_entity_prev_rotation, (void *jvm, void *entity, Rot rotation));
FN(void *, java_get_game_settings_use_item_key, (void *jvm, void *game_settings));
FN(void, java_set_key_binding_pressed, (void *jvm, void *kb, bool pressed));
FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(Vec3, java_get_entity_prev_pos_x, (void *jvm, void *entity));
// world.playerEntities, not the whole loaded-entity list. The full list is full of Hypixel's
// cosmetic entities (pets, armour stands, particles-as-entities, holograms), and locking onto a
// pet looks exactly like "it aims at everything" - which is what it was doing.
FN(void *, java_get_world_player_entities, (void *jvm, void *world));
FN(void *, java_get_world_loaded_entity_list, (void *jvm, void *world));
FN(int, java_get_entity_entity_id, (void *jvm, void *entity));
FN(int, java_call_list_size, (void *jvm, void *list));
FN(void *, java_call_list_get, (void *jvm, void *list, int i));
FN(void *, java_get_entity_bounding_box, (void *jvm, void *entity));
FN(Box, java_get_axis_aligned_bb_min_x, (void *jvm, void *bounding_box));
FN(unsigned char, java_get_entity_is_dead, (void *jvm, void *entity));
FN(unsigned char, java_get_entity_player_sleeping, (void *jvm, void *entity));
FN(unsigned char, java_call_entity_player_is_invisible_to_player, (void *jvm, void *a, void *b));
FN(unsigned char, entity_flags_has, (void *jvm, void *entity, int flag));
FN(float, java_call_entity_living_base_get_health, (void *jvm, void *entity));
FN(unsigned char, is_silent_rotation_active, ());
FN(unsigned char, update_rotation_priority_peak, (int ms));
FN(void, apply_silent_rotation, (Rot rotation));          // turn BY this much (relative)
FN(Rot, get_silent_rotation_offset, ());
FN(Rot, rot_add, (Rot a, Rot b));
FN(Rot, rot_scale, (Rot rotation, float k));
FN(float, rot_length, (Rot rotation));
FN(float, limit_rotation_delta, (float lo, float hi, float v));

FN(Vec3, vec3_add, (Vec3 a, Vec3 b));
FN(Vec3, vec3_sub, (Vec3 a, Vec3 b));
FN(Vec3, vec3_scale, (Vec3 v, double k));
FN(Vec3, vec3_lerp, (Vec3 a, Vec3 b, double t));
FN(Vec3, vec3_make, (double x, double y, double z));
FN(Vec3, vec3_add_eye_height, (Vec3 v));
FN(Box, aabb_expand, (Box b, double grow));
FN(Vec3, aabb_clamp_point, (Box b, Vec3 p));
FN(double, vec3_distance, (Vec3 a, Vec3 b));
FN(Rot, rot_look_at, (Vec3 eye, Vec3 target));
FN(Rot, rot_sub, (Rot a, Rot b));
FN(float, wrap_angle, (float a));
FN(double, lerp_double, (double a, double b, double t));
FN(float, noise_1d, (float x));

// GL entry points are double pointers and contain commas, so they are declared directly rather
// than through FN() (which takes exactly three macro arguments).
extern "C" void (**ptr_to_pGlPushMatrix)();
extern "C" void (**ptr_to_pGlPopMatrix)();
extern "C" void (**ptr_to_pGlTranslatef)(float, float, float);
extern "C" void (**ptr_to_pGlScalef)(float, float, float);
extern "C" void (**ptr_to_pGlLineWidth)(float);
extern "C" void (**ptr_to_pGlColor3f)(float, float, float);
extern "C" void (**ptr_to_pGlEnable)(unsigned);
extern "C" void (**ptr_to_pGlEnableClientState)(unsigned);
extern "C" void (**ptr_to_pGlVertexPointer)(int, unsigned, int, const void *);
extern "C" void (**ptr_to_pGlDrawArrays)(unsigned, int, int);
FN(void, gl_array_xyz_float, ());
FN(void, gl_array_color2, ());
FN(void, disable_vertex_array_state, ());

// ---- settings ---------------------------------------------------------------
struct BowSlider {
    char pad[0x10];
    const char *label;
    const char *format;
    float step, scale, min, max, value, saved;
};
struct BowBool {
    char pad[0x10];
    const char *label;
    unsigned char value, saved;
    char pad_1a[6];
};
// NOTE: there is deliberately no range-setting struct here. The engine's range widget casts to
// RangeSetting (padding_00[0x18], then label / format / range_format, then the floats) - see
// src/engine/gui/profiles.cpp. Getting that layout one field wrong hands a float pair to sprintf
// as the format string, which is an access violation inside ucrtbase. Every numeric option is a
// plain slider instead: that layout is padding_00[0x10] + label + format + floats, and it is the
// one this file has always matched.

extern "C" const char str_bow_name[] = "BowAim";
extern "C" const char str_bow_desc[] =
    "Predicts the arrow and aims it. Only engages while drawing.";
extern "C" const char str_bow_main[] = "Aim";
extern "C" const char str_bow_require_draw[] = "Require draw";
extern "C" const char str_bow_silent[] = "Silent";
extern "C" const char str_bow_lock_vertical[] = "Lock vertical";
extern "C" const char str_bow_speed[] = "Speed";
extern "C" const char str_bow_curve[] = "Curve";
extern "C" const char str_bow_predict[] = "Predict";
extern "C" const char str_bow_random[] = "Random";
extern "C" const char str_bow_range[] = "Range";
extern "C" const char str_bow_fov[] = "FOV";
extern "C" const char str_bow_target[] = "Target";
extern "C" const char str_bow_require_los[] = "Require line of sight";
extern "C" const char str_bow_ignore_invis[] = "Ignore invisible";
extern "C" const char str_bow_physics[] = "Physics";
extern "C" const char str_bow_gravity[] = "Gravity";
extern "C" const char str_bow_drag[] = "Air drag";
extern "C" const char str_bow_ticks[] = "Max ticks";
extern "C" const char str_bow_draw_ticks[] = "Draw ticks";
extern "C" const char str_bow_follow_draw[] = "Follow draw";
extern "C" const char str_bow_power[] = "Power scale";
extern "C" const char str_bow_visual[] = "Visual";
extern "C" const char str_bow_target_mobs[] = "Target mobs";
extern "C" const char str_bow_auto_release[] = "Auto release";
extern "C" const char str_bow_max_flight[] = "Max flight";
extern "C" const char str_bow_show[] = "Show trajectory";
extern "C" const char str_bow_show_link[] = "Show target link";
extern "C" const char str_bow_width[] = "line_width";
extern "C" const char str_bow_color_hit[] = "color_hit";
extern "C" const char str_bow_color_miss[] = "color_miss";
extern "C" const char str_bow_fmt1[] = "%.1f";
extern "C" const char str_bow_fmt0[] = "%.0f";
extern "C" const char str_bow_fmt2[] = "%.2f";
extern "C" const char str_bow_fmt3[] = "%.3f";
extern "C" const char str_bow_fmt_ms[] = "%.0fms";

static BowBool g_require_draw = { {}, str_bow_require_draw, 1, 1, {} };
static BowBool g_silent = { {}, str_bow_silent, 0, 0, {} };
static BowBool g_lock_vertical = { {}, str_bow_lock_vertical, 0, 0, {} };
static BowBool g_ignore_invis = { {}, str_bow_ignore_invis, 1, 1, {} };
static BowBool g_require_los = { {}, str_bow_require_los, 0, 0, {} };
static BowBool g_follow_draw = { {}, str_bow_follow_draw, 1, 1, {} };
static BowBool g_show_traj = { {}, str_bow_show, 1, 1, {} };
// Off = players only (world.playerEntities). On = every living thing, for PvE: the loaded-entity
// list also carries items, arrows and XP orbs, so the body height below is what filters those out.
static BowBool g_target_mobs = { {}, str_bow_target_mobs, 0, 0, {} };
static BowBool g_auto_release = { {}, str_bow_auto_release, 1, 1, {} };
// Beyond this many ticks of flight the target has time to see the arrow and step aside: the arrow
// covers the last stretch almost flat, and a 0.6 block wide body only needs to move 0.4 blocks to
// make it miss. Twelve ticks is 0.6 s, which is already generous.
// 0 = no limit. Anything else caps how long the arrow may be in the air before this module refuses
// the shot (beyond that the target can see it coming and step aside).
// Default 0 = no limit. Shipping it at 12 silently refused every shot past ~36 blocks, which reads
// as "it does not aim at long range" rather than as a setting.
static BowSlider g_max_flight = { {}, str_bow_max_flight, str_bow_fmt0, 1.0f, 1.0f, 0.0f, 30.0f, 0.0f, 0.0f };

// The cubic curve: speed_scale is what the slider sets; the correction applied on any frame is
//  proportional to (remaining_angle / kCurveRefDegrees)^3, so a 30 degree error gets the full
//  scale and a 5 degree error gets about 0.5% of it. That ratio is the whole feel.
//
// Range and units copied from the client's own Aim Assist (see aim_assist.cpp line ~290: the
// slider tops out at 30 and feeds the rate, it is not a percentage). 30 here means "full speed on
// a large error", which is what the Aim Assist slider means at its top end too.
static BowSlider g_speed = { {}, str_bow_speed, str_bow_fmt0, 1.0f, 1.0f, 1.0f, 30.0f, 28.0f, 28.0f };
static BowSlider g_curve = { {}, str_bow_curve, str_bow_fmt1, 0.5f, 1.0f, 1.0f, 6.0f, 2.0f, 2.0f };
// Predict is now "how many extra milliseconds to lead by", not a percentage: that is the knob a
// player actually understands, and it is where the connection's one-way delay belongs. The arrow
// flight time is added on top of it automatically.
static BowSlider g_predict = { {}, str_bow_predict, str_bow_fmt_ms, 10.0f, 1.0f, 0.0f, 500.0f, 40.0f, 40.0f };
static BowSlider g_gravity = { {}, str_bow_gravity, str_bow_fmt3, 0.005f, 1.0f, 0.005f, 0.1f, 0.05f, 0.05f };
static BowSlider g_ticks = { {}, str_bow_ticks, str_bow_fmt0, 5.0f, 1.0f, 20.0f, 200.0f, 100.0f, 100.0f };
static BowSlider g_width = { {}, str_bow_width, str_bow_fmt1, 0.1f, 1.0f, 0.5f, 5.0f, 2.0f, 2.0f };
static BowSlider g_range = { {}, str_bow_range, str_bow_fmt0, 1.0f, 1.0f, 4.0f, 128.0f, 128.0f, 128.0f };
static BowSlider g_fov = { {}, str_bow_fov, str_bow_fmt0, 1.0f, 1.0f, 5.0f, 170.0f, 90.0f, 90.0f };

// Fixed colours: two more range widgets for hit/miss were the worst offenders of the layout bug
// above, and they are not worth a row each.
static const float kColourHit[3] = {1.0f, 0.72f, 0.16f};
static const float kColourMiss[3] = {0.55f, 0.58f, 0.62f};

// ---- state ------------------------------------------------------------------
static unsigned short g_have_solution;      // bit 0: solution valid this tick
// g_aim_pitch is solved at tick time, but the aim point is a WORLD point and both the player and
// the target keep moving between ticks - a sprinting player covers 0.3 blocks per tick. So store
// the part that only ballistics contributes (solved pitch minus the straight line to the point) and
// re-derive yaw/pitch every frame from the live eye position. Without this the crosshair is always
// aiming from where the player stood at the last tick.
static float g_pitch_offset;
static int g_draw_now;
// Peak draw reached during the CURRENT draw session. Draw only ever increases until the string is
// released, so the peak at any moment is exactly the draw the arrow will leave with (within one
// tick). Solving for a hard-coded full draw instead meant that any release before 20 ticks flew
// with less power than the solution assumed and landed short by several blocks - which is what the
// `under-drawn` lines in the log were counting.
static int g_draw_peak;      // ticks of draw for the current tick, read by the auto release in frame
// The lead computed on the tick, kept as an offset so the frame can rebuild the aim point from the
// target's INTERPOLATED position instead of reusing a world point that is up to 50 ms stale.
static Vec3 g_lead;
// Smoothed target velocity. pos - prevPos is a single tick of displacement, so a target that
// changes direction makes it jump by up to twice its speed in one tick - and the lead jumps with
// it, which is why the aim was good one shot and wild the next.
static Vec3 g_vel_smooth;
static void *g_vel_entity;
static float g_aim_yaw, g_aim_pitch;        // solved rotation
static unsigned char g_target_valid;
static Vec3 g_target_point;                 // where we want the arrow to land
static void *g_target_entity;
static float g_last_speed_applied;
static TrajectoryResult g_display;          // collision-checked, for the renderer
static unsigned short g_display_valid;
static Point g_screen_points[128];

static const int kMaxScan = 256;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ---- bow state --------------------------------------------------------------
enum { ITEM_BOW = 0x105, ITEM_PEARL = 0x170, ITEM_SNOWBALL = 0x14c, ITEM_EGG = 0x158 };

// How many ticks the bow has been drawn, and whether it can fire at all.
static int bow_draw_state(Ctx *ctx, unsigned char *holding_bow) {
    *holding_bow = 0;
    void *stack = item_held_hotbar_slot(ctx->jvm, ctx->player);
    if (!stack)
        return 0;
    int id = java_call_static_item_get_id_from_item(ctx->jvm, stack);
    if (id != ITEM_BOW)
        return 0;
    *holding_bow = 1;
    if (!java_call_entity_player_is_using_item(ctx->jvm, ctx->player))
        return 0;
    // itemInUseCount counts down from 72000; the difference is the draw time.
    int used = java_get_entity_player_item_in_use_count(ctx->jvm, ctx->player);
    int draw = 72000 - used;
    if (draw < 0)
        draw = 0;
    if (draw > 20)
        draw = 20;
    return draw;
}

// ---- target selection (Driplite aim_pickTarget / aim_updateTarget) -----------
//
// Returns 0 when the entity is unusable. The disqualifying checks are the same ones Driplite
// applies, in the same order, because the order matters: the cheap rejections come first so a
// crowded world does not cost a bounding box read per entity.
//
// my_rot is passed in rather than fetched here: the rotation is the same for every candidate,
// and this runs once per entity in the world, so fetching it inside would double the JNI cost.
// why: set to the reason the candidate was rejected (0 = accepted). The caller histograms it so one
// log line says whether a far target was dropped for distance, for FOV or for the flag test -
// "nothing happens at long range" has to be answerable without guessing.
static unsigned char target_ok(Ctx *ctx, void *e, Vec3 eye, Rot my_rot, float range, float fov,
                               double *out_dist2, Vec3 *out_point, int *why, Box *out_box) {
    *why = 1;
    if (java_get_entity_is_dead(ctx->jvm, e) || java_get_entity_player_sleeping(ctx->jvm, e))
        return 0;
    *why = 2;
    if (g_ignore_invis.value == 1 &&
        java_call_entity_player_is_invisible_to_player(ctx->jvm, e, ctx->player))
        return 0;
    *why = 3;
    if (entity_flags_has(ctx->jvm, e, 7))
        return 0;

    Vec3 pos = java_get_entity_pos_x(ctx->jvm, e);
    if (g_predict.value > 0.0f) {
        // Lead the target by its own movement, scaled by the slider.
        Vec3 prev = java_get_entity_prev_pos_x(ctx->jvm, e);
        pos = vec3_lerp(prev, pos, 1.0 + (double)(g_predict.value / 100.0f));
    }

    // Expand by hand. The engine exports aabb_expand(out, box, d) taking pointers, while every
    // module in this project declares it as aabb_expand(Box, double) by value - the calling
    // conventions do not match, one argument arrives uninitialised, and the box that comes back
    // (and the box it writes through) cannot be trusted. Six adds are cheaper than that.
    Box hitbox = java_get_axis_aligned_bb_min_x(ctx->jvm, java_get_entity_bounding_box(ctx->jvm, e));
    hitbox.lo.x -= 0.1; hitbox.lo.y -= 0.1; hitbox.lo.z -= 0.1;
    hitbox.hi.x += 0.1; hitbox.hi.y += 0.1; hitbox.hi.z += 0.1;
    Vec3 nearest = aabb_clamp_point(hitbox, eye);
    // Reject anything sitting almost straight above or below. With the horizontal separation near
    // zero the look direction has no yaw to speak of and the pitch solve degenerates to +-90, which
    // is the "it aims at the sky" case people hit when they push FOV up.
    {
        double hx = nearest.x - eye.x;
        double hz = nearest.z - eye.z;
        if (hx * hx + hz * hz < 2.25)
            return 0;
    }
    double dist = vec3_distance(eye, nearest);
    *why = 4;
    if (dist > (double)range) {
        *out_dist2 = dist * dist;      // so the caller can report how far the nearest reject was
        return 0;
    }

    // Aim inside the target's own bounding box, 60% up: that is the chest for a standing player
    // (1.08 of 1.8), still chest for a crouching one, and it scales for every other body shape.
    // The eye point (1.62) was too high - one tick of drop put point-blank shots under the feet -
    // and a fixed +0.9 ignored crouching and differing entity heights.
    Vec3 aim = vec3_lerp(hitbox.lo, hitbox.hi, 0.6);

    // FOV test, Driplite-style: measure against the silent rotation when one is running, so an
    // aim that is already silently applied is not rejected for disagreeing with the visible one.
    Rot view = my_rot;
    if (is_silent_rotation_active())
        view = rot_add(my_rot, get_silent_rotation_offset());
    Rot look = rot_look_at(eye, aim);
    float off_yaw = wrap_angle(rot_sub(look, view).yaw);
    float off_pitch = look.pitch - view.pitch;
    float off = sqrtf(off_yaw * off_yaw + off_pitch * off_pitch);
    *why = 5;
    if (off > fov) {
        *out_dist2 = dist * dist;
        return 0;
    }

    *why = 6;
    if (g_require_los.value == 1) {
        // Cheap proxy for line of sight: if the straight line to the aim point is longer than
        // the arrow's reach at full draw it is out of range anyway, so only the range matters.
        if (dist > 96.0)
            return 0;
    }

    *why = 0;
    *out_dist2 = dist * dist;
    *out_point = aim;
    *out_box = hitbox;
    return 1;
}

// ---- pitch solver -----------------------------------------------------------
//
// The solver scores a pitch by whether the arrow would actually go THROUGH the target's box, not by
// how close its landing point gets to some point. Those are different questions and the difference
// is the whole reason a jumping or crouching target was impossible to hit: "land closest to the
// chest" is satisfied by a shot that sails over the box, while "passes through the box" is the
// thing the server checks.
//
// Blocks stay on (a shot that hits the ground short genuinely missed). Entities stay off inside the
// simulation - a candidate body would stop the ray early and bias the solve; the box test below is
// what decides the hit instead.

// Segment vs axis-aligned box (slab method, parameter restricted to the segment).
static bool seg_hits_box(Vec3 a, Vec3 b, Box box) {
    double tmin = 0.0, tmax = 1.0;
    const double s[3] = {a.x, a.y, a.z};
    const double d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    const double lo[3] = {box.lo.x, box.lo.y, box.lo.z};
    const double hi[3] = {box.hi.x, box.hi.y, box.hi.z};
    for (int i = 0; i < 3; i++) {
        if (d[i] > -1e-9 && d[i] < 1e-9) {
            if (s[i] < lo[i] || s[i] > hi[i])
                return false;
        } else {
            double t1 = (lo[i] - s[i]) / d[i];
            double t2 = (hi[i] - s[i]) / d[i];
            if (t1 > t2) {
                double t = t1;
                t1 = t2;
                t2 = t;
            }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax)
                return false;
        }
    }
    return true;
}

// Index of the segment that enters the box, or -1.
static int trajectory_hits_box(const TrajectoryResult *r, Box box) {
    for (int i = 1; i < r->count; i++)
        if (seg_hits_box(r->points[i - 1], r->points[i], box))
            return i;
    return -1;
}

// One simulated shot: score it, and report whether it passed through the box.
static float score_pitch(Ctx *ctx, Vec3 start, float yaw, float pitch, Box box,
                         const TrajectoryParams *p, int *out_hit_tick) {
    TrajectoryResult r;
    Rot rot;
    rot.yaw = yaw;
    rot.pitch = pitch;
    trajectory_simulate(ctx, start, rot, p, &r);
    if (r.count <= 0) {
        *out_hit_tick = -1;
        return 3.4e38f;
    }
    int hit = trajectory_hits_box(&r, box);
    if (hit >= 0) {
        *out_hit_tick = hit;
        return (float)hit;          // a hit always beats a miss; the earlier the tick the flatter
    }                               // the arc, which is what actually connects on a moving target
    *out_hit_tick = -1;
    Vec3 mid = vec3_make((box.lo.x + box.hi.x) * 0.5, (box.lo.y + box.hi.y) * 0.5,
                         (box.lo.z + box.hi.z) * 0.5);
    Vec3 end = r.points[r.count - 1];
    double dx = end.x - mid.x, dy = end.y - mid.y, dz = end.z - mid.z;
    return 100000.0f + (float)(dx * dx + dy * dy + dz * dz);
}

static float solve_pitch(Ctx *ctx, Vec3 start, float yaw, Box box,
                         const TrajectoryParams *base, float hint, int *out_hit_tick) {
    TrajectoryParams p = *base;
    p.hit_blocks = 1;
    p.hit_entities = 0;

    float best = 0.0f;
    float best_score = 3.4e38f;
    int best_tick = -1;
    *out_hit_tick = -1;

    if (hint > -900.0f) {
        int t = -1;
        float s = score_pitch(ctx, start, yaw, hint, box, &p, &t);
        best = hint;
        best_score = s;
        best_tick = t;
    }

    // Coarse sweep. Minecraft pitch: -90 straight up, +90 down.
    for (float pitch = (hint > -900.0f ? 1.0f : -89.0f); pitch <= 60.0f; pitch += 10.0f) {
        int t = -1;
        float s = score_pitch(ctx, start, yaw, pitch, box, &p, &t);
        if (s < best_score) {
            best_score = s;
            best = pitch;
            best_tick = t;
        }
    }

    // Refine around the best sample.
    float lo = best - 10.0f, hi = best + 10.0f;
    for (float pitch = lo; pitch <= hi; pitch += 1.0f) {
        int t = -1;
        float s = score_pitch(ctx, start, yaw, pitch, box, &p, &t);
        if (s < best_score) {
            best_score = s;
            best = pitch;
            best_tick = t;
        }
    }

    *out_hit_tick = best_tick;
    return best;
}

// How many ticks the arrow is in the air before it reaches `target`. Used for the lead: the target
// keeps walking during the flight, so the aim point has to be where it will be by then, and the
// flight time itself depends on where that is - hence the second solve further down.
static int trajectory_flight_ticks(Ctx *ctx, Vec3 start, float yaw, float pitch, Vec3 target,
                                   const TrajectoryParams *base) {
    TrajectoryParams p = *base;
    p.hit_blocks = 1;
    p.hit_entities = 0;
    TrajectoryResult r;
    Rot rot;
    rot.yaw = yaw;
    rot.pitch = pitch;
    trajectory_simulate(ctx, start, rot, &p, &r);
    if (r.count <= 0)
        return 0;
    int best_i = r.count - 1;
    double best_d = 1e30;
    for (int i = 0; i < r.count; i++) {
        double dx = r.points[i].x - target.x;
        double dy = r.points[i].y - target.y;
        double dz = r.points[i].z - target.z;
        double d = dx * dx + dy * dy + dz * dz;
        if (d < best_d) {
            best_d = d;
            best_i = i;
        }
    }
    return best_i;
}

// ---- tick -------------------------------------------------------------------
// Diagnostics: "it does nothing" has four possible causes here (not in game / not holding a bow /
// holding but not drawing / drawing with no target in range), and they are indistinguishable from
// the outside. One throttled line per second names the cause in %TEMP%\slinky.log.
extern "C" void bow_aim_assist_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    g_have_solution = 0;
    g_display_valid = 0;
    static unsigned tick_no;
    int verbose = ((++tick_no % 20) == 0);

    if (ctx->in_game != 1) {
        if (verbose) slinky_logf("bow: idle in_game=%d", (int)ctx->in_game);
        return;
    }

    unsigned char holding_bow = 0;
    int draw = bow_draw_state(ctx, &holding_bow);
    g_draw_now = draw;
    if (!holding_bow) {
        if (verbose) slinky_logf("bow: idle no bow in hand (draw=%d)", draw);
        return;
    }

    // The gate that keeps this out of the way: while the bow is not being drawn there is nothing
    // to aim, so the crosshair is left completely alone.
    int draw_ticks;
    if (g_require_draw.value == 1) {
        if (draw <= 0) {
            if (verbose) slinky_logf("bow: idle bow held, not drawing (require_draw=1)");
            return;
        }
        draw_ticks = draw;
    } else {
        draw_ticks = 20;        // assume a fully drawn bow when the gate is off
    }
    if (g_follow_draw.value == 1 && draw > 0)
        draw_ticks = draw;

    // Peak of this draw session = the power the arrow will actually leave with.
    if (draw <= 0)
        g_draw_peak = 0;
    else if (draw > g_draw_peak)
        g_draw_peak = draw;

    int solve_ticks = g_draw_peak;
    if (solve_ticks < 12)
        solve_ticks = 12;       // below this the power is so low the solver degenerates
    float power = trajectory_bow_power(solve_ticks);
    if (power <= 0.0f)
        return;

    Rot my_rot = get_entity_rotation(ctx->jvm, ctx->player);
    // Two different origins, and mixing them up is what made it aim at the feet:
    //   - every angle (rot_look_at, FOV, range) starts at the EYE
    //   - trajectory_simulate takes the FEET, because throwable_velocity adds the eye height
    //     itself. Passing the eye position into the simulation lifts the arrow's origin by
    //     another 1.62, and passing the feet into rot_look_at drops every angle by the same
    //     amount - about 18 degrees at five blocks, which reads as "it aims at their feet".
    Vec3 feet = java_get_entity_pos_x(ctx->jvm, ctx->player);
    Vec3 eye = vec3_add_eye_height(feet);

    // Build the parameter set from the sliders so the preview matches the configured physics.
    TrajectoryParams params;
    params.gravity = g_gravity.value;
    params.air_drag = 0.99f;
    params.entity_margin = 0.3f;
    params.max_ticks = (int)g_ticks.value;
    params.hit_blocks = 1;
    params.hit_entities = 1;
    params.power = power;      // without this the predictor falls back to a variable nobody sets

    // Pick the nearest target: scan everything, keep the best, and shrink the bound as we go.
    Vec3 eye_for_range = eye;
    double best = 1e30;
    Vec3 best_point = {0, 0, 0};
    Box best_box = {0, 0, 0, 0, 0, 0};
    void *best_entity = nullptr;
    int found = 0;

    int reasons[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    double nearest_reject = 1e30;
    int scanned = 0;
    void *list = (g_target_mobs.value == 1) ? java_get_world_loaded_entity_list(ctx->jvm, ctx->world)
                                            : java_get_world_player_entities(ctx->jvm, ctx->world);
    if (list) {
        int size = java_call_list_size(ctx->jvm, list);
        scanned = size;
        for (int i = 0; i < size; i++) {
            void *e = java_call_list_get(ctx->jvm, list, i);
            if (!e)
                continue;
            // IsSameObject, not pointer equality: the player entry in that list is a different
            // local reference to the same Java object, so `e == ctx->player` was false for your
            // own body. The module then targeted itself - zero horizontal distance, so the look
            // vector has no yaw to speak of and the pitch solves straight up (the -90/-90 and
            // dist 0.0 rows in the log).
            JNIEnv *env = ((Jvm *)ctx->jvm)->env;
            if ((*env)->IsSameObject(env, (jobject)e, (jobject)ctx->player))
                continue;
            // EntityLivingBase: every one has a positive entity id, a cosmetic or marker does not.
            if (java_get_entity_entity_id(ctx->jvm, e) <= 0)
                continue;
            if (g_target_mobs.value == 1) {
                // In mob mode the list is the whole loaded-entity list, which also holds items,
                // arrows, XP orbs and thrown projectiles. Their boxes are all under 0.6 tall
                // (item 0.25, arrow 0.5, orb 0.5) while the shortest mob, a chicken, is 0.7 -
                // so body height is the cheapest reliable filter.
                Box ebb = java_get_axis_aligned_bb_min_x(
                    ctx->jvm, java_get_entity_bounding_box(ctx->jvm, e));
                if (ebb.hi.y - ebb.lo.y < 0.6)
                    continue;
            }
            double d2 = 0.0;
            Vec3 pt;
            int why = 0;
            Box ebox;
            if (!target_ok(ctx, e, eye_for_range, my_rot, g_range.value, g_fov.value, &d2, &pt, &why, &ebox)) {
                if (why >= 0 && why < 8)
                    reasons[why]++;
                if (d2 > 0.0 && d2 < nearest_reject)
                    nearest_reject = d2;
                continue;
            }
            if (d2 > best)
                continue;
            best = d2;
            best_point = pt;
            best_box = ebox;
            best_entity = e;
            found = 1;
        }
    }

    if (!found) {
        if (verbose)
            slinky_logf("bow: no target scanned=%d dead=%d invis=%d flags=%d far=%d fov=%d los=%d nearest=%.1f (range=%.0f fov=%.0f)",
                        scanned, reasons[1], reasons[2], reasons[3], reasons[4], reasons[5], reasons[6],
                        (nearest_reject < 1e29 ? sqrt(nearest_reject) : -1.0),
                        (double)g_range.value, (double)g_fov.value);
        return;
    }

    // Reach check: a low-power draw simply cannot get there, and aiming anyway just trains the
    // player to blame the aim. v^2/g is the no-drag range; 0.7 approximates the drag loss.
    {
        double hdist0 = sqrt((best_point.x - eye.x) * (best_point.x - eye.x) +
                             (best_point.z - eye.z) * (best_point.z - eye.z));
        double reach = (double)power * (double)power / (double)params.gravity * 0.7;
        if (hdist0 > reach) {
            if (verbose)
                slinky_logf("bow: out of reach dist=%.1f reach=%.1f power=%.2f draw=%d",
                            hdist0, reach, (double)power, draw);
            return;
        }
    }

    g_target_valid = 1;
    g_target_point = best_point;
    g_lead = vec3_make(0.0, 0.0, 0.0);
    g_target_entity = best_entity;

    // Lead the target. Solve once at its current position to learn how long the arrow is in the
    // air, move the aim point by (target velocity x flight time), then solve again there. Without
    // this a walking player is always missed by roughly (speed x flight time): at 30 blocks and a
    // full draw that is about half a second, and a strafing player moves three blocks in it.
    Vec3 aim = best_point;
    Rot look = rot_look_at(eye, aim);
    float yaw = look.yaw;
    int hit_tick = -1;
    // Point blank: the flight is under one tick, so a straight line beats a solved arc.
    float pitch = (sqrtf((float)best) < 8.0f)
                      ? look.pitch
                      : solve_pitch(ctx, feet, yaw, best_box, &params, -999.0f, &hit_tick);

    int flight = 0;
    double last_velx = 0.0, last_velz = 0.0;
    if (g_predict.value > 0.0f && best_entity) {
        Vec3 tpos = java_get_entity_pos_x(ctx->jvm, best_entity);
        Vec3 tprev = java_get_entity_prev_pos_x(ctx->jvm, best_entity);
        Vec3 tvel = vec3_sub(tpos, tprev);
        // Instantaneous displacement, lightly smoothed. Heavy smoothing (0.4) lagged two or three
        // ticks behind a direction change, which is exactly where a strafing player lives.
        Vec3 tvel_raw = tvel;
        if (best_entity != g_vel_entity) {
            g_vel_entity = best_entity;
            g_vel_smooth = tvel_raw;
        } else {
            g_vel_smooth = vec3_lerp(g_vel_smooth, tvel_raw, 0.75);
        }
        // Direction stability: how far the smoothed velocity sits from the instantaneous one. When
        // a target changes direction these diverge, and the extrapolation is the least trustworthy
        // exactly then - so scale the lead down instead of throwing the crosshair past them.
        double vmag = sqrt(tvel_raw.x * tvel_raw.x + tvel_raw.z * tvel_raw.z);
        double vdiff = sqrt((g_vel_smooth.x - tvel_raw.x) * (g_vel_smooth.x - tvel_raw.x) +
                            (g_vel_smooth.z - tvel_raw.z) * (g_vel_smooth.z - tvel_raw.z));
        double trust = 1.0;
        if (vmag > 0.02) {
            double ratio = vdiff / vmag;
            if (ratio > 1.0) ratio = 1.0;
            trust = 1.0 - ratio * 0.6;          // a hard turn drops the lead to 40%
        }
        tvel = g_vel_smooth;
        last_velx = tvel.x;
        last_velz = tvel.z;

        // Flight time from the solver, not from distance / average speed. The solve above already
        // ran the real arc and reported the tick it would enter the box (`hit_tick`), which is the
        // exact answer; distance over an assumed 3.0 blocks/tick overshot it badly - the log showed
        // hit=5 alongside fly=8, so the lead was 60% too long and every shot landed ahead of a
        // walking target. The distance estimate stays only as a fallback for a solve that missed.
        double lead_ticks;
        if (hit_tick > 0) {
            lead_ticks = (double)hit_tick + (double)g_predict.value / 50.0;
        } else {
            double hdx = aim.x - eye.x, hdz = aim.z - eye.z;
            double hdist = sqrt(hdx * hdx + hdz * hdz);
            lead_ticks = hdist / 3.0 + (double)g_predict.value / 50.0;
        }
        flight = (int)(lead_ticks + 0.5);
        // Give up on shots that take too long to arrive. Nothing about the prediction is wrong here
        // - it is that the target will have stepped out of the way by the time the arrow covers the
        // last few blocks. Aiming anyway just parks the crosshair next to a person we cannot hit.
        if (g_max_flight.value > 0.5f && (double)flight > (double)g_max_flight.value) {
            if (verbose)
                slinky_logf("bow: skip, flight %d > max %.0f ticks (they can step aside)", flight,
                            (double)g_max_flight.value);
            return;
        }

        Vec3 lead = vec3_scale(tvel, lead_ticks * trust);
        // 0.65 of the geometric lead on the horizontal axes: under-leading is recoverable by hand,
        // over-leading parks the crosshair in empty air.
        lead.x *= 0.65;
        lead.z *= 0.65;
        // Vertical is capped hard - a jump reverses its y velocity inside one flight time.
        double vy_ticks = lead_ticks > 4.0 ? 4.0 : lead_ticks;
        lead.y = tvel.y * vy_ticks * 0.6;
        {
            const double kMaxLead = 4.5;
            double len = sqrt(lead.x * lead.x + lead.z * lead.z);
            if (len > kMaxLead) {
                lead.x *= kMaxLead / len;
                lead.z *= kMaxLead / len;
            }
        }

        // Move the WHOLE box by the lead and solve against that box. Solving against a moved point
        // and checking a box elsewhere was the mismatch that made moving targets unhittable: the
        // arc was fitted to a point the target's body would never occupy.
        Box aim_box = best_box;
        aim_box.lo.x += lead.x; aim_box.hi.x += lead.x;
        aim_box.lo.y += lead.y; aim_box.hi.y += lead.y;
        aim_box.lo.z += lead.z; aim_box.hi.z += lead.z;

        aim = vec3_lerp(aim_box.lo, aim_box.hi, 0.6);
        look = rot_look_at(eye, aim);
        yaw = look.yaw;
        pitch = (sqrtf((float)best) < 8.0f)
                    ? look.pitch
                    : solve_pitch(ctx, feet, yaw, aim_box, &params, look.pitch, &hit_tick);
        g_target_point = aim;
        g_lead = lead;
    }

    // Randomisation was dropped: on a moving target it read as "the aim is drunk", and it was one
    // more row for no measurable hit-rate gain.

    // A solve that lands past 85 degrees is a diverged solution, not an aim: give up for this tick
    // instead of yanking the view into the sky.
    if (!(fabsf(pitch) < 85.0f))
        return;

    g_pitch_offset = pitch - rot_look_at(eye, aim).pitch;   // 弹道相对直线的抬升量
    g_aim_yaw = yaw;
    g_aim_pitch = clampf(pitch, -90.0f, 90.0f);
    if (g_lock_vertical.value == 1)
        g_aim_pitch = my_rot.pitch;
    g_have_solution = 1;

    // The collision-checked preview that used to be built here existed only for this module's own
    // renderer, which is gone (Trajectories draws the arc). Nothing consumes g_display now.
    g_display_valid = 0;

    if (verbose)
        // aim_y - tgt_y must be the 60% point of the target's box (about 1.08 for a standing
        // player). If it reads near 0 the aim point fell on their feet; if eye_y - tgt_y is not
        // ~1.62 the position reads themselves are wrong.
        {
            double tgt_y = 0.0;
            if (best_entity)
                tgt_y = java_get_entity_pos_x(ctx->jvm, best_entity).y;

            {
                double lx = aim.x - best_point.x, lz = aim.z - best_point.z;
                slinky_logf("bow: aimed v10 yaw=%.1f pitch=%.1f dist=%.1f view=%.1f draw=%d solve=%d aim_y=%.2f tgt_y=%.2f fly=%d hit=%d lead=%.2f pred=%.0f vel=(%.3f,%.3f) off=(%.2f,%.2f)",
                            (double)g_aim_yaw, (double)g_aim_pitch, (double)sqrtf((float)best),
                            (double)my_rot.yaw, draw, solve_ticks,
                            (double)aim.y, tgt_y, flight, hit_tick, sqrt(lx * lx + lz * lz),
                            (double)g_predict.value,
                            last_velx, last_velz, lx, lz);
            }
        }
}

// ---- rotation ---------------------------------------------------------------
//
// This is where the "sticks tight but does not grab" behaviour lives, and it is the part taken
// from Driplite. Instead of snapping to the solved rotation, each frame moves the crosshair by a
// fraction of the angle still left, and that fraction grows with the CUBE of the angle:
//
//     fraction = (remaining_deg / kCurveRef)^curve
//
// With curve = 3 and kCurveRef = 30, a 30 degree error is corrected at full speed while a 5
// degree error is corrected at about 0.5% of it. Large corrections converge fast; fine aim is
// almost untouched, which is exactly the "he xi de hen jin, dan shi you bu qiang" feel. Because
// the applied delta is also clamped to the configured speed, a manual mouse flick always wins.
static const float kCurveRefDegrees = 30.0f;

extern "C" void bow_aim_assist_frame(void *frame_evt) {
    if (g_have_solution != 1)
        return;
    FrameEvt *h = (FrameEvt *)frame_evt;
    Ctx *ctx = h->ctx;
    if (!ctx || ctx->in_game != 1)
        return;
    if (!update_rotation_priority_peak(100))
        return;

    // Where the crosshair currently points, including any silent offset already in flight, so
    // repeated frames do not stack on top of an unfinished correction.
    Rot current = get_entity_rotation(ctx->jvm, ctx->player);
    Rot offset = get_silent_rotation_offset();
    current = rot_add(current, offset);

    // Re-derive the angles from the live eye position instead of replaying the tick's numbers:
    // the aim point is fixed in the world, the shooter is not.
    Vec3 feet_now = java_get_entity_pos_x(ctx->jvm, ctx->player);
    Vec3 eye_now = vec3_add_eye_height(feet_now);
    // Rebuild the aim point from the target's interpolated position every frame. Reusing the tick's
    // world point meant the crosshair chased a position that could be 50 ms old - on a strafing
    // target that alone is half a block of lag, and it is the difference between tracking like a
    // tick-rate bot and tracking like a frame-rate one.
    Vec3 aim_now = g_target_point;
    if (g_target_valid == 1 && g_target_entity) {
        Vec3 tp = java_get_entity_pos_x(ctx->jvm, g_target_entity);
        Vec3 tpv = java_get_entity_prev_pos_x(ctx->jvm, g_target_entity);
        Vec3 ti = vec3_lerp(tpv, tp, (double)h->partial);
        ti.y += 0.9;                       // the box-relative aim height
        aim_now = vec3_add(ti, g_lead);
    }
    Rot live = rot_look_at(eye_now, aim_now);
    Rot target;
    target.yaw = live.yaw;
    target.pitch = clampf(live.pitch + g_pitch_offset, -89.0f, 89.0f);
    Rot delta = rot_sub(target, current);
    delta.yaw = wrap_angle(delta.yaw);

    // Cubic curve, per axis so vertical aim is not dragged around by a large horizontal error.
    float fy = clampf(fabsf(delta.yaw) / kCurveRefDegrees, 0.0f, 1.0f);
    float fp = clampf(fabsf(delta.pitch) / kCurveRefDegrees, 0.0f, 1.0f);
    float cy = (float)powf(fy, g_curve.value);
    float cp = (float)powf(fp, g_curve.value);

    Rot step;
    float gain = g_speed.value / 30.0f;
    float gy = cy * gain;
    float gp = cp * gain;
    // Floor on the gain. The rotation this client applies is not written straight into the entity:
    // set_rotation_active (silent_rotation_and_chat_colors.cpp) consumes the pending offset in
    // whole mouse_sensitivity_step units, which is about 0.15 degrees at the default sensitivity.
    // A pure cubic therefore never moves at all for the errors it is meant to smooth - 3 degrees
    // gives 0.1% of the angle, a few hundredths of a degree per frame, which never adds up to one
    // sensitivity step inside a tick. Driplite's formula has a +B constant term for exactly this
    // reason; the floor is that term.
    const float kMinGain = 0.17f;
    if (delta.yaw != 0.0f && gy < kMinGain)
        gy = kMinGain;
    if (delta.pitch != 0.0f && gp < kMinGain)
        gp = kMinGain;
    step.yaw = delta.yaw * gy;
    step.pitch = delta.pitch * gp;

    if (g_lock_vertical.value == 1)
        step.pitch = 0.0f;

    // Never overshoot: the step may not exceed the remaining angle.
    step.yaw = limit_rotation_delta(-fabsf(delta.yaw), fabsf(delta.yaw), step.yaw);
    step.pitch = limit_rotation_delta(-fabsf(delta.pitch), fabsf(delta.pitch), step.pitch);

    if (step.yaw != 0.0f || step.pitch != 0.0f) {
        g_last_speed_applied = sqrtf(step.yaw * step.yaw + step.pitch * step.pitch);
        // Write the rotation directly instead of queueing it through apply_silent_rotation.
        // That queue is consumed once per tick in whole mouse_sensitivity_step units, so anything
        // smaller than one step (0.15 deg at default sensitivity) is silently dropped, and the
        // pending offset is cleared every frame by update_mouse_sensitivity_step. The result is a
        // crosshair that converges to "roughly right" and never lands. Writing current+step every
        // frame is what the open-source aimbots do and it removes the quantisation entirely.
        Rot next;
        next.yaw = wrap_angle(current.yaw + step.yaw);
        next.pitch = clampf(current.pitch + step.pitch, -89.0f, 89.0f);
        set_entity_rotation(ctx->jvm, ctx->player, next);
        // Auto release: once the bow is full and the crosshair has actually arrived, let go. Waiting
        // for "arrived" matters - firing while the correction is still converging throws the arrow
        // along whatever the current angle is, which is the one case where auto fire is worse than
        // pressing the button yourself.
        if (g_auto_release.value == 1 && g_draw_now >= 20) {
            float dy_left = wrap_angle(target.yaw - current.yaw);
            float dp_left = target.pitch - current.pitch;
            if (fabsf(dy_left) < 2.5f && fabsf(dp_left) < 2.5f) {
                void *gs = get_game_settings(ctx->jvm, ctx->minecraft);
                if (gs) {
                    void *use_key = java_get_game_settings_use_item_key(ctx->jvm, gs);
                    if (use_key) {
                        java_set_key_binding_pressed(ctx->jvm, use_key, false);
                        static int rel_logged;
                        if (!rel_logged) {
                            rel_logged = 1;
                            slinky_logf("bow: auto release fired (yaw err %.2f, pitch err %.2f)",
                                        (double)dy_left, (double)dp_left);
                        }
                    }
                }
            }
        }
        // Sync prevRotation as well. The renderer draws the look direction as
        // prev + (current - prev) * partialTicks, and prev only advances once per tick - so writing
        // current every frame while leaving prev alone makes the drawn crosshair lag the real one
        // by up to half a tick. That is exactly "I can see the crosshair on him but the arrow
        // misses": the shot uses current, your eyes are reading the interpolated value. Since this
        // module already advances the rotation smoothly every frame, the tick interpolation is
        // doing nothing but lying.
        set_entity_prev_rotation(ctx->jvm, ctx->player, next);
        static unsigned frame_no;
        if ((++frame_no % 60) == 0)
            slinky_logf("bow: frame apply dy=%.3f dp=%.3f gain=%.3f remaining=%.2f",
                        (double)step.yaw, (double)step.pitch, (double)gy, (double)delta.yaw);
    }
}

// Claims the rotation for this module: h[8] is yaw, h[9] is pitch. Claiming only the yaw keeps
// the pitch under the game's control when the module is set not to lock it.
extern "C" void bow_aim_assist_on_rotation_update(char *h) {
    if (g_have_solution != 1)
        return;
    // h[8] is the "on" argument of apply_player_rotation: filling it puts the client into silent
    // mode, where the accumulated rotation is only applied to the rendered view and the server
    // keeps seeing the old yaw. An arrow is fired along the real rotation, so a silent aim here
    // aims nothing - the shot leaves in the old direction no matter how good the crosshair looks.
    //
    // The client's own Aim Assist leaves this at 0 unless its Silent mode is on; the pending
    // offset is then consumed by set_rotation_active into set_entity_rotation, which is a real
    // turn. Same thing here: only claim the silent channel when the Silent option is enabled.
    if (g_silent.value == 1) {
        h[8] = 1;
        if (g_lock_vertical.value != 1)
            h[9] = 1;
    }
}

// ---- render --------------------------------------------------------------------
// None. The client's own Trajectories module already draws the arrow arc from the same shared
// predictor (see src/engine/modlib/trajectory_predict.h), so painting a second one here only
// doubled the geometry and brought its own state-restore bugs.

// ---- gui / register ---------------------------------------------------------
extern "C" void bow_aim_assist_disable(EvHook *hook) {
    (void)hook;
    g_have_solution = 0;
    g_display_valid = 0;
    g_target_valid = 0;
    g_target_entity = nullptr;
}

// Twelve rows instead of twenty-one. Everything that did not change how the module feels in game
// is gone: random jitter, air drag, power scale, manual draw ticks, hit/miss colours, the target
// link toggle, lock-vertical and max ticks. Lock-vertical and max ticks still exist as variables
// (their defaults are the sane ones: free pitch, 100 ticks) so saved profiles keep loading.
//
// The engine's range widget is gone too - see the note at the top of the setting storage block for
// why it could never be used safely from here.
extern "C" void bow_aim_assist_gui(void *gui, void *window) {
    W_label(gui, window, str_bow_main);
    W_bool(window, gui, &g_require_draw);      // the option the whole feel depends on
    W_bool(window, gui, &g_silent);
    W_slider(window, gui, &g_speed);
    W_slider(window, gui, &g_curve);
    W_slider(window, gui, &g_predict);

    W_label(gui, window, str_bow_target);
    W_slider(window, gui, &g_range);
    W_slider(window, gui, &g_fov);
    W_bool(window, gui, &g_require_los);

    W_bool(window, gui, &g_target_mobs);
    W_bool(window, gui, &g_auto_release);
    W_slider(window, gui, &g_max_flight);

    W_label(gui, window, str_bow_physics);
    W_bool(window, gui, &g_follow_draw);
    W_slider(window, gui, &g_gravity);
}

extern "C" void bow_aim_assist_register(void *module) {
    R_bool(module, str_bow_require_draw, &g_require_draw);
    R_bool(module, str_bow_silent, &g_silent);
    R_bool(module, str_bow_lock_vertical, &g_lock_vertical);
    R_num(module, str_bow_speed, &g_speed);
    R_num(module, str_bow_curve, &g_curve);
    R_num(module, str_bow_predict, &g_predict);
    R_num(module, str_bow_range, &g_range);
    R_num(module, str_bow_fov, &g_fov);
    R_bool(module, str_bow_ignore_invis, &g_ignore_invis);
    R_bool(module, str_bow_require_los, &g_require_los);
    R_bool(module, str_bow_follow_draw, &g_follow_draw);
    R_num(module, str_bow_gravity, &g_gravity);
    R_num(module, str_bow_ticks, &g_ticks);
    R_bool(module, str_bow_target_mobs, &g_target_mobs);
    R_bool(module, str_bow_auto_release, &g_auto_release);
    R_num(module, str_bow_max_flight, &g_max_flight);
    R_bool(module, str_bow_show, &g_show_traj);
    R_num(module, str_bow_width, &g_width);
}

// ---- descriptor -------------------------------------------------------------
struct BowHSlot { void *handler; void *context; };
struct BowDescriptor {
    const char *name; const char *description; unsigned category; unsigned char enabled;
    unsigned char pad_15[3]; unsigned flags; unsigned bind; unsigned char hold;
    unsigned char pad_21[0x27];
    BowHSlot handlers[27];
    void (*settings_gui)(void *, void *);
    void (*settings_register)(void *);
    void *suffix;
};
static_assert(sizeof(BowDescriptor) == 0x210, "descriptor size");

extern "C" BowDescriptor bow_aim_assist_module_descriptor = {
    str_bow_name,
    str_bow_desc,
    0,                      // category: Combat
    0,
    {0, 0, 0},
    0,
    0,
    0,
    {},
    {
        {0, 0},                                              // 0
        { (void *)bow_aim_assist_frame, 0 },                 // 1  frame (apply rotation)
        {0, 0}, {0, 0}, {0, 0},                              // 2..4
        { (void *)bow_aim_assist_disable, 0 },               // 5  toggle / disable
        {0, 0},                                              // 6  world render (画轨迹交给原版 Trajectories)
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 7..11
        {0, 0},                                              // 12
        { (void *)bow_aim_assist_on_rotation_update, 0 },    // 13 rotation override
        {0, 0}, {0, 0}, {0, 0}, {0, 0},                      // 14..17
        { (void *)bow_aim_assist_tick, 0 },                  // 18 tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},              // 19..23
        {0, 0}, {0, 0}, {0, 0},                              // 24..26
    },
    bow_aim_assist_gui,
    bow_aim_assist_register,
    0,
};
