// Inv Manager - sort your inventory automatically while it is open.
//
// For each armour slot and each hotbar slot, the settings say which kind of item belongs
// there (sword, blocks, bow, ...). While the inventory is open, the module finds the best
// such item, and clicks it into place: shift-click for armour, "number key" swaps for the
// hotbar, and pick-up / put-down for merging stacks. Clicks are spaced by "click_delay",
// longer for longer mouse moves ("scale_by_mouse_distance") and at random ("randomise").
//
// Functions:
//   inv_manager_score_item_stack  how good an item is for a slot
//   inv_manager_tick              every tick: the next click
//   inv_manager_plan_step         helper: find the best item for one hotbar slot
#include "slinky/engine.h"
extern "C" void slinky_logf(const char *fmt, ...);
#include "slinky/jids.h"

// ---- settings ----
extern "C" unsigned char inv_manager_try_paired_slot;   // "swap_sides": also try the other hand's / side's kind
extern "C" char inv_manager_enum_slot_1[];               // per hotbar slot settings, 0x40 bytes each (+0x30 wanted kind)
extern "C" float inv_manager_start_delay;                // ticks after opening the inventory
extern "C" float inv_manager_click_delay_b, inv_manager_click_delay_b_second;   // click delay = first / second
extern "C" unsigned char inv_manager_randomise;
extern "C" unsigned char inv_manager_scale_by_mouse_distance;

// ---- game / engine functions ----
FN(void *, java_call_item_stack_get_item, (void *jvm, void *stack));
FN(int, java_call_static_item_get_id_from_item, (void *jvm, void *item));
FN(int, java_get_item_stack_metadata, (void *jvm, void *stack));
FN(int, java_get_item_stack_stack_size, (void *jvm, void *stack));
FN(void *, java_call_item_stack_get_enchantment_tag_list, (void *jvm, void *stack));
FN(unsigned char, java_call_item_stack_has_display_name, (void *jvm, void *stack));
FN(unsigned short, score_item_inventory_sorter_category, (void **s, int slot, int id, int dmg, void *nbt, unsigned char flag));   // score for a kind (0 = does not fit)

// How good the item stack is as kind `kind`. With "swap_sides", an item that does not fit
// may still score (lower) as the paired kind.
extern "C" unsigned inv_manager_score_item_stack(void **s, void *stack, int kind) {
    void *item = java_call_item_stack_get_item(*s, stack);
    if (!item)
        return 0;
    int item_id = java_call_static_item_get_id_from_item(*s, item);
    int damage = java_get_item_stack_metadata(*s, stack);
    void *enchantments = java_call_item_stack_get_enchantment_tag_list(*s, stack);
    unsigned char named = java_call_item_stack_has_display_name(*s, stack);
    unsigned score = score_item_inventory_sorter_category(s, kind, item_id, damage, enchantments, named);
    if (inv_manager_try_paired_slot != 1)
        return score;
    if (score)
        return score << 16;   // a real fit always beats a paired one
    int paired;
    switch (kind) {
    case 22: paired = 25; break;
    case 17: paired = 7; break;
    case 7: paired = 17; break;
    default: return 0;
    }
    return score_item_inventory_sorter_category(s, paired, item_id, damage, enchantments, named);
}

struct Pair { float x, y; };
struct Range { float lo, hi; };

// The module's progress (all in one block of memory).
struct InvState {
    int state;                      // what we are doing (see below)
    int padding_04;
    unsigned long long mask;        // targets already handled
    int action;                     // the planned click: 0 shift-click, 1 swap with hotbar `slot`, 2 pick up and put in `slot`
    unsigned char slot, padding_15, padding_16, padding_17;
    int saved;                      // state to go back to after the click
    unsigned char sub;              // current target within the state
    unsigned char window;           // window slot to click
    unsigned char advance;          // go to the next target after the click
    unsigned char padding_1f;
    float delay;                    // ticks until the next click
    int padding_24;
    Pair pos;                       // where the "mouse" is (for the delay)
};
enum InvStep {
    STEP_ARMOUR = 0,      // plan the armour slots
    STEP_HOTBAR = 1,      // plan the hotbar slots
    STEP_CLEAN = 2,       // drop the junk that survived the sort ("Inventory cleaner")
    STEP_DONE = 3,
    STEP_CLICK = 6,       // do the planned click
    STEP_PUT_DOWN = 7,    // (pick up action) put the stack down
    STEP_DROP_REST = 8,   // drop what is left on the cursor
};

// The mode switch lives on Chest Stealer, which has a real Myau/Expo dropdown. The Inv Manager card has no
// dropdown of its own - its GUI is the shipped one and cannot gain a control - and the "Instant" switch on that
// card is a free-standing object nothing in the GUI ever writes to, so toggling it changed nothing. Following
// Chest Stealer's Mode instead means one place to switch both, and it actually works.
extern "C" unsigned char chest_stealer_mode_setting[];   // enum: 0 Myau, 1 Expo
static int inv_is_expo(void) { return *(int *)(chest_stealer_mode_setting + 0x30) != 0; }

