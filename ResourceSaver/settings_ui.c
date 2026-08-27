#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/string.h"

/*
 * Resource Saver v1.2.0 in-game settings panel.
 *
 * The panel is drawn as an overlay while Terraria's IngameOptions.Draw is
 * running. This keeps input in Terraria's own settings screen and works with
 * Android touch because Terraria maps touch to Main.mouseX/Y/mouseLeft.
 */

typedef struct rs_vector2_t {
    float x;
    float y;
} rs_vector2_t;

typedef struct rs_color_t {
    uint32_t packed_value;
} rs_color_t;

static patch_handle_t g_main_mouse_x = PATCH_NULL;
static patch_handle_t g_main_mouse_y = PATCH_NULL;
static patch_handle_t g_main_mouse_left = PATCH_NULL;
static patch_handle_t g_utils_draw_border_string = PATCH_NULL;
static patch_hook_id_t g_options_draw_hook = PATCH_HOOK_INVALID_ID;

static bool g_panel_open = false;
static bool g_prev_mouse_left = false;
static int g_draw_failure_logged = 0;

static patch_handle_t g_open_label = PATCH_NULL;
static patch_handle_t g_close_label = PATCH_NULL;
static patch_handle_t g_defaults_label = PATCH_NULL;
static patch_handle_t g_hint_label = PATCH_NULL;
static patch_handle_t g_on_labels[RS_FEATURE_COUNT];
static patch_handle_t g_off_labels[RS_FEATURE_COUNT];

static const char* g_feature_names[RS_FEATURE_COUNT] = {
    "Master switch",
    "Regular ammo +20%",
    "Special ammo +10%",
    "Magic mana -15%",
    "Summon/Sentry mana -25%",
    "Mana regen +20%",
    "Melee potion recovery +10%",
    "Combat buff duration +20%",
    "Bait saver +20%"
};

static rs_color_t make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rs_color_t c;
    c.packed_value = ((uint32_t)r) |
                     ((uint32_t)g << 8) |
                     ((uint32_t)b << 16) |
                     ((uint32_t)a << 24);
    return c;
}

static int point_in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static patch_handle_t create_text(const char* text) {
    return text ? patchlib_string_create(text) : PATCH_NULL;
}

static void init_labels(void) {
    g_open_label = create_text("Resource Saver  [ OPEN ]");
    g_close_label = create_text("Resource Saver  [ CLOSE ]");
    g_defaults_label = create_text("[ Restore defaults ]");
    g_hint_label = create_text("Tap a row to enable / disable");

    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        char on[128];
        char off[128];
        snprintf(on, sizeof(on), "%s   [ ON ]", g_feature_names[i]);
        snprintf(off, sizeof(off), "%s   [ OFF ]", g_feature_names[i]);
        g_on_labels[i] = create_text(on);
        g_off_labels[i] = create_text(off);
    }
}

static int draw_text(
    patch_handle_t sprite_batch,
    patch_handle_t text,
    float x,
    float y,
    rs_color_t color,
    float scale
) {
    if (!g_utils_draw_border_string || !sprite_batch || !text) return 0;

    rs_vector2_t pos = { x, y };
    rs_vector2_t return_size = { 0.0f, 0.0f };
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    int max_chars = -1;

    void* call_args[8] = {
        &sprite_batch,
        &text,
        &pos,
        &color,
        &scale,
        &anchor_x,
        &anchor_y,
        &max_chars
    };

    return patchlib_method_invoke_args(
        g_utils_draw_border_string,
        NULL,
        &return_size,
        call_args
    ) ? 1 : 0;
}

