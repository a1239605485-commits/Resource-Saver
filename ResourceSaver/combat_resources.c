#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.1.0 - precise combat resource rules
 *
 * Stable base: v1.0.3 proved that ResetEffects + Update and these Player
 * fields work on Terraria Android 1.4.5.6.4.
 *
 * v1.1.0 keeps that base and only adds classification when HeldItem is
 * available. If HeldItem cannot be read in a frame, the code falls back to
 * the v1.0.3 behavior instead of aborting the whole feature.
 *
 * Rules:
 *   regular ammo                  -> +20% vanilla conservation flag
 *   rockets / rare special ammo  -> +10% vanilla conservation flag
 *   sand / solution tool ammo    -> no Resource Saver conservation
 *   all mana use                 -> manaCost x 0.85
 *   summon / sentry mana use     -> manaCost x 0.75
 *   natural mana regen           -> +20%
 *   melee-held potion sickness   -> recovers 10% faster
 */

/* Terraria AmmoID values used by vanilla. */
#define RS_AMMO_NONE          0
#define RS_AMMO_GEL           23
#define RS_AMMO_ARROW         40
#define RS_AMMO_COIN          71
#define RS_AMMO_FALLEN_STAR   75
#define RS_AMMO_BULLET        97
#define RS_AMMO_SAND          169
#define RS_AMMO_DART          283
#define RS_AMMO_ALE           353
#define RS_AMMO_ROCKET        771
#define RS_AMMO_SOLUTION      780
#define RS_AMMO_FLARE         931
#define RS_AMMO_SNOWBALL      949
#define RS_AMMO_STYNGER       1261
#define RS_AMMO_CANDY_CORN    1783
#define RS_AMMO_JACK_O_LANTERN 1785
#define RS_AMMO_STAKE         1836
#define RS_AMMO_NAIL          3108

static patch_handle_t g_player_potion_delay = PATCH_NULL;
static patch_handle_t g_player_buff_type = PATCH_NULL;
static patch_handle_t g_player_buff_time = PATCH_NULL;
static patch_handle_t g_player_mana_regen = PATCH_NULL;
static patch_handle_t g_player_mana_regen_count = PATCH_NULL;
static patch_handle_t g_player_stat_mana = PATCH_NULL;
static patch_handle_t g_player_stat_mana_max2 = PATCH_NULL;
static patch_handle_t g_player_mana_cost = PATCH_NULL;
static patch_handle_t g_player_ammo_cost80 = PATCH_NULL;
static patch_handle_t g_player_huntress_ammo_cost90 = PATCH_NULL;

static patch_handle_t g_held_item_getter = PATCH_NULL;
static patch_handle_t g_item_mana = PATCH_NULL;
static patch_handle_t g_item_use_ammo = PATCH_NULL;
static patch_handle_t g_item_damage = PATCH_NULL;
static patch_handle_t g_item_melee = PATCH_NULL;
static patch_handle_t g_item_summon = PATCH_NULL;
static patch_handle_t g_item_sentry = PATCH_NULL;

static patch_hook_id_t g_reset_effects_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;

typedef struct player_runtime_state_t {
    patch_handle_t player;
    int mana_regen_remainder;
    int potion_tick;
    int held_class_valid;
    int held_is_melee;
    int held_is_summon;
    int held_use_ammo;
} player_runtime_state_t;

static player_runtime_state_t g_states[256];

/* Diagnostic counters: these are effect-application frames, not fake "items saved". */
static uint64_t g_stat_ammo20_frames = 0;
static uint64_t g_stat_ammo10_frames = 0;
static uint64_t g_stat_mana15_frames = 0;
static uint64_t g_stat_summon25_frames = 0;
static uint64_t g_stat_melee_potion_extra_ticks = 0;
static uint64_t g_stat_held_fallback_frames = 0;
static int g_logged_held_success = 0;
static int g_logged_held_failure = 0;

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
            g_states[i].held_class_valid = 0;
            g_states[i].held_is_melee = 0;
            g_states[i].held_is_summon = 0;
            g_states[i].held_use_ammo = 0;
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

static int is_regular_ammo_group(int use_ammo) {
    switch (use_ammo) {
        case RS_AMMO_GEL:
        case RS_AMMO_ARROW:
        case RS_AMMO_BULLET:
        case RS_AMMO_DART:
        case RS_AMMO_FLARE:
        case RS_AMMO_SNOWBALL:
            return 1;
        default:
            return 0;
    }
}

static int is_tool_ammo_group(int use_ammo) {
    return use_ammo == RS_AMMO_SAND || use_ammo == RS_AMMO_SOLUTION;
}

