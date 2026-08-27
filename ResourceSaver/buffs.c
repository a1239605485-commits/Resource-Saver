#include <stddef.h>
#include <limits.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/type.h"

/*
 * Resource Saver v1.0.1 - combat buff duration
 *
 * Vanilla Terraria 1.4.5 Player.AddBuff is:
 *   AddBuff(int type, int time, bool fromNetPvP = false)
 * i.e. THREE parameters.
 *
 * v1.0.0 incorrectly looked for a four-parameter overload and therefore never
 * installed the hook on vanilla Android Terraria.
 *
 * TEFKernel prefix semantics:
 *   false = continue original method
 *   true  = skip original method
 *
 * v1.0.0 also returned true by mistake. Both problems are fixed here.
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
        return false; /* continue original */
    }

    int* buff_type = (int*)args[0];
    int* time_to_add = (int*)args[1];

    if (!is_supported_combat_buff(*buff_type)) {
        return false;
    }

    /* Only extend long potion-style applications (>= 30 seconds). */
    if (*time_to_add < 1800) {
        return false;
    }

    if (*time_to_add <= INT_MAX - (*time_to_add / 5)) {
        *time_to_add += *time_to_add / 5; /* +20% */
    }

    return false; /* IMPORTANT: false = run vanilla AddBuff */
}

void resource_saver_buffs_init(void) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");

    if (!player_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Buff init failed: Terraria.Player not found");
        return;
    }

    /* Vanilla Android Terraria: AddBuff(type, time, fromNetPvP) => 3 params. */
    patch_handle_t add_buff = patchlib_type_get_method_by_param_count(
        player_type, "AddBuff", 3);

    if (add_buff) {
        g_add_buff_hook = patchlib_install_prepost_hook(
            add_buff, add_buff_prefix, NULL);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                     "Combat buff duration hook v1.0.1: %s (id=%d, method=%p)",
                     g_add_buff_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
                     (int)g_add_buff_hook,
                     add_buff);

    if (add_buff) patchlib_free(add_buff);
    patchlib_free(player_type);
}

void resource_saver_buffs_cleanup(void) {
    if (g_add_buff_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_add_buff_hook);
        g_add_buff_hook = PATCH_HOOK_INVALID_ID;
    }
}
