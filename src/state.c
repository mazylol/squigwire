#include "include/state.h"
#include "include/fs.h"
#include "include/util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int _current_preset_path(const char *state_dir, char *out, size_t out_sz) {
    return path_in_state(state_dir, "current_preset", out, out_sz);
}

static int _daemon_pid_path(const char *state_dir, char *out, size_t out_sz) {
    return path_in_state(state_dir, "daemon.pid", out, out_sz);
}

int write_current_preset(const char *state_dir, const char *name) {
    char path[PATH_MAX];
    int rc = _current_preset_path(state_dir, path, sizeof(path));
    if (rc < 0)
        return rc;
    return write_text_file(path, name);
}

int read_current_preset_name(const char *state_dir, char *name, size_t name_sz) {
    char path[PATH_MAX];
    int rc = _current_preset_path(state_dir, path, sizeof(path));
    if (rc < 0)
        return rc;
    rc = read_first_line(path, name, name_sz);
    if (rc < 0)
        return rc;
    if (strlen(name) >= name_sz)
        return -ENAMETOOLONG;
    return 0;
}

int write_daemon_pid(const char *state_dir) {
    char path[PATH_MAX];
    char pid_buf[64];
    int rc = _daemon_pid_path(state_dir, path, sizeof(path));
    if (rc < 0)
        return rc;
    snprintf(pid_buf, sizeof(pid_buf), "%ld", (long)getpid());
    return write_text_file(path, pid_buf);
}

void remove_daemon_pid(const char *state_dir) {
    char path[PATH_MAX];
    if (_daemon_pid_path(state_dir, path, sizeof(path)) == 0)
        unlink(path);
}

static int _read_daemon_pid_from_state(const char *state_dir, pid_t *pid_out) {
    char path[PATH_MAX];
    char line[64];
    long v;

    if (path_in_state(state_dir, "daemon.pid", path, sizeof(path)) < 0)
        return -EINVAL;
    if (read_first_line(path, line, sizeof(line)) < 0)
        return -ENOENT;
    v = strtol(line, NULL, 10);
    if (v <= 0)
        return -EINVAL;
    *pid_out = (pid_t)v;
    return 0;
}

static int _read_daemon_pid_from_systemd(pid_t *pid_out) {
    FILE *p = popen("systemctl --user show -p MainPID --value squigwire.service 2>/dev/null", "r");
    char line[64];
    long v;

    if (p == NULL)
        return -errno;
    if (fgets(line, sizeof(line), p) == NULL) {
        pclose(p);
        return -ENOENT;
    }
    pclose(p);

    v = strtol(trim(line), NULL, 10);
    if (v <= 0)
        return -ENOENT;
    *pid_out = (pid_t)v;
    return 0;
}

int resolve_daemon_pid(const char *state_dir, pid_t *pid_out) {
    int rc = _read_daemon_pid_from_state(state_dir, pid_out);
    if (rc < 0)
        rc = _read_daemon_pid_from_systemd(pid_out);
    return rc;
}
