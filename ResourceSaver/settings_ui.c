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
 * Resource Saver v1.2.3 - 中文游戏内设置面板
 *
 * 关键修复：
 * MonoGame 的 SpriteBatch.Begin 在 C# 中可以写成 Begin()，但 IL/IL2CPP 元数据
 * 实际是一个 7 参数方法（参数只是 optional），所以按 0 参数寻找 Begin 会失败。
 * v1.2.2 因此没有安装 UI Hook。
 *
 * v1.2.3 不再自己 Begin/End，也不再依赖 Main.Draw。
 * 改为 Hook SpriteBatch.End() 的 Prefix：此时 SpriteBatch 已经处于活动绘制批次中，
 * 我们直接调用 Terraria.Utils.DrawBorderString 把中文按钮/面板加入当前批次，
 * 然后返回 false，让原版 End() 正常继续执行。
 *
 * 优先只在 Main.spriteBatch 上绘制。如果该字段在某个 Android 构建不可访问，
 * 自动退化为在可用 SpriteBatch.End 前尝试绘制，以保证不会因单个字段缺失而
 * 整个 UI Hook 都无法安装。
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
static patch_handle_t g_sprite_batch_end = PATCH_NULL;

static patch_hook_id_t g_end_hook = PATCH_HOOK_INVALID_ID;

static bool g_panel_open = false;
static bool g_prev_mouse_left = false;
static int g_draw_failure_logged = 0;
static int g_missing_mouse_logged = 0;
static unsigned long long g_end_calls = 0;
static unsigned long long g_draw_calls = 0;

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
    g_button_open = create_text("【资源节省设置】 点击打开");
    g_button_close = create_text("【资源节省设置】 点击收起");
    g_title = create_text("—— 精打细算：资源节省设置 ——");
    g_tip = create_text("点击项目独立开启/关闭，设置自动保存");
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
    if (g_main_player_inventory) {
        bool open = false;
        patchlib_field_get_value(g_main_player_inventory, NULL, &open);
        return open ? 1 : 0;
    }

    /* 找不到 playerInventory 时常驻显示，避免 UI 再次完全不可见。 */
    return 1;
}

static int is_main_sprite_batch(patch_handle_t instance) {
    if (!instance) return 0;

    if (g_main_sprite_batch) {
        patch_handle_t main_batch = PATCH_NULL;
        patchlib_field_get_value(g_main_sprite_batch, NULL, &main_batch);

        if (main_batch) {
            return main_batch == instance ? 1 : 0;
        }
    }

    /* 无法取得 Main.spriteBatch 时不阻断 UI，允许回退绘制。 */
    return 1;
}

static void draw_settings_overlay(patch_handle_t sprite_batch) {
    if (!sprite_batch) return;
    if (!should_show_entry()) {
        g_prev_mouse_left = false;
        return;
    }

    ++g_draw_calls;

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

    if (!g_main_mouse_left && !g_missing_mouse_logged) {
        g_missing_mouse_logged = 1;
        mod_logger_write(
            MOD_LOG_LEVEL_WARNING,
            "ResourceSaver",
            "Chinese UI v1.2.3: Main.mouseLeft not found; panel can draw but touch toggle may be unavailable"
        );
    }

    if (screen_width < 480) screen_width = 480;
    if (screen_height < 320) screen_height = 320;

    bool click = mouse_left && !g_prev_mouse_left;

    /*
     * 手机横屏优先：固定右上方。面板宽度不会超过屏幕减去两侧边距。
     */
    int width = 440;
    if (width > screen_width - 40) width = screen_width - 40;
    const int row_h = 36;
    int x = screen_width - width - 24;
    if (x < 20) x = 20;
    int top = 90;
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
        0.90f
    );

    if (!draw_ok && !g_draw_failure_logged) {
        g_draw_failure_logged = 1;
        mod_logger_write(
            MOD_LOG_LEVEL_WARNING,
            "ResourceSaver",
            "Chinese UI v1.2.3: DrawBorderString invocation failed inside SpriteBatch.End prefix"
        );
    }

    if (click && button_hover) {
        g_panel_open = !g_panel_open;
        click = false;
    }

    if (g_panel_open) {
        int y = top + row_h + 8;

        draw_text(sprite_batch, g_title, (float)x, (float)y, yellow, 0.80f);
        y += row_h;
        draw_text(sprite_batch, g_tip, (float)x, (float)y, gray, 0.66f);
        y += row_h + 2;

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
                0.72f
            );

            if (click && hover) {
                resource_saver_config_toggle((rs_feature_t)i);
                click = false;
            }

            y += row_h;
        }

        y += 4;
        int defaults_hover = point_in_rect(mouse_x, mouse_y, x, y, width, row_h);
        draw_text(
            sprite_batch,
            g_restore_defaults,
            (float)x,
            (float)y,
            defaults_hover ? yellow : white,
            0.74f
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
            0.74f
        );
        if (click && close_hover) {
            g_panel_open = false;
            click = false;
        }
    }

    g_prev_mouse_left = mouse_left;
}

