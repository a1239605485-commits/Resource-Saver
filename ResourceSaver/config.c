#include "config.h"

#include <stdio.h>
#include <string.h>

#include "mod_logger.h"

static resource_saver_config_t g_config;
static char g_config_path[1024];

static void set_defaults(void) {
    g_config.master_enabled = true;
    g_config.regular_ammo = true;
    g_config.special_ammo = true;
    g_config.magic_mana = true;
    g_config.summon_mana = true;
    g_config.mana_regen = true;
    g_config.melee_potion = true;
    g_config.combat_buff = true;
    g_config.bait = true;
}

static bool parse_bool(const char* value, bool fallback) {
    if (!value) return fallback;
    while (*value == ' ' || *value == '\t') ++value;

    if (!strncmp(value, "1", 1) || !strncmp(value, "true", 4) || !strncmp(value, "on", 2))
        return true;
    if (!strncmp(value, "0", 1) || !strncmp(value, "false", 5) || !strncmp(value, "off", 3))
        return false;
    return fallback;
}

static void apply_entry(const char* key, const char* value) {
    if (!strcmp(key, "master")) g_config.master_enabled = parse_bool(value, g_config.master_enabled);
    else if (!strcmp(key, "regular_ammo")) g_config.regular_ammo = parse_bool(value, g_config.regular_ammo);
    else if (!strcmp(key, "special_ammo")) g_config.special_ammo = parse_bool(value, g_config.special_ammo);
    else if (!strcmp(key, "magic_mana")) g_config.magic_mana = parse_bool(value, g_config.magic_mana);
    else if (!strcmp(key, "summon_mana")) g_config.summon_mana = parse_bool(value, g_config.summon_mana);
    else if (!strcmp(key, "mana_regen")) g_config.mana_regen = parse_bool(value, g_config.mana_regen);
    else if (!strcmp(key, "melee_potion")) g_config.melee_potion = parse_bool(value, g_config.melee_potion);
    else if (!strcmp(key, "combat_buff")) g_config.combat_buff = parse_bool(value, g_config.combat_buff);
    else if (!strcmp(key, "bait")) g_config.bait = parse_bool(value, g_config.bait);
}

void resource_saver_config_save(void) {
    if (!g_config_path[0]) return;

    FILE* fp = fopen(g_config_path, "w");
    if (!fp) {
        mod_logger_write(MOD_LOG_LEVEL_WARNING, "ResourceSaver",
                         "Could not save config: %s", g_config_path);
        return;
    }

    fprintf(fp,
            "# Resource Saver v1.2.6\n"
            "# 1 = enabled, 0 = disabled\n"
            "master=%d\n"
            "regular_ammo=%d\n"
            "special_ammo=%d\n"
            "magic_mana=%d\n"
            "summon_mana=%d\n"
            "mana_regen=%d\n"
            "melee_potion=%d\n"
            "combat_buff=%d\n"
            "bait=%d\n",
            g_config.master_enabled ? 1 : 0,
            g_config.regular_ammo ? 1 : 0,
            g_config.special_ammo ? 1 : 0,
            g_config.magic_mana ? 1 : 0,
            g_config.summon_mana ? 1 : 0,
            g_config.mana_regen ? 1 : 0,
            g_config.melee_potion ? 1 : 0,
            g_config.combat_buff ? 1 : 0,
            g_config.bait ? 1 : 0);

    fclose(fp);
}

