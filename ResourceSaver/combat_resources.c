#include <stddef.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.0.3 - stable core
 *
 * The v1.0.2 hooks were all installed, but combat logic returned early when
 * Player.HeldItem could not be invoked at runtime. This version removes that
 * dependency from all core effects.
 *
 * Stable effects applied after Player.ResetEffects():
 *   - ammoCost80 = true              -> vanilla 20% ammo conservation
 *   - manaCost *= 0.85               -> 15% lower mana cost
 *   - accTackleBox = true            -> vanilla bait-conservation mechanic
 *
 * Optional HeldItem classification is only used to upgrade summon/sentry mana
 * saving to 25%. If HeldItem cannot be invoked, the base 15% still works.
 *
 * Player.Update(int) postfix:
 *   - +20% mana regen accumulation
 *   - potion sickness recovers 10% faster (all classes, stability-first)
 */

static patch_handle_t g_player_potion_delay = PATCH_NULL;
static patch_handle_t g_player_buff_type = PATCH_NULL;
static patch_handle_t g_player_buff_time = PATCH_NULL;
static patch_handle_t g_player_mana_regen = PATCH_NULL;
static patch_handle_t g_player_mana_regen_count = PATCH_NULL;
static patch_handle_t g_player_stat_mana = PATCH_NULL;
static patch_handle_t g_player_stat_mana_max2 = PATCH_NULL;
static patch_handle_t g_player_mana_cost = PATCH_NULL;
static patch_handle_t g_player_ammo_cost80 = PATCH_NULL;
static patch_handle_t g_player_acc_tackle_box = PATCH_NULL;

/* Optional summon classification. */
static patch_handle_t g_held_item_getter = PATCH_NULL;
static patch_handle_t g_item_mana = PATCH_NULL;
static patch_handle_t g_item_summon = PATCH_NULL;
static patch_handle_t g_item_sentry = PATCH_NULL;

static patch_hook_id_t g_reset_effects_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;

typedef struct player_runtime_state_t {
    patch_handle_t player;
    int mana_regen_remainder;
    int potion_tick;
} player_runtime_state_t;

static player_runtime_state_t g_states[256];

static player_runtime_state_t* state_for(patch_handle_t player) {
    if (!player) return NULL;

    for (size_t i = 0; i < 256; ++i) {
        if (g_states[i].player == player) return &g_states[i];
    }

    for (size_t i = 0; i < 256; ++i) {
        if (!g_states[i].player) {
            g_states[i].player = player;
            g_states[i].mana_regen_remainder = 0;
            g_states[i].potion_tick = 0;
            return &g_states[i];
        }
    }

    return NULL;
}

static patch_handle_t try_get_held_item(patch_handle_t player) {
    if (!player || !g_held_item_getter) return PATCH_NULL;

    patch_handle_t held = PATCH_NULL;
    if (!patchlib_method_invoke_args(g_held_item_getter, player, &held, NULL))
        return PATCH_NULL;

    return held;
}

static void reset_effects_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)args;
    (void)result;
    (void)sig;

    if (!instance) return;

    /*
     * Stable ammo saving: apply Terraria's own 20% conservation flag.
     * It only matters when vanilla actually consumes ammo.
     */
    if (g_player_ammo_cost80) {
        bool enabled = true;
        patchlib_field_set_value(g_player_ammo_cost80, instance, &enabled);
    }

    /* Stable bait saving using Terraria's own Tackle Box calculation. */
    if (g_player_acc_tackle_box) {
        bool enabled = true;
        patchlib_field_set_value(g_player_acc_tackle_box, instance, &enabled);
    }

    /*
     * Stable mana saving: never depend on HeldItem. Every item that consumes
     * mana uses Player.manaCost, so 0.85 applies the requested 15% reduction.
     */
    if (g_player_mana_cost) {
        float mana_cost = 1.0f;
        patchlib_field_get_value(g_player_mana_cost, instance, &mana_cost);
        float multiplier = 0.85f;

        /* Optional: summon/sentry gets 25% saving if HeldItem works. */
        patch_handle_t held = try_get_held_item(instance);
        if (held && g_item_mana) {
            int item_mana = 0;
            patchlib_field_get_value(g_item_mana, held, &item_mana);
            if (item_mana > 0) {
                bool summon = false;
                bool sentry = false;
                if (g_item_summon)
                    patchlib_field_get_value(g_item_summon, held, &summon);
                if (g_item_sentry)
                    patchlib_field_get_value(g_item_sentry, held, &sentry);
                if (summon || sentry)
                    multiplier = 0.75f;
            }
        }

        mana_cost *= multiplier;
        if (mana_cost < 0.05f) mana_cost = 0.05f;
        patchlib_field_set_value(g_player_mana_cost, instance, &mana_cost);
    }
}

