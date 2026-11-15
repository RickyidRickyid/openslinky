// Bed ESP — draw boxes around beds through walls.
//
// RECONSTRUCTION ADDITION. The original client shipped no bed module: a bed was only reachable as one entry
// of Block ESP's allowed_blocks list ("Bed block", id 26), which tied it to Block ESP's range, colour, render
// limit and to the global chunk-search tables. This module is independent: it walks the blocks around the
// player and asks the world for the id at each position.
//
// HOW IT STAYS UP TO DATE (2026-11-14 rework)
//
// One radial pass builds the index when the player enters a world (or moves away from where the last pass
// ran); after that the module is event-driven and costs nothing per tick:
//
//   event 0x14 kind 2  a single block changed (World.setBlockState)   -> add / remove that one position
//   event 0x14 kind 0  a chunk was unloaded                           -> drop every position inside it
//   event 0x14 kind 1  a chunk region was marked dirty by chunk data  -> ignored (the radial pass covers it)
//
// 0x14 is the block/chunk change event the world hooks dispatch (src/engine/base/java_natives_world.cpp);
// dispatch_event() indexes handler lists by the event's type dword, so 0x14 = 20 arrives in descriptor slot
// 20. That is the closest this build can get to Myau's MixinBlockRendererDispatcher: bed placements and
// breaks arrive the instant they happen instead of waiting for the next scan.
//
// The previous version ended every completed pass by starting a new one on the following tick and restarted
// the running pass whenever the player stepped out of the scanned box. A pass needs ~9 ticks at the budget
// below and walking takes you out of the box faster than that at speed, so a pass could restart forever and
// the index was rebuilt from scratch tens of times a minute: the log showed 11 million cells scanned.
//
// Callbacks (descriptor slots):
//   +0x48 handlers[0]  tick             - schedule / advance a pass, prune slowly in the steady state
//   +0x70 handlers[5]  toggle           - reset the index when the module is switched on
//   +0x78 handlers[6]  world render     - draw the boxes
//   +0xe8 handlers[20] world change     - 0x14, the instant add/remove above
#include "slinky/engine.h"
#include "slinky/jids.h"
static unsigned long long bed_esp_last_reported = ~0ULL;
FN(void, slinky_logf, (const char *fmt, ...));

struct Vec3 { double x, y, z; };
struct Found { Vec3 pos; int id; int padding_1c; };

// ---- module descriptor (0x210 bytes, the layout the engine walks) ----
struct HSlotCfg { void *handler; void *context; };
struct ModuleDescriptor {
    const char *name;                       // +0x000
    const char *description;                // +0x008
    unsigned category;                      // +0x010  2 = Visual
    unsigned char enabled;                  // +0x014
    unsigned char pad_15[3];
    unsigned flags;                         // +0x018
    unsigned bind;                          // +0x01c
    unsigned char hold;                     // +0x020
    unsigned char pad_21[0x27];             // +0x021 .. +0x047
    HSlotCfg handlers[27];                  // +0x048, one 16-byte slot per event type
    void (*settings_gui)(void *, void *);   // +0x1f8
    void (*settings_register)(void *);      // +0x200
    void *pad_208;                          // +0x208 (the overlay's suffix callback; unused here)
};
static_assert(sizeof(ModuleDescriptor) == 0x210, "descriptor size");
static_assert(__builtin_offsetof(ModuleDescriptor, category) == 0x10, "category");
static_assert(__builtin_offsetof(ModuleDescriptor, enabled) == 0x14, "enabled");
static_assert(__builtin_offsetof(ModuleDescriptor, handlers) == 0x48, "handlers");
static_assert(__builtin_offsetof(ModuleDescriptor, settings_gui) == 0x1f8, "gui");

