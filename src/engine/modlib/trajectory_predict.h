// Shared projectile trajectory prediction.
//
// This exists because the flight maths was written twice inside Trajectories: once in
// trajectories_tick to find what the projectile hits, and again in trajectories_render to
// rebuild the same polyline at the current frame's interpolated position. Bow Aim Assist needs
// the same simulation a third time, and its aim solver needs to call it repeatedly while
// iterating on the launch pitch. Keeping three copies in step is not workable, so the maths
// lives here and every caller goes through it.
//
// The types below are the ones the modules already used; they are defined here once and the
// modules include this header instead of redefining them.
#pragma once
#include "slinky/engine.h"

struct Vec3 { double x, y, z; };
struct Rot { float yaw, pitch; };
struct Box { Vec3 lo, hi; };

// Everything that affects the flight, so a caller can express "what if gravity were higher"
// without editing the integrator.
struct TrajectoryParams {
    float gravity;              // 0.05 for a bow, 0.03 for pearls and throwables
    float air_drag;             // speed multiplier applied every tick (0.99 in vanilla)
    float entity_margin;        // projectiles hit entities slightly outside their box (0.3)
    int max_ticks;              // give up after this many steps (vanilla despawns at 100)
    unsigned char hit_blocks;   // stop at the first solid block
    unsigned char hit_entities; // stop at the first entity
    // Launch speed. <= 0 falls back to the client's own throwable maths, which reads
    // trajectories_launch_power - a variable only the Trajectories module maintains. Any other
    // module MUST set this, or every simulated pitch lands on the launch point.
    float power;
};

// One simulated flight. `deltas` is what lets a renderer rebuild the line at an interpolated
// start position without re-running the simulation: the whole path translates with the start
// point, and the per-step differences are unchanged.
struct TrajectoryResult {
    Vec3 points[128];           // flight path, points[0] is one tick after launch
    Vec3 deltas[128];           // points[i] - points[i-1] (or start for i == 0)
    int count;                  // number of valid entries
    unsigned char hit;          // stopped early because something was in the way
    unsigned char hit_kind;     // 0 = nothing, 1 = block, 2 = entity
    Vec3 hit_point;             // where it stopped
    Box hit_box;                // the block or entity it stopped on
    void *hit_entity;           // the entity, or null
    Vec3 hit_entity_motion;     // that entity's movement this tick, for prediction correction
    Vec3 start;                 // launch point used, so a renderer can offset from it
    Vec3 launch_velocity;       // velocity right after launch
};

// Vanilla bow speed for a draw of `draw_ticks` ticks; 0 while the bow is too weakly drawn.
extern "C" float trajectory_bow_power(int draw_ticks);

// Simulates the flight from `start` along `rotation` and fills `out`.
// `params` may be null, which selects the bow defaults.
extern "C" void trajectory_simulate(Ctx *ctx, Vec3 start, Rot rotation,
                                    const TrajectoryParams *params, TrajectoryResult *out);

// The default parameter set for a bow, shared so callers do not each pick their own numbers.
extern "C" const TrajectoryParams *trajectory_bow_defaults(void);
