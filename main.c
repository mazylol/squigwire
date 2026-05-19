#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#define MAX_CHANNELS 8
#define DEFAULT_SAMPLE_RATE 48000.0f
#define DEFAULT_CONFIG_PATH "~/.config/squigwire/squigwire.conf"
#define DEFAULT_STATE_SUBDIR "/squigwire"
#define PRESET_NAME_MAX 128

typedef uint32_t u32;

struct port {
    void *unused;
};

struct biquad {
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;
};

struct peq_band {
    float freq_hz;
    float q;
    float gain_db;
};

struct preset_entry {
    char name[PRESET_NAME_MAX];
    char filters_path[PATH_MAX];
};

struct config_file {
    struct preset_entry *presets;
    u32 n_presets;
    char default_preset[PRESET_NAME_MAX];
};

struct data {
    struct pw_main_loop *loop;
    struct pw_filter *filter;
    struct port *in_ports[MAX_CHANNELS];
    struct port *out_ports[MAX_CHANNELS];
    struct biquad *eq;
    struct peq_band *bands;
    float preamp;
    float sample_rate;
    bool have_graph_rate;
    bool reload_requested;
    u32 n_channels;
    u32 n_bands;
    char config_path[PATH_MAX];
    char state_dir[PATH_MAX];
    char current_preset[PRESET_NAME_MAX];
};

