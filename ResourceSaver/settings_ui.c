#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/tefstd/vector.h"

/*
 * Resource Saver v1.2.4 - SAFE UI DIAGNOSTIC
 *
 * IMPORTANT:
 * This version intentionally installs NO rendering hook.
 * v1.2.3 proved that hooking SpriteBatch.End and calling DrawBorderString
 * from inside the End prefix can abort the Android IL2CPP process during
 * startup.  The goal of v1.2.4 is therefore:
 *   1) restore stable game startup;
 *   2) keep the v1.1.0 gameplay/config core untouched;
 *   3) enumerate the real Android 1.4.5.6.4 drawing/UI methods in logs;
 *   4) use that exact runtime information for the next safe UI build.
 */

static bool contains_ci(const char* text, const char* needle) {
    if (!text || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char* p = text; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] &&
               (char)tolower((unsigned char)p[i]) ==
               (char)tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

static bool interesting_method(const char* name) {
    return contains_ci(name, "draw") ||
           contains_ci(name, "interface") ||
           contains_ci(name, "inventory") ||
           contains_ci(name, "menu") ||
           contains_ci(name, "ui") ||
           contains_ci(name, "cursor") ||
           contains_ci(name, "mouse");
}

static void dump_methods(const char* ns, const char* type_name, const char* label) {
    patch_handle_t type = patchlib_type_get_type(ns, type_name);
    if (!type) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                         "UI diagnostic: type not found: %s.%s", ns, type_name);
        return;
    }

    tefstd_vector_t methods;
    memset(&methods, 0, sizeof(methods));

    if (!tefstd_vector_init(&methods, sizeof(patch_handle_t))) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                         "UI diagnostic: vector init failed for %s", label);
        patchlib_free(type);
        return;
    }

    if (!patchlib_type_get_methods(type, false, &methods)) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                         "UI diagnostic: method enumeration failed for %s", label);
        tefstd_vector_destroy(&methods);
        patchlib_free(type);
        return;
    }

    size_t count = tefstd_vector_size(&methods);
    size_t shown = 0;
    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                     "UI diagnostic: %s method_count=%zu", label, count);

    for (size_t i = 0; i < count; ++i) {
        patch_handle_t* slot = (patch_handle_t*)tefstd_vector_at(&methods, i);
        if (!slot || !*slot) continue;

        patch_handle_t method = *slot;
        const char* name = patchlib_method_get_name(method);
        if (!name || !interesting_method(name)) continue;

        int params = patchlib_method_get_param_count(method);
        bool is_instance = patchlib_method_is_instance(method);
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                         "UI-METHOD %s :: %s params=%d instance=%d",
                         label, name, params, is_instance ? 1 : 0);
        ++shown;
    }

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                     "UI diagnostic: %s interesting_methods=%zu", label, shown);

    tefstd_vector_destroy(&methods);
    patchlib_free(type);
}

void resource_saver_settings_ui_init(void) {
    mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                     "Chinese settings UI v1.2.4 SAFE MODE: rendering hook disabled after v1.2.3 startup crash");

    /* Runtime discovery only: no hook is installed here. */
    dump_methods("Terraria", "Main", "Terraria.Main");
    dump_methods("Terraria", "Utils", "Terraria.Utils");
    dump_methods("Microsoft.Xna.Framework.Graphics", "SpriteBatch", "SpriteBatch");

    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                     "UI diagnostic complete: game should remain on stable v1.1.0 gameplay core");
}

void resource_saver_settings_ui_cleanup(void) {
    /* No UI hook/resources to release in safe diagnostic mode. */
}
