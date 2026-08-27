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
 * Resource Saver v1.2.6 - Android verified inventory UI
 *
 * Verified by the user's Android 1.4.5.6.4 runtime log:
 *   Terraria.Main.DrawInterface_27_Inventory  EXISTS
 *   Terraria.Utils.DrawBorderString           EXISTS
 *   SpriteBatch._beginCalled                  expected MonoGame state field
 *
 * Safety rules:
 * 1) Hook the verified inventory method by exact name, not param-count lookup.
 * 2) Never call SpriteBatch.Begin/End ourselves.
 * 3) Draw only when SpriteBatch._beginCalled == true.
 * 4) If the batch is not active, skip that stage instead of risking a crash.
 *
 * We attach both Prefix and Postfix. The overlay is drawn at whichever side of
 * DrawInterface_27_Inventory still has an active SpriteBatch. A per-call guard
 * prevents double rendering if both sides happen to be active.
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
static patch_handle_t g_main_screen_width = PATCH_NULL;
static patch_handle_t g_main_screen_height = PATCH_NULL;
static patch_handle_t g_main_ui_scale = PATCH_NULL;
static patch_handle_t g_main_sprite_batch = PATCH_NULL;

static patch_handle_t g_sprite_batch_begin_called = PATCH_NULL;
static patch_handle_t g_utils_draw_border_string = PATCH_NULL;

static patch_hook_id_t g_inventory_hook = PATCH_HOOK_INVALID_ID;

static bool g_panel_open = false;
static bool g_prev_mouse_left = false;
static bool g_drawn_this_inventory_call = false;

static patch_handle_t g_entry_open = PATCH_NULL;
static patch_handle_t g_entry_close = PATCH_NULL;
static patch_handle_t g_title = PATCH_NULL;
static patch_handle_t g_tip = PATCH_NULL;
static patch_handle_t g_restore = PATCH_NULL;
static patch_handle_t g_close = PATCH_NULL;
static patch_handle_t g_on_labels[RS_FEATURE_COUNT];
static patch_handle_t g_off_labels[RS_FEATURE_COUNT];

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

static patch_handle_t make_text(const char* s) {
    return s ? patchlib_string_create(s) : PATCH_NULL;
}

