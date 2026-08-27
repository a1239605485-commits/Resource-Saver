#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"

/*
 * Resource Saver v1.2.5 - SAFE EXACT UI PROBE
 *
 * No drawing hook is installed in this build.
 * We query exact method names through patchlib_type_get_method().
 * Successful queries are logged by TEFKernel itself (visible in runtime log),
 * and are also written to <private_dir>/ui_probe.txt.
 */

static FILE* g_probe_file = NULL;

static void probe_write(const char* fmt, ...) {
    if (!g_probe_file) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_probe_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_probe_file);
    fflush(g_probe_file);
}

static void probe_method(patch_handle_t type, const char* label, const char* name) {
    if (!type || !name) return;

    /* Exact lookup. On success TEFKernel prints: Found method '<name>' at ... */
    patch_handle_t method = patchlib_type_get_method(type, name);
    if (!method) {
        probe_write("MISS  %s :: %s", label, name);
        return;
    }

    int params = patchlib_method_get_param_count(method);
    bool instance = patchlib_method_is_instance(method);
    probe_write("FOUND %s :: %s params=%d instance=%d", label, name, params, instance ? 1 : 0);

    patchlib_free(method);
}

static void probe_type_methods(
    const char* ns,
    const char* type_name,
    const char* label,
    const char* const* names,
    size_t count
) {
    patch_handle_t type = patchlib_type_get_type(ns, type_name);
    if (!type) {
        probe_write("TYPE-MISS %s.%s", ns, type_name);
        return;
    }

    probe_write("TYPE-FOUND %s", label);
    for (size_t i = 0; i < count; ++i) {
        probe_method(type, label, names[i]);
    }

    patchlib_free(type);
}

void resource_saver_settings_ui_init(const char* private_dir) {
    char path[1024];
    if (private_dir && private_dir[0]) {
        snprintf(path, sizeof(path), "%s/ui_probe.txt", private_dir);
        g_probe_file = fopen(path, "w");
    }

    probe_write("Resource Saver v1.2.5 UI exact probe");
    probe_write("No rendering hook is installed in this build.");

    static const char* const main_methods[] = {
        "DrawInventory",
        "DrawInterface_27_Inventory",
        "DrawInterface",
        "DoDraw",
        "Draw",
        "Draw_Inner",
        "DrawMenu",
        "SetupDrawInterfaceLayers",
        "DrawInterface_26_InterfaceLogic1",
        "DrawInterface_30_Hotbar",
        "DrawInterface_35_YouDied",
        "DrawInterface_36_Cursor",
        "DrawInterface_36_MouseText",
        "DrawInterface_37_DiagnoseNet",
        "DrawInterface_38_MouseCarriedObject",
        "DrawInterface_39_MouseOver"
    };

    static const char* const utils_methods[] = {
        "DrawBorderString",
        "DrawBorderStringFourWay",
        "DrawBorderStringBig"
    };

    static const char* const sprite_batch_methods[] = {
        "Begin",
        "End",
        "Draw",
        "DrawString"
    };

    static const char* const user_interface_methods[] = {
        "Draw",
        "Update",
        "SetState",
        "Use"
    };

    static const char* const ui_element_methods[] = {
        "Draw",
        "DrawSelf",
        "DrawChildren",
        "Update",
        "LeftClick",
        "Click"
    };

    static const char* const game_interface_layer_methods[] = {
        "Draw"
    };

    static const char* const legacy_layer_methods[] = {
        "DrawSelf"
    };

    probe_type_methods("Terraria", "Main", "Terraria.Main",
                       main_methods, sizeof(main_methods) / sizeof(main_methods[0]));
    probe_type_methods("Terraria", "Utils", "Terraria.Utils",
                       utils_methods, sizeof(utils_methods) / sizeof(utils_methods[0]));
    probe_type_methods("Microsoft.Xna.Framework.Graphics", "SpriteBatch", "SpriteBatch",
                       sprite_batch_methods, sizeof(sprite_batch_methods) / sizeof(sprite_batch_methods[0]));
    probe_type_methods("Terraria.UI", "UserInterface", "Terraria.UI.UserInterface",
                       user_interface_methods, sizeof(user_interface_methods) / sizeof(user_interface_methods[0]));
    probe_type_methods("Terraria.UI", "UIElement", "Terraria.UI.UIElement",
                       ui_element_methods, sizeof(ui_element_methods) / sizeof(ui_element_methods[0]));
    probe_type_methods("Terraria.UI", "GameInterfaceLayer", "Terraria.UI.GameInterfaceLayer",
                       game_interface_layer_methods, sizeof(game_interface_layer_methods) / sizeof(game_interface_layer_methods[0]));
    probe_type_methods("Terraria.UI", "LegacyGameInterfaceLayer", "Terraria.UI.LegacyGameInterfaceLayer",
                       legacy_layer_methods, sizeof(legacy_layer_methods) / sizeof(legacy_layer_methods[0]));

    probe_write("Probe complete.");
    if (g_probe_file) {
        fclose(g_probe_file);
        g_probe_file = NULL;
    }
}

void resource_saver_settings_ui_cleanup(void) {
    if (g_probe_file) {
        fclose(g_probe_file);
        g_probe_file = NULL;
    }
}
