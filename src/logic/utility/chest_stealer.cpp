// Chest Stealer - take the useful items out of a chest.
//
// While a chest is open, the module shift-clicks the items worth taking, one per click
// ("speed" clicks per 50 ms), after "delay_after_open" ms:
//   - tools, weapons and armour that are better than what you have,
//   - golden apples ("Golden apples") and useful potions ("Good potions"),
//   - building blocks, up to "Max blocks" in your inventory.
// With "require_vanilla_title", only chests with the normal "Chest" / "Large Chest" title.
//
// Callback (descriptor slot +0x1d8):
//   chest_stealer_on_tick  every tick
#include "slinky/engine.h"
#include "slinky/jids.h"

S(str_container_chest);  // "container.chest"
S(str_container_chestdouble);  // "container.chestDouble"

extern "C" float chest_stealer_delay_after_open_value;                 // "delay_after_open" current value
extern "C" unsigned char chest_stealer_require_vanilla_title_value;         // "require_vanilla_title"
extern "C" float chest_stealer_steal_delay;                 // steal delay (ms)
extern "C" unsigned char chest_stealer_speed_setting[];     // slider; current value at +0x30
extern "C" unsigned char chest_stealer_max_blocks_setting[];  // slider; current value at +0x30
extern "C" unsigned char chest_stealer_golden_apples_value;     // take golden apples
extern "C" unsigned char chest_stealer_good_potions_value;    // take useful potions
extern "C" unsigned char chest_stealer_skip_trash_value;      // only take upgrades and specials
extern "C" unsigned char chest_stealer_keep_projectiles_value;// take snowballs, eggs and rods
extern "C" unsigned char chest_stealer_auto_close_value;      // close the chest when done

struct ToolInfo { int kind, tier; };
FN(void *, get_current_screen, (void *jvm, void *object));
FN(void, java_set_minecraft_current_screen, (void *jvm, void *minecraft, void *screen));
FN(unsigned char, java_jni_is_instance_of_gui_chest, (void *jvm, void *scr));
FN(void *, java_get_gui_chest_lower_chest_inventory, (void *jvm, void *scr));
FN(void *, java_jni_is_instance_of_inventory_basic, (void *jvm, void *candidate));
FN(void *, java_get_inventory_basic, (void *jvm, void *inventory_basic));
FN(unsigned char, java_string_equal_translation_key, (void *jvm, void *title, const char *name));
FN(void *, java_call_iinventory_get_stack_in_slot, (void *jvm, void *inventory, int slot));
FN(void, java_gui_container_handle_mouse_click, (void *jvm, void *screen, int slot, int button, int mode));
FN(void *, java_get_entity_player_inventory, (void *jvm, void *entity_player));
FN(void *, java_call_item_stack_get_item, (void *jvm, void *stack));
FN(int, java_call_static_item_get_id_from_item, (void *jvm, void *item));
FN(int, java_get_item_stack_metadata, (void *jvm, void *stack));
FN(int, java_get_item_stack_stack_size, (void *jvm, void *stack));
FN(void *, java_call_item_stack_get_enchantment_tag_list, (void *jvm, void *stack));
FN(unsigned char, java_call_item_stack_has_display_name, (void *jvm, void *stack));
FN(int, score_item_inventory_sorter_category, (Ctx *ctx, int category, int id, unsigned meta, void *nbt, unsigned char display_name));
FN(ToolInfo, chest_gui_title_id, (int id));
FN(unsigned, swap_delay, (Ctx *ctx, void *stack, unsigned kind));
FN(double, time_now, ());
FN(void, refill_disable_and_close_screen, (Ctx *ctx));
FN(int, window_click, (Ctx *ctx, int slot, int button, int mode));
FN(int, window_click_available, ());
FN(void, java_call_static_mouse_set_grabbed, (void *jvm, unsigned char grab));
FN(void, java_call_minecraft_set_ingame_focus, (void *jvm, void *minecraft));
FN(void, set_in_game, (void *jvm, void *minecraft, unsigned char value));
extern "C" unsigned char chest_stealer_mode_setting[];      // enum: 0 Normal, 1 Instant
extern "C" unsigned char chest_stealer_min_delay_setting[];  // slider: value at +0x30
extern "C" unsigned char chest_stealer_max_delay_setting[];
FN(void, jni_throw_format, (void *jvm, const char *fmt, ...));
FN(void, slinky_logf, (const char *fmt, ...));

