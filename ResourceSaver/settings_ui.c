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
 * Resource Saver v1.2.2 - 中文游戏内设置面板
 *
 * v1.2.1 日志已经证明：
 * - Terraria.Main / Terraria.Utils 可以找到；
 * - 但 Android 1.4.5.6.4 并没有匹配到我们猜测的
 *   DrawInventory / DrawInterface_27_Inventory / DrawInterface 绘制目标。
 *
 * v1.2.2 改为只 Hook 已经稳定存在的 Main.Draw(GameTime)。
 * Main.Draw 返回后，原版 SpriteBatch 已结束，因此本 MOD 自己执行：
 *     SpriteBatch.Begin()
 *     Utils.DrawBorderString(...)
 *     SpriteBatch.End()
 * 这样不依赖 Terraria 内部背包绘制函数的名字。
 *
 * 默认仅在背包打开时显示入口；如果 playerInventory 字段在某个构建中
 * 不可读取，则自动退化为始终显示入口，确保不会再“完全没有按钮”。
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
static patch_handle_t g_main_player_inventory = PATCH_NULL;
static patch_handle_t g_main_sprite_batch = PATCH_NULL;

static patch_handle_t g_utils_draw_border_string = PATCH_NULL;
static patch_handle_t g_sprite_batch_begin = PATCH_NULL;
static patch_handle_t g_sprite_batch_end = PATCH_NULL;

static patch_hook_id_t g_draw_hook = PATCH_HOOK_INVALID_ID;

static bool g_panel_open = false;
static bool g_prev_mouse_left = false;
static int g_draw_failure_logged = 0;
static unsigned long long g_draw_frames = 0;

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

static void free_handle(patch_handle_t* h) {
    if (*h) {
        patchlib_free(*h);
        *h = PATCH_NULL;
    }
}

