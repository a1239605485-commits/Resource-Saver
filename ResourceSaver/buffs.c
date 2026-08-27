#include <stddef.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"

/*
 * Resource Saver v1.0.2 - combat buff duration
 *
 * Instead of modifying AddBuff's arguments in a prefix (which depends on the
 * exact IL2CPP argument layout), this version directly observes Player.buffType
 * and Player.buffTime after Player.Update(int). A new/refreshed supported buff
 * is extended once by 20%, then tracked while it counts down normally.
 */

#define RS_MAX_PLAYERS 256
#define RS_MAX_BUFF_SLOTS 64

static patch_handle_t g_player_buff_type = PATCH_NULL;
static patch_handle_t g_player_buff_time = PATCH_NULL;
static patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;

typedef struct buff_slot_state_t {
    int type;
    int last_time;
} buff_slot_state_t;

typedef struct player_buff_state_t {
    patch_handle_t player;
    buff_slot_state_t slot[RS_MAX_BUFF_SLOTS];
} player_buff_state_t;

static player_buff_state_t g_states[RS_MAX_PLAYERS];

static int is_supported_combat_buff(int id) {
    switch (id) {
        case 2:   /* Regeneration */
        case 3:   /* Swiftness */
        case 5:   /* Ironskin */
        case 6:   /* Mana Regeneration */
        case 7:   /* Magic Power */
        case 13:  /* Battle */
        case 14:  /* Thorns */
        case 16:  /* Archery */
        case 17:  /* Hunter */
        case 108: /* Titan */
        case 110: /* Summoning */
        case 112: /* Ammo Reservation */
        case 113: /* Lifeforce */
        case 114: /* Endurance */
        case 115: /* Rage */
        case 116: /* Inferno */
        case 117: /* Wrath */
        case 124: /* Warmth */
            return 1;
        default:
            return 0;
    }
}

static player_buff_state_t* state_for(patch_handle_t player) {
    if (!player) return NULL;

    for (size_t i = 0; i < RS_MAX_PLAYERS; ++i) {
        if (g_states[i].player == player) return &g_states[i];
    }

    for (size_t i = 0; i < RS_MAX_PLAYERS; ++i) {
        if (!g_states[i].player) {
            g_states[i].player = player;
            for (size_t j = 0; j < RS_MAX_BUFF_SLOTS; ++j) {
                g_states[i].slot[j].type = 0;
                g_states[i].slot[j].last_time = 0;
            }
            return &g_states[i];
        }
    }

    return NULL;
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

    if (!instance || !g_player_buff_type || !g_player_buff_time) return;

    patch_handle_t types = PATCH_NULL;
    patch_handle_t times = PATCH_NULL;
    patchlib_field_get_value(g_player_buff_type, instance, &types);
    patchlib_field_get_value(g_player_buff_time, instance, &times);
    if (!types || !times) return;

    player_buff_state_t* state = state_for(instance);
    if (!state) return;

    size_t type_len = patchlib_array_length(types);
    size_t time_len = patchlib_array_length(times);
    size_t len = type_len < time_len ? type_len : time_len;
    if (len > RS_MAX_BUFF_SLOTS) len = RS_MAX_BUFF_SLOTS;

    for (size_t i = 0; i < len; ++i) {
        int type = 0;
        int time = 0;

        if (!patchlib_array_at(types, i, &type) ||
            !patchlib_array_at(times, i, &time)) {
            continue;
        }

        buff_slot_state_t* slot = &state->slot[i];

        if (type <= 0 || time <= 0 || !is_supported_combat_buff(type)) {
            slot->type = type;
            slot->last_time = time;
            continue;
        }

        int newly_applied = (slot->type != type);
        int refreshed = (!newly_applied && time > slot->last_time + 5);

        /* Potion-style long buffs only: >= 30 seconds at 60 ticks/sec. */
        if ((newly_applied || refreshed) && time >= 1800) {
            int extra = time / 5;
            if (extra > 0 && time <= 2147483647 - extra) {
                int extended = time + extra;
                if (patchlib_array_set(times, i, &extended)) {
                    time = extended;
                }
            }
        }

        slot->type = type;
        slot->last_time = time;
    }
}

static void free_handle(patch_handle_t* h) {
    if (*h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

void resource_saver_buffs_init(void) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");

    if (!player_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Buff init failed: Terraria.Player not found");
        return;
    }

    g_player_buff_type = patchlib_type_get_field(player_type, "buffType");
    g_player_buff_time = patchlib_type_get_field(player_type, "buffTime");

    patch_handle_t update = patchlib_type_get_method_by_param_count(
        player_type, "Update", 1);

    if (update && g_player_buff_type && g_player_buff_time) {
        g_update_hook = patchlib_install_prepost_hook(
            update, NULL, update_postfix);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Combat buff hook v1.0.2: %s (id=%d Update=%p buffType=%p buffTime=%p)",
        g_update_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_update_hook,
        update,
        g_player_buff_type,
        g_player_buff_time);

    if (update) patchlib_free(update);
    patchlib_free(player_type);
}

void resource_saver_buffs_cleanup(void) {
    if (g_update_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_update_hook);
        g_update_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_player_buff_type);
    free_handle(&g_player_buff_time);

    for (size_t i = 0; i < RS_MAX_PLAYERS; ++i) {
        g_states[i].player = PATCH_NULL;
        for (size_t j = 0; j < RS_MAX_BUFF_SLOTS; ++j) {
            g_states[i].slot[j].type = 0;
            g_states[i].slot[j].last_time = 0;
        }
    }
}
