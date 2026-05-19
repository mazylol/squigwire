#include "include/fs.h"
#include "include/global.h"
#include "include/util.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int expand_path(const char *input, char *output, size_t output_size) {
    if (input == NULL || input[0] == '\0')
        return -EINVAL;

    if (input[0] == '~' && input[1] == '/') {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return -EINVAL;
        if (snprintf(output, output_size, "%s/%s", home, input + 2) >= (int)output_size)
            return -ENAMETOOLONG;
        return 0;
    }

    if (snprintf(output, output_size, "%s", input) >= (int)output_size)
        return -ENAMETOOLONG;
    return 0;
}

int get_state_dir(char *output, size_t output_size) {
    const char *base = getenv("XDG_STATE_HOME");
    if (base == NULL || base[0] == '\0') {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return -EINVAL;
        if (snprintf(output, output_size, "%s/.local/state%s", home, DEFAULT_STATE_SUBDIR) >= (int)output_size)
            return -ENAMETOOLONG;
        return 0;
    }
    if (snprintf(output, output_size, "%s%s", base, DEFAULT_STATE_SUBDIR) >= (int)output_size)
        return -ENAMETOOLONG;
    return 0;
}

int mkdir_p(const char *dir) {
    char buf[PATH_MAX];
    size_t len;

    if (snprintf(buf, sizeof(buf), "%s", dir) >= (int)sizeof(buf))
        return -ENAMETOOLONG;
    len = strlen(buf);
    if (len == 0)
        return -EINVAL;

    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) < 0 && errno != EEXIST)
                return -errno;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) < 0 && errno != EEXIST)
        return -errno;
    return 0;
}

int path_in_state(const char *state_dir, const char *name, char *out, size_t out_sz) {
    if (snprintf(out, out_sz, "%s/%s", state_dir, name) >= (int)out_sz)
        return -ENAMETOOLONG;
    return 0;
}

int write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -errno;
    if (fputs(text, f) < 0 || fputc('\n', f) == EOF) {
        int err = errno;
        fclose(f);
        return -err;
    }
    if (fclose(f) != 0)
        return -errno;
    return 0;
}

int read_first_line(const char *path, char *buf, size_t buf_size) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -errno;
    if (fgets(buf, (int)buf_size, f) == NULL) {
        int err = ferror(f) ? errno : ENODATA;
        fclose(f);
        return -err;
    }
    fclose(f);
    char *t = trim(buf);
    if (t != buf)
        memmove(buf, t, strlen(t) + 1);
    if (buf[0] == '\0')
        return -ENODATA;
    return 0;
}
