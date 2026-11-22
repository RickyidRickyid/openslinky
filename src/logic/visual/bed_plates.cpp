// Bed Plates — how well each bed is protected, drawn on the bed itself.
//
// RECONSTRUCTION ADDITION (2026-11-21). Ported from the shape of LiquidBounce's / Grizzly's BedPlates:
// Bed Wars players need to know what a bed is wrapped in before they commit to it, and walking
// around the defence to count blocks is the thing this saves you from.
//
// The original clients put a small TEXT panel above each bed ("2 wool, 1 wood, 3 obsidian"). This
// build has no world-to-screen text path in the module layer - the GUI font renderer lives in the
// 2D overlay, and reaching into it from a world-render handler is a much larger change than the
// feature is worth. So the same information is encoded as COLOUR on a filled plate over the bed:
//
//    green   nothing around it - walk up and break it
//    yellow  a light wrap (up to 6 blocks, no obsidian)
//    orange  a real defence (7-14 blocks, or any blast-resistant block)
//    red     obsidian or bedrock present - needs a diamond pick or an explosion
//
// The numbers behind the colour are logged (verbose), so the classification can be checked.
//
// WHERE THE BEDS COME FROM: Bed ESP. That module already maintains a live list through the chunk
// events, so this one does not run a second sweep - it reads the list and inspects the blocks
// around each entry, four beds per tick on a rotating cursor. A full pass over 100 beds therefore
// takes 25 ticks, which is far more often than a defence changes.

#include "slinky/engine.h"

struct Vec3 { double x, y, z; };

extern "C" void slinky_logf(const char *fmt, ...);

FN(Vec3, java_get_block_pos_a, (void *jvm, void *block_pos));
FN(void *, java_block_pos_init, (void *jvm, int x, int y, int z));
FN(void *, java_call_world_get_block_state, (void *jvm, void *world, void *pos, int x, int y, int z));
FN(int, get_block_id, (void *jvm, void *blk));

extern "C" unsigned long long bed_esp_get_beds(Vec3 *out, unsigned long long max);
// Bed ESP's sweep. Its bed list is only advanced by its own tick handler, which the engine never
// calls while that module is off - so a Bed Plates that merely read the list would always see zero
// beds unless the user also happened to enable Bed ESP. Driving its step from here costs one extra
// slice of scanning when both modules are on (harmless: the sweep just finishes sooner).
extern "C" void bed_esp_on_tick(void *event);
extern "C" void (**ptr_to_pGlLineWidth)(float);
extern "C" void (**ptr_to_pGlColor4f)(float, float, float, float);
extern "C" void (**ptr_to_pGlColor3f)(float, float, float);
extern "C" void (**ptr_to_pGlPushMatrix)();
extern "C" void (**ptr_to_pGlPopMatrix)();
extern "C" void (**ptr_to_pGlTranslatef)(float, float, float);
extern "C" void (**ptr_to_pGlScalef)(float, float, float);
extern "C" void (**ptr_to_pGlEnable)(unsigned);
extern "C" void (**ptr_to_pGlDisable)(unsigned);
FN(void, gl_array_xyz_float, (void *h));
FN(void, gl_array_color1, ());
FN(void, gl_array_color2, ());
FN(void, disable_vertex_array_state, ());
enum { GL_DEPTH_TEST_CAP = 0x0B71 };

// Copied from bed_esp.cpp: each module carries its own copies of these, there is no shared header.
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
    HSlotCfg handlers[27];                  // +0x048
    void (*settings_gui)(void *, void *);   // +0x1f8
    void (*settings_register)(void *);      // +0x200
    void *pad_208;                          // +0x208
};
static_assert(sizeof(ModuleDescriptor) == 0x210, "descriptor size");

// Block ids (1.8.9). Only the ones that meaningfully change how a bed has to be broken are named.
enum {
    ID_AIR = 0,
    ID_OBSIDIAN = 49,
    ID_BEDROCK = 7,
    ID_ENDER_STONE = 121,
    ID_WOOL_WHITE = 35,
};

struct Info {
    int solid;
    int hard;       // obsidian / bedrock / end stone
    unsigned char level;
};

static Vec3 g_beds[100];
static struct Info g_info[100];
static unsigned long long g_count;
static unsigned long long g_cursor;
static unsigned long long g_last_report;

// ---- settings ---------------------------------------------------------------

extern "C" const char str_bed_plates[] = "Bed Plates";
extern "C" const char str_bed_plates_desc[] = "Colours a bed by how well it is protected.";
extern "C" const char str_bed_plates_main[] = "Main";
extern "C" const char str_bed_plates_range[] = "range";
extern "C" const char str_bed_plates_width[] = "line_width";
extern "C" const char str_bed_plates_filled[] = "Filled";
extern "C" const char str_bed_plates_labels[] = "Log levels";
extern "C" const char str_bed_plates_fmt0[] = "%.0f";