static void reduce_potion_sickness_buff_one_tick(patch_handle_t player) {
    if (!g_player_buff_type || !g_player_buff_time) return;

    patch_handle_t types = PATCH_NULL;
    patch_handle_t times = PATCH_NULL;

    patchlib_field_get_value(g_player_buff_type, player, &types);
    patchlib_field_get_value(g_player_buff_time, player, &times);
    if (!types || !times) return;

    size_t type_len = patchlib_array_length(types);
    size_t time_len = patchlib_array_length(times);
    size_t len = type_len < time_len ? type_len : time_len;

    for (size_t i = 0; i < len; ++i) {
        int buff_id = 0;
        if (!patchlib_array_at(types, i, &buff_id)) continue;
        if (buff_id != 21) continue; /* Potion Sickness */

        int time = 0;
        if (!patchlib_array_at(times, i, &time)) return;
        if (time > 0) {
            --time;
            patchlib_array_set(times, i, &time);
        }
        return;
    }
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

    player_runtime_state_t* state = state_for(instance);
    if (!state) return;

    /* +20% natural mana-regeneration accumulation. */
    if (g_player_mana_regen && g_player_mana_regen_count) {
        int mana_regen = 0;
        int mana_regen_count = 0;
        int stat_mana = 0;
        int stat_mana_max2 = 0;

        patchlib_field_get_value(g_player_mana_regen, instance, &mana_regen);
        patchlib_field_get_value(g_player_mana_regen_count, instance, &mana_regen_count);

        if (g_player_stat_mana && g_player_stat_mana_max2) {
            patchlib_field_get_value(g_player_stat_mana, instance, &stat_mana);
            patchlib_field_get_value(g_player_stat_mana_max2, instance, &stat_mana_max2);
        }

        if (mana_regen > 0 &&
            (!g_player_stat_mana || !g_player_stat_mana_max2 || stat_mana < stat_mana_max2)) {
            state->mana_regen_remainder += mana_regen;
            int extra = state->mana_regen_remainder / 5;
            state->mana_regen_remainder %= 5;

            if (extra > 0) {
                mana_regen_count += extra;
                patchlib_field_set_value(g_player_mana_regen_count, instance, &mana_regen_count);
            }
        }
    }

    /*
     * Potion sickness recovers 10% faster.
     * v1.0.2 gated this behind HeldItem/melee detection; that reintroduced the
     * same runtime getter dependency. v1.0.3 intentionally applies it to all
     * classes so the mechanic is reliable.
     */
    if (g_player_potion_delay) {
        int potion_delay = 0;
        patchlib_field_get_value(g_player_potion_delay, instance, &potion_delay);

        if (potion_delay <= 0) {
            state->potion_tick = 0;
        }
        else {
            ++state->potion_tick;
            if (state->potion_tick >= 10) {
                state->potion_tick = 0;
                --potion_delay;
                if (potion_delay < 0) potion_delay = 0;
                patchlib_field_set_value(g_player_potion_delay, instance, &potion_delay);
                reduce_potion_sickness_buff_one_tick(instance);
            }
        }
    }
}

