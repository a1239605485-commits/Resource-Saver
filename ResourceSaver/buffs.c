#include <stddef.h>
#include <limits.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/type.h"

/*
 * Resource Saver - combat buff duration
 *
 * Player.AddBuff(int type, int timeToAdd, bool quiet, bool foodHack)
 * is intercepted before vanilla applies the buff. For selected long-duration
 * combat buffs we increase the incoming duration by 20%.
 *
 * The 30-second minimum keeps short internal/proc buffs from being stretched;
 * these IDs are primarily potion-style combat buffs at long durations.
 */

static patch_hook_id_t g_add_buff_hook = PATCH_HOOK_INVALID_ID;

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

static bool add_buff_prefix(
    patch_handle_t instance,
    void** args,
    const patch_method_signature_t* sig,
    void* result
) {
    (void)instance;
    (void)sig;
    (void)result;

    if (!args || !args[0] || !args[1]) {
        return true; /* continue original */
    }

    int* buff_type = (int*)args[0];
    int* time_to_add = (int*)args[1];

    if (!is_supported_combat_buff(*buff_type)) {
        return true;
    }

    /* 30 s minimum: only extend normal long-duration potion applications. */
    if (*time_to_add < 1800) {
        return true;
    }

    if (*time_to_add <= INT_MAX / 6 * 5) {
        *time_to_add += *time_to_add / 5; /* +20% */
    }

    return true; /* normal execution */
}

void resource_saver_buffs_init(void) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (!player_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Buff init failed: Terraria.Player not found");
        return;
    }

    patch_handle_t add_buff = patchlib_type_get_method_by_param_count(
        player_type, "AddBuff", 4);

    if (add_buff) {
        g_add_buff_hook = patchlib_install_prepost_hook(
            add_buff, add_buff_prefix, NULL);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                     "Combat buff duration hook: %s (id=%d)",
                     g_add_buff_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
                     (int)g_add_buff_hook);

    if (add_buff) patchlib_free(add_buff);
    patchlib_free(player_type);
}

void resource_saver_buffs_cleanup(void) {
    if (g_add_buff_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_add_buff_hook);
        g_add_buff_hook = PATCH_HOOK_INVALID_ID;
    }
}
