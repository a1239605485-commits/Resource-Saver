#include "mod_logger.h"

/*
 * Resource Saver v1.0.3
 *
 * Bait saving is now implemented by setting Player.accTackleBox in the
 * ResetEffects postfix in combat_resources.c. This deliberately uses
 * Terraria's own bait-consumption formula instead of guessing the exact
 * 1.4.5 fishing consumption call site.
 */

void resource_saver_fishing_init(void) {
    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
        "Bait saver v1.0.3 uses vanilla accTackleBox mechanic");
}

void resource_saver_fishing_cleanup(void) {
}
