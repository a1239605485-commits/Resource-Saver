#include <stddef.h>
#include <stdint.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver - bait conservation
 *
 * Vanilla already has bait-consumption rules based on bait power and tackle
 * equipment. This module adds one independent 20% save after vanilla actually
 * consumes bait. It snapshots bait stacks around
 * Player.ItemCheck_CheckFishingBobbers(bool) and restores one unit on success.
 */

#define RS_MAX_BAIT_SLOTS 64

static patch_handle_t g_player_inventory = PATCH_NULL;
static patch_handle_t g_item_type = PATCH_NULL;
static patch_handle_t g_item_stack = PATCH_NULL;
static patch_handle_t g_item_bait = PATCH_NULL;
static patch_handle_t g_item_set_defaults = PATCH_NULL;
static patch_hook_id_t g_fishing_hook = PATCH_HOOK_INVALID_ID;

static uint32_t g_rng_state = 0x91E10DA5u;

typedef struct bait_slot_snapshot_t {
    patch_handle_t item;
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
        g_snapshot[i].item = PATCH_NULL;
        g_snapshot[i].type = 0;
        g_snapshot[i].stack = 0;
        g_snapshot[i].bait = 0;
        g_snapshot[i].valid = 0;
    }
}

static bool fishing_prefix(
    patch_handle_t instance,
    void** args,
    const patch_method_signature_t* sig,
    void* result
) {
    (void)args;
    (void)sig;
    (void)result;

    clear_snapshot();

    if (!instance || !g_player_inventory || !g_item_type ||
        !g_item_stack || !g_item_bait) {
        return true;
    }

    patch_handle_t inventory = PATCH_NULL;
    patchlib_field_get_value(g_player_inventory, instance, &inventory);
    if (!inventory) return true;

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

        g_snapshot[i].item = item;
        g_snapshot[i].type = type;
        g_snapshot[i].stack = stack;
        g_snapshot[i].bait = bait;
        g_snapshot[i].valid = 1;
    }

    return true; /* continue vanilla fishing logic */
}

static void fishing_postfix(
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

        /* No bait was consumed from this slot. */
        if (current_type == g_snapshot[i].type && current_stack >= g_snapshot[i].stack) {
            continue;
        }

        /* If the slot changed to another real item, do not touch it. */
        if (current_type != 0 && current_type != g_snapshot[i].type) {
            continue;
        }

        /* Only restore when vanilla decreased the bait stack. */
        if (current_type == g_snapshot[i].type && current_stack >= g_snapshot[i].stack) {
            continue;
        }

        if (!roll_20_percent()) continue;

        if (current_type == g_snapshot[i].type && current_stack >= 0) {
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
    g_item_type = patchlib_type_get_field(item_type, "type");
    g_item_stack = patchlib_type_get_field(item_type, "stack");
    g_item_bait = patchlib_type_get_field(item_type, "bait");
    g_item_set_defaults = patchlib_type_get_method_by_param_count(
        item_type, "SetDefaults", 1);

    patch_handle_t fishing = patchlib_type_get_method_by_param_count(
        player_type, "ItemCheck_CheckFishingBobbers", 1);

    if (fishing && g_player_inventory && g_item_type && g_item_stack && g_item_bait) {
        g_fishing_hook = patchlib_install_prepost_hook(
            fishing, fishing_prefix, fishing_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Bait saver hook: %s (id=%d, SetDefaults=%p)",
        g_fishing_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_fishing_hook, g_item_set_defaults);

    if (fishing) patchlib_free(fishing);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
}

void resource_saver_fishing_cleanup(void) {
    if (g_fishing_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_fishing_hook);
        g_fishing_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_player_inventory);
    free_handle(&g_item_type);
    free_handle(&g_item_stack);
    free_handle(&g_item_bait);
    free_handle(&g_item_set_defaults);
    clear_snapshot();
}