// ---- setting storage (the layouts the engine's visitors cast to) ----
struct SliderCfg {                 // matches widget_slider / visit_float_setting
    char pad[0x10];
    const char *label;             // +0x10
    const char *format;            // +0x18
    float step, scale, min, max, value, saved;
};
struct BoolCfg {                   // matches widget_bool / visit_bool_setting
    char pad[0x10];
    const char *label;             // +0x10
    unsigned char value, saved;    // +0x18 / +0x19
    char pad_1a[6];
};
struct ColorCfg {                  // matches widget_color / visit_color_setting
    const char *label;             // +0x00
    float r, g, b;                 // +0x08 current
    float sr, sg, sb;              // +0x14 saved
    char pad_20[8];
};
static_assert(__builtin_offsetof(SliderCfg, value) == 0x30, "slider value");
static_assert(__builtin_offsetof(BoolCfg, value) == 0x18, "bool value");
static_assert(__builtin_offsetof(ColorCfg, r) == 0x08, "colour current");


extern "C" const char str_bed_esp[] = "Bed ESP";
extern "C" const char str_highlights_beds[] = "Highlights beds.";
extern "C" const char str_bed_esp_main[] = "Main";
extern "C" const char str_bed_esp_range[] = "range";
extern "C" const char str_bed_esp_width[] = "line_width";
extern "C" const char str_bed_esp_filled[] = "Filled";
extern "C" const char str_bed_esp_color[] = "color";
extern "C" const char str_bed_esp_range_format[] = "%.0f blocks";
extern "C" const char str_bed_esp_width_format[] = "%.2f px";
extern "C" SliderCfg bed_esp_range_setting = { {}, str_bed_esp_range, str_bed_esp_range_format,
                                               1.0f, 1.0f, 1.0f, 128.0f, 32.0f, 32.0f };
extern "C" SliderCfg bed_esp_width_setting = { {}, str_bed_esp_width, str_bed_esp_width_format,
                                               0.05f, 1.0f, 0.5f, 4.0f, 2.0f, 2.0f };
// Off by default: an outline reads as a bed, a filled box reads as a slab and hides the texture under it.
extern "C" const char str_bed_esp_outline[] = "Outline";
extern "C" BoolCfg bed_esp_outline_setting = { {}, str_bed_esp_outline, 0, 0, {} };
extern "C" BoolCfg bed_esp_filled_setting = { {}, str_bed_esp_filled, 1, 1, {} };
extern "C" ColorCfg bed_esp_color_setting = { str_bed_esp_color, 0.55f, 0.0f, 1.0f, 0.55f, 0.0f, 1.0f, {} };

// ---- module state ----
static unsigned long long bed_esp_found_count;      // what the renderer draws
static unsigned long long bed_esp_pending_count;    // being collected by the running pass
static Found bed_esp_beds[100];
static Found bed_esp_pending[100];

// ---- OpenGL (same entry points Chest ESP uses) ----
extern "C" void (**ptr_to_pGlLineWidth)(float);
extern "C" void (**ptr_to_pGlColor3f)(float, float, float);
extern "C" void (**ptr_to_pGlColor4f)(float, float, float, float);
extern "C" void (**ptr_to_pGlPopMatrix)();
extern "C" void (**ptr_to_pGlPushMatrix)();
extern "C" void (**ptr_to_pGlTranslatef)(float, float, float);
extern "C" void (**ptr_to_pGlScalef)(float, float, float);
FN(void, gl_array_xyz_float, (void *h));        // set up the unit box (-1..1) vertex array
FN(void, gl_array_color1, ());                  // draw it filled
FN(void, gl_array_color2, ());                  // draw its outline
FN(void, disable_vertex_array_state, ());
FN(Vec3, java_get_block_pos_a, (void *jvm, void *block_pos));
FN(float, libc_cosf, (float x));
FN(double, libc_sin, (double x));
struct Rot2 { float yaw, pitch; };
FN(Rot2, get_entity_rotation, (void *jvm, void *entity));
extern "C" void (**ptr_to_pGlEnable)(unsigned);
extern "C" void (**ptr_to_pGlDisable)(unsigned);
enum { GL_DEPTH_TEST_CAP = 0x0B71 };

