#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "mod_logger.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/string.h"

/*
 * Resource Saver v1.2.7 - Safe Chinese settings UI
 *
 * Why this version exists:
 * v1.2.6 proved that DrawInterface_27_Inventory itself can be hooked, but
 * invoking Terraria.Utils.DrawBorderString through patchlib caused IL2CPP to
 * abort when the world began drawing. DrawBorderString uses Vector2/Color
 * value-type parameters, while TEFKernel's generic invoke API only exposes
 * primitive/object parameter categories. We therefore stop calling any draw
 * method that requires value-type structs.
 *
 * This version delegates all actual rendering to vanilla Main.MouseText(...),
 * whose explicit parameters are only String/int/byte/int/int/int/int. Terraria
 * internally handles SpriteBatch, fonts, colors and Vector2 values.
 *
 * Safety rules:
 *   1. Core gameplay code remains based on v1.1.0.
 *   2. UI uses a POSTFIX only; the vanilla draw method is never skipped.
 *   3. No SpriteBatch.Begin/End calls.
 *   4. No DrawBorderString/DrawString/Vector2/Color reflection calls.
 *   5. UI renders only while vanilla SpriteBatch is already active.
 *   6. If any required method/field is missing, the UI simply disables itself.
 */

static patch_handle_t g_main_instance = PATCH_NULL;
static patch_handle_t g_main_mouse_x = PATCH_NULL;
static patch_handle_t g_main_mouse_y = PATCH_NULL;
static patch_handle_t g_main_mouse_left = PATCH_NULL;
static patch_handle_t g_main_screen_width = PATCH_NULL;
static patch_handle_t g_main_screen_height = PATCH_NULL;
static patch_handle_t g_main_player_inventory = PATCH_NULL;
static patch_handle_t g_main_sprite_batch = PATCH_NULL;
static patch_handle_t g_sprite_batch_begin_called = PATCH_NULL;

static patch_handle_t g_mouse_text_method = PATCH_NULL;
static bool g_mouse_text_is_instance = true;

static patch_hook_id_t g_ui_hook = PATCH_HOOK_INVALID_ID;

static patch_handle_t g_entry_text = PATCH_NULL;
static patch_handle_t g_panel_text = PATCH_NULL;

static bool g_panel_open = false;
static bool g_prev_mouse_left = false;

/* Generous touch hitboxes for Android. */
static const int ENTRY_WIDTH = 430;
static const int ENTRY_HEIGHT = 56;
static const int PANEL_WIDTH = 700;
static const int PANEL_ROW_HEIGHT = 38;
static const int PANEL_TOP_PADDING = 18;

static const char* g_feature_names[RS_FEATURE_COUNT] = {
    "总开关",
    "普通弹药节省 20%",
    "特殊弹药节省 10%",
    "魔法魔力消耗 -15%",
    "召唤/哨兵魔耗 -25%",
    "自然回魔速度 +20%",
    "近战药水冷却 +10%",
    "战斗增益时长 +20%",
    "鱼饵节省 20%"
};

