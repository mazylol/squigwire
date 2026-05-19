#ifndef CONFIG_H
#define CONFIG_H

#include "global.h"

#include <limits.h>

struct preset_entry {
    char name[PRESET_NAME_MAX];
    char filters_path[PATH_MAX];
};

struct config_file {
    struct preset_entry *presets;
    u32 n_presets;
    char default_preset[PRESET_NAME_MAX];
};

void free_config(struct config_file *cfg);

int parse_config_file(const char *config_path, struct config_file *cfg);

const struct preset_entry *find_preset(const struct config_file *cfg, const char *name);

#endif // CONFIG_H