// ---- settings added by the cleaner port (storage lives here, the GUI only shows them) ----
struct SliderCfg {                 // matches widget_slider / visit_float_setting
    char pad[0x10];
    const char *label;
    const char *format;
    float step, scale, min, max, value, saved;
};
struct BoolCfg {                   // matches widget_bool / visit_bool_setting
    char pad[0x10];
    const char *label;
    unsigned char value, saved;
    char pad_1a[6];
};
extern "C" const char str_cleaner_label[] = "Inventory cleaner";
extern "C" const char str_cleaner_format[] = "%.0f ms";
extern "C" const char str_close_inventory_label[] = "Close inventory";
extern "C" const char str_middle_clean_label[] = "Middle click to clean";
extern "C" const char str_cleaner_key[] = "inventory_cleaner";
extern "C" const char str_close_inventory_key[] = "close_inventory";
extern "C" const char str_middle_clean_key[] = "middle_click_to_clean";
// Range starts at 1, not 0. The value is the gap between two drops, so 0 reads as "no delay, fastest" -
// which is what it now means. It used to mean "cleaning off", an invisible second meaning on a control whose
// label says nothing about on/off, and the only way to find out was that nothing was ever dropped.
extern "C" SliderCfg inv_manager_cleaner_setting = { {}, str_cleaner_label, str_cleaner_format,
                                                     1.0f, 1.0f, 1.0f, 500.0f, 60.0f, 60.0f };
extern "C" BoolCfg inv_manager_close_inventory_setting = { {}, str_close_inventory_label, 1, 1, {} };
extern "C" BoolCfg inv_manager_middle_clean_setting = { {}, str_middle_clean_label, 0, 0, {} };
// Reference defaults: Blocks 128 (range 64..2304), Throws Amount 64 (range 16..320), KeepOre true.
extern "C" const char str_blocks_label[] = "Max blocks";
extern "C" const char str_throws_label[] = "Max throws";
extern "C" const char str_keep_ore_label[] = "Keep ores";
extern "C" const char str_plain_number_format[] = "%.0f";
extern "C" const char str_blocks_key[] = "max_blocks";
extern "C" const char str_throws_key[] = "max_throws";
extern "C" const char str_keep_ore_key[] = "keep_ores";
extern "C" SliderCfg inv_manager_blocks_setting = { {}, str_blocks_label, str_plain_number_format,
                                                    1.0f, 1.0f, 64.0f, 2304.0f, 128.0f, 128.0f };
extern "C" SliderCfg inv_manager_throws_setting = { {}, str_throws_label, str_plain_number_format,
                                                    1.0f, 1.0f, 16.0f, 320.0f, 64.0f, 64.0f };
extern "C" BoolCfg inv_manager_keep_ore_setting = { {}, str_keep_ore_label, 1, 1, {} };
static float cleaner_delay() {
    // Clamped, not treated as "off". A profile written before the range change can still carry a 0, and the
    // honest reading of 0 here is "no delay", so it becomes the fastest setting instead of silently disabling
    // the whole cleaner.
    float d = inv_manager_cleaner_setting.value;
    return d < 0.0f ? 0.0f : d;    // the throw branch already spaces it one per tick
}
static bool middle_click_clean() { return inv_manager_middle_clean_setting.value == 1; }
static bool close_inventory_when_done() { return inv_manager_close_inventory_setting.value == 1; }
static int block_budget() { return (int)inv_manager_blocks_setting.value; }
static int throw_budget() { return (int)inv_manager_throws_setting.value; }
static bool keep_ores() { return inv_manager_keep_ore_setting.value == 1; }
// The label spells out what the switch does now that the two modes have names. On = the Expo behaviour
// (run the whole plan in one tick), off = the Myau behaviour (one click per tick with a wait).
extern "C" const char str_instant_label[] = "Mode follows Chest Stealer";
extern "C" const char str_instant_key[] = "instant";
extern "C" BoolCfg inv_manager_instant_setting = { {}, str_instant_label, 1, 1, {} };   // whole plan in one tick

struct InvHook {
    char padding_00[8];
    Ctx *ctx;               // +0x08 game context
    unsigned char busy;     // +0x10 set to 1: we use the inventory this tick
};

// A planning function for one target: true when it planned a click.
typedef unsigned char (*Step)(Ctx *c, InvState *st);

extern "C" InvState inv_manager_rotation_walk;                    // the progress
extern "C" unsigned char *const inv_manager_slot_setting_ptrs[2]; // armour / hotbar part enabled
extern "C" const unsigned char inv_manager_targets_per_state[2];  // number of targets: armour, hotbar
extern "C" const Step ptr_to_plan_inventory_swap[2];              // planning function: armour, hotbar (inv_manager_plan_step)

