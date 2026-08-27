#include <stddef.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.0.1 - combat resources
 *
 * This revision intentionally avoids tModLoader-only helper methods such as
 * Player.IsAmmoFreeThisShot() and Player.GetManaCost(). The Android Terraria
 * build is vanilla IL2CPP, so we use vanilla Player/Item fields and the stable
 * Player.ResetEffects()/Player.Update(int) methods instead.
 *
 * Features handled here:
 *   - Normal combat ammo: +20% conservation through vanilla ammoCost80 flag.
 *   - Rockets/special ammo: +10% conservation through vanilla huntressAmmoCost90 flag.
 *   - Magic weapon mana cost: -15%.
 *   - Summon/sentry mana cost: -25%.
 *   - Natural mana regeneration accumulation: +20%.
 *   - Melee-held potion sickness cooldown recovery: +10%.
 */

/* Player fields */
static patch_handle_t g_player_inventory = PATCH_NULL;
static patch_handle_t g_player_selected_item = PATCH_NULL;
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

/* Item fields */
static patch_handle_t g_item_use_ammo = PATCH_NULL;
static patch_handle_t g_item_magic = PATCH_NULL;
static patch_handle_t g_item_summon = PATCH_NULL;
static patch_handle_t g_item_sentry = PATCH_NULL;
static patch_handle_t g_item_melee = PATCH_NULL;
static patch_handle_t g_item_damage = PATCH_NULL;

/* Hooks */
static patch_hook_id_t g_reset_effects_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;

typedef struct player_runtime_state_t {
    patch_handle_t player;
    int mana_regen_remainder;
    int melee_potion_tick;
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
            g_states[i].melee_potion_tick = 0;
            return &g_states[i];
        }
    }

    return NULL;
}

/* Terraria AmmoID values used by vanilla. */
enum {
    AMMO_NONE = 0,
    AMMO_GEL = 23,
    AMMO_ARROW = 40,
    AMMO_COIN = 71,
    AMMO_FALLEN_STAR = 75,
    AMMO_BULLET = 97,
    AMMO_SAND = 169,
    AMMO_DART = 283,
    AMMO_ALE = 353,
    AMMO_ROCKET = 771,
    AMMO_SOLUTION = 780,
    AMMO_FLARE = 931,
    AMMO_SNOWBALL = 949,
    AMMO_STYNGER_BOLT = 1261,
    AMMO_CANDY_CORN = 1783,
    AMMO_JACK_O_LANTERN = 1785,
    AMMO_STAKE = 1836,
    AMMO_NAIL = 3108
};

typedef enum ammo_save_kind_t {
    AMMO_SAVE_NONE = 0,
    AMMO_SAVE_NORMAL_20 = 1,
    AMMO_SAVE_SPECIAL_10 = 2
} ammo_save_kind_t;

