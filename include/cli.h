#ifndef CLI_H
#define CLI_H

void usage(const char *argv0);

int run_cli(int argc, char **argv, const char *argv0, const char *config_path, const char *state_dir);

#endif // CLI_H