void resource_saver_config_init(const char* private_dir) {
    set_defaults();
    g_config_path[0] = '\0';

    if (private_dir && private_dir[0]) {
        size_t len = strlen(private_dir);
        const char* separator = (len > 0 && private_dir[len - 1] == '/') ? "" : "/";
        snprintf(g_config_path, sizeof(g_config_path), "%s%sconfig.ini", private_dir, separator);
    }
    else {
        snprintf(g_config_path, sizeof(g_config_path), "config.ini");
    }

    FILE* fp = fopen(g_config_path, "r");
    if (!fp) {
        resource_saver_config_save();
        mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                         "Config created with defaults: %s", g_config_path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\r' || *p == '\n')
            continue;

        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = p;
        char* value = eq + 1;

        char* key_end = key + strlen(key);
        while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) *--key_end = '\0';
        char* value_end = value + strlen(value);
        while (value_end > value && (value_end[-1] == '\r' || value_end[-1] == '\n' ||
                                     value_end[-1] == ' ' || value_end[-1] == '\t')) *--value_end = '\0';

        apply_entry(key, value);
    }
    fclose(fp);

    mod_logger_write(
        MOD_LOG_LEVEL_INFO,
        "ResourceSaver",
        "Config loaded: master=%d ammo=%d special=%d magic=%d summon=%d regen=%d meleePotion=%d buff=%d bait=%d",
        g_config.master_enabled ? 1 : 0,
        g_config.regular_ammo ? 1 : 0,
        g_config.special_ammo ? 1 : 0,
        g_config.magic_mana ? 1 : 0,
        g_config.summon_mana ? 1 : 0,
        g_config.mana_regen ? 1 : 0,
        g_config.melee_potion ? 1 : 0,
        g_config.combat_buff ? 1 : 0,
        g_config.bait ? 1 : 0);
}

void resource_saver_config_cleanup(void) {
    resource_saver_config_save();
}

const resource_saver_config_t* resource_saver_config_get(void) {
    return &g_config;
}

bool resource_saver_feature_raw_enabled(rs_feature_t feature) {
    switch (feature) {
        case RS_FEATURE_MASTER: return g_config.master_enabled;
        case RS_FEATURE_REGULAR_AMMO: return g_config.regular_ammo;
        case RS_FEATURE_SPECIAL_AMMO: return g_config.special_ammo;
        case RS_FEATURE_MAGIC_MANA: return g_config.magic_mana;
        case RS_FEATURE_SUMMON_MANA: return g_config.summon_mana;
        case RS_FEATURE_MANA_REGEN: return g_config.mana_regen;
        case RS_FEATURE_MELEE_POTION: return g_config.melee_potion;
        case RS_FEATURE_COMBAT_BUFF: return g_config.combat_buff;
        case RS_FEATURE_BAIT: return g_config.bait;
        default: return false;
    }
}

bool resource_saver_feature_enabled(rs_feature_t feature) {
    if (feature == RS_FEATURE_MASTER) return g_config.master_enabled;
    return g_config.master_enabled && resource_saver_feature_raw_enabled(feature);
}

void resource_saver_config_toggle(rs_feature_t feature) {
    switch (feature) {
        case RS_FEATURE_MASTER: g_config.master_enabled = !g_config.master_enabled; break;
        case RS_FEATURE_REGULAR_AMMO: g_config.regular_ammo = !g_config.regular_ammo; break;
        case RS_FEATURE_SPECIAL_AMMO: g_config.special_ammo = !g_config.special_ammo; break;
        case RS_FEATURE_MAGIC_MANA: g_config.magic_mana = !g_config.magic_mana; break;
        case RS_FEATURE_SUMMON_MANA: g_config.summon_mana = !g_config.summon_mana; break;
        case RS_FEATURE_MANA_REGEN: g_config.mana_regen = !g_config.mana_regen; break;
        case RS_FEATURE_MELEE_POTION: g_config.melee_potion = !g_config.melee_potion; break;
        case RS_FEATURE_COMBAT_BUFF: g_config.combat_buff = !g_config.combat_buff; break;
        case RS_FEATURE_BAIT: g_config.bait = !g_config.bait; break;
        default: return;
    }

    resource_saver_config_save();
    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver",
                     "Setting changed: feature=%d rawEnabled=%d master=%d",
                     (int)feature,
                     resource_saver_feature_raw_enabled(feature) ? 1 : 0,
                     g_config.master_enabled ? 1 : 0);
}

void resource_saver_config_restore_defaults(void) {
    set_defaults();
    resource_saver_config_save();
    mod_logger_write(MOD_LOG_LEVEL_INFO, "ResourceSaver", "Settings restored to defaults");
}