static char *trim(char *s) {
    char *end;
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int expand_path(const char *input, char *output, size_t output_size) {
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

static int get_state_dir(char *output, size_t output_size) {
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

static int mkdir_p(const char *dir) {
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

static int path_in_state(const char *state_dir, const char *name, char *out, size_t out_sz) {
    if (snprintf(out, out_sz, "%s/%s", state_dir, name) >= (int)out_sz)
        return -ENAMETOOLONG;
    return 0;
}

static int write_text_file(const char *path, const char *text) {
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

static int read_first_line(const char *path, char *buf, size_t buf_size) {
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

static int append_preset(struct config_file *cfg, const char *name, const char *path_raw) {
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

static void free_config(struct config_file *cfg) {
    free(cfg->presets);
    cfg->presets = NULL;
    cfg->n_presets = 0;
    cfg->default_preset[0] = '\0';
}

static int parse_config_file(const char *config_path, struct config_file *cfg) {
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
            int rc = append_preset(cfg, "default", value);
            if (rc < 0) {
                fclose(f);
                return rc;
            }
            if (cfg->default_preset[0] == '\0')
                snprintf(cfg->default_preset, sizeof(cfg->default_preset), "%s", "default");
            continue;
        }

        if (strncmp(key, "preset.", 7) == 0) {
            int rc = append_preset(cfg, key + 7, value);
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

static const struct preset_entry *find_preset(const struct config_file *cfg, const char *name) {
    for (u32 i = 0; i < cfg->n_presets; ++i) {
        if (strcmp(cfg->presets[i].name, name) == 0)
            return &cfg->presets[i];
    }
    return NULL;
}

static int append_band(struct peq_band **bands, u32 *n_bands, struct peq_band band) {
    struct peq_band *new_bands = realloc(*bands, (size_t)(*n_bands + 1) * sizeof(*new_bands));
    if (new_bands == NULL)
        return -ENOMEM;
    *bands = new_bands;
    (*bands)[*n_bands] = band;
    (*n_bands)++;
    return 0;
}

static int parse_filters_file(const char *filters_path, struct peq_band **bands, u32 *n_bands, float *preamp_db) {
    FILE *f = fopen(filters_path, "r");
    char line[2048];
    bool have_preamp = false;

    if (f == NULL)
        return -errno;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = trim(line);
        float db = 0.0f;

        if (*s == '\0' || *s == '#')
            continue;

        if (sscanf(s, "Preamp: %f dB", &db) == 1) {
            *preamp_db = db;
            have_preamp = true;
            continue;
        }

        if (strncmp(s, "Filter", 6) == 0) {
            int filter_number = 0;
            char state[8] = { 0 };
            char type[8] = { 0 };
            struct peq_band band = { 0 };

            if (sscanf(s, "Filter %d: %7s %7s Fc %f Hz Gain %f dB Q %f", &filter_number, state, type, &band.freq_hz,
                       &band.gain_db, &band.q) == 6) {
                if (strcasecmp(state, "ON") != 0 || strcasecmp(type, "PK") != 0)
                    continue;
                if (band.freq_hz <= 0.0f || band.q <= 0.0f)
                    continue;
                if (append_band(bands, n_bands, band) < 0) {
                    fclose(f);
                    return -ENOMEM;
                }
            }
        }
    }

    fclose(f);
    if (!have_preamp)
        *preamp_db = 0.0f;
    if (*n_bands == 0)
        return -EINVAL;
    return 0;
}

static void biquad_peaking(struct biquad *b, float fs, float f0, float Q, float gain_db) {
    if (fs <= 1.0f || f0 <= 0.0f || Q <= 0.0f || f0 >= fs * 0.5f) {
        b->b0 = 1.0f;
        b->b1 = 0.0f;
        b->b2 = 0.0f;
        b->a1 = 0.0f;
        b->a2 = 0.0f;
        b->x1 = b->x2 = b->y1 = b->y2 = 0.0f;
        return;
    }

    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    float cosw0 = cosf(w0);

    float b0 = 1 + alpha * A;
    float b1 = -2 * cosw0;
    float b2 = 1 - alpha * A;
    float a0 = 1 + alpha / A;
    float a1 = -2 * cosw0;
    float a2 = 1 - alpha / A;

    b->b0 = b0 / a0;
    b->b1 = b1 / a0;
    b->b2 = b2 / a0;
    b->a1 = a1 / a0;
    b->a2 = a2 / a0;
    b->x1 = b->x2 = b->y1 = b->y2 = 0.0f;
}

static struct biquad *band_state(struct data *data, u32 ch, u32 band) {
    return &data->eq[(size_t)ch * data->n_bands + band];
}

static void redesign_eq_bank(struct data *data, float sample_rate) {
    data->sample_rate = sample_rate;
    for (u32 ch = 0; ch < data->n_channels; ++ch) {
        for (u32 b = 0; b < data->n_bands; ++b) {
            struct peq_band band = data->bands[b];
            biquad_peaking(band_state(data, ch, b), data->sample_rate, band.freq_hz, band.q, band.gain_db);
        }
    }
}

static float rate_from_position(const struct spa_io_position *position) {
    u32 num = position->clock.rate.num;
    u32 denom = position->clock.rate.denom;
    float r1, r2;

    if (num == 0 || denom == 0)
        return 0.0f;

    r1 = (float)num / (float)denom;
    r2 = (float)denom / (float)num;
    if (r1 >= 8000.0f && r1 <= 768000.0f)
        return r1;
    if (r2 >= 8000.0f && r2 <= 768000.0f)
        return r2;
    return 0.0f;
}

static int current_preset_path(const struct data *data, char *out, size_t out_sz) {
    return path_in_state(data->state_dir, "current_preset", out, out_sz);
}

static int daemon_pid_path(const struct data *data, char *out, size_t out_sz) {
    return path_in_state(data->state_dir, "daemon.pid", out, out_sz);
}

static int write_current_preset(const struct data *data, const char *name) {
    char path[PATH_MAX];
    int rc = current_preset_path(data, path, sizeof(path));
    if (rc < 0)
        return rc;
    return write_text_file(path, name);
}

static int read_current_preset_name(const struct data *data, char *name, size_t name_sz) {
    char path[PATH_MAX];
    int rc = current_preset_path(data, path, sizeof(path));
    if (rc < 0)
        return rc;
    rc = read_first_line(path, name, name_sz);
    if (rc < 0)
        return rc;
    if (strlen(name) >= name_sz)
        return -ENAMETOOLONG;
    return 0;
}

static int load_preset_into_runtime(struct data *data, const char *requested_preset, bool persist_state) {
    struct config_file cfg = { 0 };
    struct peq_band *new_bands = NULL;
    struct biquad *new_eq = NULL;
    u32 new_n_bands = 0;
    float preamp_db = 0.0f;
    float sample_rate = data->have_graph_rate ? data->sample_rate : DEFAULT_SAMPLE_RATE;
    char selected[PRESET_NAME_MAX] = { 0 };
    char from_state[PRESET_NAME_MAX] = { 0 };
    const struct preset_entry *preset = NULL;
    int rc;

    rc = parse_config_file(data->config_path, &cfg);
    if (rc < 0)
        return rc;

    if (requested_preset != NULL && requested_preset[0] != '\0') {
        snprintf(selected, sizeof(selected), "%s", requested_preset);
    } else if (read_current_preset_name(data, from_state, sizeof(from_state)) == 0) {
        snprintf(selected, sizeof(selected), "%s", from_state);
    } else {
        snprintf(selected, sizeof(selected), "%s", cfg.default_preset);
    }

    preset = find_preset(&cfg, selected);
    if (preset == NULL) {
        free_config(&cfg);
        return -ENOENT;
    }

    rc = parse_filters_file(preset->filters_path, &new_bands, &new_n_bands, &preamp_db);
    if (rc < 0) {
        free(new_bands);
        free_config(&cfg);
        return rc;
    }

    new_eq = calloc((size_t)data->n_channels * new_n_bands, sizeof(*new_eq));
    if (new_eq == NULL) {
        free(new_bands);
        free_config(&cfg);
        return -ENOMEM;
    }

    free(data->bands);
    free(data->eq);
    data->bands = new_bands;
    data->eq = new_eq;
    data->n_bands = new_n_bands;
    data->preamp = powf(10.0f, preamp_db / 20.0f);
    snprintf(data->current_preset, sizeof(data->current_preset), "%s", selected);
    redesign_eq_bank(data, sample_rate);

    if (persist_state)
        write_current_preset(data, selected);

    fprintf(stderr, "active preset '%s': %u PK bands from %s (preamp %.2f dB)\n", selected, new_n_bands,
            preset->filters_path, preamp_db);

    free_config(&cfg);
    return 0;
}

static void on_process(void *userdata, struct spa_io_position *position) {
    struct data *data = userdata;
    u32 n_samples = position->clock.duration;
    float graph_rate = rate_from_position(position);
    float *in[MAX_CHANNELS] = { 0 };
    float *out[MAX_CHANNELS] = { 0 };

    if (data->reload_requested) {
        int rc = load_preset_into_runtime(data, NULL, false);
        if (rc < 0)
            fprintf(stderr, "reload failed: %s\n", strerror(-rc));
        data->reload_requested = false;
    }

    if (graph_rate > 0.0f) {
        if (!data->have_graph_rate) {
            redesign_eq_bank(data, graph_rate);
            data->have_graph_rate = true;
        } else {
            float rel = fabsf(graph_rate - data->sample_rate) / data->sample_rate;
            if (rel > 0.05f)
                redesign_eq_bank(data, graph_rate);
        }
    }

    for (u32 ch = 0; ch < data->n_channels; ++ch) {
        in[ch] = pw_filter_get_dsp_buffer(data->in_ports[ch], n_samples);
        out[ch] = pw_filter_get_dsp_buffer(data->out_ports[ch], n_samples);
        if (in[ch] == NULL || out[ch] == NULL)
            return;
    }

    for (u32 n = 0; n < n_samples; ++n) {
        for (u32 ch = 0; ch < data->n_channels; ++ch) {
            float y = in[ch][n] * data->preamp;
            for (u32 b = 0; b < data->n_bands; ++b) {
                struct biquad *biq = band_state(data, ch, b);
                float x = y;
                y = biq->b0 * x + biq->b1 * biq->x1 + biq->b2 * biq->x2 - biq->a1 * biq->y1 - biq->a2 * biq->y2;
                biq->x2 = biq->x1;
                biq->x1 = x;
                biq->y2 = biq->y1;
                biq->y1 = y;
            }
            out[ch][n] = y;
        }
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static void do_quit(void *userdata, int signal_number) {
    struct data *data = userdata;
    (void)signal_number;
    pw_main_loop_quit(data->loop);
}

static void do_reload(void *userdata, int signal_number) {
    struct data *data = userdata;
    (void)signal_number;
    data->reload_requested = true;
}

static int write_daemon_pid(const struct data *data) {
    char path[PATH_MAX];
    char pid_buf[64];
    int rc = daemon_pid_path(data, path, sizeof(path));
    if (rc < 0)
        return rc;
    snprintf(pid_buf, sizeof(pid_buf), "%ld", (long)getpid());
    return write_text_file(path, pid_buf);
}

static void remove_daemon_pid(const struct data *data) {
    char path[PATH_MAX];
    if (daemon_pid_path(data, path, sizeof(path)) == 0)
        unlink(path);
}

static int read_daemon_pid(const char *state_dir, pid_t *pid_out) {
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

static int read_daemon_pid_from_systemd(pid_t *pid_out) {
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

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s --daemon [--config PATH]\n"
            "  %s preset list [--config PATH]\n"
            "  %s preset current [--config PATH]\n"
            "  %s preset set <name> [--config PATH]\n"
            "  %s preset reload\n",
            argv0, argv0, argv0, argv0, argv0);
}

static int cli_preset_list(const char *config_path, const char *state_dir) {
    struct config_file cfg = { 0 };
    char current[PRESET_NAME_MAX] = { 0 };
    char current_path[PATH_MAX];
    int rc = parse_config_file(config_path, &cfg);
    if (rc < 0) {
        fprintf(stderr, "failed to parse config '%s': %s\n", config_path, strerror(-rc));
        return 1;
    }

    if (path_in_state(state_dir, "current_preset", current_path, sizeof(current_path)) == 0)
        read_first_line(current_path, current, sizeof(current));

    for (u32 i = 0; i < cfg.n_presets; ++i) {
        bool is_default = strcmp(cfg.default_preset, cfg.presets[i].name) == 0;
        bool is_current = current[0] != '\0' && strcmp(current, cfg.presets[i].name) == 0;
        printf("%c %s%s%s -> %s\n", is_current ? '*' : ' ', cfg.presets[i].name, is_default ? " (default)" : "",
               is_current ? " (current)" : "", cfg.presets[i].filters_path);
    }
    free_config(&cfg);
    return 0;
}

static int cli_preset_current(const char *config_path, const char *state_dir) {
    struct config_file cfg = { 0 };
    char current[PRESET_NAME_MAX] = { 0 };
    char current_path[PATH_MAX];
    int rc = parse_config_file(config_path, &cfg);
    if (rc < 0) {
        fprintf(stderr, "failed to parse config '%s': %s\n", config_path, strerror(-rc));
        return 1;
    }
    if (path_in_state(state_dir, "current_preset", current_path, sizeof(current_path)) < 0 ||
        read_first_line(current_path, current, sizeof(current)) < 0 || find_preset(&cfg, current) == NULL) {
        snprintf(current, sizeof(current), "%s", cfg.default_preset);
    }
    printf("%s\n", current);
    free_config(&cfg);
    return 0;
}

static int cli_preset_set(const char *config_path, const char *state_dir, const char *name) {
    struct config_file cfg = { 0 };
    char state_file[PATH_MAX];
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

    if (path_in_state(state_dir, "current_preset", state_file, sizeof(state_file)) < 0 ||
        write_text_file(state_file, name) < 0) {
        fprintf(stderr, "failed to persist current preset\n");
        return 1;
    }

    rc = read_daemon_pid(state_dir, &pid);
    if (rc < 0)
        rc = read_daemon_pid_from_systemd(&pid);
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

static int cli_preset_reload(const char *state_dir) {
    pid_t pid;
    int rc = read_daemon_pid(state_dir, &pid);
    if (rc < 0)
        rc = read_daemon_pid_from_systemd(&pid);
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

static int run_daemon(const char *config_path) {
    struct data data = { 0 };
    const struct spa_pod *params[1];
    u32 n_params = 0;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    int rc;

    data.n_channels = 2;
    data.sample_rate = DEFAULT_SAMPLE_RATE;
    if (data.n_channels > MAX_CHANNELS)
        return 1;

    if (snprintf(data.config_path, sizeof(data.config_path), "%s", config_path) >= (int)sizeof(data.config_path))
        return 1;
    rc = get_state_dir(data.state_dir, sizeof(data.state_dir));
    if (rc < 0 || mkdir_p(data.state_dir) < 0) {
        fprintf(stderr, "failed to initialize state dir\n");
        return 1;
    }

    pw_init(NULL, NULL);

    rc = load_preset_into_runtime(&data, NULL, true);
    if (rc < 0) {
        fprintf(stderr, "failed to load preset: %s\n", strerror(-rc));
        return 1;
    }

    data.loop = pw_main_loop_new(NULL);
    if (data.loop == NULL)
        return 1;

    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGUSR1, do_reload, &data);

    data.filter = pw_filter_new_simple(pw_main_loop_get_loop(data.loop), "squigwire",
                                       pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter",
                                                         PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_NODE_PASSIVE, "follow", NULL),
                                       &filter_events, &data);
    if (data.filter == NULL)
        return 1;

    for (u32 ch = 0; ch < data.n_channels; ++ch) {
        char in_name[16];
        char out_name[16];

        snprintf(in_name, sizeof(in_name), "input_%s", ch == 0 ? "FL" : "FR");
        snprintf(out_name, sizeof(out_name), "output_%s", ch == 0 ? "FL" : "FR");

        data.in_ports[ch] = pw_filter_add_port(
                data.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(struct port),
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, in_name, NULL), NULL, 0);

        data.out_ports[ch] = pw_filter_add_port(
                data.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(struct port),
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, out_name, NULL), NULL, 0);

        if (data.in_ports[ch] == NULL || data.out_ports[ch] == NULL)
            return 1;
    }

    params[n_params++] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency,
                                                   &SPA_PROCESS_LATENCY_INFO_INIT(.ns = 10 * SPA_NSEC_PER_MSEC));

    if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, params, n_params) < 0) {
        fprintf(stderr, "can't connect\n");
        return 1;
    }

    write_daemon_pid(&data);
    pw_main_loop_run(data.loop);
    remove_daemon_pid(&data);

    pw_filter_destroy(data.filter);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    free(data.eq);
    free(data.bands);
    return 0;
}

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

    if (argc >= 3 && strcmp(argv[1], "preset") == 0) {
        if (strcmp(argv[2], "list") == 0)
            return cli_preset_list(config_path, state_dir);
        if (strcmp(argv[2], "current") == 0)
            return cli_preset_current(config_path, state_dir);
        if (strcmp(argv[2], "set") == 0 && argc >= 4)
            return cli_preset_set(config_path, state_dir, argv[3]);
        if (strcmp(argv[2], "reload") == 0)
            return cli_preset_reload(state_dir);
    }

    usage(argv[0]);

    return 1;
}