// ---- game / engine ----
FN(Vec3, java_get_entity_pos_x, (void *jvm, void *entity));
FN(int, get_block_id, (void *jvm, void *blk));
FN(Vec3, vec3_sub, (Vec3 a, Vec3 b));

// java_call_world_get_block_state already returns a Block: it calls get_block_from_pos internally before
// handing the result back. Wrapping it again breaks it, because a Block has no getBlock() of its own.
enum { BLOCK_BED = 26 };

FN(void *, java_block_pos_init, (void *jvm, int x, int y, int z));
FN(void *, java_call_world_get_block_state, (void *jvm, void *world, void *pos, int x, int y, int z));
FN(double, libc_floor, (double x));

static int bed_esp_scan_layer;        // -HALF .. +HALF while a pass is running
static int bed_esp_scan_dx, bed_esp_scan_dz;   // where the slice stopped last tick
static int bed_esp_scan_x0, bed_esp_scan_z0;
static unsigned char bed_esp_scanning;
static unsigned char bed_esp_pass_done;        // a pass has completed for bed_esp_pass_world
static void *bed_esp_pass_world;               // the world the completed pass was run in
static unsigned long long bed_esp_prune_cursor;
static unsigned bed_esp_event_logged;

enum { BED_SCAN_HALF_HEIGHT = 16 };  // 4 was too narrow: a bed one floor up or down was never seen
// Block lookups per tick. The budget is what decides how long a pass takes: a 65 x 33 x 65 box is 139,425
// cells, so at 512 a pass took 272 ticks - nearly fourteen seconds before a bed appeared. Each lookup is three
// int-field writes plus two calls. 16384 of them is a few milliseconds per tick, and a full 139,425-cell pass
// lands in nine ticks - under half a second - which is what makes the first box appear almost immediately.
enum { BED_SCAN_BUDGET = 16384 };
// Steady state: every 40 ticks, re-read this many tracked positions. The change events keep the index exact,
// so this is only a safety net for anything the events cannot report (chunk data for a neighbour's base, a
// block changed while the module was off, a packet missed during a lag spike).
enum { BED_PRUNE_PER_CYCLE = 8 };

static void bed_esp_track(int x, int y, int z) {
    for (unsigned long long i = 0; i < bed_esp_found_count; i++) {
        Vec3 p = bed_esp_beds[i].pos;
        if (p.x == (double)x && p.y == (double)y && p.z == (double)z) return;
    }
    if (bed_esp_found_count >= 100) return;
    unsigned long long k = bed_esp_found_count;
    bed_esp_beds[k].pos = Vec3{(double)x, (double)y, (double)z};
    bed_esp_beds[k].id = BLOCK_BED;
    bed_esp_beds[k].padding_1c = 0;
    bed_esp_found_count = k + 1;
    slinky_logf("bed_esp: + bed %d %d %d", x, y, z);
}

static void bed_esp_untrack(int x, int y, int z) {
    for (unsigned long long i = 0; i < bed_esp_found_count; i++) {
        Vec3 p = bed_esp_beds[i].pos;
        if (p.x != (double)x || p.y != (double)y || p.z != (double)z) continue;
        for (unsigned long long k = i + 1; k < bed_esp_found_count; k++) bed_esp_beds[k - 1] = bed_esp_beds[k];
        bed_esp_found_count--;
        slinky_logf("bed_esp: - bed %d %d %d", x, y, z);
        return;
    }
}

static void bed_esp_untrack_chunk(int cx, int cz) {
    unsigned long long kept = 0;
    for (unsigned long long i = 0; i < bed_esp_found_count; i++) {
        Vec3 p = bed_esp_beds[i].pos;
        int same = (((int)p.x) >> 4) == cx && (((int)p.z) >> 4) == cz;
        if (same) continue;
        if (kept != i) bed_esp_beds[kept] = bed_esp_beds[i];
        kept++;
    }
    if (kept != bed_esp_found_count)
        slinky_logf("bed_esp: chunk %d,%d unloaded, %llu bed(s) dropped",
                    cx, cz, bed_esp_found_count - kept);
    bed_esp_found_count = kept;
}

