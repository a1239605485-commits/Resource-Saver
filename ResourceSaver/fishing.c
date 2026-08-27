#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.0.1 - bait conservation
 *
 * v1.0.0 tried to hook ItemCheck_CheckFishingBobbers(bool), which is not a
 * stable vanilla Android Terraria method name in this build.
 *
 * v1.0.1 uses Player.Update(int), which is already proven to exist in this
 * game build. Before each local Player.Update, while a fishing pole is held,
 * we snapshot bait stacks. After the update, if vanilla consumed exactly one
 * bait item, there is an independent 20% chance to restore it.
 */

#define RS_MAX_BAIT_SLOTS 59

static patch_handle_t g_player_inventory = PATCH_NULL;
static patch_handle_t g_player_selected_item = PATCH_NULL;

static patch_handle_t g_item_type = PATCH_NULL;
static patch_handle_t g_item_stack = PATCH_NULL;
static patch_handle_t g_item_bait = PATCH_NULL;
static patch_handle_t g_item_fishing_pole = PATCH_NULL;
static patch_handle_t g_item_set_defaults = PATCH_NULL;

static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;

static uint32_t g_rng_state = 0x91E10DA5u;

typedef struct bait_slot_snapshot_t {
    int type;
    int stack;
    int bait;
    int valid;
} bait_slot_snapshot_t;

static patch_handle_t g_snapshot_owner = PATCH_NULL;
static patch_handle_t g_snapshot_inventory = PATCH_NULL;
static size_t g_snapshot_length = 0;
static bait_slot_snapshot_t g_snapshot[RS_MAX_BAIT_SLOTS];

static uint32_t rs_rand_u32(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x ? x : 0x7F4A7C15u;
    return g_rng_state;
}

static int roll_20_percent(void) {
    return (rs_rand_u32() % 100u) < 20u;
}

static void clear_snapshot(void) {
    g_snapshot_owner = PATCH_NULL;
    g_snapshot_inventory = PATCH_NULL;
    g_snapshot_length = 0;

    for (size_t i = 0; i < RS_MAX_BAIT_SLOTS; ++i) {
        g_snapshot[i].type = 0;
        g_snapshot[i].stack = 0;
        g_snapshot[i].bait = 0;
        g_snapshot[i].valid = 0;
    }
}

static patch_handle_t get_held_item(patch_handle_t player) {
    if (!player || !g_player_inventory || !g_player_selected_item) return PATCH_NULL;

    patch_handle_t inventory = PATCH_NULL;
    int selected = -1;

    patchlib_field_get_value(g_player_inventory, player, &inventory);
    patchlib_field_get_value(g_player_selected_item, player, &selected);

    if (!inventory || selected < 0) return PATCH_NULL;

    size_t length = patchlib_array_length(inventory);
    if ((size_t)selected >= length) return PATCH_NULL;

    patch_handle_t held = PATCH_NULL;
    if (!patchlib_array_at(inventory, (size_t)selected, &held)) return PATCH_NULL;

    return held;
}

static bool update_prefix(
    patch_handle_t instance,
    void** args,
    const patch_method_signature_t* sig,
    void* result
) {
    (void)args;
    (void)sig;
    (void)result;

    clear_snapshot();

    if (!instance || !g_player_inventory || !g_player_selected_item ||
        !g_item_type || !g_item_stack || !g_item_bait || !g_item_fishing_pole) {
        return false; /* continue Player.Update */
    }

    /* Only watch inventory while a fishing pole is currently held. */
    patch_handle_t held = get_held_item(instance);
    if (!held) return false;

    int fishing_power = 0;
    patchlib_field_get_value(g_item_fishing_pole, held, &fishing_power);
    if (fishing_power <= 0) return false;

    patch_handle_t inventory = PATCH_NULL;
    patchlib_field_get_value(g_player_inventory, instance, &inventory);
    if (!inventory) return false;

    size_t length = patchlib_array_length(inventory);
    if (length > RS_MAX_BAIT_SLOTS) length = RS_MAX_BAIT_SLOTS;

    g_snapshot_owner = instance;
    g_snapshot_inventory = inventory;
    g_snapshot_length = length;

    for (size_t i = 0; i < length; ++i) {
        patch_handle_t item = PATCH_NULL;
        if (!patchlib_array_at(inventory, i, &item) || !item) continue;

        int type = 0;
        int stack = 0;
        int bait = 0;

        patchlib_field_get_value(g_item_type, item, &type);
        patchlib_field_get_value(g_item_stack, item, &stack);
        patchlib_field_get_value(g_item_bait, item, &bait);

        if (type <= 0 || stack <= 0 || bait <= 0) continue;

        g_snapshot[i].type = type;
        g_snapshot[i].stack = stack;
        g_snapshot[i].bait = bait;
        g_snapshot[i].valid = 1;
    }

    return false; /* IMPORTANT: false = run vanilla Player.Update */
}