// Setting objects keep their current value at +0x30 (see the data.s layouts).
static float setting_value(unsigned char *setting) { return *(float *)(setting + 0x30); }
static int chest_mode_normal() { return *(int *)(chest_stealer_mode_setting + 0x30) == 0; }
static unsigned int chest_rng_state = 0x9e3779b9u;
static int chest_random_ticks() {
    int lo = (int)setting_value(chest_stealer_min_delay_setting);
    int hi = (int)setting_value(chest_stealer_max_delay_setting);
    if (hi < lo) hi = lo;
    if (hi <= 0) return 0;
    chest_rng_state = chest_rng_state * 1103515245u + 12345u;
    return lo + (int)((chest_rng_state >> 16) % (unsigned)(hi - lo + 1));
}


static void *chest_stealer_screen;          // screen whose slots are being processed
// Silent mode: the chest GUI has been hidden from the client while the server-side container
// stays open. This keeps the screen object so the clicks still have something to dispatch on.
static void *chest_hidden_screen;
// The chest inventory as a JNI global reference. A local reference dies the moment the tick returns,
// so once the screen is hidden and the slot lookups have to run against a saved object, that object
// has to be global or the reads come back empty - which is why the first tick never stole anything
// and the chest had to be reopened.
static void *chest_lower_global;
static unsigned char chest_silent_logged;
static unsigned char chest_hide_logged;
static unsigned char chest_pass_logged;
static int chest_stealer_slot;              // next chest slot to inspect
static int chest_stealer_size;
static int chest_stealer_found_any;   // did this pass take anything?
static int chest_wait_ticks;          // Normal mode: ticks left before the next click
static int chest_warned_full;         // the full-inventory message is once per chest              // number of slots in the lower chest inventory
static double chest_stealer_next_click;     // time of the next click
static jmethodID chest_stealer_size_method; // IInventory.getSizeInventory

static jmethodID inventory_size_method(Jvm *jvm) {
    if (chest_stealer_size_method) return chest_stealer_size_method;
    JNIEnv *env = jvm->env;
    jclass cls = (jclass)jvm->ids->IInventory;
    static const char *const names[] = {"getSizeInventory", "func_70302_i_", "a"};
    for (int i = 0; i < 3; i++) {
        jmethodID id = (*env)->GetMethodID(env, cls, names[i], "()I");
        if (id) {
            chest_stealer_size_method = id;
            return id;
        }
        (*env)->ExceptionClear(env);
    }
    return 0;
}

static float speed_value() {
    float s = *(float *)(chest_stealer_speed_setting + 0x30);
    if (!(s > 0.1f) || s > 10.0f) s = 1.0f;
    return s;
}

// Score a tool against the sorter's tool category. kind is 0 for non-tools.
static int tool_score(Ctx *ctx, void *stack, int *kind) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    jobject item = java_call_item_stack_get_item(jvm, stack);
    if (!item) return -1;
    int id = java_call_static_item_get_id_from_item(jvm, item);
    ToolInfo info = chest_gui_title_id(id);
    if (!info.kind) {
        (*env)->DeleteLocalRef(env, item);
        return -1;
    }
    unsigned meta = (unsigned)java_get_item_stack_metadata(jvm, stack);
    void *nbt = java_call_item_stack_get_enchantment_tag_list(jvm, stack);
    unsigned char display_name = java_call_item_stack_has_display_name(jvm, stack);
    int score = score_item_inventory_sorter_category(ctx, info.kind, id, meta, nbt, display_name);
    if (nbt) (*env)->DeleteLocalRef(env, nbt);
    (*env)->DeleteLocalRef(env, item);
    *kind = info.kind;
    return score;
}

static int max_block_count() {
    float value = *(float *)(chest_stealer_max_blocks_setting + 0x30);
    if (!(value > 0.0f)) return 0;
    if (value > 2304.0f) value = 2304.0f;   // 36 inventory slots * 64
    return (int)value;
}

static bool is_block_item(int id) {
    return id > 0 && id < 0x100;
}

