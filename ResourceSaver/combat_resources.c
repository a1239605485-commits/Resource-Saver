#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver - combat resource module
 *
 * Features:
 *   1. Normal combat ammo: +20% independent conservation chance.
 *   2. Rockets / special ammo: +10% independent conservation chance.
 *   3. Magic weapons: mana cost -15%.
 *   4. Natural mana regeneration accumulation +20%.
 *   5. Summon / sentry weapons: mana cost -25%.
 *   6. While holding a melee weapon: potion sickness recovers 10% faster.
 *
 * Important: the ammo hook only changes a false vanilla conservation result
 * to true. If vanilla already conserved the ammo, we leave it untouched.
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

/* Item fields */
static patch_handle_t g_item_use_ammo = PATCH_NULL;
static patch_handle_t g_item_magic = PATCH_NULL;
static patch_handle_t g_item_summon = PATCH_NULL;
static patch_handle_t g_item_sentry = PATCH_NULL;
static patch_handle_t g_item_melee = PATCH_NULL;
static patch_handle_t g_item_damage = PATCH_NULL;

/* Hooks */
static patch_hook_id_t g_ammo_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_mana_cost_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_mana_regen_hook = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_player_update_hook = PATCH_HOOK_INVALID_ID;

/* Small per-player state used to keep fractional +20%/+10% bonuses exact. */
typedef struct player_runtime_state_t {
    patch_handle_t player;
    int mana_regen_remainder;
    int melee_potion_tick;
} player_runtime_state_t;

static player_runtime_state_t g_states[256];

/* Independent PRNG so we do not touch Terraria's random stream. */
static uint32_t g_rng_state = 0x6C8E9CF5u;

static uint32_t rs_rand_u32(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x ? x : 0xA341316Cu;
    return g_rng_state;
}

static int roll_percent(int percent) {
    if (percent <= 0) return 0;
    if (percent >= 100) return 1;
    return (int)(rs_rand_u32() % 100u) < percent;
}

static player_runtime_state_t* state_for(patch_handle_t player) {
    if (!player) return NULL;

    for (size_t i = 0; i < 256; ++i) {
        if (g_states[i].player == player) {
            return &g_states[i];
        }
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

/*
 * Returns the extra conservation chance for a weapon's useAmmo category.
 * 20%: conventional combat ammo.
 * 10%: rockets and rarer/special combat ammo.
 *  0%: non-combat utility ammo such as Clentaminator Solution and Sand.
 */
static int extra_ammo_save_percent(int use_ammo) {
    switch (use_ammo) {
        case AMMO_GEL:
        case AMMO_ARROW:
        case AMMO_BULLET:
        case AMMO_DART:
        case AMMO_ALE:
        case AMMO_FLARE:
        case AMMO_SNOWBALL:
            return 20;

        case AMMO_COIN:
        case AMMO_FALLEN_STAR:
        case AMMO_ROCKET:
        case AMMO_STYNGER_BOLT:
        case AMMO_CANDY_CORN:
        case AMMO_JACK_O_LANTERN:
        case AMMO_STAKE:
        case AMMO_NAIL:
            return 10;

        case AMMO_NONE:
        case AMMO_SAND:
        case AMMO_SOLUTION:
            return 0;

        default:
            /* Future/unknown combat ammo gets the conservative special rate. */
            return use_ammo > 0 ? 10 : 0;
    }
}

/* Player.IsAmmoFreeThisShot(Item weapon, Item ammo, int projToShoot) postfix */
static void ammo_free_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)instance;
    (void)sig;

    if (!args || !result || !g_item_use_ammo) return;

    bool* conserved = (bool*)result;

    /* Preserve any vanilla conservation that has already succeeded. */
    if (*conserved) return;

    patch_handle_t weapon = args[0] ? *(patch_handle_t*)args[0] : PATCH_NULL;
    if (!weapon) return;

    int use_ammo = 0;
    patchlib_field_get_value(g_item_use_ammo, weapon, &use_ammo);

    int chance = extra_ammo_save_percent(use_ammo);
    if (chance > 0 && roll_percent(chance)) {
        *conserved = true;
    }
}

static int ceil_percent_of(int value, int percent) {
    if (value <= 0) return 0;
    int reduced = (value * percent + 99) / 100;
    return reduced < 1 ? 1 : reduced;
}

/* Player.GetManaCost(Item item) postfix */
static void mana_cost_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)instance;
    (void)sig;

    if (!args || !result) return;

    int* mana_cost = (int*)result;
    if (*mana_cost <= 0) return;

    patch_handle_t item = args[0] ? *(patch_handle_t*)args[0] : PATCH_NULL;
    if (!item) return;

    bool is_magic = false;
    bool is_summon = false;
    bool is_sentry = false;

    if (g_item_magic) patchlib_field_get_value(g_item_magic, item, &is_magic);
    if (g_item_summon) patchlib_field_get_value(g_item_summon, item, &is_summon);
    if (g_item_sentry) patchlib_field_get_value(g_item_sentry, item, &is_sentry);

    /* Summon/sentry takes priority so it never receives both reductions. */
    if (is_summon || is_sentry) {
        *mana_cost = ceil_percent_of(*mana_cost, 75); /* -25% */
    }
    else if (is_magic) {
        *mana_cost = ceil_percent_of(*mana_cost, 85); /* -15% */
    }
}