/*
 * SpriteBatch.End 的 Prefix。
 * 此时 Begin 已经由 Terraria 原版调用，SpriteBatch 仍处于活动状态。
 * 这里绘制后返回 false，保持项目中已验证的 TEFKernel Prefix 语义：
 * false = 继续执行原版方法。
 */
static bool sprite_batch_end_prefix(
    patch_handle_t instance,
    void** args,
    const patch_method_signature_t* sig,
    void* result
) {
    (void)args;
    (void)sig;
    (void)result;

    ++g_end_calls;

    if (!instance) return false;
    if (!is_main_sprite_batch(instance)) return false;

    draw_settings_overlay(instance);
    return false;
}

void resource_saver_settings_ui_init(void) {
    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t utils_type = patchlib_type_get_type("Terraria", "Utils");
    patch_handle_t sprite_batch_type = patchlib_type_get_type(
        "Microsoft.Xna.Framework.Graphics",
        "SpriteBatch"
    );

    if (!main_type || !utils_type || !sprite_batch_type) {
        mod_logger_write(
            MOD_LOG_LEVEL_ERROR,
            "ResourceSaver",
            "Chinese UI v1.2.3 init failed: Main=%p Utils=%p SpriteBatch=%p",
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

    /* End() 在 MonoGame 元数据中是真正的 0 参数实例方法。 */
    g_sprite_batch_end = patchlib_type_get_method_by_param_count(
        sprite_batch_type,
        "End",
        0
    );

    init_labels();

    /*
     * v1.2.3 的安装条件只需要：End + DrawBorderString。
     * Main.spriteBatch / 鼠标 / playerInventory 缺失都不会再阻止 Hook 安装。
     */
    if (g_sprite_batch_end && g_utils_draw_border_string) {
        g_end_hook = patchlib_install_prepost_hook(
            g_sprite_batch_end,
            sprite_batch_end_prefix,
            NULL
        );
    }

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Chinese settings UI v1.2.3: %s hook=%d End=%p DrawBorderString=%p spriteBatchField=%p mouse=(%p,%p,%p) inventory=%p",
        g_end_hook == PATCH_HOOK_INVALID_ID ? "failed" : "ready",
        (int)g_end_hook,
        g_sprite_batch_end,
        g_utils_draw_border_string,
        g_main_sprite_batch,
        g_main_mouse_x,
        g_main_mouse_y,
        g_main_mouse_left,
        g_main_player_inventory
    );

done:
    if (main_type) patchlib_free(main_type);
    if (utils_type) patchlib_free(utils_type);
    if (sprite_batch_type) patchlib_free(sprite_batch_type);
}

void resource_saver_settings_ui_cleanup(void) {
    if (g_end_hook != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_end_hook);
        g_end_hook = PATCH_HOOK_INVALID_ID;
    }

    free_handle(&g_main_mouse_x);
    free_handle(&g_main_mouse_y);
    free_handle(&g_main_mouse_left);
    free_handle(&g_main_screen_width);
    free_handle(&g_main_screen_height);
    free_handle(&g_main_player_inventory);
    free_handle(&g_main_sprite_batch);
    free_handle(&g_utils_draw_border_string);
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
    g_missing_mouse_logged = 0;
    g_end_calls = 0;
    g_draw_calls = 0;
}
