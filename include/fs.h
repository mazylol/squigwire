#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

// Somewhat generic file ops

int expand_path(const char *input, char *output, size_t output_size);

int get_state_dir(char *output, size_t output_size);

int mkdir_p(const char *dir);

int path_in_state(const char *state_dir, const char *name, char *out, size_t out_sz);

int write_text_file(const char *path, const char *text);

int read_first_line(const char *path, char *buf, size_t buf_size);

#endif // FS_H