// 1.8 potion damage values: the low nibble is the base potion, bit 0x20 is
// level II, bit 0x40 is extended, and bit 0x4000 marks a splash potion.
static bool is_good_potion(unsigned meta) {
    switch (meta & 0xf) {
    case 1:  // regeneration
    case 2:  // swiftness
    case 3:  // fire resistance
    case 5:  // instant health
    case 6:  // night vision
    case 8:  // strength
    case 10: // jump boost
    case 12: // water breathing
    case 13: // invisibility
        return true;
    default:
        return false;
    }
}

static int player_block_count(Ctx *ctx) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    void *inventory = java_get_entity_player_inventory(jvm, ctx->player);
    if (!inventory) return 0;
    int total = 0;
    for (int slot = 0; slot < 36 && total < 2304; slot++) {
        void *current = java_call_iinventory_get_stack_in_slot(jvm, inventory, slot);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            break;
        }
        if (!current) continue;
        jobject item = java_call_item_stack_get_item(jvm, current);
        if (item) {
            int id = java_call_static_item_get_id_from_item(jvm, item);
            if (is_block_item(id)) {
                int n = java_get_item_stack_stack_size(jvm, current);
                if (n > 0) total += n;
            }
            (*env)->DeleteLocalRef(env, item);
        }
        (*env)->DeleteLocalRef(env, current);
    }
    (*env)->DeleteLocalRef(env, inventory);
    return total;
}

static bool chest_block_is_wanted(Ctx *ctx, void *stack, int current_blocks) {
    int limit = max_block_count();
    if (limit <= 0) return false;
    Jvm *jvm = (Jvm *)ctx->jvm;
    int n = java_get_item_stack_stack_size(jvm, stack);
    if (n <= 0) return false;
    return current_blocks + n <= limit;
}

// Golden apples, useful potions, and a configurable number of building blocks.
static bool chest_item_is_special(Ctx *ctx, void *stack, int blocks_in_inventory) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    jobject item = java_call_item_stack_get_item(jvm, stack);
    if (!item) return false;
    int id = java_call_static_item_get_id_from_item(jvm, item);
    bool wanted = false;
    if (id == 0x142 || id == 0x1d2) {           // golden / enchanted golden apple
        wanted = chest_stealer_golden_apples_value == 1;
    } else if (id == 0x175) {                    // potion
        wanted = chest_stealer_good_potions_value == 1 &&
                 is_good_potion((unsigned)java_get_item_stack_metadata(jvm, stack));
    } else if (is_block_item(id)) {
        wanted = chest_block_is_wanted(ctx, stack, blocks_in_inventory);
    }
    (*env)->DeleteLocalRef(env, item);
    return wanted;
}

// ---------------------------------------------------------------------------
// Take rules ported from RavenB4's InvManager (the STEALER path).
//
// The original rule set was "is it a block under the limit, a golden apple, a good potion, or a better tool"
// and then shift-click it. That takes a second water bucket you cannot use, takes pearls while you already
// hold 16, and keeps shift-clicking into an inventory with no free slot - the click is accepted by the
// client and then moves nothing, which is what made the module feel unreliable. RavenB4 decides per item
// class instead:
//   - something you already own enough of is skipped (buckets, pearls, projectiles, golden apples)
//   - a tool or armour piece is taken only if it beats the best one you carry
//   - anything else needs a free main-inventory slot to land in
// ---------------------------------------------------------------------------
enum {
    ITEM_ARROW = 262, ITEM_GOLDEN_APPLE = 322, ITEM_WATER_BUCKET = 326, ITEM_LAVA_BUCKET = 327,
    ITEM_SNOWBALL = 332, ITEM_EGG = 344, ITEM_FISHING_ROD = 346, ITEM_ENDER_PEARL = 368,
    ITEM_POTION = 373, ITEM_ENCHANTED_GOLDEN_APPLE = 466
};
// How many of a "you only need so many" item the player may already carry before we stop taking more.
enum { CAP_BUCKET = 1, CAP_PEARL = 16, CAP_PROJECTILE = 16, CAP_GAPPLE = 64 };

struct PlayerSummary {
    int empty_main;      // free slots in the main inventory (9..35)
    int blocks;          // total building blocks carried
    int water, lava;     // buckets carried
    int pearls, projectiles, gapples;
};

