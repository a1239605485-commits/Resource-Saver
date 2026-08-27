#include <stddef.h>
#include <stdbool.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/method.h"

/*
 * Resource Saver v1.0.3 - combat buff duration
 *
 * Vanilla Terraria has Player.AddBuff(int type, int time, bool fromNetPvP).
 * Hooking the application point is simpler and more reliable than watching
 * buff arrays after Player.Update.
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

    if (!args || !args[0] || !args[1]) return false;

    int* type = (int*)args[0];
    int* time = (int*)args[1];

    if (!type || !time) return false;
    if (!is_supported_combat_buff(*type)) return false;

    /* Only long potion-like buffs (30 seconds or more). */
    if (*time >= 1800 && *time <= 1789569705) {
        *time += *time / 5; /* +20% */
    }

    return false; /* continue vanilla AddBuff */
}

void resource_saver_buffs_init(void) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (!player_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
            "Buff v1.0.3 init failed: Terraria.Player not found");
        return;
    }

    patch_handle_t add_buff = patchlib_type_get_method_by_param_count(
        player_type, "AddBuff", 3);

    if (add_buff) {
        g_add_buff_hook = patchlib_install_prepost_hook(
            add_buff, add_buff_prefix, NULL);
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Combat buff v1.0.3 hook: %s (id=%d AddBuff=%p)",
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
