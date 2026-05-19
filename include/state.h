#ifndef STATE_H
#define STATE_H

#include <stddef.h>
#include <sys/types.h>

int write_current_preset(const char *state_dir, const char *name);

int read_current_preset_name(const char *state_dir, char *name, size_t name_sz);

int write_daemon_pid(const char *state_dir);

void remove_daemon_pid(const char *state_dir);

int resolve_daemon_pid(const char *state_dir, pid_t *pid_out);

#endif // STATE_H
