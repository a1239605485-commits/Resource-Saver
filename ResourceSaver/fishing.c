#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.0.2 - bait conservation
 *
 * v1.0.1 watched Player.Update, which is not where Terraria 1.4.5 performs
 * the actual fishing catch/bait consumption work. v1.0.2 watches the vanilla
 * Projectile.FishingCheck() call instead, snapshots the owning player's bait
 * before the check, then restores one consumed bait with 20% probability.
 */

#define RS_MAX_BAIT_SLOTS 59

static patch_handle_t g_main_player = PATCH_NULL;
static patch_handle_t g_projectile_owner = PATCH_NULL;
static patch_handle_t g_player_inventory = PATCH_NULL;

static patch_handle_t g_item_type = PATCH_NULL;
static patch_handle_t g_item_stack = PATCH_NULL;
static patch_handle_t g_item_bait = PATCH_NULL;
static patch_handle_t g_item_set_defaults_1 = PATCH_NULL;
static patch_handle_t g_item_set_defaults_2 = PATCH_NULL;

static patch_hook_id_t g_fishing_hook = PATCH_HOOK_INVALID_ID;
static uint32_t g_rng_state = 0x91E10DA5u;

typedef struct bait_slot_snapshot_t {
    int type;
    int stack;
    int bait;
    int valid;
} bait_slot_snapshot_t;

static patch_handle_t g_snapshot_projectile = PATCH_NULL;
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
    g_snapshot_projectile = PATCH_NULL;
    g_snapshot_inventory = PATCH_NULL;
    g_snapshot_length = 0;

    for (size_t i = 0; i < RS_MAX_BAIT_SLOTS; ++i) {
        g_snapshot[i].type = 0;
        g_snapshot[i].stack = 0;
        g_snapshot[i].bait = 0;
        g_snapshot[i].valid = 0;
    }
}

static patch_handle_t get_projectile_owner_player(patch_handle_t projectile) {
    if (!projectile || !g_projectile_owner || !g_main_player) return PATCH_NULL;

    int owner = -1;
    patchlib_field_get_value(g_projectile_owner, projectile, &owner);
    if (owner < 0) return PATCH_NULL;

    patch_handle_t players = PATCH_NULL;
    patchlib_field_get_value(g_main_player, NULL, &players);
    if (!players) return PATCH_NULL;

    size_t length = patchlib_array_length(players);
    if ((size_t)owner >= length) return PATCH_NULL;

    patch_handle_t player = PATCH_NULL;
    if (!patchlib_array_at(players, (size_t)owner, &player)) return PATCH_NULL;
    return player;
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

    patch_handle_t player = get_projectile_owner_player(instance);
    if (!player || !g_player_inventory || !g_item_type || !g_item_stack || !g_item_bait)
        return false;

    patch_handle_t inventory = PATCH_NULL;
    patchlib_field_get_value(g_player_inventory, player, &inventory);
    if (!inventory) return false;

    size_t length = patchlib_array_length(inventory);
    if (length > RS_MAX_BAIT_SLOTS) length = RS_MAX_BAIT_SLOTS;

    g_snapshot_projectile = instance;
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

    return false; /* false = continue vanilla FishingCheck */
}

static int restore_last_bait_item(patch_handle_t item, int old_type) {
    if (!item || old_type <= 0) return 0;

    if (g_item_set_defaults_2) {
        patch_handle_t variant = PATCH_NULL;
        void* set_args[2] = { &old_type, &variant };
        if (patchlib_method_invoke_args(
                g_item_set_defaults_2, item, NULL, set_args)) {
            int one = 1;
            patchlib_field_set_value(g_item_stack, item, &one);
            return 1;
        }
    }

    if (g_item_set_defaults_1) {
        void* set_args[1] = { &old_type };
        if (patchlib_method_invoke_args(
                g_item_set_defaults_1, item, NULL, set_args)) {
            int one = 1;
            patchlib_field_set_value(g_item_stack, item, &one);
            return 1;
        }
    }

    return 0;
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

    if (!instance || instance != g_snapshot_projectile || !g_snapshot_inventory) {
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
            consumed = g_snapshot[i].stack;
        }
        else {
            continue;
        }

        if (consumed != 1) continue;
        if (!roll_20_percent()) break;

        if (current_type == g_snapshot[i].type) {
            int restored = current_stack + 1;
            if (restored > g_snapshot[i].stack)
                restored = g_snapshot[i].stack;
            patchlib_field_set_value(g_item_stack, item, &restored);
        }
        else if (current_type == 0) {
            restore_last_bait_item(item, g_snapshot[i].type);
        }

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
    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t projectile_type = patchlib_type_get_type("Terraria", "Projectile");
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    patch_handle_t item_type = patchlib_type_get_type("Terraria", "Item");

    if (!main_type || !projectile_type || !player_type || !item_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
            "Fishing init failed: Main=%p Projectile=%p Player=%p Item=%p",
            main_type, projectile_type, player_type, item_type);
        goto done;
    }

    g_main_player = patchlib_type_get_field(main_type, "player");
    g_projectile_owner = patchlib_type_get_field(projectile_type, "owner");
    g_player_inventory = patchlib_type_get_field(player_type, "inventory");

    g_item_type = patchlib_type_get_field(item_type, "type");
    g_item_stack = patchlib_type_get_field(item_type, "stack");
    g_item_bait = patchlib_type_get_field(item_type, "bait");

    g_item_set_defaults_2 = patchlib_type_get_method_by_param_count(
        item_type, "SetDefaults", 2);
    g_item_set_defaults_1 = patchlib_type_get_method_by_param_count(
        item_type, "SetDefaults", 1);

    patch_handle_t fishing_check = patchlib_type_get_method_by_param_count(
        projectile_type, "FishingCheck", 0);

    if (fishing_check && g_main_player && g_projectile_owner &&
        g_player_inventory && g_item_type && g_item_stack && g_item_bait) {
        g_fishing_hook = patchlib_install_prepost_hook(
            fishing_check, fishing_prefix, fishing_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Bait saver hook v1.0.2: %s (id=%d FishingCheck=%p Main.player=%p owner=%p bait=%p SetDefaults1=%p SetDefaults2=%p)",
        g_fishing_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_fishing_hook,
        fishing_check,
        g_main_player,
        g_projectile_owner,
        g_item_bait,
        g_item_set_defaults_1,
        g_item_set_defaults_2);

    if (fishing_check) patchlib_free(fishing_check);

done:
    if (main_type) patchlib_free(main_type);
    if (projectile_type) patchlib_free(projectile_type);
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
}

void resource_saver_fishing_cleanup(void) {
    if (g_fishing_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_fishing_hook);
        g_fishing_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_main_player);
    free_handle(&g_projectile_owner);
    free_handle(&g_player_inventory);
    free_handle(&g_item_type);
    free_handle(&g_item_stack);
    free_handle(&g_item_bait);
    free_handle(&g_item_set_defaults_1);
    free_handle(&g_item_set_defaults_2);

    clear_snapshot();
}
