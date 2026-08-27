#include <stddef.h>

#include "mod_core.h"
#include "mod_logger.h"
#include "config.h"

void (*mod_logger_write)(
    mod_log_level_t level,
    const char* tag,
    const char* fmt,
    ...
) = NULL;

void resource_saver_combat_init(void);
void resource_saver_combat_cleanup(void);
void resource_saver_buffs_init(void);
void resource_saver_buffs_cleanup(void);
void resource_saver_fishing_init(void);
void resource_saver_fishing_cleanup(void);
void resource_saver_settings_ui_init(void);
void resource_saver_settings_ui_cleanup(void);

static void init_mod(kernel_mod_handle_t* handle) {
    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Resource Saver v1.2.4 initializing (v1.1.0 core, safe UI diagnostic, author: liuxin)"
    );

    resource_saver_config_init(handle ? handle->private_dir : NULL);

    /* 功能实现保持 v1.1.0 为基线，仅增加配置开关判断。 */
    resource_saver_combat_init();
    resource_saver_buffs_init();
    resource_saver_fishing_init();

    /* Android 中文独立设置按钮：挂在背包绘制界面。 */
    resource_saver_settings_ui_init();

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Resource Saver v1.2.4 initialized"
    );
}

static void cleanup_mod(kernel_mod_handle_t* handle) {
    (void)handle;

    resource_saver_settings_ui_cleanup();
    resource_saver_fishing_cleanup();
    resource_saver_buffs_cleanup();
    resource_saver_combat_cleanup();
    resource_saver_config_cleanup();

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Resource Saver v1.2.4 unloaded"
    );
}

static kernel_mod_info_t g_info = {
    .pkg_id = "celso.resourcesaver",
    .version_code = 202608281,
    .api_version = 1,
    .version = "1.2.4"
};

static kernel_mod_info_t* get_info(void) {
    return &g_info;
}

static kernel_mod_ops_t g_ops = {
    init_mod,
    cleanup_mod,
    get_info
};

kernel_mod_ops_t* create_kernel_mod(void) {
    return &g_ops;
}