// ---- event 0x14: block / chunk changes ----
// Layout produced by hook_set_block_1_7, hook_set_block_1_8, hook_invalidate_chunk and hook_unload_chunk in
// src/engine/base/java_natives_world.cpp:
//   kind 0 = chunk unloaded  (v[0] = chunk x, v[1] = chunk z)
//   kind 1 = chunk dirty     (v[0] = chunk x, v[1] = chunk z)
//   kind 2 = block changed   (v[0..2] = x y z, v[3] = the block id now at that position)
struct EvWorldChange {
    long long type;      // +0x00  0x14
    void *jvm;           // +0x08
    void *world;         // +0x10  the WorldClient the change happened in
    int kind;            // +0x18
    int v[4];            // +0x1c
    int z[7];
};

extern "C" void bed_esp_on_world_change(void *event) {
    EvWorldChange *e = (EvWorldChange *)event;
    if (!e) return;
    if (bed_esp_event_logged < 12) {
        bed_esp_event_logged++;
        slinky_logf("bed_esp: ev20 kind=%d v=(%d,%d,%d,%d)", e->kind, e->v[0], e->v[1], e->v[2], e->v[3]);
    }
    switch (e->kind) {
    case 2:                                   // a single block changed
        if (e->v[3] == BLOCK_BED) bed_esp_track(e->v[0], e->v[1], e->v[2]);
        else bed_esp_untrack(e->v[0], e->v[1], e->v[2]);
        break;
    case 0:                                   // chunk unloaded
        bed_esp_untrack_chunk(e->v[0], e->v[1]);
        break;
    default:                                  // a chunk became dirty: the radial pass already covers that area
        break;
    }
}

// ---- event 5: switched on ----
struct EvToggle { char pad[8]; void *ctx; void *mod; unsigned char on; char rest[0x2f]; };
extern "C" void bed_esp_on_toggle(void *event) {
    EvToggle *e = (EvToggle *)event;
    if (!e || e->on == 0) return;
    // Whatever was collected before is not trusted: block changes that happened while the module was off were
    // never reported (a disabled module is not in the dispatch list). Start clean and re-index.
    bed_esp_found_count = 0;
    bed_esp_pending_count = 0;
    bed_esp_scanning = 0;
    bed_esp_pass_done = 0;
    bed_esp_prune_cursor = 0;
    slinky_logf("bed_esp: on, index reset");
}