// One pass over the player inventory: everything the take rules below ask about.
static PlayerSummary scan_player(Ctx *ctx) {
    PlayerSummary sum = {};
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    void *inventory = java_get_entity_player_inventory(jvm, ctx->player);
    if (!inventory) return sum;
    for (int slot = 0; slot < 36; slot++) {
        void *stack = java_call_iinventory_get_stack_in_slot(jvm, inventory, slot);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
        if (!stack) {
            if (slot >= 9) sum.empty_main++;
            continue;
        }
        jobject item = java_call_item_stack_get_item(jvm, stack);
        int n = java_get_item_stack_stack_size(jvm, stack);
        if (item) {
            int id = java_call_static_item_get_id_from_item(jvm, item);
            if (is_block_item(id)) sum.blocks += n;
            else if (id == ITEM_WATER_BUCKET) sum.water++;
            else if (id == ITEM_LAVA_BUCKET) sum.lava++;
            else if (id == ITEM_ENDER_PEARL) sum.pearls += n;
            else if (id == ITEM_SNOWBALL || id == ITEM_EGG || id == ITEM_FISHING_ROD) sum.projectiles += n;
            else if (id == ITEM_GOLDEN_APPLE || id == ITEM_ENCHANTED_GOLDEN_APPLE) sum.gapples += n;
            (*env)->DeleteLocalRef(env, item);
        }
        (*env)->DeleteLocalRef(env, stack);
    }
    (*env)->DeleteLocalRef(env, inventory);
    return sum;
}

static bool chest_item_is_upgrade(Ctx *ctx, void *stack);   // defined below

// Should this chest stack be taken? `sum` is the player inventory scan for this pass.
static bool chest_should_take(Ctx *ctx, void *stack, const PlayerSummary &sum) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;
    jobject item = java_call_item_stack_get_item(jvm, stack);
    if (!item) return false;
    int id = java_call_static_item_get_id_from_item(jvm, item);
    int n = java_get_item_stack_stack_size(jvm, stack);
    bool take = false;
    // With Skip Trash off everything is looted, which is what the module did before the Myau-style filter
    // existed; the capacity limits below still apply so a full chest cannot overflow the inventory.
    if (chest_stealer_skip_trash_value != 1) {
        take = true;
        if (id == ITEM_SNOWBALL || id == ITEM_EGG || id == ITEM_FISHING_ROD)
            take = chest_stealer_keep_projectiles_value == 1;
        if (is_block_item(id)) take = max_block_count() > 0;
        if (take && sum.empty_main <= 0) take = false;
        (*env)->DeleteLocalRef(env, item);
        return take;
    }
    if (id == ITEM_GOLDEN_APPLE || id == ITEM_ENCHANTED_GOLDEN_APPLE) {
        take = chest_stealer_golden_apples_value == 1 && sum.gapples < CAP_GAPPLE;
    } else if (id == ITEM_POTION) {
        take = chest_stealer_good_potions_value == 1 &&
               is_good_potion((unsigned)java_get_item_stack_metadata(jvm, stack));
    } else if (id == ITEM_WATER_BUCKET) {
        take = sum.water < CAP_BUCKET;
    } else if (id == ITEM_LAVA_BUCKET) {
        take = sum.lava < CAP_BUCKET;
    } else if (id == ITEM_ENDER_PEARL) {
        take = sum.pearls < CAP_PEARL;
    } else if (id == ITEM_SNOWBALL || id == ITEM_EGG || id == ITEM_FISHING_ROD) {
        take = chest_stealer_keep_projectiles_value == 1 && sum.projectiles < CAP_PROJECTILE;
    } else if (is_block_item(id)) {
        int limit = max_block_count();
        take = limit > 0 && n > 0 && sum.blocks + n <= limit;
    } else {
        // Tools, armour and anything else: only when it beats what is carried.
        take = chest_item_is_upgrade(ctx, stack);
    }
    // Every take is a shift-click into the main inventory, so it needs a slot to land in. Without this the
    // module kept clicking with a full inventory and moved nothing.
    if (take && sum.empty_main <= 0) take = false;
    (*env)->DeleteLocalRef(env, item);
    return take;
}