static void free_handle(patch_handle_t* h) {
    if (h && *h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

static void init_texts(void) {
    g_entry_open = make_text("【资源节省设置】 点击打开");
    g_entry_close = make_text("【资源节省设置】 点击收起");
    g_title = make_text("—— 精打细算：资源节省设置 ——");
    g_tip = make_text("点击对应项目即可开启或关闭，设置自动保存");
    g_restore = make_text("【恢复默认设置】");
    g_close = make_text("【关闭设置面板】");

    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        char on[192];
        char off[192];
        snprintf(on, sizeof(on), "%s    【开启】", g_feature_names[i]);
        snprintf(off, sizeof(off), "%s    【关闭】", g_feature_names[i]);
        g_on_labels[i] = make_text(on);
        g_off_labels[i] = make_text(off);
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
    if (!sprite_batch || !text || !g_utils_draw_border_string) return 0;

    rs_vector2_t pos = { x, y };
    rs_vector2_t result_size = { 0.0f, 0.0f };
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    int max_chars = -1;

    void* args[8] = {
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
        PATCH_NULL,
        &result_size,
        args
    ) ? 1 : 0;
}

static patch_handle_t get_sprite_batch(void) {
    patch_handle_t sb = PATCH_NULL;
    if (g_main_sprite_batch) {
        patchlib_field_get_value(g_main_sprite_batch, PATCH_NULL, &sb);
    }
    return sb;
}

static bool sprite_batch_is_active(patch_handle_t sb) {
    if (!sb || !g_sprite_batch_begin_called) return false;
    bool active = false;
    patchlib_field_get_value(g_sprite_batch_begin_called, sb, &active);
    return active;
}

static float get_ui_scale(void) {
    float scale = 1.0f;
    if (g_main_ui_scale) {
        patchlib_field_get_value(g_main_ui_scale, PATCH_NULL, &scale);
    }
    if (scale < 0.5f || scale > 4.0f) scale = 1.0f;
    return scale;
}

static void draw_and_handle_settings(void) {
    if (g_drawn_this_inventory_call) return;

    patch_handle_t sb = get_sprite_batch();
    if (!sprite_batch_is_active(sb)) return;

    g_drawn_this_inventory_call = true;

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

    float ui_scale = get_ui_scale();
    int ui_w = (int)((float)screen_width / ui_scale);
    int ui_h = (int)((float)screen_height / ui_scale);
    if (ui_w < 640) ui_w = 640;
    if (ui_h < 360) ui_h = 360;

    /* Draw in UI coordinates; convert rectangle back to physical mouse pixels. */
    const int panel_w = 520;
    const int row_h = 34;
    int x = ui_w - panel_w - 24;
    if (x < 250) x = 250;
    int y0 = 62;

    bool click = mouse_left && !g_prev_mouse_left;

    int hit_x = (int)((float)x * ui_scale);
    int hit_y0 = (int)((float)y0 * ui_scale);
    int hit_w = (int)((float)panel_w * ui_scale);
    int hit_h = (int)((float)row_h * ui_scale);

    rs_color_t white = make_color(245, 245, 245, 255);
    rs_color_t green = make_color(105, 255, 135, 255);
    rs_color_t red = make_color(255, 120, 120, 255);
    rs_color_t yellow = make_color(255, 230, 90, 255);
    rs_color_t cyan = make_color(90, 235, 255, 255);
    rs_color_t gray = make_color(205, 205, 205, 255);

    bool entry_hover = point_in_rect(mouse_x, mouse_y, hit_x, hit_y0, hit_w, hit_h);
    draw_text(
        sb,
        g_panel_open ? g_entry_close : g_entry_open,
        (float)x,
        (float)y0,
        entry_hover ? yellow : cyan,
        0.86f
    );

    if (click && entry_hover) {
        g_panel_open = !g_panel_open;
        click = false;
    }

    if (g_panel_open) {
        int y = y0 + row_h + 8;
        draw_text(sb, g_title, (float)x, (float)y, yellow, 0.78f);
        y += row_h;
        draw_text(sb, g_tip, (float)x, (float)y, gray, 0.62f);
        y += row_h + 4;

        for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
            bool enabled = resource_saver_feature_raw_enabled((rs_feature_t)i);

            int hy = (int)((float)y * ui_scale);
            bool hover = point_in_rect(mouse_x, mouse_y, hit_x, hy, hit_w, hit_h);

            rs_color_t color = hover ? yellow : (enabled ? green : red);
            draw_text(
                sb,
                enabled ? g_on_labels[i] : g_off_labels[i],
                (float)x,
                (float)y,
                color,
                0.70f
            );

            if (click && hover) {
                resource_saver_config_toggle((rs_feature_t)i);
                click = false;
            }
            y += row_h;
        }

        y += 4;
        int hy_restore = (int)((float)y * ui_scale);
        bool restore_hover = point_in_rect(mouse_x, mouse_y, hit_x, hy_restore, hit_w, hit_h);
        draw_text(sb, g_restore, (float)x, (float)y,
                  restore_hover ? yellow : white, 0.72f);
        if (click && restore_hover) {
            resource_saver_config_restore_defaults();
            click = false;
        }

        y += row_h;
        int hy_close = (int)((float)y * ui_scale);
        bool close_hover = point_in_rect(mouse_x, mouse_y, hit_x, hy_close, hit_w, hit_h);
        draw_text(sb, g_close, (float)x, (float)y,
                  close_hover ? yellow : white, 0.72f);
        if (click && close_hover) {
            g_panel_open = false;
            click = false;
        }
    }

    g_prev_mouse_left = mouse_left;
}

/*
 * IMPORTANT: in the verified Resource Saver v1.1.0 codebase Prefix hooks return
 * false to continue the vanilla method. Keep the same behavior here.
 */
static bool inventory_prefix(
    patch_handle_t instance,
    void** args,
    const patch_method_signature_t* sig,
    void* result
) {
    (void)instance;
    (void)args;
    (void)sig;
    (void)result;

    g_drawn_this_inventory_call = false;
    draw_and_handle_settings();
    return false;
}

static void inventory_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)instance;
    (void)args;
    (void)result;
    (void)sig;

    draw_and_handle_settings();

    /* If no safe drawing stage was available, still update the click edge. */
    if (!g_drawn_this_inventory_call && g_main_mouse_left) {
        bool mouse_left = false;
        patchlib_field_get_value(g_main_mouse_left, PATCH_NULL, &mouse_left);
        g_prev_mouse_left = mouse_left;
    }
}

