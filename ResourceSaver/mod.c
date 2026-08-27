#include <stddef.h>

#include "mod_core.h"
#include "mod_logger.h"

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

static void init_mod(kernel_mod_handle_t* handle) {
    (void)handle;

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Resource Saver v1.1.1 initializing (author: liuxin)"
    );

    resource_saver_combat_init();
    resource_saver_buffs_init();
    resource_saver_fishing_init();

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Resource Saver v1.1.1 initialized"
    );
}

static void cleanup_mod(kernel_mod_handle_t* handle) {
    (void)handle;

    resource_saver_fishing_cleanup();
    resource_saver_buffs_cleanup();
    resource_saver_combat_cleanup();

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Resource Saver v1.1.1 unloaded"
    );
}

static kernel_mod_info_t g_info = {
    .pkg_id = "celso.resourcesaver",
    .version_code = 202608276,
    .api_version = 1,
    .version = "1.1.1"
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
