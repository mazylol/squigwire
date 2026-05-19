#include "include/cli.h"
#include "include/daemon.h"
#include "include/fs.h"
#include "include/global.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    char config_path[PATH_MAX] = { 0 };
    char state_dir[PATH_MAX] = { 0 };
    bool daemon_mode = false;
    int i = 1;

    if (expand_path(DEFAULT_CONFIG_PATH, config_path, sizeof(config_path)) < 0) {
        fprintf(stderr, "invalid default config path\n");
        return 1;
    }
    if (get_state_dir(state_dir, sizeof(state_dir)) < 0 || mkdir_p(state_dir) < 0) {
        fprintf(stderr, "failed to initialize state dir\n");
        return 1;
    }

    while (i < argc) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            if (expand_path(argv[i + 1], config_path, sizeof(config_path)) < 0) {
                fprintf(stderr, "invalid config path\n");
                return 1;
            }
            for (int j = i; j + 2 <= argc; ++j)
                argv[j] = argv[j + 2];
            argc -= 2;
            continue;
        }
        ++i;
    }

    if (argc == 1 || (argc >= 2 && strcmp(argv[1], "--daemon") == 0))
        daemon_mode = true;

    if (daemon_mode)
        return run_daemon(config_path);

    return run_cli(argc, argv, argv[0], config_path, state_dir);
}