FN(void *, get_current_screen, (void *jvm, void *object));
FN(unsigned char, java_jni_is_instance_of_gui_inventory, (void *jvm, void *g));
FN(void, java_gui_container_handle_mouse_click, (void *jvm, void *g, int slot, int button, int mode));
FN(int, window_click, (Ctx *ctx, int slot, int button, int mode));
FN(int, window_click_available, ());
FN(Pair, slot_index, (int slot));                     // screen position of a window slot
FN(float, rot_delta_length, (Pair a, Pair b));        // distance between two positions
FN(Range, rot_make_floats, (float lo, float hi));
FN(float, random_rotation, (Range r));                // random value in the range
FN(void *, java_get_entity_player_inventory, (void *jvm, void *entity_player));
FN(void *, java_call_iinventory_get_stack_in_slot, (void *jvm, void *inventory, int slot));
FN(void *, java_call_item_stack_get_item, (void *jvm, void *stack));
FN(int, java_call_static_item_get_id_from_item, (void *jvm, void *item));
FN(bool, java_call_static_mouse_is_button_down, (void *jvm, int button));
FN(void, refill_disable_and_close_screen, (Ctx *ctx));

// ---------------------------------------------------------------------------
// Cleaner, ported from Myau's InvManager (OpenMyau-Plus / MyauReborn).
//
// The reference does not ask "is this item useful" per slot. It first computes a KEEP SET - the single best
// stack for each armour type and for each hotbar class the user configured - and then throws out everything
// in the main inventory that is not in that set and is not special. A spare sword, a spare axe or an
// outclassed armour piece is not special, so it lands in the drop list: that is what makes old gear vanish
// once a better one is carried. Keeping every sword forever (the previous rule) was the bug.
// ---------------------------------------------------------------------------
enum {
    CLASS_JUNK = 0,
    CLASS_SWORD, CLASS_PICKAXE, CLASS_SHOVEL, CLASS_AXE,
    CLASS_HELMET, CLASS_CHESTPLATE, CLASS_LEGGINGS, CLASS_BOOTS,
    CLASS_BOW, CLASS_BLOCK, CLASS_THROWABLE, CLASS_GAPPLE, CLASS_ORE, CLASS_PEARL
};

// 1.8.9 item ids are fixed, so the class can be decided from the id alone.
static int item_class(int id) {
    if (id <= 0) return CLASS_JUNK;
    switch (id) {
    case 267: case 268: case 272: case 276: case 283: return CLASS_SWORD;
    case 270: case 274: case 257: case 278: case 285: return CLASS_PICKAXE;
    case 269: case 273: case 256: case 277: case 284: return CLASS_SHOVEL;
    case 271: case 275: case 258: case 279: case 286: return CLASS_AXE;
    case 298: case 302: case 306: case 310: case 314: return CLASS_HELMET;
    case 299: case 303: case 307: case 311: case 315: return CLASS_CHESTPLATE;
    case 300: case 304: case 308: case 312: case 316: return CLASS_LEGGINGS;
    case 301: case 305: case 309: case 313: case 317: return CLASS_BOOTS;
    case 261: return CLASS_BOW;
    case 332: case 344: return CLASS_THROWABLE;                       // snowball, egg
    case 322: case 466: return CLASS_GAPPLE;                          // golden / enchanted golden apple
    case 368: return CLASS_PEARL;
    case 264: case 388: case 265: case 266: case 263: case 406: case 331: return CLASS_ORE;
    default: break;
    }
    if (id < 0x100) return CLASS_BLOCK;
    return CLASS_JUNK;
}

// Stacks the cleaner must never throw out. Mirrors the reference: useful potions, ender pearls, food, spawn
// eggs and the nether star. Anything else that is not in the keep set is junk.
static bool is_special_item(int id, int meta) {
    if (id == 373) {                                                  // potion: useful effects only
        switch (meta & 0xf) {
        case 1: case 2: case 3: case 5: case 6: case 8: case 10: case 12: case 13: return true;
        default: return false;
        }
    }
    if (id == 368) return true;                                       // ender pearl
    if (id == 383) return true;                                       // spawn egg
    if (id == 399) return true;                                       // nether star
    switch (id) {                                                     // food, except the spider eye
    case 260: case 282: case 297: case 319: case 320: case 322: case 349: case 350: case 357:
    case 360: case 363: case 364: case 365: case 366: case 367: case 375: case 391: case 392:
    case 393: case 394: case 400: case 411: case 412: case 413: case 423: case 466:
        return true;
    default:
        return false;
    }
}

struct ItemInfo { int id, meta, count; };

