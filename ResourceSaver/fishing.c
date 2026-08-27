#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "config.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.2.0 - fixed extra 20% bait saving
 *
 * The stable v1.0.3 used accTackleBox, which definitely worked but did not
 * equal a fixed +20% independent save chance. v1.1.0 instead hooks the
 * already-proven Player.Update(int), snapshots bait stacks before the update,
 * and if exactly one bait was consumed during the update, restores it with an
 * independent 20% roll.
 *
 * This intentionally avoids selectedItem / fishing-pole method dependencies.
 */

#define RS_MAX_BAIT_SLOTS 59

static patch_handle_t g_player_inventory = PATCH_NULL;
static patch_handle_t g_item_type = PATCH_NULL;
static patch_handle_t g_item_stack = PATCH_NULL;
static patch_handle_t g_item_bait = PATCH_NULL;
static patch_handle_t g_item_set_defaults_1 = PATCH_NULL;
static patch_handle_t g_item_set_defaults_2 = PATCH_NULL;

static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
static uint32_t g_rng_state = 0x91E10DA5u;
static uint64_t g_detected_consumptions = 0;
static uint64_t g_saved_bait = 0;

typedef struct bait_slot_snapshot_t {
    int type;
    int stack;
    int bait;
    int valid;
} bait_slot_snapshot_t;

typedef struct bait_player_snapshot_t {
    patch_handle_t player;
    patch_handle_t inventory;
    size_t length;
    int active;
    bait_slot_snapshot_t slots[RS_MAX_BAIT_SLOTS];
} bait_player_snapshot_t;

/* Player.Update is normally serial on the game thread, but keep several slots
 * so nested/remote Player updates do not overwrite another player's snapshot. */
static bait_player_snapshot_t g_snapshots[16];

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

static void clear_snapshot(bait_player_snapshot_t* s) {
    if (!s) return;
    s->player = PATCH_NULL;
    s->inventory = PATCH_NULL;
    s->length = 0;
    s->active = 0;

    for (size_t i = 0; i < RS_MAX_BAIT_SLOTS; ++i) {
        s->slots[i].type = 0;
        s->slots[i].stack = 0;
        s->slots[i].bait = 0;
        s->slots[i].valid = 0;
    }
}

static bait_player_snapshot_t* snapshot_for_player(patch_handle_t player) {
    if (!player) return NULL;

    for (size_t i = 0; i < 16; ++i) {
        if (g_snapshots[i].active && g_snapshots[i].player == player)
            return &g_snapshots[i];
    }

    for (size_t i = 0; i < 16; ++i) {
        if (!g_snapshots[i].active) {
            clear_snapshot(&g_snapshots[i]);
            g_snapshots[i].player = player;
            return &g_snapshots[i];
        }
    }

    return &g_snapshots[0];
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

    if (!resource_saver_feature_enabled(RS_FEATURE_BAIT)) {
        return false;
    }

    if (!instance || !g_player_inventory || !g_item_type ||
        !g_item_stack || !g_item_bait) {
        return false; /* continue vanilla Player.Update */
    }

    bait_player_snapshot_t* s = snapshot_for_player(instance);
    if (!s) return false;
    clear_snapshot(s);
    s->player = instance;

    patch_handle_t inventory = PATCH_NULL;
    patchlib_field_get_value(g_player_inventory, instance, &inventory);
    if (!inventory) return false;

    size_t length = patchlib_array_length(inventory);
    if (length > RS_MAX_BAIT_SLOTS) length = RS_MAX_BAIT_SLOTS;

    s->inventory = inventory;
    s->length = length;
    s->active = 1;

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

        s->slots[i].type = type;
        s->slots[i].stack = stack;
        s->slots[i].bait = bait;
        s->slots[i].valid = 1;
    }

    return false; /* false = run original Player.Update */
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