struct PlatesColour {
    char pad[0x10];
    const char *label;
    float r, g, b;
    char rest[0x14];
};

struct PlatesSlider {
    char pad[0x10];
    const char *label;
    const char *format;
    float step, scale, min, max, value, saved;
};

struct PlatesBool {
    char pad[0x10];
    const char *label;
    unsigned char value, saved;
    char pad_1a[6];
};

// step, scale, min, max, value, saved - all six are required. Leaving the tail off leaves
// max = value = 0, which reads as "range 0" and draws nothing at all.
static PlatesSlider g_range = { {}, str_bed_plates_range, str_bed_plates_fmt0,
                                1.0f, 1.0f, 4.0f, 128.0f, 128.0f, 128.0f };
static PlatesSlider g_width = { {}, str_bed_plates_width, str_bed_plates_fmt0,
                                0.05f, 1.0f, 0.5f, 4.0f, 2.0f, 2.0f };
static PlatesBool g_filled = { {}, str_bed_plates_filled, 1, 1, {} };
static PlatesBool g_labels = { {}, str_bed_plates_labels, 0, 0, {} };

// ---- classification ---------------------------------------------------------

// 26 positions around the bed: the six faces plus the ring above it. A Bed Wars defence is built
// exactly there, so counting that shell is enough and keeps the cost down.
static void classify(Ctx *ctx, Vec3 p, Info *out) {
    out->solid = 0;
    out->hard = 0;

    void *pos = java_block_pos_init(ctx->jvm, 0, 0, 0);
    if (!pos)
        return;

    int bx = (int)p.x, by = (int)p.y, bz = (int)p.z;
    static const int offs[26][3] = {
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
        {-1, 0, -1}, {-1, 0, 1}, {1, 0, -1}, {1, 0, 1},
        {-2, 0, 0}, {2, 0, 0}, {0, 0, -2}, {0, 0, 2},
        {-1, -1, 0}, {1, -1, 0}, {0, -1, -1}, {0, -1, 1},
        {-1, 1, 0}, {1, 1, 0}, {0, 1, -1}, {0, 1, 1},
        {-2, 1, 0}, {2, 1, 0}, {0, 1, -2}, {0, 1, 2},
    };

    for (int i = 0; i < 26; i++) {
        void *blk = java_call_world_get_block_state(ctx->jvm, ctx->world, pos, bx + offs[i][0],
                                                    by + offs[i][1], bz + offs[i][2]);
        if (!blk)
            continue;
        int id = get_block_id(ctx->jvm, blk);
        if (id == ID_AIR)
            continue;
        out->solid++;
        if (id == ID_OBSIDIAN || id == ID_BEDROCK || id == ID_ENDER_STONE)
            out->hard++;
    }

    if (out->hard > 0)
        out->level = 3;                     // obsidian or the like: not a wooden pick job
    else if (out->solid > 14)
        out->level = 3;
    else if (out->solid > 6)
        out->level = 2;
    else if (out->solid > 0)
        out->level = 1;
    else
        out->level = 0;
}

// ---- events -----------------------------------------------------------------

struct Ev {
    char pad[8];
    Ctx *ctx;
};

extern "C" void bed_plates_tick(void *event) {
    Ev *e = (Ev *)event;
    if (!e || !e->ctx || !e->ctx->world)
        return;

    bed_esp_on_tick(event);                 // advance the bed sweep that supplies our list
    g_count = bed_esp_get_beds(g_beds, 100);
    {
        static unsigned long long dbg;
        if (++dbg % 40 == 0)
            slinky_logf("bed_plates: tick count=%llu first=(%.0f,%.0f,%.0f)", g_count,
                        g_count ? g_beds[0].x : 0.0, g_count ? g_beds[0].y : 0.0,
                        g_count ? g_beds[0].z : 0.0);
    }
    if (g_count == 0)
        return;

    // Four beds per tick on a rotating cursor: 26 world reads each, so a hundred beds costs 104
    // getBlockState calls a tick spread across 25 of them. Classifying all of them every tick would
    // be 2600 calls and would show up as a stutter.
    for (int k = 0; k < 4 && g_count > 0; k++) {
        unsigned long long i = g_cursor % g_count;
        g_cursor++;
        classify(e->ctx, g_beds[i], &g_info[i]);
    }

    if (g_labels.value == 1) {
        unsigned long long now = 0;
        (void)now;
        if (g_cursor - g_last_report >= g_count * 2ULL) {
            g_last_report = g_cursor;
            for (unsigned long long i = 0; i < g_count && i < 4; i++)
                slinky_logf("bed_plates: bed %llu at (%.0f,%.0f,%.0f) solid=%d hard=%d level=%d",
                            i, g_beds[i].x, g_beds[i].y, g_beds[i].z, g_info[i].solid,
                            g_info[i].hard, (int)g_info[i].level);
        }
    }
}