static ItemInfo read_item(Ctx *ctx, void *stack) {
    ItemInfo out = {0, 0, 0};
    if (!stack) return out;
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    jobject item = java_call_item_stack_get_item(jvm, stack);
    if (item) {
        out.id = java_call_static_item_get_id_from_item(jvm, item);
        (*env)->DeleteLocalRef(env, item);
    }
    out.meta = java_get_item_stack_metadata(jvm, stack);
    out.count = java_get_item_stack_stack_size(jvm, stack);
    return out;
}

// Quality within an equipment class: the sorter table the hotbar pass already uses, so both agree on order.
// chest_gui_title_id gives the item its sorter category (kind 0 = not equipment).
struct ToolInfo { int kind, tier; };
FN(ToolInfo, chest_gui_title_id, (int id));

// Armour value: points blended with protection, the same number the sorter uses when choosing what to equip.
FN(unsigned, swap_delay, (Ctx *ctx, void *stack, unsigned kind));

static bool equipment_score(Ctx *ctx, void *stack, int *out_score) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    jobject item = java_call_item_stack_get_item(jvm, stack);
    if (!item) return false;
    int id = java_call_static_item_get_id_from_item(jvm, item);
    (*env)->DeleteLocalRef(env, item);
    // Armour is not in chest_gui_title_id (that table stops at 286, tools and weapons only), so every helmet,
    // chestplate, pair of leggings and pair of boots used to fall through as unscorable and the cleaner kept
    // all of them - the "it never drops gear worse than mine" report. swap_delay already computes exactly the
    // armour value used for equipping, so score armour through it.
    int armour_class = item_class(id);
    if (armour_class >= CLASS_HELMET && armour_class <= CLASS_BOOTS) {
        *out_score = (int)swap_delay(ctx, stack, (unsigned)(armour_class - CLASS_HELMET));
        return true;
    }

    ToolInfo info = chest_gui_title_id(id);
    if (!info.kind) return false;
    *out_score = (int)inv_manager_score_item_stack((void **)ctx, stack, info.kind);
    return true;
}

// Best main-inventory slot carrying `wanted`. `count_slots` limits the scan (36 = bag + hotbar).
static int best_slot_of_class(Ctx *ctx, int wanted, int count_slots) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    void *inventory = java_get_entity_player_inventory(jvm, ctx->player);
    if (!inventory) return -1;
    int best_slot = -1, best_score = -1;
    for (int slot = 0; slot < count_slots; slot++) {
        void *stack = java_call_iinventory_get_stack_in_slot(jvm, inventory, slot);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
        if (!stack) continue;
        ItemInfo info = read_item(ctx, stack);
        int score = 0;
        bool match = info.id > 0 && item_class(info.id) == wanted && equipment_score(ctx, stack, &score);
        (*env)->DeleteLocalRef(env, stack);
        if (match && score > best_score) { best_score = score; best_slot = slot; }
    }
    (*env)->DeleteLocalRef(env, inventory);
    return best_slot;
}

// Hotbar-kind setting value -> item class. The option order is the client's own enum table:
// 0 None, 1 Axe, 2 Blocks, 3 Bow, 4 Chest, 5 Ender pearl, 6 Fireball, 7 Fishing rod, 8 Flint and steel,
// 9 Golden apple, 10 Healing potion, 11 Jump potion, 12 Ladders, 13 Lava bucket, 14 Pickaxe, 15 Shears,
// 16 Shovel, 17 Snowballs/eggs, 18 Soup, 19 Speed potion, 20 Stick, 21 Strength potion, 22 Sword, 23 TNT,
// 24 Water bucket, 25 Weapon, 26 Wool
static int class_from_slot_setting(int index) {
    int *value = (int *)(inv_manager_enum_slot_1 + index * 0x40 + 0x30);
    if (!value) return 0;
    switch (*value) {
    case 22: return CLASS_SWORD;
    case 2: return CLASS_BLOCK;
    case 3: return CLASS_BOW;
    case 14: return CLASS_PICKAXE;
    case 1: return CLASS_AXE;
    case 16: return CLASS_SHOVEL;
    case 9: return CLASS_GAPPLE;
    case 17: return CLASS_THROWABLE;
    case 5: return CLASS_PEARL;
    default: return 0;                       // None, or a class this cleaner does not rank
    }
}

// STEP_CLEAN: one drop per call (the click delay paces it). Returns 1 when a slot was chosen.
static int clean_keep[16];
static int clean_keep_class[16];        // item_class of each protected slot
static int clean_keep_count = -1;      // -1: not built yet

// Best score of each equipment class across the whole player inventory, armour slots included. The clean pass
// compares a spare piece against this instead of against the keep set.
//
// Comparing against the keep set was wrong: that set only carries the classes the hotbar settings ask for, so a
// spare pickaxe with no Pickaxe slot configured had no reference entry, the comparison baseline stayed -1, and
// "is there something better than this?" always answered no. Nothing of an unconfigured class was ever thrown
// out. Scanning once and keeping a per-class maximum also makes the equipped armour (slots 36..39) the baseline
// it should have been all along.
static int clean_best[16];             // indexed by item_class, -1 = no such item
static int clean_best_slot[16];        // the slot that best one lives in, -1 = none
static int clean_best_ready = -1;