static void update_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)args;
    (void)result;
    (void)sig;

    if (!instance) return;

    if (!resource_saver_feature_enabled(RS_FEATURE_BAIT)) {
        for (size_t k = 0; k < 16; ++k) {
            if (g_snapshots[k].active && g_snapshots[k].player == instance) {
                clear_snapshot(&g_snapshots[k]);
                break;
            }
        }
        return;
    }

    bait_player_snapshot_t* s = NULL;
    for (size_t k = 0; k < 16; ++k) {
        if (g_snapshots[k].active && g_snapshots[k].player == instance) {
            s = &g_snapshots[k];
            break;
        }
    }
    if (!s || !s->inventory) return;

    for (size_t i = 0; i < s->length; ++i) {
        if (!s->slots[i].valid) continue;

        patch_handle_t item = PATCH_NULL;
        if (!patchlib_array_at(s->inventory, i, &item) || !item) continue;

        int current_type = 0;
        int current_stack = 0;
        patchlib_field_get_value(g_item_type, item, &current_type);
        patchlib_field_get_value(g_item_stack, item, &current_stack);

        int consumed = 0;
        if (current_type == s->slots[i].type) {
            consumed = s->slots[i].stack - current_stack;
        }
        else if (current_type == 0) {
            consumed = s->slots[i].stack;
        }
        else {
            continue;
        }

        /* Vanilla fishing consumes a single bait on a successful consume. */
        if (consumed != 1) continue;

        ++g_detected_consumptions;

        if (roll_20_percent()) {
            int restored = 0;

            if (current_type == s->slots[i].type) {
                int new_stack = current_stack + 1;
                if (new_stack > s->slots[i].stack)
                    new_stack = s->slots[i].stack;
                patchlib_field_set_value(g_item_stack, item, &new_stack);
                restored = 1;
            }
            else if (current_type == 0) {
                restored = restore_last_bait_item(item, s->slots[i].type);
            }

            if (restored) {
                ++g_saved_bait;
            }
        }

        /* One update/catch should consume at most one bait. */
        break;
    }

    clear_snapshot(s);
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
        mod_logger_write(
            MOD_LOG_LEVEL_ERROR,
            "ResourceSaver",
            "Fishing v1.2.0 init failed: Player=%p Item=%p",
            player_type,
            item_type
        );
        goto done;
    }

    g_player_inventory = patchlib_type_get_field(player_type, "inventory");
    g_item_type = patchlib_type_get_field(item_type, "type");
    g_item_stack = patchlib_type_get_field(item_type, "stack");
    g_item_bait = patchlib_type_get_field(item_type, "bait");

    g_item_set_defaults_2 = patchlib_type_get_method_by_param_count(
        item_type, "SetDefaults", 2);
    g_item_set_defaults_1 = patchlib_type_get_method_by_param_count(
        item_type, "SetDefaults", 1);

    patch_handle_t update = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    if (update && g_player_inventory && g_item_type && g_item_stack && g_item_bait) {
        g_update_hook = patchlib_install_prepost_hook(
            update, update_prefix, update_postfix);
    }

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Bait saver v1.2.0 fixed-20 hook: %s (id=%d Update=%p inventory=%p bait=%p SetDefaults1=%p SetDefaults2=%p)",
        g_update_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_update_hook,
        update,
        g_player_inventory,
        g_item_bait,
        g_item_set_defaults_1,
        g_item_set_defaults_2
    );

    if (update) patchlib_free(update);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
}

void resource_saver_fishing_cleanup(void) {
    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Bait diagnostics: detected_consumptions=%llu saved_by_mod=%llu",
        (unsigned long long)g_detected_consumptions,
        (unsigned long long)g_saved_bait
    );

    if (g_update_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_update_hook);
        g_update_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_player_inventory);
    free_handle(&g_item_type);
    free_handle(&g_item_stack);
    free_handle(&g_item_bait);
    free_handle(&g_item_set_defaults_1);
    free_handle(&g_item_set_defaults_2);

    for (size_t i = 0; i < 16; ++i)
        clear_snapshot(&g_snapshots[i]);
}