static void free_handle(patch_handle_t* h) {
    if (h && *h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

static int point_in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void rebuild_panel_text(void) {
    free_handle(&g_panel_text);

    char buffer[2048];
    size_t used = 0;

    int n = snprintf(
        buffer + used,
        sizeof(buffer) - used,
        "【精打细算：资源节省设置】\n"
        "点击对应项目即可开启或关闭，设置会自动保存\n"
    );
    if (n < 0) return;
    used += (size_t)n;

    for (int i = 0; i < RS_FEATURE_COUNT && used < sizeof(buffer); ++i) {
        bool enabled = resource_saver_feature_raw_enabled((rs_feature_t)i);
        n = snprintf(
            buffer + used,
            sizeof(buffer) - used,
            "%s    【%s】\n",
            g_feature_names[i],
            enabled ? "开启" : "关闭"
        );
        if (n < 0) return;
        if ((size_t)n >= sizeof(buffer) - used) {
            used = sizeof(buffer) - 1;
            break;
        }
        used += (size_t)n;
    }

    if (used < sizeof(buffer)) {
        n = snprintf(
            buffer + used,
            sizeof(buffer) - used,
            "【恢复默认设置】\n"
            "【关闭设置面板】"
        );
        if (n > 0) {
            size_t add = (size_t)n;
            if (add >= sizeof(buffer) - used) add = sizeof(buffer) - used - 1;
            used += add;
        }
    }

    buffer[sizeof(buffer) - 1] = '\0';
    g_panel_text = patchlib_string_create(buffer);
}

static patch_handle_t get_main_instance(void) {
    if (!g_mouse_text_is_instance) return PATCH_NULL;
    if (!g_main_instance) return PATCH_NULL;

    patch_handle_t instance = PATCH_NULL;
    patchlib_field_get_value(g_main_instance, PATCH_NULL, &instance);
    return instance;
}

static patch_handle_t get_sprite_batch(void) {
    if (!g_main_sprite_batch) return PATCH_NULL;
    patch_handle_t sb = PATCH_NULL;
    patchlib_field_get_value(g_main_sprite_batch, PATCH_NULL, &sb);
    return sb;
}

static bool sprite_batch_is_active(void) {
    if (!g_sprite_batch_begin_called) return false;

    patch_handle_t sb = get_sprite_batch();
    if (!sb) return false;

    bool active = false;
    patchlib_field_get_value(g_sprite_batch_begin_called, sb, &active);
    return active;
}

/*
 * Vanilla Main.MouseText signature used here:
 *   MouseText(string cursorText,
 *             int rare,
 *             byte diff,
 *             int hackedMouseX,
 *             int hackedMouseY,
 *             int hackedScreenWidth,
 *             int hackedScreenHeight)
 *
 * All explicit parameters are primitive/object types, so this is much safer
 * for patchlib_method_invoke_args than DrawBorderString(Vector2, Color, ...).
 */
static bool vanilla_mouse_text(
    patch_handle_t text,
    int x,
    int y,
    int screen_width,
    int screen_height
) {
    if (!g_mouse_text_method || !text) return false;

    patch_handle_t instance = get_main_instance();
    if (g_mouse_text_is_instance && !instance) return false;

    int rare = 0;
    uint8_t diff = 0;

    void* args[7] = {
        &text,
        &rare,
        &diff,
        &x,
        &y,
        &screen_width,
        &screen_height
    };

    return patchlib_method_invoke_args(
        g_mouse_text_method,
        g_mouse_text_is_instance ? instance : PATCH_NULL,
        NULL,
        args
    );
}

static void handle_panel_click(
    int mouse_x,
    int mouse_y,
    int panel_x,
    int panel_y,
    bool click
) {
    if (!click) return;

    /*
     * MouseText adds a small vanilla offset around the supplied hacked mouse
     * coordinates. Hitboxes are intentionally wide/tall to remain easy to tap.
     *
     * Row mapping inside our multiline text:
     *   line 0 title
     *   line 1 hint
     *   lines 2..10 features 0..8
     *   line 11 restore defaults
     *   line 12 close panel
     */
    int content_y = panel_y + PANEL_TOP_PADDING;
    int feature_start_y = content_y + PANEL_ROW_HEIGHT * 2;

    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        int row_y = feature_start_y + PANEL_ROW_HEIGHT * i;
        if (point_in_rect(mouse_x, mouse_y, panel_x, row_y,
                          PANEL_WIDTH, PANEL_ROW_HEIGHT)) {
            resource_saver_config_toggle((rs_feature_t)i);
            rebuild_panel_text();
            return;
        }
    }

    int restore_y = feature_start_y + PANEL_ROW_HEIGHT * RS_FEATURE_COUNT;
    if (point_in_rect(mouse_x, mouse_y, panel_x, restore_y,
                      PANEL_WIDTH, PANEL_ROW_HEIGHT)) {
        resource_saver_config_restore_defaults();
        rebuild_panel_text();
        return;
    }

    int close_y = restore_y + PANEL_ROW_HEIGHT;
    if (point_in_rect(mouse_x, mouse_y, panel_x, close_y,
                      PANEL_WIDTH, PANEL_ROW_HEIGHT)) {
        g_panel_open = false;
    }
}

static void draw_and_handle_ui(void) {
    /* Settings are intentionally available only while inventory is open. */
    if (g_main_player_inventory) {
        bool inventory_open = false;
        patchlib_field_get_value(g_main_player_inventory, PATCH_NULL, &inventory_open);
        if (!inventory_open) {
            if (g_main_mouse_left) {
                bool left = false;
                patchlib_field_get_value(g_main_mouse_left, PATCH_NULL, &left);
                g_prev_mouse_left = left;
            }
            return;
        }
    }

    /* Do not ask vanilla to draw unless its current SpriteBatch is active. */
    if (!sprite_batch_is_active()) return;

    int mouse_x = 0;
    int mouse_y = 0;
    int screen_width = 1280;
    int screen_height = 720;
    bool mouse_left = false;

    if (g_main_mouse_x) patchlib_field_get_value(g_main_mouse_x, PATCH_NULL, &mouse_x);
    if (g_main_mouse_y) patchlib_field_get_value(g_main_mouse_y, PATCH_NULL, &mouse_y);
    if (g_main_mouse_left) patchlib_field_get_value(g_main_mouse_left, PATCH_NULL, &mouse_left);
    if (g_main_screen_width) patchlib_field_get_value(g_main_screen_width, PATCH_NULL, &screen_width);
    if (g_main_screen_height) patchlib_field_get_value(g_main_screen_height, PATCH_NULL, &screen_height);

    if (screen_width < 640) screen_width = 640;
    if (screen_height < 360) screen_height = 360;

    bool click = mouse_left && !g_prev_mouse_left;

    if (!g_panel_open) {
        int x = screen_width - ENTRY_WIDTH - 30;
        if (x < 20) x = 20;
        int y = 72;

        vanilla_mouse_text(g_entry_text, x, y, screen_width, screen_height);

        if (click && point_in_rect(mouse_x, mouse_y, x, y,
                                   ENTRY_WIDTH, ENTRY_HEIGHT)) {
            g_panel_open = true;
            rebuild_panel_text();
        }
    }
    else {
        int x = screen_width - PANEL_WIDTH - 30;
        if (x < 20) x = 20;
        int y = 70;

        if (!g_panel_text) rebuild_panel_text();
        vanilla_mouse_text(g_panel_text, x, y, screen_width, screen_height);
        handle_panel_click(mouse_x, mouse_y, x, y, click);
    }

    g_prev_mouse_left = mouse_left;
}

/* Postfix only: never interfere with the vanilla inventory/UI method. */
static void ui_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)instance;
    (void)args;
    (void)result;
    (void)sig;
    draw_and_handle_ui();
}