extern "C" void inv_manager_clean_reset(void) {
    clean_keep_count = -1;
    clean_best_ready = -1;
}


static void build_best_scores(Ctx *ctx) {
    for (int c = 0; c < 16; c++) { clean_best[c] = -1; clean_best_slot[c] = -1; }
    void *inventory = java_get_entity_player_inventory(ctx->jvm, ctx->player);
    if (!inventory) { clean_best_ready = 0; return; }
    JNIEnv *env = ((Jvm *)ctx->jvm)->env;
    for (int slot = 0; slot < 40; slot++) {
        void *stack = java_call_iinventory_get_stack_in_slot(ctx->jvm, inventory, slot);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
        if (!stack) continue;
        ItemInfo info = read_item(ctx, stack);
        int cls = item_class(info.id);
        if (cls > 0 && cls < 16) {
            int score = 0;
            if (equipment_score(ctx, stack, &score)) {
                // Strictly greater only: on a tie the earlier slot keeps the title, so exactly one item per
                // class survives and every duplicate of it can be thrown out.
                if (score > clean_best[cls]) {
                    clean_best[cls] = score;
                    clean_best_slot[cls] = slot;
                }
            }
        }
        (*env)->DeleteLocalRef(env, stack);
    }
    (*env)->DeleteLocalRef(env, inventory);
    clean_best_ready = 1;
    slinky_logf("inv: best scores built");
}

static void build_keep_set(Ctx *ctx) {
    clean_keep_count = 0;
    for (int i = 0; i < 9 && clean_keep_count < 12; i++) {
        int want = class_from_slot_setting(i);
        if (!want) continue;
        int slot = best_slot_of_class(ctx, want, 36);
        if (slot >= 0) {
            clean_keep_class[clean_keep_count] = want;
            clean_keep[clean_keep_count++] = slot;
        }
    }
    // 40 slots, not 36: the four armour slots live at 36..39 and they are what "better than mine" means. The
    // clean pass itself only walks 0..35, so an entry here for an equipped piece is a score reference only; it
    // can never be dropped.
    for (int t = 0; t < 4 && clean_keep_count < 16; t++) {
        int slot = best_slot_of_class(ctx, CLASS_HELMET + t, 40);
        if (slot >= 0) {
            clean_keep_class[clean_keep_count] = CLASS_HELMET + t;
            clean_keep[clean_keep_count++] = slot;
        }
    }
    if (clean_best_ready < 0) build_best_scores(ctx);
    slinky_logf("inv: keep set built (%d slots)", clean_keep_count);
}

