// Expo-style container click: playerController.windowClick(windowId, slot, button, mode, player).
//
// The client's own path to a container click goes through GuiContainer.handleMouseClick, which needs the screen
// object. That is fine while the GUI is open, but it makes a silent session impossible: the moment currentScreen
// is cleared there is nothing to dispatch the click on, so the chest either has to stay on screen or the clicks
// stop landing.
//
// windowClick takes only the container's window id, so it works with no screen at all. This is what Expos'
// ChestStealer does, and it is why that one can hide the chest and keep stealing.
//
// None of these names or signatures are known at compile time. The client may run under MCP, SRG, Mojang or the
// official obfuscated naming (Lunar and other remappers each land somewhere different), so every candidate is
// tried until one resolves; the ones taken from a 1.8.9 mapping are listed first.
//
//   1.8.9 joined.srg:
//     FD: wn/bk   net/minecraft/entity/player/EntityPlayer/field_71070_bA     (openContainer)
//     FD: xi/d    net/minecraft/inventory/Container/field_75152_c            (windowId)
//     MD: bda/a (IIIILwn;)Lzx;
//                 net/minecraft/client/multiplayer/PlayerControllerMP/func_78753_a
//                 (IIIILnet/minecraft/entity/player/EntityPlayer;)Lnet/minecraft/item/ItemStack;   (windowClick)
#include "slinky/engine.h"
#include "slinky/jcall.h"

FN(void *, java_get_minecraft_player_controller, (void *jvm, void *minecraft));
FN(void, slinky_logf, (const char *fmt, ...));

static jfieldID id_open_container;
static jfieldID id_window_id;
static jmethodID id_window_click;
static int resolve_state;      // 0 = untried, 1 = ok, -1 = unavailable

static jfieldID try_field(Jvm *jvm, jclass cls, const char *const *names, const char *const *sigs) {
    JNIEnv *env = jvm->env;
    if (!cls) return 0;
    for (int i = 0; names[i]; i++) {
        for (int j = 0; sigs[j]; j++) {
            jfieldID f = (*env)->GetFieldID(env, cls, names[i], sigs[j]);
            if (f) return f;
            (*env)->ExceptionClear(env);
        }
    }
    return 0;
}

static jmethodID try_method(Jvm *jvm, jclass cls, const char *const *names, const char *const *sigs) {
    JNIEnv *env = jvm->env;
    if (!cls) return 0;
    for (int i = 0; names[i]; i++) {
        for (int j = 0; sigs[j]; j++) {
            jmethodID m = (*env)->GetMethodID(env, cls, names[i], sigs[j]);
            if (m) return m;
            (*env)->ExceptionClear(env);
        }
    }
    return 0;
}

static void resolve(Jvm *jvm, void *player, void *controller) {
    resolve_state = -1;
    if (!jvm || !player || !controller) return;

    JNIEnv *env = jvm->env;
    jclass player_cls = (*env)->GetObjectClass(env, (jobject)player);
    jclass ctrl_cls = (*env)->GetObjectClass(env, (jobject)controller);
    if (!player_cls || !ctrl_cls) {
        if (player_cls) (*env)->DeleteLocalRef(env, player_cls);
        if (ctrl_cls) (*env)->DeleteLocalRef(env, ctrl_cls);
        return;
    }

    static const char *const open_names[] = { "openContainer", "field_71070_bA", "f_71070_bA", "bk", 0 };
    static const char *const open_sigs[] = {
        "Lnet/minecraft/inventory/Container;",
        "Lnet/minecraft/class_1263;",       // Mojang intermediary
        "Lxi;",                             // 1.8.9 obfuscated
        0,
    };
    id_open_container = try_field(jvm, player_cls, open_names, open_sigs);

    if (id_open_container) {
        // The container's own class is only known once we have one in hand.
        jobject container = (*env)->GetObjectField(env, (jobject)player, id_open_container);
        if (container) {
            jclass container_cls = (*env)->GetObjectClass(env, container);
            static const char *const wid_names[] = { "windowId", "field_75152_c", "f_75152_c", "d", 0 };
            static const char *const wid_sigs[] = { "I", 0 };
            id_window_id = try_field(jvm, container_cls, wid_names, wid_sigs);
            if (container_cls) (*env)->DeleteLocalRef(env, container_cls);
            (*env)->DeleteLocalRef(env, container);
        }
    }

    static const char *const wc_names[] = { "windowClick", "func_78753_a", "f_78753_a", "a", 0 };
    static const char *const wc_sigs[] = {
        "(IIIILnet/minecraft/entity/player/EntityPlayer;)Lnet/minecraft/item/ItemStack;",
        "(IIIILnet/minecraft/world/entity/player/Player;)Ljava/lang/Object;",
        "(IIIILwn;)Lzx;",
        0,
    };
    id_window_click = try_method(jvm, ctrl_cls, wc_names, wc_sigs);

    (*env)->DeleteLocalRef(env, player_cls);
    (*env)->DeleteLocalRef(env, ctrl_cls);

    if (id_open_container && id_window_id && id_window_click) {
        resolve_state = 1;
        slinky_logf("window_click: resolved (openContainer, windowId, windowClick)");
    } else {
        slinky_logf("window_click: unresolved (open=%d wid=%d wc=%d)",
                    id_open_container != 0, id_window_id != 0, id_window_click != 0);
    }
}

// Whether the silent path is usable this session.
extern "C" int window_click_available(void) { return resolve_state == 1; }

// One container click. `slot` is a window slot, button 0 left, mode 0 pickup / 1 shift-click / 4 throw.
// Returns non-zero when the click was dispatched.
extern "C" int window_click(Ctx *ctx, int slot, int button, int mode) {
    if (!ctx || !ctx->player || !ctx->minecraft) return 0;
    Jvm *jvm = (Jvm *)ctx->jvm;
    void *controller = java_get_minecraft_player_controller(ctx->jvm, ctx->minecraft);
    if (!controller) return 0;

    if (resolve_state == 0) resolve(jvm, ctx->player, controller);
    if (resolve_state != 1) return 0;

    JNIEnv *env = jvm->env;
    jobject container = (*env)->GetObjectField(env, (jobject)ctx->player, id_open_container);
    if (!container) return 0;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 0; }

    int window_id = (*env)->GetIntField(env, container, id_window_id);
    (*env)->DeleteLocalRef(env, container);

    (*env)->CallObjectMethod(env, (jobject)controller, id_window_click,
                             (jint)window_id, (jint)slot, (jint)button, (jint)mode,
                             (jobject)ctx->player);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    return 1;
}