extern "C" void bed_plates_on_toggle(void *event) {
    g_cursor = 0;
    g_last_report = 0;
    for (int i = 0; i < 100; i++) {
        g_info[i].solid = 0;
        g_info[i].hard = 0;
        g_info[i].level = 0;
    }
}

extern "C" void bed_plates_render_world(char *h) {
    gl_array_xyz_float(h);
    (*ptr_to_pGlLineWidth)(g_width.value);
    // No depth test: a defended bed is surrounded by blocks, and half the point of this module is
    // seeing it through them.
    (*ptr_to_pGlDisable)(GL_DEPTH_TEST_CAP);

    Vec3 *camera = (Vec3 *)(h + 8);
    double range = (double)g_range.value;
    unsigned char filled = g_filled.value;
    {
        static unsigned long long dbg;
        if (++dbg % 120 == 0)
            slinky_logf("bed_plates: render count=%llu range=%.0f cam=(%.0f,%.0f,%.0f)", g_count,
                        range, camera->x, camera->y, camera->z);
    }

    for (unsigned long long i = 0; i < g_count; i++) {
        Vec3 p = g_beds[i];
        double dx = p.x + 0.5 - camera->x;
        double dy = p.y + 0.5 - camera->y;
        double dz = p.z + 0.5 - camera->z;
        if (dx * dx + dy * dy + dz * dz > range * range)
            continue;

        float r, g, b;
        switch (g_info[i].level) {
        case 0: r = 0.20f; g = 1.00f; b = 0.40f; break;   // bare
        case 1: r = 1.00f; g = 0.90f; b = 0.20f; break;   // light wrap
        case 2: r = 1.00f; g = 0.55f; b = 0.10f; break;   // real defence
        default: r = 1.00f; g = 0.20f; b = 0.20f; break;  // obsidian
        }

        (*ptr_to_pGlPushMatrix)();
        // CAMERA-RELATIVE, not world coordinates. The modelview matrix already carries the
        // camera transform, so translating by a world position parks the plate hundreds of
        // blocks from anywhere the player is looking - which is why nothing ever showed up.
        (*ptr_to_pGlTranslatef)((float)dx, (float)dy, (float)dz);
        (*ptr_to_pGlScalef)(0.52f, 0.42f, 0.52f);
        if (filled) {
            (*ptr_to_pGlColor4f)(r, g, b, 0.35f);
            gl_array_color1();          // filled box
        } else {
            (*ptr_to_pGlColor3f)(r, g, b);
            gl_array_color2();          // wireframe
        }
        (*ptr_to_pGlPopMatrix)();
    }

    disable_vertex_array_state();
    (*ptr_to_pGlEnable)(GL_DEPTH_TEST_CAP);
}

// ---- GUI --------------------------------------------------------------------

extern "C" void bed_plates_gui(void *gui, void *window) {
    W_label(gui, window, str_bed_plates_main);
    W_slider(window, gui, &g_range);
    W_slider(window, gui, &g_width);
    W_bool(window, gui, &g_filled);
    W_bool(window, gui, &g_labels);
}

extern "C" void bed_plates_register(void *module) {
    R_num(module, str_bed_plates_range, &g_range);
    R_num(module, str_bed_plates_width, &g_width);
    R_bool(module, str_bed_plates_filled, &g_filled);
    R_bool(module, str_bed_plates_labels, &g_labels);
}

// ---- descriptor -------------------------------------------------------------

extern "C" ModuleDescriptor bed_plates_module_descriptor = {
    str_bed_plates,
    str_bed_plates_desc,
    2,                      // category: Visual
    0,                      // starts off
    {0, 0, 0},
    0,                      // ordinary module (saved in the profile, has a toggle knob)
    0,                      // no default bind
    0,                      // no hold mode
    {},
        {   // one line per event slot
        { (void *)bed_plates_tick,         0 },   //  0 tick
        {0, 0}, {0, 0}, {0, 0}, {0, 0},            //  1-4
        { (void *)bed_plates_on_toggle,    0 },   //  5 toggle
        { (void *)bed_plates_render_world, 0 },   //  6 world render
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},    //  7-11
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},    // 12-16
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},    // 17-21
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},    // 22-26
    },
    bed_plates_gui,
    bed_plates_register,
    0,
};