static unsigned char inv_manager_clean_step(Ctx *ctx, InvState *st) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    void *inventory = java_get_entity_player_inventory(jvm, ctx->player);
    if (!inventory) return 0;

    // Keep set, cached for the whole inventory session: building it costs 13 class scans of 36 slots (each
    // a handful of JNI calls), and the cleaner only ever removes non-keepers, so the best of each class
    // cannot change underneath it. Rebuilding per drop made a full pass tens of thousands of JNI calls.
    if (clean_keep_count < 0) build_keep_set(ctx);
    const int *keep = clean_keep;
    const int keep_count = clean_keep_count;
    // The keep set no longer gates cleaning. It exists to protect the specific slots the hotbar settings chose,
    // but build_keep_set only ever fills it with items equipment_score can rank, and blocks, snowballs, gapples
    // and pearls are not rankable - so on a normal PvP inventory it came out empty and this guard turned the
    // whole cleaner off ("nothing classified"). The per-class best/slot tables below are what actually keeps
    // one of each kind, and they are built from the same scan, so cleaning is safe without the keep set.
    if (keep_count == 0) {
        static int warned;
        if (!warned) { warned = 1; slinky_logf("inv: keep set empty - cleaning anyway (per-class best still applies)"); }
    }

    int blocks_carried = 0, throws_carried = 0;
    for (int slot = 0; slot < 36; slot++) {
        void *stack = java_call_iinventory_get_stack_in_slot(jvm, inventory, slot);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
        if (!stack) continue;
        ItemInfo info = read_item(ctx, stack);
        (*env)->DeleteLocalRef(env, stack);
        if (item_class(info.id) == CLASS_BLOCK) blocks_carried += info.count;
        else if (item_class(info.id) == CLASS_THROWABLE) throws_carried += info.count;
    }

    unsigned char found = 0;
    int blocks_dropped = 0, throws_dropped = 0;   // running totals: only the excess goes
    for (unsigned char slot = st->sub; slot < 36; slot++) {
        bool protected_slot = false;
        for (int k = 0; k < keep_count; k++)
            if (keep[k] == (int)slot) protected_slot = true;
        if (protected_slot) continue;

        void *stack = java_call_iinventory_get_stack_in_slot(jvm, inventory, slot);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
        if (!stack) continue;
        ItemInfo info = read_item(ctx, stack);

        int cls = item_class(info.id);
        bool drop = false;
        // Conservative on purpose. The first version of this dropped everything that was not in the keep
        // set, which also threw away every item the classifier did not recognise (arrows, books, TNT, flint
        // and steel, ender chests, name tags, saddles ...) and any second copy of a class. That is far too
        // much to lose on a live account, so the rule is now: only drop when a BETTER copy of the same
        // class is actually being kept, and never drop something the classifier cannot place.
        if (cls == CLASS_JUNK) {
            drop = false;                                  // not classified: leave it alone
        } else if (cls == CLASS_GAPPLE || cls == CLASS_PEARL || is_special_item(info.id, info.meta)) {
            drop = false;
        } else if (cls == CLASS_THROWABLE) {
            drop = throws_carried - throws_dropped > throw_budget();   // only the excess
            if (drop) throws_dropped += info.count;
        } else if (cls == CLASS_ORE) {
            drop = false;                                  // ores are never worth throwing out
        } else if (cls == CLASS_BLOCK) {
            drop = blocks_carried - blocks_dropped > block_budget();   // spend the budget, then the rest
            if (drop) blocks_dropped += info.count;
        } else {
            // A spare weapon, tool or armour piece: drop it only when the slot being kept is strictly
            // better. Ties are kept, so an identical second piece stays.
            int mine = 0;
            bool scored = equipment_score(ctx, stack, &mine);
            if (scored) {
                if (clean_best_ready < 0) build_best_scores(ctx);
                // Keep exactly one per class: the slot that holds the best score. Comparing scores with ">"
                // instead kept every duplicate, because five stone pickaxes all score the same and none of
                // them is strictly worse than the best. A zero best score means the class could not be
                // ranked at all, and nothing of it is thrown out.
                if (cls > 0 && cls < 16 && clean_best[cls] > 0)
                    drop = clean_best_slot[cls] != (int)slot;
            }
        }
        (*env)->DeleteLocalRef(env, stack);
        if (!drop) continue;
        slinky_logf("inv: drop slot=%d id=%d class=%d", (int)slot, info.id, cls);
        st->sub = (unsigned char)(slot + 1);
        st->window = (unsigned char)(slot < 9 ? slot + 36 : slot);   // window slot of an inventory slot
        found = 1;
        break;
    }
    (*env)->DeleteLocalRef(env, inventory);
    return found;
}
FN(void *, java_get_inventory_player_main_inventory, (void *jvm, void *inventory_player));
FN(int, java_get_item_stack_stack_size, (void *jvm, void *stack));
FN(int, java_call_item_stack_get_max_stack_size, (void *jvm, void *stack));
FN(unsigned char, java_call_static_item_stack_are_item_stack_tags_equal, (void *jvm, void *item_stack, void *other_object));

// Move the "mouse" to `target`: wait the click delay (scaled by the distance / randomly).
static void move_to(Pair target) {
    float delay = inv_manager_click_delay_b / inv_manager_click_delay_b_second;
    if (inv_manager_scale_by_mouse_distance == 1) {
        float distance = rot_delta_length(target, inv_manager_rotation_walk.pos) * 0.5f;
        delay *= distance > 1.0f ? distance : 1.0f;
    }
    if (inv_manager_randomise == 1)
        delay *= random_rotation(rot_make_floats(1.0f, 1.25f));
    inv_manager_rotation_walk.delay = delay + inv_manager_rotation_walk.delay;
    inv_manager_rotation_walk.pos = target;
}

// One step of the sort/clean plan. The return value says whether the caller may run again in the same tick:
// ADVANCE_FREE and ADVANCE_NEXT continue, ADVANCE_CURSOR has to wait for the next tick (a pick-up must settle
// before the slot is used again), ADVANCE_STOP means the plan is finished or nothing is left to do.
enum { ADVANCE_STOP = 0, ADVANCE_CURSOR = 1, ADVANCE_FREE = 2, ADVANCE_NEXT = 3 };
static int clean_logged;

// One container click. windowClick is preferred: it takes the container's window id instead of a screen, so it
// keeps working with the inventory hidden and does not pay for the GuiContainer's bookkeeping. handleMouseClick
// stays as the fallback for when those ids did not resolve.
static void inv_click(Ctx *ctx, void *screen, int slot, int button, int mode) {
    if (slot != -999 && window_click(ctx, slot, button, mode)) return;
    java_gui_container_handle_mouse_click(ctx->jvm, screen, slot, button, mode);
}