/* Player.UpdateManaRegen() postfix */
static void mana_regen_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)args;
    (void)result;
    (void)sig;

    if (!instance || !g_player_mana_regen || !g_player_mana_regen_count) return;

    int mana_regen = 0;
    int mana_regen_count = 0;
    int stat_mana = 0;
    int stat_mana_max2 = 0;

    patchlib_field_get_value(g_player_mana_regen, instance, &mana_regen);
    patchlib_field_get_value(g_player_mana_regen_count, instance, &mana_regen_count);

    if (g_player_stat_mana && g_player_stat_mana_max2) {
        patchlib_field_get_value(g_player_stat_mana, instance, &stat_mana);
        patchlib_field_get_value(g_player_stat_mana_max2, instance, &stat_mana_max2);
        if (stat_mana >= stat_mana_max2) return;
    }

    if (mana_regen <= 0) return;

    player_runtime_state_t* state = state_for(instance);
    if (!state) return;

    /* Add exactly 20% over time while preserving fractional increments. */
    state->mana_regen_remainder += mana_regen;
    int extra = state->mana_regen_remainder / 5;
    state->mana_regen_remainder %= 5;

    if (extra > 0) {
        mana_regen_count += extra;
        patchlib_field_set_value(g_player_mana_regen_count, instance, &mana_regen_count);
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

/* Keep the Potion Sickness buff timer in sync when we add an extra cooldown tick. */
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

/* Player.Update(int) postfix: +10% potion cooldown recovery while holding melee. */
static void player_update_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)args;
    (void)result;
    (void)sig;

    if (!instance || !g_player_potion_delay || !g_item_melee || !g_item_damage) return;

    int potion_delay = 0;
    patchlib_field_get_value(g_player_potion_delay, instance, &potion_delay);
    if (potion_delay <= 0) {
        player_runtime_state_t* s = state_for(instance);
        if (s) s->melee_potion_tick = 0;
        return;
    }

    patch_handle_t held = get_held_item(instance);
    if (!held) return;

    bool melee = false;
    int damage = 0;
    patchlib_field_get_value(g_item_melee, held, &melee);
    patchlib_field_get_value(g_item_damage, held, &damage);

    if (!melee || damage <= 0) return;

    player_runtime_state_t* state = state_for(instance);
    if (!state) return;

    /* Vanilla already removes 1 tick each update. Every 10th update remove 1 extra. */
    state->melee_potion_tick++;
    if (state->melee_potion_tick < 10) return;
    state->melee_potion_tick = 0;

    if (potion_delay > 0) {
        --potion_delay;
        patchlib_field_set_value(g_player_potion_delay, instance, &potion_delay);
        reduce_potion_sickness_buff_one_tick(instance);
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

    if (!player_type || !item_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Combat init failed: Player=%p Item=%p", player_type, item_type);
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

    g_item_use_ammo = patchlib_type_get_field(item_type, "useAmmo");
    g_item_magic = patchlib_type_get_field(item_type, "magic");
    g_item_summon = patchlib_type_get_field(item_type, "summon");
    g_item_sentry = patchlib_type_get_field(item_type, "sentry");
    g_item_melee = patchlib_type_get_field(item_type, "melee");
    g_item_damage = patchlib_type_get_field(item_type, "damage");

    patch_handle_t ammo_method = patchlib_type_get_method_by_param_count(
        player_type, "IsAmmoFreeThisShot", 3);
    patch_handle_t mana_cost_method = patchlib_type_get_method_by_param_count(
        player_type, "GetManaCost", 1);
    patch_handle_t mana_regen_method = patchlib_type_get_method_by_param_count(
        player_type, "UpdateManaRegen", 0);
    patch_handle_t update_method = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    if (ammo_method && g_item_use_ammo) {
        g_ammo_hook = patchlib_install_prepost_hook(ammo_method, NULL, ammo_free_postfix);
    }
    if (mana_cost_method && g_item_magic && g_item_summon && g_item_sentry) {
        g_mana_cost_hook = patchlib_install_prepost_hook(mana_cost_method, NULL, mana_cost_postfix);
    }
    if (mana_regen_method && g_player_mana_regen && g_player_mana_regen_count) {
        g_mana_regen_hook = patchlib_install_prepost_hook(mana_regen_method, NULL, mana_regen_postfix);
    }
    if (update_method && g_player_potion_delay && g_player_inventory &&
        g_player_selected_item && g_item_melee && g_item_damage) {
        g_player_update_hook = patchlib_install_prepost_hook(update_method, NULL, player_update_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Combat hooks: ammo=%d manaCost=%d manaRegen=%d meleePotion=%d",
        (int)g_ammo_hook, (int)g_mana_cost_hook,
        (int)g_mana_regen_hook, (int)g_player_update_hook);

    if (ammo_method) patchlib_free(ammo_method);
    if (mana_cost_method) patchlib_free(mana_cost_method);
    if (mana_regen_method) patchlib_free(mana_regen_method);
    if (update_method) patchlib_free(update_method);

done:
    if (player_type) patchlib_free(player_type);
    if (item_type) patchlib_free(item_type);
}

void resource_saver_combat_cleanup(void) {
    if (g_ammo_hook != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_ammo_hook);
    if (g_mana_cost_hook != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_mana_cost_hook);
    if (g_mana_regen_hook != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_mana_regen_hook);
    if (g_player_update_hook != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_player_update_hook);

    g_ammo_hook = g_mana_cost_hook = g_mana_regen_hook = g_player_update_hook = PATCH_HOOK_INVALID_ID;

    free_handle(&g_player_inventory);
    free_handle(&g_player_selected_item);
    free_handle(&g_player_potion_delay);
    free_handle(&g_player_buff_type);
    free_handle(&g_player_buff_time);
    free_handle(&g_player_mana_regen);
    free_handle(&g_player_mana_regen_count);
    free_handle(&g_player_stat_mana);
    free_handle(&g_player_stat_mana_max2);

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
