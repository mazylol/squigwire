#include "include/cli.h"
#include "include/config.h"
#include "include/state.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s --daemon [--config PATH]\n"
            "  %s preset list [--config PATH]\n"
            "  %s preset current [--config PATH]\n"
            "  %s preset set <name> [--config PATH]\n"
            "  %s preset reload\n",
            argv0, argv0, argv0, argv0, argv0);
}

static int _cli_preset_list(const char *config_path, const char *state_dir) {
    struct config_file cfg = { 0 };
    char current[PRESET_NAME_MAX] = { 0 };
    int rc = parse_config_file(config_path, &cfg);
    if (rc < 0) {
        fprintf(stderr, "failed to parse config '%s': %s\n", config_path, strerror(-rc));
        return 1;
    }

    if (read_current_preset_name(state_dir, current, sizeof(current)) < 0)
        current[0] = '\0';

    for (u32 i = 0; i < cfg.n_presets; ++i) {
        bool is_default = strcmp(cfg.default_preset, cfg.presets[i].name) == 0;
        bool is_current = current[0] != '\0' && strcmp(current, cfg.presets[i].name) == 0;
        printf("%c %s%s%s -> %s\n", is_current ? '*' : ' ', cfg.presets[i].name, is_default ? " (default)" : "",
               is_current ? " (current)" : "", cfg.presets[i].filters_path);
    }
    free_config(&cfg);
    return 0;
}

static int _cli_preset_current(const char *config_path, const char *state_dir) {
    struct config_file cfg = { 0 };
    char current[PRESET_NAME_MAX] = { 0 };
    int rc = parse_config_file(config_path, &cfg);
    if (rc < 0) {
        fprintf(stderr, "failed to parse config '%s': %s\n", config_path, strerror(-rc));
        return 1;
    }
    if (read_current_preset_name(state_dir, current, sizeof(current)) < 0 || find_preset(&cfg, current) == NULL) {
        snprintf(current, sizeof(current), "%s", cfg.default_preset);
    }
    printf("%s\n", current);
    free_config(&cfg);
    return 0;
}

static int _cli_preset_set(const char *config_path, const char *state_dir, const char *name) {
    struct config_file cfg = { 0 };
    pid_t pid;
    int rc = parse_config_file(config_path, &cfg);
    if (rc < 0) {
        fprintf(stderr, "failed to parse config '%s': %s\n", config_path, strerror(-rc));
        return 1;
    }
    if (find_preset(&cfg, name) == NULL) {
        fprintf(stderr, "preset '%s' not found\n", name);
        free_config(&cfg);
        return 1;
    }
    free_config(&cfg);

    if (write_current_preset(state_dir, name) < 0) {
        fprintf(stderr, "failed to persist current preset\n");
        return 1;
    }

    rc = resolve_daemon_pid(state_dir, &pid);
    if (rc < 0) {
        printf("preset set to '%s' (daemon not running)\n", name);
        return 0;
    }
    if (kill(pid, SIGUSR1) < 0) {
        printf("preset set to '%s' (daemon signal failed)\n", name);
        return 0;
    }
    printf("preset set to '%s' and reload signaled\n", name);
    return 0;
}

static int _cli_preset_reload(const char *state_dir) {
    pid_t pid;
    int rc = resolve_daemon_pid(state_dir, &pid);
    if (rc < 0) {
        fprintf(stderr, "daemon not running\n");
        return 1;
    }
    if (kill(pid, SIGUSR1) < 0) {
        fprintf(stderr, "failed to signal daemon: %s\n", strerror(errno));
        return 1;
    }
    printf("reload signaled\n");
    return 0;
}

int run_cli(int argc, char **argv, const char *argv0, const char *config_path, const char *state_dir) {
    if (argc >= 3 && strcmp(argv[1], "preset") == 0) {
        if (strcmp(argv[2], "list") == 0)
            return _cli_preset_list(config_path, state_dir);
        if (strcmp(argv[2], "current") == 0)
            return _cli_preset_current(config_path, state_dir);
        if (strcmp(argv[2], "set") == 0 && argc >= 4)
            return _cli_preset_set(config_path, state_dir, argv[3]);
        if (strcmp(argv[2], "reload") == 0)
            return _cli_preset_reload(state_dir);
    }

    usage(argv0);
    return 1;
}