static int inv_manager_advance(Ctx *ctx, void *screen, InvState &st) {
    int state = st.state;
    int button = 0, mode = 0;
    for (;;) {
        switch (state) {
        case STEP_CLEAN:
            // After the two sort passes, drop what does not belong in a PvP inventory. A delay of 0 turns it
            // off; with "Middle click to clean" it only runs while the middle mouse button is held.
            if (!clean_logged) { clean_logged = 1; slinky_logf("inv: clean step, drop gap %.0f ms", cleaner_delay()); }
            if (!middle_click_clean() || java_call_static_mouse_is_button_down(ctx->jvm, 2)) {
                if (inv_manager_clean_step(ctx, &st)) {
                    inv_click(ctx, screen, st.window, 0, 4);                                     // throw the stack
                    st.delay = cleaner_delay() / 50.0f;
                    // ADVANCE_FREE so the per-tick budget below applies. Twelve throws in one tick (240 a
                    // second) was too fast and the server silently ignored them; one per tick was correct but
                    // took nearly two seconds for a full inventory. Three per tick - sixty a second - clears
                    // 36 slots in about 0.6 s and stays inside what the server accepts.
                    st.saved++;        // count of throws issued this pass, used to decide whether to go again
                    return ADVANCE_FREE;
                }
            }
            // A throw only advances st.sub, so a click the server refused leaves its item in a slot the pass
            // has already walked past and it survives until the inventory is opened again - the "sometimes it
            // just does not drop" report. If this pass issued any throws at all, walk the slots again: the
            // ones that did go through are gone, so the second pass only retries what is left. Three passes
            // then stop, which keeps a permanently refused item from looping forever.
            if (st.saved > 0 && st.action < 3) {
                st.action++;
                st.saved = 0;
                st.sub = 0;
                return ADVANCE_FREE;
            }
            st.state = ++state;
            __builtin_memset((char *)&st + 4, 0, 0x1c);
            continue;

        case STEP_ARMOUR:
        case STEP_HOTBAR:
            // Plan the next target that needs a click, then fall through to the click in this same call.
            if (*inv_manager_slot_setting_ptrs[state]) {
                for (unsigned char targets = inv_manager_targets_per_state[state]; st.sub < targets; st.sub++) {
                    if (ptr_to_plan_inventory_swap[state](ctx, &st)) {
                        move_to(slot_index(st.window));
                        st.state = STEP_CLICK;
                        st.saved = state;
                        st.advance = state != STEP_HOTBAR;
                        return ADVANCE_NEXT;
                    }
                }
            }
            st.state = ++state;
            __builtin_memset((char *)&st + 4, 0, 0x1c);
            continue;

        case STEP_CLICK: {
            int action = st.action;
            unsigned char slot = st.slot;
            if (action == 2) {          // pick up
                button = 0;
                mode = 0;
            } else if (action == 1) {   // swap with hotbar slot (like pressing its number key)
                button = slot;
                mode = 2;
            } else if (action == 0) {   // shift-click
                button = 0;
                mode = 1;
            }
            inv_click(ctx, screen, st.window, button, mode);
            st.mask |= (long long)(1 << (st.sub & 31));
            if (st.advance == 1)
                st.sub++;
            if (action == 2) {
                // Then put it down in `slot`; that half needs the next tick.
                move_to(slot_index(slot));
                st.state = STEP_PUT_DOWN;
                st.window = slot;
                return ADVANCE_CURSOR;
            }
            st.state = st.saved;
            return ADVANCE_FREE;        // shift-click / swap: safe to continue in this same tick
        }

        case STEP_PUT_DOWN: {
            inv_click(ctx, screen, st.window, 0, 0);
            Pair outside = st.pos;
            outside.x += -2.0f;
            move_to(outside);
            st.state = STEP_DROP_REST;
            return ADVANCE_CURSOR;
        }

        case STEP_DROP_REST:
            inv_click(ctx, screen, -999, 0, 0);                                     // click outside the window
            st.state = st.saved;
            return ADVANCE_FREE;

        default:
            return ADVANCE_STOP;
        }
    }
}
extern "C" void inv_manager_tick(InvHook *hook) {
    InvState &st = inv_manager_rotation_walk;
    Ctx *ctx = hook->ctx;

    // Inventory not open: get ready for next time (start delay, mouse in the middle).
    void *screen = get_current_screen(ctx->jvm, ctx->minecraft);
    if (!screen || !java_jni_is_instance_of_gui_inventory(ctx->jvm, screen)) {
        __builtin_memset(&st, 0, 0x20);
        st.delay = inv_manager_start_delay;
        if (inv_manager_randomise == 1)
            st.delay = random_rotation(rot_make_floats(1.0f, 1.5f)) * st.delay;
        st.pos = slot_index(13);
        return;
    }

    int state = st.state;
    if (state == STEP_DONE) {
        // Everything is sorted; close the inventory if the setting asks for it.
        // `action` doubles as the "already closed" marker (it is zeroed when DONE is entered).
        if (close_inventory_when_done() && st.action != 99) {
            st.action = 99;
            refill_disable_and_close_screen(ctx);
        }
        return;
    }
    if (st.delay > 0.0f) {
        st.delay += -1.0f;
        if (st.delay > 0.0f)
            return;
    }
    hook->busy = 1;

    // "Instant" runs the whole plan in one tick, the way the reference client's Instant mode does, instead of
    // spending one tick per click. Only cursor-free clicks repeat inside the tick - shift-click, hotbar swap
    // and throw - because a pick-up / put-down pair needs the slot to settle before it is used again.
    // Follows Chest Stealer's Mode. Expo spends the whole budget on the sort (shift-clicks settle instantly);
    // Myau does one click per tick. Throws are rate-limited separately in STEP_CLEAN, because a burst of
    // throws is what the server refuses.
    int budget = inv_is_expo() ? 12 : 1;
    for (int i = 0; i < budget; i++) {
        int progress = inv_manager_advance(ctx, screen, st);
        if (progress == ADVANCE_STOP || progress == ADVANCE_CURSOR) break;
    }
}