static void free_handle(patch_handle_t* h) {
    if (*h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

void resource_saver_combat_init(void) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    patch_handle_t item_type = patchlib_type_get_type("Terraria", "Item");
    patch_handle_t held_item_property = PATCH_NULL;

    if (!player_type || !item_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
            "Combat v1.0.3 init failed: Player=%p Item=%p", player_type, item_type);
        goto done;
    }

    g_player_potion_delay = patchlib_type_get_field(player_type, "potionDelay");
    g_player_buff_type = patchlib_type_get_field(player_type, "buffType");
    g_player_buff_time = patchlib_type_get_field(player_type, "buffTime");
    g_player_mana_regen = patchlib_type_get_field(player_type, "manaRegen");
    g_player_mana_regen_count = patchlib_type_get_field(player_type, "manaRegenCount");
    g_player_stat_mana = patchlib_type_get_field(player_type, "statMana");
    g_player_stat_mana_max2 = patchlib_type_get_field(player_type, "statManaMax2");
    g_player_mana_cost = patchlib_type_get_field(player_type, "manaCost");
    g_player_ammo_cost80 = patchlib_type_get_field(player_type, "ammoCost80");
    g_player_acc_tackle_box = patchlib_type_get_field(player_type, "accTackleBox");

    /* Optional summon/sentry classification only. */
    held_item_property = patchlib_type_get_property(player_type, "HeldItem");
    if (held_item_property)
        g_held_item_getter = patchlib_property_get_get_method(held_item_property);
    g_item_mana = patchlib_type_get_field(item_type, "mana");
    g_item_summon = patchlib_type_get_field(item_type, "summon");
    g_item_sentry = patchlib_type_get_field(item_type, "sentry");

    patch_handle_t reset_effects = patchlib_type_get_method_by_param_count(
        player_type, "ResetEffects", 0);
    patch_handle_t update = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    /* Core hook no longer requires HeldItem. */
    if (reset_effects && g_player_ammo_cost80 && g_player_mana_cost) {
        g_reset_effects_hook = patchlib_install_prepost_hook(
            reset_effects, NULL, reset_effects_postfix);
    }

    if (update) {
        g_update_hook = patchlib_install_prepost_hook(update, NULL, update_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Combat v1.0.3 hooks: ResetEffects=%d Update=%d ammo80=%p manaCost=%p tackle=%p HeldItem=%p",
        (int)g_reset_effects_hook,
        (int)g_update_hook,
        g_player_ammo_cost80,
        g_player_mana_cost,
        g_player_acc_tackle_box,
        g_held_item_getter);

    if (reset_effects) patchlib_free(reset_effects);
    if (update) patchlib_free(update);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
    (void)held_item_property;
}

void resource_saver_combat_cleanup(void) {
    if (g_reset_effects_hook != PATCH_HOOK_INVALID_ID)
        patchlib_uninstall_hook(g_reset_effects_hook);
    if (g_update_hook != PATCH_HOOK_INVALID_ID)
        patchlib_uninstall_hook(g_update_hook);

    g_reset_effects_hook = PATCH_HOOK_INVALID_ID;
    g_update_hook = PATCH_HOOK_INVALID_ID;

    free_handle(&g_player_potion_delay);
    free_handle(&g_player_buff_type);
    free_handle(&g_player_buff_time);
    free_handle(&g_player_mana_regen);
    free_handle(&g_player_mana_regen_count);
    free_handle(&g_player_stat_mana);
    free_handle(&g_player_stat_mana_max2);
    free_handle(&g_player_mana_cost);
    free_handle(&g_player_ammo_cost80);
    free_handle(&g_player_acc_tackle_box);

    free_handle(&g_held_item_getter);
    free_handle(&g_item_mana);
    free_handle(&g_item_summon);
    free_handle(&g_item_sentry);

    for (size_t i = 0; i < 256; ++i) {
        g_states[i].player = PATCH_NULL;
        g_states[i].mana_regen_remainder = 0;
        g_states[i].potion_tick = 0;
    }
}