static unsigned bed_esp_tick_counter;
extern "C" void bed_esp_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    if (!ctx->world || !ctx->player) {
        bed_esp_pending_count = 0;
        bed_esp_scanning = 0;
        bed_esp_pass_done = 0;
        bed_esp_found_count = 0;
        return;
    }
    JNIEnv *env = ((Jvm *)ctx->jvm)->env;
    void *pos_template = java_block_pos_init(ctx->jvm, 0, 0, 0);
    if (!pos_template) return;

    int radius = (int)bed_esp_range_setting.value;
    if (radius < 4) radius = 4;
    if (radius > 96) radius = 96;

    Vec3 my_pos = java_get_entity_pos_x(ctx->jvm, ctx->player);
    int px = (int)libc_floor(my_pos.x);
    int py = (int)libc_floor(my_pos.y);
    int pz = (int)libc_floor(my_pos.z);

    // Sample the blocks the player is actually standing on and looking at. If the bed in front of the player
    // does not show up here, the coordinates this module reads are not the player's.
    {
        static int look_logged;
        if (look_logged < 2) {
            look_logged++;
            Rot2 look = get_entity_rotation(ctx->jvm, ctx->player);
            double rad = (double)look.yaw * 0.017453292519943295;
            int fx = px - (int)(libc_sin(rad) * 2.0);
            int fz = pz + (int)(libc_cosf((float)rad) * 2.0);
            int bx = px - (int)(libc_sin(rad) * 5.0);
            int bz = pz + (int)(libc_cosf((float)rad) * 5.0);
            void *a1 = java_call_world_get_block_state(ctx->jvm, ctx->world, pos_template, px, py - 1, pz);
            void *a2 = java_call_world_get_block_state(ctx->jvm, ctx->world, pos_template, fx, py, fz);
            void *a3 = java_call_world_get_block_state(ctx->jvm, ctx->world, pos_template, bx, py, bz);
            slinky_logf("bed_esp: LOOK yaw=%.0f foot_id=%d front2(%d,%d,%d)=%d front5(%d,%d,%d)=%d",
                        look.yaw,
                        a1 ? get_block_id(ctx->jvm, a1) : -1,
                        fx, py, fz, a2 ? get_block_id(ctx->jvm, a2) : -1,
                        bx, py, bz, a3 ? get_block_id(ctx->jvm, a3) : -1);
            if (a1) (*env)->DeleteLocalRef(env, (jobject)a1);
            if (a2) (*env)->DeleteLocalRef(env, (jobject)a2);
            if (a3) (*env)->DeleteLocalRef(env, (jobject)a3);
        }
    }

    // ---- pass scheduling ----
    if (!bed_esp_scanning) {
        int reindex = 0;
        if (!bed_esp_pass_done) {
            reindex = 1;                                  // first pass for this world / since the toggle
        } else {
            int dxm = px - bed_esp_scan_x0; if (dxm < 0) dxm = -dxm;
            int dzm = pz - bed_esp_scan_z0; if (dzm < 0) dzm = -dzm;
            // Walking away from the area the index was built for: one more pass. Half the radius, so a pass
            // that takes nine ticks cannot be overtaken by walking.
            if (dxm > radius / 2 || dzm > radius / 2) reindex = 1;
        }
        if (bed_esp_pass_world != ctx->world) {
            // Different world: everything tracked belongs to the old one.
            bed_esp_pass_world = ctx->world;
            bed_esp_found_count = 0;
            bed_esp_pending_count = 0;
            bed_esp_pass_done = 0;
            bed_esp_prune_cursor = 0;
            reindex = 1;
        }
        if (!reindex) {
            // Steady state: the change events keep the index exact, so the only per-tick work left is the
            // slow prune and the position bookkeeping.
            if ((++bed_esp_tick_counter % 40u) == 0u) {
                unsigned long long n = bed_esp_found_count;
                for (int k = 0; k < BED_PRUNE_PER_CYCLE && n; k++) {
                    if (bed_esp_prune_cursor >= n) bed_esp_prune_cursor = 0;
                    unsigned long long i = bed_esp_prune_cursor;
                    Vec3 p = bed_esp_beds[i].pos;
                    void *blk = java_call_world_get_block_state(ctx->jvm, ctx->world, pos_template,
                                                                (int)p.x, (int)p.y, (int)p.z);
                    int bid = blk ? get_block_id(ctx->jvm, blk) : 0;
                    if (blk) (*env)->DeleteLocalRef(env, (jobject)blk);
                    if (bid == BLOCK_BED) { bed_esp_prune_cursor++; continue; }
                    for (unsigned long long q = i + 1; q < n; q++) bed_esp_beds[q - 1] = bed_esp_beds[q];
                    bed_esp_found_count = n - 1;
                    n--;
                    slinky_logf("bed_esp: pruned %d %d %d (id=%d)", (int)p.x, (int)p.y, (int)p.z, bid);
                }
            }
            return;
        }
        bed_esp_scanning = 1;
        bed_esp_pass_done = 0;
        bed_esp_scan_layer = 0;
        bed_esp_scan_dx = -radius;
        bed_esp_scan_dz = -radius;
        bed_esp_scan_x0 = px;
        bed_esp_scan_z0 = pz;
        bed_esp_pending_count = 0;
        return;
    }

    // One sliced step of the layer: at most BED_SCAN_BUDGET lookups, then remember the exact cell and
    // continue next tick. Whole-layer scans here were what stalled the game.
    //
    // The pass is never restarted mid-flight any more. The old code abandoned it as soon as the player left
    // the scanned box; at sprint speed that happened after ten ticks, and a pass needs more than that, so the
    // index was rebuilt from zero over and over and a bed could stay invisible for a whole game.
    static unsigned long long pass_cells;
    int budget = BED_SCAN_BUDGET;
    int dx = bed_esp_scan_dx;
    int dz = bed_esp_scan_dz;
    int y;

    // A tick may walk several layers. The loops used to end the tick the moment one layer finished, so a pass
    // could never go faster than one layer per tick no matter how big the budget was - which is why raising it
    // from 512 to 4096 made no difference. Jumping back here spends the rest of the budget on the next layer.