static int is_special_ammo_group(int use_ammo) {
    switch (use_ammo) {
        case RS_AMMO_COIN:
        case RS_AMMO_FALLEN_STAR:
        case RS_AMMO_ALE:
        case RS_AMMO_ROCKET:
        case RS_AMMO_STYNGER:
        case RS_AMMO_CANDY_CORN:
        case RS_AMMO_JACK_O_LANTERN:
        case RS_AMMO_STAKE:
        case RS_AMMO_NAIL:
            return 1;
        default:
            return 0;
    }
}

static void classify_held_item(
    patch_handle_t player,
    player_runtime_state_t* state,
    patch_handle_t* out_held
) {
    if (out_held) *out_held = PATCH_NULL;
    if (!state) return;

    state->held_class_valid = 0;
    state->held_is_melee = 0;
    state->held_is_summon = 0;
    state->held_use_ammo = 0;

    patch_handle_t held = try_get_held_item(player);
    if (!held) {
        ++g_stat_held_fallback_frames;
        if (!g_logged_held_failure) {
            g_logged_held_failure = 1;
            mod_logger_write(
                MOD_LOG_LEVEL_WARNING,
                "ResourceSaver",
                "HeldItem runtime read failed; v1.1.0 will use stable fallback rules for this frame"
            );
        }
        return;
    }

    int item_mana = 0;
    int item_damage = 0;
    int use_ammo = 0;
    bool melee = false;
    bool summon = false;
    bool sentry = false;

    if (g_item_mana) patchlib_field_get_value(g_item_mana, held, &item_mana);
    if (g_item_damage) patchlib_field_get_value(g_item_damage, held, &item_damage);
    if (g_item_use_ammo) patchlib_field_get_value(g_item_use_ammo, held, &use_ammo);
    if (g_item_melee) patchlib_field_get_value(g_item_melee, held, &melee);
    if (g_item_summon) patchlib_field_get_value(g_item_summon, held, &summon);
    if (g_item_sentry) patchlib_field_get_value(g_item_sentry, held, &sentry);

    state->held_class_valid = 1;
    state->held_is_melee = (melee && item_damage > 0) ? 1 : 0;
    state->held_is_summon = ((summon || sentry) && item_mana > 0) ? 1 : 0;
    state->held_use_ammo = use_ammo;

    if (out_held) *out_held = held;

    if (!g_logged_held_success) {
        g_logged_held_success = 1;
        mod_logger_write(
            MOD_LOG_LEVEL_INFO,
            "ResourceSaver",
            "HeldItem classification active: mana=%d damage=%d useAmmo=%d melee=%d summon=%d sentry=%d",
            item_mana,
            item_damage,
            use_ammo,
            melee ? 1 : 0,
            summon ? 1 : 0,
            sentry ? 1 : 0
        );
    }
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

    player_runtime_state_t* state = state_for(instance);
    if (!state) return;

    patch_handle_t held = PATCH_NULL;
    classify_held_item(instance, state, &held);
    (void)held;

    /*
     * Ammo saving.
     * When classification works, use the requested 20% / 10% split.
     * If HeldItem cannot be read, retain v1.0.3's proven 20% fallback.
     */
    if (g_player_ammo_cost80 || g_player_huntress_ammo_cost90) {
        bool enabled = true;

        if (!state->held_class_valid) {
            if (g_player_ammo_cost80) {
                patchlib_field_set_value(g_player_ammo_cost80, instance, &enabled);
                ++g_stat_ammo20_frames;
            }
        }
        else {
            int use_ammo = state->held_use_ammo;

            if (use_ammo == RS_AMMO_NONE || is_tool_ammo_group(use_ammo)) {
                /* No Resource Saver ammo flag for non-ammo or tool ammo. */
            }
            else if (is_regular_ammo_group(use_ammo)) {
                if (g_player_ammo_cost80) {
                    patchlib_field_set_value(g_player_ammo_cost80, instance, &enabled);
                    ++g_stat_ammo20_frames;
                }
            }
            else if (is_special_ammo_group(use_ammo)) {
                if (g_player_huntress_ammo_cost90) {
                    patchlib_field_set_value(g_player_huntress_ammo_cost90, instance, &enabled);
                    ++g_stat_ammo10_frames;
                }
                else if (g_player_ammo_cost80) {
                    /* Compatibility fallback if the 10% field is absent. */
                    patchlib_field_set_value(g_player_ammo_cost80, instance, &enabled);
                    ++g_stat_ammo20_frames;
                }
            }
            else {
                /* Unknown/custom vanilla ammo group: conservative 10%. */
                if (g_player_huntress_ammo_cost90) {
                    patchlib_field_set_value(g_player_huntress_ammo_cost90, instance, &enabled);
                    ++g_stat_ammo10_frames;
                }
            }
        }
    }

    /*
     * Mana saving.
     * Base -15% is always stable. Summon/sentry upgrades to -25% when the
     * held item classification succeeds.
     */
    if (g_player_mana_cost) {
        float mana_cost = 1.0f;
        patchlib_field_get_value(g_player_mana_cost, instance, &mana_cost);

        float multiplier = 0.85f;
        if (state->held_class_valid && state->held_is_summon) {
            multiplier = 0.75f;
            ++g_stat_summon25_frames;
        }
        else {
            ++g_stat_mana15_frames;
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

    /* Melee-only potion sickness recovery +10%. */
    if (g_player_potion_delay) {
        int potion_delay = 0;
        patchlib_field_get_value(g_player_potion_delay, instance, &potion_delay);

        if (potion_delay <= 0 || !state->held_class_valid || !state->held_is_melee) {
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
                ++g_stat_melee_potion_extra_ticks;
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
        mod_logger_write(
            MOD_LOG_LEVEL_ERROR,
            "ResourceSaver",
            "Combat v1.1.0 init failed: Player=%p Item=%p",
            player_type,
            item_type
        );
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
    g_player_huntress_ammo_cost90 = patchlib_type_get_field(player_type, "huntressAmmoCost90");

    held_item_property = patchlib_type_get_property(player_type, "HeldItem");
    if (held_item_property)
        g_held_item_getter = patchlib_property_get_get_method(held_item_property);

    g_item_mana = patchlib_type_get_field(item_type, "mana");
    g_item_use_ammo = patchlib_type_get_field(item_type, "useAmmo");
    g_item_damage = patchlib_type_get_field(item_type, "damage");
    g_item_melee = patchlib_type_get_field(item_type, "melee");
    g_item_summon = patchlib_type_get_field(item_type, "summon");
    g_item_sentry = patchlib_type_get_field(item_type, "sentry");

    patch_handle_t reset_effects = patchlib_type_get_method_by_param_count(
        player_type, "ResetEffects", 0);
    patch_handle_t update = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    if (reset_effects && g_player_ammo_cost80 && g_player_mana_cost) {
        g_reset_effects_hook = patchlib_install_prepost_hook(
            reset_effects, NULL, reset_effects_postfix);
    }

    if (update) {
        g_update_hook = patchlib_install_prepost_hook(
            update, NULL, update_postfix);
    }

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Combat v1.1.0 hooks: ResetEffects=%d Update=%d ammo20=%p ammo10=%p manaCost=%p HeldItem=%p useAmmo=%p melee=%p summon=%p sentry=%p",
        (int)g_reset_effects_hook,
        (int)g_update_hook,
        g_player_ammo_cost80,
        g_player_huntress_ammo_cost90,
        g_player_mana_cost,
        g_held_item_getter,
        g_item_use_ammo,
        g_item_melee,
        g_item_summon,
        g_item_sentry
    );

    if (reset_effects) patchlib_free(reset_effects);
    if (update) patchlib_free(update);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
    (void)held_item_property;
}

void resource_saver_combat_cleanup(void) {
    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Combat diagnostics: ammo20_frames=%llu ammo10_frames=%llu mana15_frames=%llu summon25_frames=%llu melee_potion_extra_ticks=%llu held_fallback_frames=%llu",
        (unsigned long long)g_stat_ammo20_frames,
        (unsigned long long)g_stat_ammo10_frames,
        (unsigned long long)g_stat_mana15_frames,
        (unsigned long long)g_stat_summon25_frames,
        (unsigned long long)g_stat_melee_potion_extra_ticks,
        (unsigned long long)g_stat_held_fallback_frames
    );

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
    free_handle(&g_player_huntress_ammo_cost90);

    free_handle(&g_held_item_getter);
    free_handle(&g_item_mana);
    free_handle(&g_item_use_ammo);
    free_handle(&g_item_damage);
    free_handle(&g_item_melee);
    free_handle(&g_item_summon);
    free_handle(&g_item_sentry);

    for (size_t i = 0; i < 256; ++i) {
        g_states[i].player = PATCH_NULL;
        g_states[i].mana_regen_remainder = 0;
        g_states[i].potion_tick = 0;
        g_states[i].held_class_valid = 0;
        g_states[i].held_is_melee = 0;
        g_states[i].held_is_summon = 0;
        g_states[i].held_use_ammo = 0;
    }
}