void resource_saver_settings_ui_init(const char* private_dir) {
    (void)private_dir;

    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t sprite_batch_type = patchlib_type_get_type(
        "Microsoft.Xna.Framework.Graphics", "SpriteBatch");

    if (!main_type || !sprite_batch_type) goto done;

    g_main_instance = patchlib_type_get_field(main_type, "instance");
    g_main_mouse_x = patchlib_type_get_field(main_type, "mouseX");
    g_main_mouse_y = patchlib_type_get_field(main_type, "mouseY");
    g_main_mouse_left = patchlib_type_get_field(main_type, "mouseLeft");
    g_main_screen_width = patchlib_type_get_field(main_type, "screenWidth");
    g_main_screen_height = patchlib_type_get_field(main_type, "screenHeight");
    g_main_player_inventory = patchlib_type_get_field(main_type, "playerInventory");
    g_main_sprite_batch = patchlib_type_get_field(main_type, "spriteBatch");
    g_sprite_batch_begin_called = patchlib_type_get_field(sprite_batch_type, "_beginCalled");

    /* Prefer exact 7-parameter vanilla MouseText overload. */
    g_mouse_text_method = patchlib_type_get_method_by_param_count(main_type, "MouseText", 7);
    if (!g_mouse_text_method) {
        patch_handle_t candidate = patchlib_type_get_method(main_type, "MouseText");
        if (candidate && patchlib_method_get_param_count(candidate) == 7) {
            g_mouse_text_method = candidate;
            candidate = PATCH_NULL;
        }
        if (candidate) patchlib_free(candidate);
    }

    if (g_mouse_text_method) {
        g_mouse_text_is_instance = patchlib_method_is_instance(g_mouse_text_method);
    }

    /*
     * DrawInterface_33_MouseText is the safest place for Main.MouseText because
     * vanilla itself calls MouseText in this layer. If unavailable, fall back
     * to the verified inventory layer. Both are hooked postfix-only.
     */
    patch_handle_t draw_method = patchlib_type_get_method(main_type, "DrawInterface_33_MouseText");
    if (!draw_method) {
        draw_method = patchlib_type_get_method(main_type, "DrawInterface_27_Inventory");
    }

    g_entry_text = patchlib_string_create("【资源节省设置】 点击打开");
    rebuild_panel_text();

    bool instance_ok = !g_mouse_text_is_instance || g_main_instance;

    if (draw_method &&
        g_mouse_text_method &&
        instance_ok &&
        g_main_sprite_batch &&
        g_sprite_batch_begin_called &&
        g_entry_text) {
        g_ui_hook = patchlib_install_prepost_hook(
            draw_method,
            NULL,
            ui_postfix
        );
    }

    if (draw_method) patchlib_free(draw_method);

done:
    if (main_type) patchlib_free(main_type);
    if (sprite_batch_type) patchlib_free(sprite_batch_type);
}

void resource_saver_settings_ui_cleanup(void) {
    if (g_ui_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_ui_hook);
        g_ui_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_main_instance);
    free_handle(&g_main_mouse_x);
    free_handle(&g_main_mouse_y);
    free_handle(&g_main_mouse_left);
    free_handle(&g_main_screen_width);
    free_handle(&g_main_screen_height);
    free_handle(&g_main_player_inventory);
    free_handle(&g_main_sprite_batch);
    free_handle(&g_sprite_batch_begin_called);
    free_handle(&g_mouse_text_method);

    free_handle(&g_entry_text);
    free_handle(&g_panel_text);

    g_panel_open = false;
    g_prev_mouse_left = false;
}