next_layer:
    // Layers are walked outwards from the player, not bottom to top. A bed is almost always at or one block
    // from your feet, so under the old -16..+16 order it was not even looked at until 15 of 33 layers were
    // done. Nearest-first means the layer you are standing on is the first one scanned.
    {
        int idx = bed_esp_scan_layer;
        int off = (idx & 1) ? -((idx + 1) >> 1) : (idx >> 1);
        y = py + off;
    }
    for (; dx <= radius; dx++) {
        int x = bed_esp_scan_x0 + dx;
        for (; dz <= radius; dz++) {
            int z = bed_esp_scan_z0 + dz;
            void *blk = java_call_world_get_block_state(ctx->jvm, ctx->world, pos_template, x, y, z);

            // get_block_from_pos hands back a fresh JNI local reference on every call and the budget is 16384 a
            // tick - far past the local-reference table's default size. Past that the lookups start coming back
            // wrong, which is why a pass could run to completion and only ever see dirt, grass, bedrock and air
            // while the block under the player was wool. Read the id, release the reference, move on.
            int blk_id = blk ? get_block_id(ctx->jvm, blk) : 0;
            if (blk) (*env)->DeleteLocalRef(env, (jobject)blk);
            pass_cells++;

            if (blk_id == BLOCK_BED) {
                bed_esp_track(x, y, z);
                // Also queue it for this pass's publish step below, which drops positions that stopped being
                // beds once the pass is complete.
                if (bed_esp_pending_count < 100) {
                    unsigned long long k = bed_esp_pending_count;
                    bed_esp_pending_count = k + 1;
                    bed_esp_pending[k].pos = Vec3{(double)x, (double)y, (double)z};
                    bed_esp_pending[k].id = BLOCK_BED;
                    bed_esp_pending[k].padding_1c = 0;
                }
            }
            if (--budget <= 0) {
                // Resume exactly here next tick. When the budget runs out on the last column of a row, the
                // cursor moves to the next row instead of being saved one past the end - saving radius + 1 made
                // the next tick skip its inner loop entirely, which wasted a tick per row.
                if (dz >= radius) {
                    bed_esp_scan_dx = dx + 1;
                    bed_esp_scan_dz = -radius;
                } else {
                    bed_esp_scan_dx = dx;
                    bed_esp_scan_dz = dz + 1;
                }
                return;
            }
        }
        dz = -radius;
    }

    // Publish what the tick found straight away. Waiting for the whole 33-layer pass meant the first box only
    // appeared after 36 ticks (about 1.8 s).
    for (unsigned long long i = 0; i < bed_esp_pending_count; i++) {
        Vec3 pos = bed_esp_pending[i].pos;
        int duplicate = 0;
        for (unsigned long long k = 0; k < bed_esp_found_count; k++) {
            if (bed_esp_beds[k].pos.x == pos.x && bed_esp_beds[k].pos.y == pos.y &&
                bed_esp_beds[k].pos.z == pos.z) { duplicate = 1; break; }
        }
        if (duplicate) continue;
        if (bed_esp_found_count >= 100) break;
        bed_esp_beds[bed_esp_found_count] = bed_esp_pending[i];
        bed_esp_found_count++;
    }

    // The layer is finished, so the cursor has to go back to the start of the grid for the next one. This reset
    // used to live only inside the pass-completed branch below, so every layer after the first resumed from
    // whatever mid-row cursor the previous layer had left behind and scanned only the right-hand part of the
    // grid. A whole pass ended up covering about six percent of its cells, which is why a bed two blocks away
    // was never seen.
    bed_esp_scan_dx = -radius;
    bed_esp_scan_dz = -radius;
    dx = -radius;
    dz = -radius;

    if (++bed_esp_scan_layer > BED_SCAN_HALF_HEIGHT * 2) {
        bed_esp_scanning = 0;

        // The pass is complete: verified positions that stopped being beds, then stop. New beds arrive through
        // event 0x14 from here on, and the prune above catches anything the events could not report.
        {
            unsigned long long kept = 0;
            for (unsigned long long i = 0; i < bed_esp_found_count; i++) {
                int bx = (int)bed_esp_beds[i].pos.x;
                int by = (int)bed_esp_beds[i].pos.y;
                int bz = (int)bed_esp_beds[i].pos.z;
                void *blk = java_call_world_get_block_state(ctx->jvm, ctx->world, pos_template, bx, by, bz);
                int bid = blk ? get_block_id(ctx->jvm, blk) : 0;
                if (blk) (*env)->DeleteLocalRef(env, (jobject)blk);
                if (bid == BLOCK_BED) {
                    if (kept != i) bed_esp_beds[kept] = bed_esp_beds[i];
                    kept++;
                }
            }
            if (kept != bed_esp_found_count)
                slinky_logf("bed_esp: %llu bed(s) gone, %llu left",
                            bed_esp_found_count - kept, kept);
            bed_esp_found_count = kept;
        }

        slinky_logf("bed_esp: pass done, %llu tracked, %llu cells (centre %d,%d,%d r=%d y+-%d)",
                    bed_esp_found_count, pass_cells,
                    bed_esp_scan_x0, bed_esp_scan_z0, py, radius, BED_SCAN_HALF_HEIGHT);
        pass_cells = 0;
        bed_esp_pass_done = 1;
        return;
    }
    if (budget > 0) goto next_layer;
}

