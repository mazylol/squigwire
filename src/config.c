#include "include/config.h"
#include "include/fs.h"
#include "include/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void free_config(struct config_file *cfg) {
    free(cfg->presets);
    cfg->presets = NULL;
    cfg->n_presets = 0;
    cfg->default_preset[0] = '\0';
}

static int _append_preset(struct config_file *cfg, const char *name, const char *path_raw) {
    struct preset_entry *new_presets;
    char expanded[PATH_MAX] = { 0 };
    int rc;

    if (name == NULL || name[0] == '\0' || strlen(name) >= PRESET_NAME_MAX)
        return -EINVAL;

    rc = expand_path(path_raw, expanded, sizeof(expanded));
    if (rc < 0)
        return rc;

    for (u32 i = 0; i < cfg->n_presets; ++i) {
        if (strcmp(cfg->presets[i].name, name) == 0) {
            if (snprintf(cfg->presets[i].filters_path, sizeof(cfg->presets[i].filters_path), "%s", expanded) >=
                (int)sizeof(cfg->presets[i].filters_path))
                return -ENAMETOOLONG;
            return 0;
        }
    }

    new_presets = realloc(cfg->presets, (size_t)(cfg->n_presets + 1) * sizeof(*new_presets));
    if (new_presets == NULL)
        return -ENOMEM;
    cfg->presets = new_presets;

    memset(&cfg->presets[cfg->n_presets], 0, sizeof(cfg->presets[cfg->n_presets]));
    if (snprintf(cfg->presets[cfg->n_presets].name, sizeof(cfg->presets[cfg->n_presets].name), "%s", name) >=
        (int)sizeof(cfg->presets[cfg->n_presets].name))
        return -ENAMETOOLONG;
    if (snprintf(cfg->presets[cfg->n_presets].filters_path, sizeof(cfg->presets[cfg->n_presets].filters_path), "%s",
                 expanded) >= (int)sizeof(cfg->presets[cfg->n_presets].filters_path))
        return -ENAMETOOLONG;

    cfg->n_presets++;
    return 0;
}

int parse_config_file(const char *config_path, struct config_file *cfg) {
    FILE *f = fopen(config_path, "r");
    char line[2048];

    if (f == NULL) {
        return -errno;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char *key = trim(line);
        char *value;
        char *eq;

        if (*key == '\0' || *key == '#')
            continue;

        eq = strchr(key, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);

        if (strcmp(key, "default_preset") == 0) {
            if (value[0] != '\0' &&
                snprintf(cfg->default_preset, sizeof(cfg->default_preset), "%s", value) >=
                        (int)sizeof(cfg->default_preset)) {
                fclose(f);
                return -ENAMETOOLONG;
            }
            continue;
        }

        if (strcmp(key, "filters_file") == 0 || strcmp(key, "eq_file") == 0) {
            int rc = _append_preset(cfg, "default", value);
            if (rc < 0) {
                fclose(f);
                return rc;
            }
            if (cfg->default_preset[0] == '\0')
                snprintf(cfg->default_preset, sizeof(cfg->default_preset), "%s", "default");
            continue;
        }

        if (strncmp(key, "preset.", 7) == 0) {
            int rc = _append_preset(cfg, key + 7, value);
            if (rc < 0) {
                fclose(f);
                return rc;
            }
        }
    }

    fclose(f);

    if (cfg->n_presets == 0)
        return -EINVAL;
    if (cfg->default_preset[0] == '\0')
        snprintf(cfg->default_preset, sizeof(cfg->default_preset), "%s", cfg->presets[0].name);
    return 0;
}

const struct preset_entry *find_preset(const struct config_file *cfg, const char *name) {
    for (u32 i = 0; i < cfg->n_presets; ++i) {
        if (strcmp(cfg->presets[i].name, name) == 0)
            return &cfg->presets[i];
    }
    return NULL;
}
