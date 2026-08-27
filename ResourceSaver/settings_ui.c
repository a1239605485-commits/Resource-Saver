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
 * Resource Saver v1.2.1 - 中文游戏内独立设置面板
 *
 * Android 1.4.5.6.4 日志证明 Terraria.IngameOptions 在当前 IL2CPP 构建中
 * 无法通过 patchlib_type_get_type("Terraria", "IngameOptions") 找到。
 * 因此本版不再依赖 IngameOptions。
 *
 * 首选 Hook：Main.DrawInventory()
 * 备用 Hook：Main.DrawInterface_27_Inventory()
 * 最后备用：Main.DrawInterface(GameTime)，仅在 playerInventory=true 时绘制。
 *
 * 打开背包后，右上区域显示“资源节省设置【打开】”。
 */

typedef struct rs_vector2_t {
    float x;
    float y;
} rs_vector2_t;

typedef struct rs_color_t {
    uint32_t packed_value;
} rs_color_t;

enum {
    RS_DRAW_NONE = 0,
    RS_DRAW_INVENTORY = 1,
    RS_DRAW_INTERFACE_27 = 2,
    RS_DRAW_INTERFACE = 3
};

static patch_handle_t g_main_mouse_x = PATCH_NULL;
static patch_handle_t g_main_mouse_y = PATCH_NULL;
static patch_handle_t g_main_mouse_left = PATCH_NULL;
static patch_handle_t g_main_screen_width = PATCH_NULL;
static patch_handle_t g_main_player_inventory = PATCH_NULL;
static patch_handle_t g_main_sprite_batch = PATCH_NULL;
static patch_handle_t g_utils_draw_border_string = PATCH_NULL;

static patch_hook_id_t g_draw_hook = PATCH_HOOK_INVALID_ID;
static int g_draw_mode = RS_DRAW_NONE;

static bool g_panel_open = false;
static bool g_prev_mouse_left = false;
static int g_runtime_draw_failure_logged = 0;

static patch_handle_t g_button_open = PATCH_NULL;
static patch_handle_t g_button_close = PATCH_NULL;
static patch_handle_t g_title = PATCH_NULL;
static patch_handle_t g_tip = PATCH_NULL;
static patch_handle_t g_restore_defaults = PATCH_NULL;
static patch_handle_t g_close_panel = PATCH_NULL;
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

static patch_handle_t create_text(const char* text) {
    return text ? patchlib_string_create(text) : PATCH_NULL;
}

static void init_labels(void) {
    g_button_open = create_text("资源节省设置  【打开】");
    g_button_close = create_text("资源节省设置  【收起】");
    g_title = create_text("—— 精打细算：资源节省设置 ——");
    g_tip = create_text("点击下面项目可独立开启或关闭，设置会自动保存");
    g_restore_defaults = create_text("【恢复默认设置】");
    g_close_panel = create_text("【关闭设置面板】");

    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        char on[160];
        char off[160];
        snprintf(on, sizeof(on), "%s    【开启】", g_feature_names[i]);
        snprintf(off, sizeof(off), "%s    【关闭】", g_feature_names[i]);
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

static int inventory_should_be_visible(void) {
    if (g_draw_mode == RS_DRAW_INVENTORY || g_draw_mode == RS_DRAW_INTERFACE_27)
        return 1;

    if (g_draw_mode == RS_DRAW_INTERFACE && g_main_player_inventory) {
        bool player_inventory = false;
        patchlib_field_get_value(g_main_player_inventory, NULL, &player_inventory);
        return player_inventory ? 1 : 0;
    }

    return 0;
}

static void draw_settings_overlay(void) {
    if (!inventory_should_be_visible()) {
        g_prev_mouse_left = false;
        return;
    }

    patch_handle_t sprite_batch = PATCH_NULL;
    if (g_main_sprite_batch)
        patchlib_field_get_value(g_main_sprite_batch, NULL, &sprite_batch);
    if (!sprite_batch) return;

    int mouse_x = 0;
    int mouse_y = 0;
    int screen_width = 800;
    bool mouse_left = false;

    if (g_main_mouse_x) patchlib_field_get_value(g_main_mouse_x, NULL, &mouse_x);
    if (g_main_mouse_y) patchlib_field_get_value(g_main_mouse_y, NULL, &mouse_y);
    if (g_main_mouse_left) patchlib_field_get_value(g_main_mouse_left, NULL, &mouse_left);
    if (g_main_screen_width) patchlib_field_get_value(g_main_screen_width, NULL, &screen_width);

    if (screen_width < 400) screen_width = 400;

    bool click = mouse_left && !g_prev_mouse_left;

    const int width = 330;
    const int row_h = 28;
    int x = screen_width - width - 14;
    if (x < 12) x = 12;
    const int top = 54;

    rs_color_t white = make_color(240, 240, 240, 255);
    rs_color_t green = make_color(110, 255, 135, 255);
    rs_color_t red = make_color(255, 120, 120, 255);
    rs_color_t yellow = make_color(255, 225, 105, 255);
    rs_color_t cyan = make_color(125, 235, 255, 255);
    rs_color_t gray = make_color(190, 190, 190, 255);

    int button_hover = point_in_rect(mouse_x, mouse_y, x, top, width, row_h);
    patch_handle_t button_text = g_panel_open ? g_button_close : g_button_open;

    if (!draw_text(sprite_batch, button_text, (float)x, (float)top,
                   button_hover ? yellow : cyan, 0.70f) && !g_runtime_draw_failure_logged) {
        g_runtime_draw_failure_logged = 1;
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                         "Chinese settings UI: DrawBorderString runtime invocation failed");
    }

    if (click && button_hover) {
        g_panel_open = !g_panel_open;
        click = false;
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                         "Chinese settings panel %s", g_panel_open ? "opened" : "closed");
    }

    if (g_panel_open) {
        int y = top + row_h + 4;

        draw_text(sprite_batch, g_title, (float)x, (float)y, yellow, 0.66f);
        y += row_h;
        draw_text(sprite_batch, g_tip, (float)x, (float)y, gray, 0.55f);
        y += row_h;

        for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
            bool raw_enabled = resource_saver_feature_raw_enabled((rs_feature_t)i);
            int hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);

            rs_color_t color;
            if (hover) color = yellow;
            else if (raw_enabled) color = green;
            else color = red;

            draw_text(
                sprite_batch,
                raw_enabled ? g_on_labels[i] : g_off_labels[i],
                (float)x,
                (float)y,
                color,
                0.59f
            );

            if (click && hover) {
                resource_saver_config_toggle((rs_feature_t)i);
                click = false;
            }

            y += row_h;
        }

        y += 3;
        int defaults_hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);
        draw_text(sprite_batch, g_restore_defaults, (float)x, (float)y,
                  defaults_hover ? yellow : white, 0.62f);
        if (click && defaults_hover) {
            resource_saver_config_restore_defaults();
            click = false;
        }

        y += row_h;
        int close_hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);
        draw_text(sprite_batch, g_close_panel, (float)x, (float)y,
                  close_hover ? yellow : white, 0.62f);
        if (click && close_hover) {
            g_panel_open = false;
            click = false;
        }
    }

    g_prev_mouse_left = mouse_left;
}