static void init_labels(void) {
    g_button_open = create_text("【资源节省设置】  点击打开");
    g_button_close = create_text("【资源节省设置】  点击收起");
    g_title = create_text("—— 精打细算：资源节省设置 ——");
    g_tip = create_text("点击项目即可独立开启/关闭，修改后自动保存");
    g_restore_defaults = create_text("【恢复默认设置】");
    g_close_panel = create_text("【关闭设置面板】");

    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        char on[192];
        char off[192];
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

static int should_show_entry(void) {
    /* 首选：只在打开背包时显示。 */
    if (g_main_player_inventory) {
        bool open = false;
        patchlib_field_get_value(g_main_player_inventory, NULL, &open);
        return open ? 1 : 0;
    }

    /*
     * 兼容回退：某些手机版若 playerInventory 字段不可访问，
     * 入口始终显示。相比完全看不到按钮，这个回退更安全。
     */
    return 1;
}

static int begin_overlay_batch(patch_handle_t sprite_batch) {
    if (!g_sprite_batch_begin || !sprite_batch) return 0;
    return patchlib_method_invoke_args(
        g_sprite_batch_begin,
        sprite_batch,
        NULL,
        NULL
    ) ? 1 : 0;
}

static void end_overlay_batch(patch_handle_t sprite_batch) {
    if (!g_sprite_batch_end || !sprite_batch) return;
    patchlib_method_invoke_args(
        g_sprite_batch_end,
        sprite_batch,
        NULL,
        NULL
    );
}

static void draw_settings_overlay(void) {
    ++g_draw_frames;

    if (!should_show_entry()) {
        g_prev_mouse_left = false;
        return;
    }

    patch_handle_t sprite_batch = PATCH_NULL;
    if (g_main_sprite_batch) {
        patchlib_field_get_value(g_main_sprite_batch, NULL, &sprite_batch);
    }
    if (!sprite_batch) return;

    if (!begin_overlay_batch(sprite_batch)) {
        if (!g_draw_failure_logged) {
            g_draw_failure_logged = 1;
            mod_logger_write(
                MOD_LOG_LEVEL_WARNING,
                "ResourceSaver",
                "Chinese UI v1.2.2: SpriteBatch.Begin() invocation failed"
            );
        }
        return;
    }

    int mouse_x = 0;
    int mouse_y = 0;
    int screen_width = 1280;
    int screen_height = 720;
    bool mouse_left = false;

    if (g_main_mouse_x) patchlib_field_get_value(g_main_mouse_x, NULL, &mouse_x);
    if (g_main_mouse_y) patchlib_field_get_value(g_main_mouse_y, NULL, &mouse_y);
    if (g_main_mouse_left) patchlib_field_get_value(g_main_mouse_left, NULL, &mouse_left);
    if (g_main_screen_width) patchlib_field_get_value(g_main_screen_width, NULL, &screen_width);
    if (g_main_screen_height) patchlib_field_get_value(g_main_screen_height, NULL, &screen_height);

    if (screen_width < 480) screen_width = 480;
    if (screen_height < 320) screen_height = 320;

    bool click = mouse_left && !g_prev_mouse_left;

    /*
     * 使用实际屏幕像素绘制，避免依赖 Terraria UI 缩放矩阵。
     * 按钮固定在右上方，尺寸刻意做大，方便 Android 触控。
     */
    const int width = 440;
    const int row_h = 38;
    int x = screen_width - width - 24;
    if (x < 20) x = 20;
    int top = 105;
    if (top + row_h > screen_height) top = 20;

    rs_color_t white = make_color(245, 245, 245, 255);
    rs_color_t green = make_color(115, 255, 145, 255);
    rs_color_t red = make_color(255, 125, 125, 255);
    rs_color_t yellow = make_color(255, 228, 95, 255);
    rs_color_t cyan = make_color(105, 235, 255, 255);
    rs_color_t gray = make_color(200, 200, 200, 255);

    int button_hover = point_in_rect(mouse_x, mouse_y, x, top, width, row_h);
    patch_handle_t button_text = g_panel_open ? g_button_close : g_button_open;

    int draw_ok = draw_text(
        sprite_batch,
        button_text,
        (float)x,
        (float)top,
        button_hover ? yellow : cyan,
        0.92f
    );

    if (!draw_ok && !g_draw_failure_logged) {
        g_draw_failure_logged = 1;
        mod_logger_write(
            MOD_LOG_LEVEL_WARNING,
            "ResourceSaver",
            "Chinese UI v1.2.2: DrawBorderString invocation failed"
        );
    }

    if (click && button_hover) {
        g_panel_open = !g_panel_open;
        click = false;
    }

    if (g_panel_open) {
        int y = top + row_h + 12;

        draw_text(sprite_batch, g_title, (float)x, (float)y, yellow, 0.82f);
        y += row_h;
        draw_text(sprite_batch, g_tip, (float)x, (float)y, gray, 0.68f);
        y += row_h + 4;

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
                0.76f
            );

            if (click && hover) {
                resource_saver_config_toggle((rs_feature_t)i);
                click = false;
            }

            y += row_h;
        }

        y += 8;
        int defaults_hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);
        draw_text(
            sprite_batch,
            g_restore_defaults,
            (float)x,
            (float)y,
            defaults_hover ? yellow : white,
            0.78f
        );
        if (click && defaults_hover) {
            resource_saver_config_restore_defaults();
            click = false;
        }

        y += row_h;
        int close_hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);
        draw_text(
            sprite_batch,
            g_close_panel,
            (float)x,
            (float)y,
            close_hover ? yellow : white,
            0.78f
        );
        if (click && close_hover) {
            g_panel_open = false;
            click = false;
        }
    }

    end_overlay_batch(sprite_batch);
    g_prev_mouse_left = mouse_left;
}