// Every frame: draw the boxes. The handler's +8 holds the camera position, like Chest ESP's.
extern "C" void bed_esp_on_render_world(char *h) {
    // Same shape as Chest ESP: set the vertex array up every frame, always tear it down. An early return here
    // used to skip both, so on frames with nothing found the shared array state was left to whoever touched it
    // last.
    gl_array_xyz_float(h);
    (*ptr_to_pGlLineWidth)(bed_esp_width_setting.value);
    // Depth test off while drawing. A bed sits on the floor and is normally surrounded by blocks - obsidian,
    // wool, the enemy team's wall - so with depth testing on every outline behind a block is simply not drawn
    // and the module looks broken exactly when it matters. Turning it off for the pass is how the name tags
    // renderer handles the same problem.
    (*ptr_to_pGlDisable)(GL_DEPTH_TEST_CAP);
    Vec3 *camera = (Vec3 *)(h + 8);
    float *colour = &bed_esp_color_setting.r;
    unsigned char filled = bed_esp_filled_setting.value;
    unsigned char outline = bed_esp_outline_setting.value;
    for (unsigned long long i = 0; i < bed_esp_found_count; i++) {
        Vec3 p = bed_esp_beds[i].pos;

        // A bed's model is 9/16 of a block tall, not a full block. A 1x1x1 box floats well above the mattress
        // and reads as a cube hovering over the bed instead of a marker on it.
        const float bed_half_h = 0.5625f * 0.5f;           // half of 9/16
        Vec3 centre = Vec3{p.x + 0.5, p.y + (double)bed_half_h, p.z + 0.5};
        float sx = 0.5f, sz = 0.5f;
        for (unsigned long long k = 0; k < bed_esp_found_count; k++) {
            if (k == i) continue;
            Vec3 q = bed_esp_beds[k].pos;
            if (q.y != p.y) continue;
            double dx = q.x - p.x;
            double dz = q.z - p.z;
            if (dx == 1.0 && dz == 0.0) {
                centre = Vec3{p.x + 1.0, p.y + (double)bed_half_h, p.z + 0.5};
                sx = 1.0f;
                break;
            }
            if (dz == 1.0 && dx == 0.0) {
                centre = Vec3{p.x + 0.5, p.y + (double)bed_half_h, p.z + 1.0};
                sz = 1.0f;
                break;
            }
        }
        centre = vec3_sub(centre, *camera);

        (*ptr_to_pGlPushMatrix)();
        (*ptr_to_pGlTranslatef)((float)centre.x, (float)centre.y, (float)centre.z);
        (*ptr_to_pGlScalef)(sx, bed_half_h, sz);
        // Filled by default, like Myau's drawFilledBox; the wireframe is a separate switch so a filled box is
        // not double-drawn with an outline sitting on its own surface.
        if (filled == 1) {
            (*ptr_to_pGlColor4f)(colour[0], colour[1], colour[2], 0.4f);
            gl_array_color1();
        }
        if (outline == 1) {
            (*ptr_to_pGlColor3f)(colour[0], colour[1], colour[2]);
            gl_array_color2();
        }
        (*ptr_to_pGlPopMatrix)();
    }
    disable_vertex_array_state();
    (*ptr_to_pGlEnable)(GL_DEPTH_TEST_CAP);
    if (bed_esp_found_count != bed_esp_last_reported) {
        bed_esp_last_reported = bed_esp_found_count;
        slinky_logf("bed_esp: drawing %llu", bed_esp_found_count);
    }
}

