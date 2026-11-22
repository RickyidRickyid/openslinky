// Shared projectile trajectory prediction — see trajectory_predict.h.
//
// The maths is the vanilla one: integrate position by velocity, multiply velocity by a constant
// drag every tick, subtract a constant from its y, and stop at the first block or entity hit.
// Blocks are tested with the game's own ray trace so the predictor can never disagree with what
// the arrow will really do; entities are tested with a ray/slab test against boxes gathered once
// per simulation (gathering them costs JNI calls, so it is not repeated per step).
#include "slinky/engine.h"
#include "slinky/jids.h"
#include "trajectory_predict.h"

FN(Vec3, vec3_add, (Vec3 a, Vec3 b));
FN(Vec3, vec3_sub, (Vec3 a, Vec3 b));
FN(Vec3, vec3_scale, (Vec3 v, double k));
FN(Vec3, vec3_reciprocal, (Vec3 v));
FN(Vec3, vec3_make, (double x, double y, double z));

FN(Box, aabb_expand, (Box b, double grow));
FN(Box, aabb_union, (Vec3 a, Vec3 b));
FN(Box, aabb_make, (Vec3 lo, Vec3 hi));
FN(unsigned char, aabb_intersects, (Box a, Box b));
FN(unsigned char, aabb_ray_slab_test, (Box b, Vec3 origin, Vec3 dir));
FN(Box, java_get_axis_aligned_bb_min_x, (void *jvm, void *bounding_box));

FN(void, throwable_velocity, (Ctx *ctx, Vec3 *start, Rot rotation, float partial, Vec3 *pos, Vec3 *vel));
FN(float, libc_sinf, (float x));
FN(float, libc_cosf, (float x));

FN(void *, java_world_ray_trace_blocks, (void *jvm, void *world, void *from, void *to,
                                        unsigned char liquids, unsigned char ignore_no_box,
                                        unsigned char last));
FN(void *, java_new_vec3_init, (void *jvm, Vec3 v));
FN(void *, java_get_moving_object_position_hit_vec, (void *jvm, void *hit));
FN(Vec3, java_get_vec3_x_coord, (void *jvm, void *vec3));
FN(Vec3, java_get_moving_object_position_block_x, (void *jvm, void *hit));

FN(void *, java_get_world_loaded_entity_list, (void *jvm, void *world));
FN(int, java_call_list_size, (void *jvm, void *list));
FN(void *, java_call_list_get, (void *jvm, void *list, int i));
FN(unsigned char, java_call_entity_can_be_collided_with, (void *jvm, void *entity));
FN(void *, java_get_entity_bounding_box, (void *jvm, void *entity));
FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(Vec3, java_get_entity_prev_pos_x, (void *jvm, void *entity));

typedef unsigned char (*IsSameObject)(void *env, void *a, void *b);

// Candidates are gathered once per simulation; 256 is far more than anything a single throw can
// reach in the air, and a fixed array keeps this off the heap (it runs inside the tick).
static const int kMaxCandidates = 256;
static Box g_candidate_boxes[kMaxCandidates];
static void *g_candidate_entities[kMaxCandidates];
static int g_candidate_count;

static const TrajectoryParams g_bow_defaults = {
    0.05f,  // gravity
    0.99f,  // air drag
    0.3f,   // entity margin
    100,    // max ticks
    1,      // hit blocks
    1,      // hit entities
    0.0f,   // power: 0 = use the client's own throwable maths
};

extern "C" const TrajectoryParams *trajectory_bow_defaults(void) {
    return &g_bow_defaults;
}

// Vanilla: draw time in ticks -> launch speed. Below two tenths of a second the bow will not
// fire at all, which is why this can return 0.
extern "C" float trajectory_bow_power(int draw_ticks) {
    float t = (float)draw_ticks / 20.0f;
    float v = (t * t + (t + t)) / 3.0f;
    if (0.1 > (double)v)
        return 0.0f;
    double p = (double)(1.0f < v ? 1.0f : v) * 1.5;
    return (float)(p + p);
}

// Collects the entities a shot could possibly reach. The filter is deliberately loose: a box
// that turns out not to be on the path simply never matches during the step loop.
static void gather_candidates(Ctx *ctx, Vec3 from, Vec3 velocity, float margin) {
    g_candidate_count = 0;
    void *list = java_get_world_loaded_entity_list(ctx->jvm, ctx->world);
    if (!list)
        return;

    // A generous region along the launch direction: everything the arrow could plausibly pass
    // through, so the per-step test only has to look at a short list.
    Vec3 lo = {from.x, from.y - 100.0, from.z};
    Vec3 hi = {velocity.x * 1000.0 + from.x, from.y + 200.0, velocity.z * 1000.0 + from.z};
    Box area = aabb_union(lo, hi);

    JNIEnv *env = ((Jvm *)ctx->jvm)->env;
    IsSameObject is_same = nullptr;
    if (env && *env)
        is_same = (IsSameObject)(*env)->IsSameObject;

    int size = java_call_list_size(ctx->jvm, list);
    for (int i = 0; i < size && g_candidate_count < kMaxCandidates; i++) {
        void *element = java_call_list_get(ctx->jvm, list, i);
        if (!element)
            continue;
        if (is_same && is_same(env, element, ctx->player))
            continue;
        if (!java_call_entity_can_be_collided_with(ctx->jvm, element))
            continue;
        // Hand-expanded on purpose: aabb_expand is exported by the engine as (out, box, d) taking
        // pointers, but declared everywhere in this project as (Box, double) by value. The two
        // calling conventions do not line up, so what it returns cannot be trusted.
        Box bb = java_get_axis_aligned_bb_min_x(
            ctx->jvm, java_get_entity_bounding_box(ctx->jvm, element));
        bb.lo.x -= (double)margin; bb.lo.y -= (double)margin; bb.lo.z -= (double)margin;
        bb.hi.x += (double)margin; bb.hi.y += (double)margin; bb.hi.z += (double)margin;
        if (!aabb_intersects(bb, area))
            continue;
        g_candidate_boxes[g_candidate_count] = bb;
        g_candidate_entities[g_candidate_count] = element;
        g_candidate_count++;
    }
}