static void draw_postfix(
    patch_handle_t instance,
    void** args,
    void* result,
    const patch_method_signature_t* sig
) {
    (void)instance;
    (void)args;
    (void)result;
    (void)sig;
    draw_settings_overlay();
}

static void free_handle(patch_handle_t* h) {
    if (*h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

void resource_saver_settings_ui_init(void) {
    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t utils_type = patchlib_type_get_type("Terraria", "Utils");
    patch_handle_t draw_target = PATCH_NULL;

    if (!main_type || !utils_type) {
        mod_logger_write(MOD_LOG_LEVEL_ERROR, "ResourceSaver",
                         "Chinese settings UI init failed: Main=%p Utils=%p",
                         main_type, utils_type);
        goto done;
    }

    g_main_mouse_x = patchlib_type_get_field(main_type, "mouseX");
    g_main_mouse_y = patchlib_type_get_field(main_type, "mouseY");
    g_main_mouse_left = patchlib_type_get_field(main_type, "mouseLeft");
    g_main_screen_width = patchlib_type_get_field(main_type, "screenWidth");
    g_main_player_inventory = patchlib_type_get_field(main_type, "playerInventory");
    g_main_sprite_batch = patchlib_type_get_field(main_type, "spriteBatch");

    g_utils_draw_border_string = patchlib_type_get_method_by_param_count(
        utils_type, "DrawBorderString", 8);

    /* Android 1.4.5.6.4: 优先使用背包绘制函数。 */
    draw_target = patchlib_type_get_method_by_param_count(main_type, "DrawInventory", 0);
    if (draw_target) {
        g_draw_mode = RS_DRAW_INVENTORY;
    }
    else {
        draw_target = patchlib_type_get_method_by_param_count(
            main_type, "DrawInterface_27_Inventory", 0);
        if (draw_target) {
            g_draw_mode = RS_DRAW_INTERFACE_27;
        }
        else {
            draw_target = patchlib_type_get_method_by_param_count(
                main_type, "DrawInterface", 1);
            if (draw_target) {
                g_draw_mode = RS_DRAW_INTERFACE;
            }
        }
    }

    init_labels();

    if (draw_target &&
        g_main_mouse_x && g_main_mouse_y && g_main_mouse_left &&
        g_main_sprite_batch && g_utils_draw_border_string) {
        g_draw_hook = patchlib_install_prepost_hook(
            draw_target, NULL, draw_postfix);
    }

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Chinese settings UI v1.2.1: %s (hook=%d mode=%d target=%p spriteBatch=%p DrawBorderString=%p mouseX=%p mouseY=%p mouseLeft=%p playerInventory=%p)",
        g_draw_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_draw_hook,
        g_draw_mode,
        draw_target,
        g_main_sprite_batch,
        g_utils_draw_border_string,
        g_main_mouse_x,
        g_main_mouse_y,
        g_main_mouse_left,
        g_main_player_inventory
    );

    if (draw_target) patchlib_free(draw_target);

done:
    if (main_type) patchlib_free(main_type);
    if (utils_type) patchlib_free(utils_type);
}

void resource_saver_settings_ui_cleanup(void) {
    if (g_draw_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_draw_hook);
        g_draw_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_main_mouse_x);
    free_handle(&g_main_mouse_y);
    free_handle(&g_main_mouse_left);
    free_handle(&g_main_screen_width);
    free_handle(&g_main_player_inventory);
    free_handle(&g_main_sprite_batch);
    free_handle(&g_utils_draw_border_string);

    g_draw_mode = RS_DRAW_NONE;
    g_panel_open = false;
    g_prev_mouse_left = false;
    g_runtime_draw_failure_logged = 0;
}