// ---- settings page / registration ----
extern "C" void bed_esp_gui(void *gui, void *window) {
    W_label(gui, window, str_bed_esp_main);
    W_slider(window, gui, &bed_esp_range_setting);
    W_slider(window, gui, &bed_esp_width_setting);
    W_bool(window, gui, &bed_esp_filled_setting);
    W_bool(window, gui, &bed_esp_outline_setting);
    W_color(window, gui, &bed_esp_color_setting);
}
extern "C" void bed_esp_register(void *module) {
    R_num(module, str_bed_esp_range, &bed_esp_range_setting);
    R_num(module, str_bed_esp_width, &bed_esp_width_setting);
    R_bool(module, str_bed_esp_filled, &bed_esp_filled_setting);
    R_bool(module, str_bed_esp_outline, &bed_esp_outline_setting);
    R_key(module, str_bed_esp_color, &bed_esp_color_setting);
}

extern "C" ModuleDescriptor bed_esp_module_descriptor = {
    str_bed_esp,
    str_highlights_beds,
    2,                      // category: Visual
    0,                      // starts off
    {0, 0, 0},
    0,                      // flags: ordinary module (saved in the profile, has a toggle knob)
    0,                      // no default bind
    0,                      // no hold mode
    {},
    {   // one line per event slot: the index below speaks for itself
        { (void *)bed_esp_on_tick,         0 },   //  0 tick
        {0, 0},                                    //  1
        {0, 0},                                    //  2
        {0, 0},                                    //  3
        {0, 0},                                    //  4
        { (void *)bed_esp_on_toggle,       0 },   //  5 toggle
        { (void *)bed_esp_on_render_world, 0 },   //  6 world render
        {0, 0},                                    //  7
        {0, 0},                                    //  8
        {0, 0},                                    //  9
        {0, 0},                                    // 10
        {0, 0},                                    // 11
        {0, 0},                                    // 12
        {0, 0},                                    // 13
        {0, 0},                                    // 14
        {0, 0},                                    // 15
        {0, 0},                                    // 16
        {0, 0},                                    // 17
        {0, 0},                                    // 18
        {0, 0},                                    // 19
        { (void *)bed_esp_on_world_change, 0 },   // 20 world change (event 0x14)
        {0, 0},                                    // 21
        {0, 0},                                    // 22
        {0, 0},                                    // 23
        {0, 0},                                    // 24
        {0, 0},                                    // 25
        {0, 0},                                    // 26
    },
    bed_esp_gui,
    bed_esp_register,
    0,
};