static void main_draw_postfix(
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

void resource_saver_settings_ui_init(void) {
    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t utils_type = patchlib_type_get_type("Terraria", "Utils");
    patch_handle_t sprite_batch_type = patchlib_type_get_type(
        "Microsoft.Xna.Framework.Graphics",
        "SpriteBatch"
    );
    patch_handle_t draw_target = PATCH_NULL;

    if (!main_type || !utils_type || !sprite_batch_type) {
        mod_logger_write(
            MOD_LOG_LEVEL_ERROR,
            "ResourceSaver",
            "Chinese UI v1.2.2 init failed: Main=%p Utils=%p SpriteBatch=%p",
            main_type,
            utils_type,
            sprite_batch_type
        );
        goto done;
    }

    g_main_mouse_x = patchlib_type_get_field(main_type, "mouseX");
    g_main_mouse_y = patchlib_type_get_field(main_type, "mouseY");
    g_main_mouse_left = patchlib_type_get_field(main_type, "mouseLeft");
    g_main_screen_width = patchlib_type_get_field(main_type, "screenWidth");
    g_main_screen_height = patchlib_type_get_field(main_type, "screenHeight");
    g_main_player_inventory = patchlib_type_get_field(main_type, "playerInventory");
    g_main_sprite_batch = patchlib_type_get_field(main_type, "spriteBatch");

    g_utils_draw_border_string = patchlib_type_get_method_by_param_count(
        utils_type,
        "DrawBorderString",
        8
    );

    g_sprite_batch_begin = patchlib_type_get_method_by_param_count(
        sprite_batch_type,
        "Begin",
        0
    );

    g_sprite_batch_end = patchlib_type_get_method_by_param_count(
        sprite_batch_type,
        "End",
        0
    );

    /*
     * 关键修复：不再依赖任何背包/界面子绘制方法。
     * Main.Draw(GameTime) 在 Android / PC 1.4.5 都是核心绘制入口。
     */
    draw_target = patchlib_type_get_method_by_param_count(
        main_type,
        "Draw",
        1
    );

    init_labels();

    /*
     * 安装 Hook 的必要条件只保留真正与绘制有关的对象。
     * mouseX/mouseY/playerInventory 缺失时仍然允许安装，避免按钮再次彻底消失。
     */
    if (draw_target &&
        g_main_sprite_batch &&
        g_utils_draw_border_string &&
        g_sprite_batch_begin &&
        g_sprite_batch_end) {

        g_draw_hook = patchlib_install_prepost_hook(
            draw_target,
            NULL,
            main_draw_postfix
        );
    }

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Chinese settings UI v1.2.2: %s hook=%d Draw=%p spriteBatch=%p DrawBorderString=%p Begin=%p End=%p mouse=(%p,%p,%p) inventory=%p",
        g_draw_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_draw_hook,
        draw_target,
        g_main_sprite_batch,
        g_utils_draw_border_string,
        g_sprite_batch_begin,
        g_sprite_batch_end,
        g_main_mouse_x,
        g_main_mouse_y,
        g_main_mouse_left,
        g_main_player_inventory
    );

    if (draw_target) patchlib_free(draw_target);

done:
    if (main_type) patchlib_free(main_type);
    if (utils_type) patchlib_free(utils_type);
    if (sprite_batch_type) patchlib_free(sprite_batch_type);
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
    free_handle(&g_main_screen_height);
    free_handle(&g_main_player_inventory);
    free_handle(&g_main_sprite_batch);
    free_handle(&g_utils_draw_border_string);
    free_handle(&g_sprite_batch_begin);
    free_handle(&g_sprite_batch_end);

    free_handle(&g_button_open);
    free_handle(&g_button_close);
    free_handle(&g_title);
    free_handle(&g_tip);
    free_handle(&g_restore_defaults);
    free_handle(&g_close_panel);

    for (int i = 0; i < RS_FEATURE_COUNT; ++i) {
        free_handle(&g_on_labels[i]);
        free_handle(&g_off_labels[i]);
    }

    g_panel_open = false;
    g_prev_mouse_left = false;
    g_draw_failure_logged = 0;
    g_draw_frames = 0;
}