extern "C" void trajectory_simulate(Ctx *ctx, Vec3 start, Rot rotation,
                                    const TrajectoryParams *params, TrajectoryResult *out) {
    if (!out)
        return;
    if (!params)
        params = &g_bow_defaults;

    out->count = 0;
    out->hit = 0;
    out->hit_kind = 0;
    out->hit_entity = nullptr;
    out->hit_entity_motion = vec3_make(0.0, 0.0, 0.0);
    out->start = start;

    // Initial velocity comes from the game's own throwable maths, so the predictor and the real
    // arrow cannot disagree about where the shot starts or which way it leaves the bow.
    Vec3 ignore_pos;
    Vec3 velocity;
    throwable_velocity(ctx, &start, rotation, 0.0f, &ignore_pos, &velocity);
    if (params->power > 0.0f) {
        // Own launch speed. throwable_velocity reads trajectories_launch_power, which is the
        // Trajectories module's variable: a caller that does not maintain it gets a zero-length
        // velocity, every sampled pitch then lands exactly on the launch point, and a pitch solver
        // cannot tell the samples apart - it returns the first one, which is -89 (straight up).
        // Direction is recomputed from the rotation with the same formula throwable_math.cpp uses.
        const double k = 0.017453292519943295;
        float yaw = (float)((double)rotation.yaw * k);
        float pitch = (float)((double)rotation.pitch * k);
        float cy = libc_cosf(yaw), sy = libc_sinf(yaw);
        float cp = libc_cosf(pitch), sp = libc_sinf(pitch);
        velocity = vec3_scale(vec3_make(-(double)sy * (double)cp, -(double)sp,
                                        (double)cp * (double)cy),
                              (double)params->power);
    }
    out->launch_velocity = velocity;

    if (params->hit_entities)
        gather_candidates(ctx, start, velocity, params->entity_margin);

    int limit = params->max_ticks;
    if (limit > 128)
        limit = 128;
    if (limit < 1)
        limit = 1;

    Vec3 pt = start;
    Vec3 prev = start;
    Vec3 vel = velocity;

    for (int n = 0; n < limit; n++) {
        prev = pt;
        pt = vec3_add(pt, vel);

        // Blocks first: the game's ray trace is the authority on what is solid, so using it
        // keeps the preview honest about slabs, fences and the like.
        void *hit = nullptr;
        if (params->hit_blocks) {
            hit = java_world_ray_trace_blocks(ctx->jvm, ctx->world,
                                              java_new_vec3_init(ctx->jvm, prev),
                                              java_new_vec3_init(ctx->jvm, pt), 0, 1, 0);
        }

        out->points[n] = pt;
        out->deltas[n] = vec3_sub(pt, prev);
        out->count = n + 1;

        if (hit) {
            out->hit = 1;
            out->hit_kind = 1;
            Vec3 at = java_get_vec3_x_coord(ctx->jvm,
                                            java_get_moving_object_position_hit_vec(ctx->jvm, hit));
            Vec3 block = java_get_moving_object_position_block_x(ctx->jvm, hit);
            out->hit_point = at;
            out->hit_box = aabb_make(block, vec3_add(block, vec3_make(1.0, 1.0, 1.0)));
            out->points[n] = at;
            out->deltas[n] = vec3_sub(at, prev);
            return;
        }

        if (params->hit_entities) {
            Vec3 dir = vec3_reciprocal(vel);
            for (int j = 0; j < g_candidate_count; j++) {
                if (!aabb_ray_slab_test(g_candidate_boxes[j], prev, dir))
                    continue;
                void *entity = g_candidate_entities[j];
                out->hit = 1;
                out->hit_kind = 2;
                out->hit_point = pt;
                out->hit_box = g_candidate_boxes[j];
                out->hit_entity = entity;
                // How far the entity moved this tick, so a caller drawing or aiming at it can
                // account for the part of that movement the frame is still behind on.
                out->hit_entity_motion =
                    vec3_sub(java_get_entity_pos_x(ctx->jvm, entity),
                             java_get_entity_prev_pos_x(ctx->jvm, entity));
                return;
            }
        }

        vel = vec3_scale(vel, (double)params->air_drag);
        vel.y -= (double)params->gravity;
    }
}