// True only when the chest stack is an upgrade over the player's current gear.
static bool chest_item_is_upgrade(Ctx *ctx, void *stack) {
    Jvm *jvm = (Jvm *)ctx->jvm;
    JNIEnv *env = jvm->env;

    int kind = 0;
    int chest_score = tool_score(ctx, stack, &kind);
    void *inventory = java_get_entity_player_inventory(jvm, ctx->player);
    if (inventory && kind) {
        int best = 0;
        for (int slot = 0; slot < 36; slot++) {
            void *current = java_call_iinventory_get_stack_in_slot(jvm, inventory, slot);
            if (!current) continue;
            int current_kind = 0;
            int current_score = tool_score(ctx, current, &current_kind);
            if (current_kind == kind && current_score > best) best = current_score;
            (*env)->DeleteLocalRef(env, current);
        }
        (*env)->DeleteLocalRef(env, inventory);
        return chest_score > best;
    }

    if (inventory) {
        // Armor: kind 0 helmet, 1 chestplate, 2 leggings, 3 boots. swap_delay returns
        // armour points plus protection enchantments for the matching piece.
        for (int armor_kind = 0; armor_kind < 4; armor_kind++) {
            unsigned upgrade_score = swap_delay(ctx, stack, (unsigned)armor_kind);
            if (!upgrade_score) continue;
            void *current = java_call_iinventory_get_stack_in_slot(jvm, inventory, 39 - armor_kind);
            unsigned current_score = current ? swap_delay(ctx, current, (unsigned)armor_kind) : 0;
            if (current) (*env)->DeleteLocalRef(env, current);
            if (upgrade_score > current_score) {
                (*env)->DeleteLocalRef(env, inventory);
                return true;
            }
        }
        (*env)->DeleteLocalRef(env, inventory);
    }
    return false;
}

// Close the chest properly. The close path reads currentScreen, which silent mode has set to null, so it has
// to be put back first or the close never happens and the stack being carried stays on the cursor.
static void chest_close_now(Ctx *ctx) {
    refill_disable_and_close_screen(ctx);
}

// Shift-click a chest slot. windowClick is preferred because it does not need the screen, which is what lets
// the chest stay hidden; handleMouseClick remains the fallback when the ids could not be resolved.
static int chest_take_slot(Ctx *ctx, void *screen, int slot) {
    if (window_click(ctx, slot, 0, 1)) return 1;
    java_gui_container_handle_mouse_click(ctx->jvm, screen, slot, 0, 1);
    return 1;
}