static void update_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)args;
    (void)result;
    (void)sig;

    if (!instance || instance != g_snapshot_owner || !g_snapshot_inventory) {
        clear_snapshot();
        return;
    }

    for (size_t i = 0; i < g_snapshot_length; ++i) {
        if (!g_snapshot[i].valid) continue;

        patch_handle_t item = PATCH_NULL;
        if (!patchlib_array_at(g_snapshot_inventory, i, &item) || !item) continue;

        int current_type = 0;
        int current_stack = 0;

        patchlib_field_get_value(g_item_type, item, &current_type);
        patchlib_field_get_value(g_item_stack, item, &current_stack);

        int consumed = 0;

        if (current_type == g_snapshot[i].type) {
            consumed = g_snapshot[i].stack - current_stack;
        }
        else if (current_type == 0) {
            /* Slot turned to air: this matters especially for the last bait. */
            consumed = g_snapshot[i].stack;
        }
        else {
            /* Slot became another real item; don't touch it. */
            continue;
        }

        /* Vanilla normally consumes one bait at a cast; require exactly one. */
        if (consumed != 1) continue;
        if (!roll_20_percent()) continue;

        if (current_type == g_snapshot[i].type) {
            int restored = current_stack + 1;
            if (restored > g_snapshot[i].stack) restored = g_snapshot[i].stack;
            patchlib_field_set_value(g_item_stack, item, &restored);
        }
        else if (current_type == 0 && g_item_set_defaults) {
            int old_type = g_snapshot[i].type;
            void* set_args[1] = { &old_type };

            if (patchlib_method_invoke_args(
                    g_item_set_defaults, item, NULL, set_args)) {
                int restored = 1;
                patchlib_field_set_value(g_item_stack, item, &restored);
            }
        }

        /* Only one bait can be consumed by one cast. */
        break;
    }

    clear_snapshot();
}

static void free_handle(patch_handle_t* h) {
    if (*h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

void resource_saver_fishing_init(void) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    patch_handle_t item_type = patchlib_type_get_type("Terraria", "Item");

    if (!player_type || !item_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Fishing init failed: Player=%p Item=%p",
                         player_type, item_type);
        goto done;
    }

    g_player_inventory = patchlib_type_get_field(player_type, "inventory");
    g_player_selected_item = patchlib_type_get_field(player_type, "selectedItem");

    g_item_type = patchlib_type_get_field(item_type, "type");
    g_item_stack = patchlib_type_get_field(item_type, "stack");
    g_item_bait = patchlib_type_get_field(item_type, "bait");
    g_item_fishing_pole = patchlib_type_get_field(item_type, "fishingPole");
    g_item_set_defaults = patchlib_type_get_method_by_param_count(
        item_type, "SetDefaults", 1);

    patch_handle_t update = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    if (update && g_player_inventory && g_player_selected_item &&
        g_item_type && g_item_stack && g_item_bait && g_item_fishing_pole) {
        g_update_hook = patchlib_install_prepost_hook(
            update, update_prefix, update_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Bait saver hook v1.0.1: %s (id=%d, Update=%p, fishingPole=%p, bait=%p, SetDefaults=%p)",
        g_update_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_update_hook,
        update,
        g_item_fishing_pole,
        g_item_bait,
        g_item_set_defaults);

    if (update) patchlib_free(update);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
}

void resource_saver_fishing_cleanup(void) {
    if (g_update_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_update_hook);
        g_update_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_player_inventory);
    free_handle(&g_player_selected_item);
    free_handle(&g_item_type);
    free_handle(&g_item_stack);
    free_handle(&g_item_bait);
    free_handle(&g_item_fishing_pole);
    free_handle(&g_item_set_defaults);

    clear_snapshot();
}