// An item's id and metadata as one number (0 for no item).
static unsigned long long item_key(Ctx *ctx, void *stack) {
    void *item = java_call_item_stack_get_item(ctx->jvm, stack);
    if (!item)
        return 0;
    unsigned item_id = java_call_static_item_get_id_from_item(ctx->jvm, item);
    return ((unsigned long long)item_id << 32) | (unsigned long long)(long long)java_get_item_stack_metadata(ctx->jvm, stack);
}

// Plan hotbar slot `st->sub`: find the stack that is best there (score x count), or one that
// can be merged into the current stack. True when a click was planned.
extern "C" unsigned char inv_manager_plan_step(Ctx *ctx, InvState *st) {
    unsigned target = st->sub;
    int wanted_kind = *(int *)(inv_manager_enum_slot_1 + target * 0x40 + 0x30);
    if (!wanted_kind)
        return 0;   // "any": leave it alone

    void *items = java_get_inventory_player_main_inventory(ctx->jvm, java_get_entity_player_inventory(ctx->jvm, ctx->player));
    JNIEnv *env = ((Jvm *)ctx->jvm)->env;
    auto get = (*env)->GetObjectArrayElement;

    // What is there now.
    void *current = get(env, items, target);
    int count = 0, max_count = 0;
    unsigned long long key = 0;
    unsigned score = 0;
    if (current) {
        count = java_get_item_stack_stack_size(ctx->jvm, current);
        max_count = java_call_item_stack_get_max_stack_size(ctx->jvm, current);
        key = item_key(ctx, current);
        score = inv_manager_score_item_stack((void **)ctx, current, wanted_kind);
    }

    // Is there an empty slot in the main inventory (9..35)? (empty_slot == null)
    void *empty_slot;
    int i = 9;
    for (;;) {
        empty_slot = get(env, items, i);
        if (!empty_slot || i == 35)
            break;
        i++;
    }

    if (count <= 0)
        count = 0;
    unsigned best = count * score;
    unsigned best_slot = target;
    unsigned char merge = 0;
    for (unsigned j = 0; j < 36; j++) {
        if (j == target || (st->mask >> j & 1))
            continue;
        void *stack = get(env, items, j);
        if (!stack)
            continue;
        int n = java_get_item_stack_stack_size(ctx->jvm, stack);
        unsigned long long other_key = item_key(ctx, stack);

        // Value when merged into the current stack (same item, both not full).
        unsigned merged = 0;
        if (count < max_count && n < max_count && other_key == key && java_call_static_item_stack_are_item_stack_tags_equal(ctx->jvm, current, stack)) {
            int total = count + n;
            merged = (total < max_count ? total : max_count) * score;
        }
        unsigned value = inv_manager_score_item_stack((void **)ctx, stack, wanted_kind) * n;
        bool use_merge = value - 1 < merged;   // (value > 0 and merging is worth at least as much)
        if (use_merge)
            value = merged;
        // Merging from the hotbar only when there is room for the rest.
        if (value > best && (!empty_slot || j >= 9 || !use_merge)) {
            best = value;
            best_slot = j;
            merge = use_merge;
        }
    }

    if (best_slot != target) {
        st->padding_15 = st->padding_16 = st->padding_17 = 0;
        st->action = !merge;                  // merge: shift-click (0), else swap into the hotbar slot (1)
        st->slot = merge ? 0 : target;
        st->window = (int)best_slot >= 9 ? best_slot : best_slot + 36;   // hotbar slots are 36..44 in the window
        st->mask |= (long long)(1 << (target & 31));
        return 1;
    }
    if (best)
        st->mask |= (long long)(1 << (target & 31));
    return 0;
}