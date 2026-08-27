#ifndef RESOURCE_SAVER_CONFIG_H
#define RESOURCE_SAVER_CONFIG_H

#include <stdbool.h>

typedef enum rs_feature_t {
    RS_FEATURE_MASTER = 0,
    RS_FEATURE_REGULAR_AMMO,
    RS_FEATURE_SPECIAL_AMMO,
    RS_FEATURE_MAGIC_MANA,
    RS_FEATURE_SUMMON_MANA,
    RS_FEATURE_MANA_REGEN,
    RS_FEATURE_MELEE_POTION,
    RS_FEATURE_COMBAT_BUFF,
    RS_FEATURE_BAIT,
    RS_FEATURE_COUNT
} rs_feature_t;

typedef struct resource_saver_config_t {
    bool master_enabled;
    bool regular_ammo;
    bool special_ammo;
    bool magic_mana;
    bool summon_mana;
    bool mana_regen;
    bool melee_potion;
    bool combat_buff;
    bool bait;
} resource_saver_config_t;

void resource_saver_config_init(const char* private_dir);
void resource_saver_config_cleanup(void);
const resource_saver_config_t* resource_saver_config_get(void);
bool resource_saver_feature_enabled(rs_feature_t feature);
bool resource_saver_feature_raw_enabled(rs_feature_t feature);
void resource_saver_config_toggle(rs_feature_t feature);
void resource_saver_config_restore_defaults(void);
void resource_saver_config_save(void);

#endif