static ammo_save_kind_t ammo_save_kind(int use_ammo) {
    switch (use_ammo) {
        case AMMO_GEL:
        case AMMO_ARROW:
        case AMMO_BULLET:
        case AMMO_DART:
        case AMMO_ALE:
        case AMMO_FLARE:
        case AMMO_SNOWBALL:
            return AMMO_SAVE_NORMAL_20;

        case AMMO_COIN:
        case AMMO_FALLEN_STAR:
        case AMMO_ROCKET:
        case AMMO_STYNGER_BOLT:
        case AMMO_CANDY_CORN:
        case AMMO_JACK_O_LANTERN:
        case AMMO_STAKE:
        case AMMO_NAIL:
            return AMMO_SAVE_SPECIAL_10;

        case AMMO_NONE:
        case AMMO_SAND:
        case AMMO_SOLUTION:
            return AMMO_SAVE_NONE;

        default:
            /* Unknown future ammo receives the conservative 10% treatment. */
            return use_ammo > 0 ? AMMO_SAVE_SPECIAL_10 : AMMO_SAVE_NONE;
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

/*
 * ResetEffects postfix.
 *
 * Vanilla ResetEffects resets manaCost/ammo conservation flags each tick.
 * Applying our modifiers immediately afterwards lets vanilla itself handle
 * the actual consumption checks later in the tick.
 */
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

    patch_handle_t held = get_held_item(instance);
    if (!held) return;

    /* Ammo conservation: use Terraria's own conservation flags. */
    if (g_item_use_ammo) {
        int use_ammo = 0;
        patchlib_field_get_value(g_item_use_ammo, held, &use_ammo);

        ammo_save_kind_t kind = ammo_save_kind(use_ammo);
        bool enabled = true;

        if (kind == AMMO_SAVE_NORMAL_20 && g_player_ammo_cost80) {
            patchlib_field_set_value(g_player_ammo_cost80, instance, &enabled);
        }
        else if (kind == AMMO_SAVE_SPECIAL_10 && g_player_huntress_ammo_cost90) {
            patchlib_field_set_value(g_player_huntress_ammo_cost90, instance, &enabled);
        }
    }

    /* Mana cost: apply by held weapon class using vanilla Item bool fields. */
    if (g_player_mana_cost) {
        bool is_magic = false;
        bool is_summon = false;
        bool is_sentry = false;

        if (g_item_magic) patchlib_field_get_value(g_item_magic, held, &is_magic);
        if (g_item_summon) patchlib_field_get_value(g_item_summon, held, &is_summon);
        if (g_item_sentry) patchlib_field_get_value(g_item_sentry, held, &is_sentry);

        float mana_cost = 1.0f;
        patchlib_field_get_value(g_player_mana_cost, instance, &mana_cost);

        if (is_summon || is_sentry) {
            mana_cost *= 0.75f; /* -25% */
            if (mana_cost < 0.05f) mana_cost = 0.05f;
            patchlib_field_set_value(g_player_mana_cost, instance, &mana_cost);
        }
        else if (is_magic) {
            mana_cost *= 0.85f; /* -15% */
            if (mana_cost < 0.05f) mana_cost = 0.05f;
            patchlib_field_set_value(g_player_mana_cost, instance, &mana_cost);
        }
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
        if (buff_id != 21) continue; /* BuffID.PotionSickness */

        int time = 0;
        if (!patchlib_array_at(times, i, &time)) return;

        if (time > 0) {
            --time;
            patchlib_array_set(times, i, &time);
        }
        return;
    }
}

/* Player.Update(int) postfix. */
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

    /* +20% natural mana regeneration accumulation. */
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

        if (mana_regen > 0 && (!g_player_stat_mana || !g_player_stat_mana_max2 || stat_mana < stat_mana_max2)) {
            state->mana_regen_remainder += mana_regen;
            int extra = state->mana_regen_remainder / 5; /* exactly 20% */
            state->mana_regen_remainder %= 5;

            if (extra > 0) {
                mana_regen_count += extra;
                patchlib_field_set_value(g_player_mana_regen_count, instance, &mana_regen_count);
            }
        }
    }

    /* +10% potion cooldown recovery while holding a damaging melee weapon. */
    if (!g_player_potion_delay || !g_item_melee || !g_item_damage) return;

    int potion_delay = 0;
    patchlib_field_get_value(g_player_potion_delay, instance, &potion_delay);

    if (potion_delay <= 0) {
        state->melee_potion_tick = 0;
        return;
    }

    patch_handle_t held = get_held_item(instance);
    if (!held) return;

    bool melee = false;
    int damage = 0;
    patchlib_field_get_value(g_item_melee, held, &melee);
    patchlib_field_get_value(g_item_damage, held, &damage);

    if (!melee || damage <= 0) {
        state->melee_potion_tick = 0;
        return;
    }

    ++state->melee_potion_tick;
    if (state->melee_potion_tick < 10) return;
    state->melee_potion_tick = 0;

    --potion_delay;
    if (potion_delay < 0) potion_delay = 0;

    patchlib_field_set_value(g_player_potion_delay, instance, &potion_delay);
    reduce_potion_sickness_buff_one_tick(instance);
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

    if (!player_type || !item_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Combat init failed: Player=%p Item=%p",
                         player_type, item_type);
        goto done;
    }

    g_player_inventory = patchlib_type_get_field(player_type, "inventory");
    g_player_selected_item = patchlib_type_get_field(player_type, "selectedItem");
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

    g_item_use_ammo = patchlib_type_get_field(item_type, "useAmmo");
    g_item_magic = patchlib_type_get_field(item_type, "magic");
    g_item_summon = patchlib_type_get_field(item_type, "summon");
    g_item_sentry = patchlib_type_get_field(item_type, "sentry");
    g_item_melee = patchlib_type_get_field(item_type, "melee");
    g_item_damage = patchlib_type_get_field(item_type, "damage");

    patch_handle_t reset_effects = patchlib_type_get_method_by_param_count(
        player_type, "ResetEffects", 0);
    patch_handle_t update = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    if (reset_effects && g_player_inventory && g_player_selected_item) {
        g_reset_effects_hook = patchlib_install_prepost_hook(
            reset_effects, NULL, reset_effects_postfix);
    }

    if (update) {
        g_update_hook = patchlib_install_prepost_hook(
            update, NULL, update_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Combat hooks v1.0.1: ResetEffects=%d Update=%d | fields: inventory=%p selected=%p manaCost=%p ammo80=%p ammo90=%p magic=%p summon=%p sentry=%p",
        (int)g_reset_effects_hook,
        (int)g_update_hook,
        g_player_inventory,
        g_player_selected_item,
        g_player_mana_cost,
        g_player_ammo_cost80,
        g_player_huntress_ammo_cost90,
        g_item_magic,
        g_item_summon,
        g_item_sentry);

    if (reset_effects) patchlib_free(reset_effects);
    if (update) patchlib_free(update);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
}

void resource_saver_combat_cleanup(void) {
    if (g_reset_effects_hook != PATCH_HOOK_INVALID_ID)
        patchlib_uninstall_hook(g_reset_effects_hook);
    if (g_update_hook != PATCH_HOOK_INVALID_ID)
        patchlib_uninstall_hook(g_update_hook);

    g_reset_effects_hook = PATCH_HOOK_INVALID_ID;
    g_update_hook = PATCH_HOOK_INVALID_ID;

    free_handle(&g_player_inventory);
    free_handle(&g_player_selected_item);
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

    free_handle(&g_item_use_ammo);
    free_handle(&g_item_magic);
    free_handle(&g_item_summon);
    free_handle(&g_item_sentry);
    free_handle(&g_item_melee);
    free_handle(&g_item_damage);

    for (size_t i = 0; i < 256; ++i) {
        g_states[i].player = PATCH_NULL;
        g_states[i].mana_regen_remainder = 0;
        g_states[i].melee_potion_tick = 0;
    }
}