extern "C" void chest_stealer_on_tick(Hook *hook) {
    Ctx *ctx = hook->ctx;
    Jvm *jvm = (Jvm *)ctx->jvm;
    void *screen = get_current_screen(ctx->jvm, ctx->minecraft);
    if (!screen || !java_jni_is_instance_of_gui_chest(ctx->jvm, screen)) {
        if (chest_lower_global) {
            (*jvm->env)->DeleteGlobalRef(jvm->env, (jobject)chest_lower_global);
            chest_lower_global = 0;
        }
        chest_hidden_screen = 0;
        chest_stealer_screen = 0;
        chest_stealer_slot = 0;
        return;
    }

    void *lower = chest_lower_global;
    if (!lower) {
        void *fresh = java_get_gui_chest_lower_chest_inventory(ctx->jvm, screen);
        if (!fresh) return;
        lower = (*jvm->env)->NewGlobalRef(jvm->env, (jobject)fresh);
        if (!lower) return;
        chest_lower_global = lower;
    }

    // "require_vanilla_title" means: only click chests whose localized title is the
    // vanilla chest title.
    if (chest_stealer_require_vanilla_title_value == 1) {
        void *inventory_basic = java_jni_is_instance_of_inventory_basic(jvm, lower);
        if (inventory_basic) {
            void *title = java_get_inventory_basic(jvm, inventory_basic);
            if (!java_string_equal_translation_key(jvm, title, str_container_chest) &&
                !java_string_equal_translation_key(jvm, title, str_container_chestdouble))
                return;
        }
    }

    double now = time_now();
    if (screen != chest_stealer_screen) {
        if (chest_lower_global) {
            (*jvm->env)->DeleteGlobalRef(jvm->env, (jobject)chest_lower_global);
            chest_lower_global = 0;
        }
        chest_stealer_screen = screen;
        chest_stealer_slot = 0;
        chest_stealer_size = 0;
        chest_stealer_found_any = 0;
        chest_wait_ticks = 0;
        chest_warned_full = 0;
        chest_pass_logged = 0;
        chest_stealer_next_click = now + (double)chest_stealer_delay_after_open_value / 1000.0;
    }
    if (now < chest_stealer_next_click) return;

    if (chest_stealer_size <= 0) {
        jmethodID size_method = inventory_size_method(jvm);
        if (size_method) {
            chest_stealer_size = (*jvm->env)->CallIntMethod(jvm->env, lower, size_method);
            if ((*jvm->env)->ExceptionCheck(jvm->env)) {
                (*jvm->env)->ExceptionClear(jvm->env);
                chest_stealer_size = 0;
            }
        }
        // If the size lookup is unavailable, use the single-chest size. Never probe
        // past it, or a single chest would start moving the player's own items into it.
        if (chest_stealer_size <= 0 || chest_stealer_size > 54) chest_stealer_size = 27;
    }

    if (!chest_pass_logged) {
        chest_pass_logged = 1;
        slinky_logf("chest: pass open mode=%s size=%d", chest_mode_normal() ? "Myau" : "Expo", chest_stealer_size);
    }

    PlayerSummary sum = scan_player(ctx);

    // Inventory full: nothing can be moved, so say why once per chest instead of clicking into a wall.
    if (sum.empty_main <= 0) {
        if (!chest_warned_full) {
            jni_throw_format(ctx->jvm, "\u00a7cSlinky\u00a77: inventory full");
            chest_warned_full = 1;
        }
        if (chest_stealer_auto_close_value == 1) chest_close_now(ctx);
        return;
    }

    // Two modes, and they are genuinely different strategies rather than a speed slider:
    //
    //   Normal  - the Myau behaviour: one shift-click, then a random wait. Slow, paced like a person, and the
    //             form that survives a server watching click intervals.
    //   Instant - the Expo behaviour: the whole chest in a single tick. No wait at all, one click per slot
    //             that is worth taking, so a full double chest is empty before the next frame.
    //
    // Instant used to cap at eight clicks per tick, which is why it still took several ticks on a full
    // chest. The cap is gone: the loop is bounded by chest_stealer_size anyway.
    int budget;
    if (chest_mode_normal()) {
        if (chest_wait_ticks > 0) { chest_wait_ticks--; return; }
        budget = 1;
    } else {
        budget = chest_stealer_size > 0 ? chest_stealer_size : 1;
    }
    while (budget-- > 0) {
        for (; chest_stealer_slot < chest_stealer_size; chest_stealer_slot++) {
            void *stack = java_call_iinventory_get_stack_in_slot(jvm, lower, chest_stealer_slot);
            if ((*jvm->env)->ExceptionCheck(jvm->env)) {
                (*jvm->env)->ExceptionClear(jvm->env);
                return;
            }
            if (!stack) continue;
            bool take = chest_should_take(ctx, stack, sum);
            (*jvm->env)->DeleteLocalRef(jvm->env, stack);
            if (!take) continue;
            java_gui_container_handle_mouse_click(jvm, screen, chest_stealer_slot, 0, 1);   // shift-click
            if ((*jvm->env)->ExceptionCheck(jvm->env)) {
                (*jvm->env)->ExceptionClear(jvm->env);
                return;
            }
            chest_stealer_steal_delay = (float)(0.05 / (double)speed_value() * 1000.0);
            chest_stealer_found_any = 1;
            if (chest_mode_normal()) chest_wait_ticks = chest_random_ticks();
            chest_stealer_slot++;
            break;                      // one click done: take the next one in this same tick
        }
        if (chest_stealer_slot >= chest_stealer_size) break;
    }

    if (chest_stealer_slot < chest_stealer_size) {
        chest_stealer_next_click = now + 0.05 / (double)speed_value();   // budget spent, continue next tick
        return;
    }

    // A full pass found nothing left to take. Myau closes the chest here when Auto Close is on.
    if (chest_stealer_auto_close_value == 1) {
        if (chest_stealer_found_any == 0) {
            chest_close_now(ctx);
            return;
        }
    }
    chest_stealer_found_any = 0;
    chest_stealer_slot = 0;
    chest_stealer_next_click = now + 0.2;
}