static void options_draw_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)instance;
    (void)result;
    (void)sig;

    if (!args || !args[1]) return;

    patch_handle_t sprite_batch = *(patch_handle_t*)args[1];
    if (!sprite_batch) return;

    int mouse_x = 0;
    int mouse_y = 0;
    bool mouse_left = false;

    if (g_main_mouse_x) patchlib_field_get_value(g_main_mouse_x, NULL, &mouse_x);
    if (g_main_mouse_y) patchlib_field_get_value(g_main_mouse_y, NULL, &mouse_y);
    if (g_main_mouse_left) patchlib_field_get_value(g_main_mouse_left, NULL, &mouse_left);

    bool click = mouse_left && !g_prev_mouse_left;

    const int x = 18;
    const int top = 64;
    const int width = 286;
    const int row_h = 29;

    rs_color_t white = make_color(235, 235, 235, 255);
    rs_color_t green = make_color(120, 255, 140, 255);
    rs_color_t red = make_color(255, 125, 125, 255);
    rs_color_t yellow = make_color(255, 230, 120, 255);
    rs_color_t gray = make_color(180, 180, 180, 255);

    int header_hover = point_in_rect(mouse_x, mouse_y, x, top, width, row_h);
    patch_handle_t header = g_panel_open ? g_close_label : g_open_label;
    if (!draw_text(sprite_batch, header, (float)x, (float)top,
                   header_hover ? yellow : white, 0.78f) && !g_draw_failure_logged) {
        g_draw_failure_logged = 1;
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                         "Settings UI text draw failed at runtime");
    }

    if (click && header_hover) {
        g_panel_open = !g_panel_open;
        click = false;
    }

    if (g_panel_open) {
        int y = top + row_h + 6;
        draw_text(sprite_batch, g_hint_label, (float)x, (float)y, gray, 0.62f);
        y += row_h;

        const resource_saver_config_t* cfg = resource_saver_config_get();
        (void)cfg;

        for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
            int hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);
            bool enabled;

            if (i == RS_FEATURE_MASTER) {
                enabled = resource_saver_feature_enabled(RS_FEATURE_MASTER);
            }
            else {
                /* Show the stored sub-switch state even when master is OFF. */
                switch ((rs_feature_t)i) {
                    case RS_FEATURE_REGULAR_AMMO: enabled = cfg->regular_ammo; break;
                    case RS_FEATURE_SPECIAL_AMMO: enabled = cfg->special_ammo; break;
                    case RS_FEATURE_MAGIC_MANA: enabled = cfg->magic_mana; break;
                    case RS_FEATURE_SUMMON_MANA: enabled = cfg->summon_mana; break;
                    case RS_FEATURE_MANA_REGEN: enabled = cfg->mana_regen; break;
                    case RS_FEATURE_MELEE_POTION: enabled = cfg->melee_potion; break;
                    case RS_FEATURE_COMBAT_BUFF: enabled = cfg->combat_buff; break;
                    case RS_FEATURE_BAIT: enabled = cfg->bait; break;
                    default: enabled = false; break;
                }
            }

            rs_color_t color = hover ? yellow : (enabled ? green : red);
            draw_text(sprite_batch,
                      enabled ? g_on_labels[i] : g_off_labels[i],
                      (float)x, (float)y, color, 0.64f);

            if (click && hover) {
                resource_saver_config_toggle((rs_feature_t)i);
                click = false;
            }

            y += row_h;
        }

        int defaults_hover = point_in_rect(mouse_x, mouse_y, x, y + 3, width, row_h);
        draw_text(sprite_batch, g_defaults_label, (float)x, (float)(y + 3),
                  defaults_hover ? yellow : white, 0.68f);

        if (click && defaults_hover) {
            resource_saver_config_restore_defaults();
            click = false;
        }
    }

    g_prev_mouse_left = mouse_left;
}

static void free_handle(patch_handle_t* h) {
    if (*h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

void resource_saver_settings_ui_init(void) {
    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t options_type = patchlib_type_get_type("Terraria", "IngameOptions");
    patch_handle_t utils_type = patchlib_type_get_type("Terraria", "Utils");

    if (!main_type || !options_type || !utils_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Settings UI init failed: Main=%p IngameOptions=%p Utils=%p",
                         main_type, options_type, utils_type);
        goto done;
    }

    g_main_mouse_x = patchlib_type_get_field(main_type, "mouseX");
    g_main_mouse_y = patchlib_type_get_field(main_type, "mouseY");
    g_main_mouse_left = patchlib_type_get_field(main_type, "mouseLeft");

    g_utils_draw_border_string = patchlib_type_get_method_by_param_count(
        utils_type, "DrawBorderString", 8);

    patch_handle_t draw = patchlib_type_get_method_by_param_count(
        options_type, "Draw", 2);

    init_labels();

    if (draw && g_main_mouse_x && g_main_mouse_y && g_main_mouse_left &&
        g_utils_draw_border_string) {
        g_options_draw_hook = patchlib_install_prepost_hook(
            draw, NULL, options_draw_postfix);
    }

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Settings UI v1.2.0: %s (id=%d Draw=%p DrawBorderString=%p mouseX=%p mouseY=%p mouseLeft=%p)",
        g_options_draw_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_options_draw_hook,
        draw,
        g_utils_draw_border_string,
        g_main_mouse_x,
        g_main_mouse_y,
        g_main_mouse_left);

    if (draw) patchlib_free(draw);

done:
    if (main_type) patchlib_free(main_type);
    if (options_type) patchlib_free(options_type);
    if (utils_type) patchlib_free(utils_type);
}

void resource_saver_settings_ui_cleanup(void) {
    if (g_options_draw_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_options_draw_hook);
        g_options_draw_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_main_mouse_x);
    free_handle(&g_main_mouse_y);
    free_handle(&g_main_mouse_left);
    free_handle(&g_utils_draw_border_string);

    g_panel_open = false;
    g_prev_mouse_left = false;
}