void resource_saver_settings_ui_init(const char* private_dir) {
    (void)private_dir;

    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t utils_type = patchlib_type_get_type("Terraria", "Utils");
    patch_handle_t sprite_batch_type = patchlib_type_get_type(
        "Microsoft.Xna.Framework.Graphics", "SpriteBatch");

    if (!main_type || !utils_type || !sprite_batch_type) goto done;

    g_main_mouse_x = patchlib_type_get_field(main_type, "mouseX");
    g_main_mouse_y = patchlib_type_get_field(main_type, "mouseY");
    g_main_mouse_left = patchlib_type_get_field(main_type, "mouseLeft");
    g_main_screen_width = patchlib_type_get_field(main_type, "screenWidth");
    g_main_screen_height = patchlib_type_get_field(main_type, "screenHeight");
    g_main_ui_scale = patchlib_type_get_field(main_type, "UIScale");
    g_main_sprite_batch = patchlib_type_get_field(main_type, "spriteBatch");

    /* MonoGame safety state. If this field is unavailable, the UI stays disabled. */
    g_sprite_batch_begin_called = patchlib_type_get_field(sprite_batch_type, "_beginCalled");

    /* Verified exact method name from Android runtime probe. */
    g_utils_draw_border_string = patchlib_type_get_method(utils_type, "DrawBorderString");
    patch_handle_t inventory_method = patchlib_type_get_method(
        main_type, "DrawInterface_27_Inventory");

    init_texts();

    if (inventory_method &&
        g_main_sprite_batch &&
        g_sprite_batch_begin_called &&
        g_utils_draw_border_string) {
        g_inventory_hook = patchlib_install_prepost_hook(
            inventory_method,
            inventory_prefix,
            inventory_postfix
        );
    }

    if (inventory_method) patchlib_free(inventory_method);

done:
    if (main_type) patchlib_free(main_type);
    if (utils_type) patchlib_free(utils_type);
    if (sprite_batch_type) patchlib_free(sprite_batch_type);
}

void resource_saver_settings_ui_cleanup(void) {
    if (g_inventory_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_inventory_hook);
        g_inventory_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_main_mouse_x);
    free_handle(&g_main_mouse_y);
    free_handle(&g_main_mouse_left);
    free_handle(&g_main_screen_width);
    free_handle(&g_main_screen_height);
    free_handle(&g_main_ui_scale);
    free_handle(&g_main_sprite_batch);
    free_handle(&g_sprite_batch_begin_called);
    free_handle(&g_utils_draw_border_string);

    free_handle(&g_entry_open);
    free_handle(&g_entry_close);
    free_handle(&g_title);
    free_handle(&g_tip);
    free_handle(&g_restore);
    free_handle(&g_close);
    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        free_handle(&g_on_labels[i]);
        free_handle(&g_off_labels[i]);
    }

    g_panel_open = false;
    g_prev_mouse_left = false;
    g_drawn_this_inventory_call = false;
}
