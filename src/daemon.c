#include "include/daemon.h"
#include "include/config.h"
#include "include/fs.h"
#include "include/global.h"
#include "include/peq.h"
#include "include/state.h"

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

struct port {
    void *unused;
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

static void _redesign_eq_bank_data(struct data *data, float sample_rate) {
    data->sample_rate = sample_rate;
    redesign_eq_bank(data->eq, data->bands, data->n_channels, data->n_bands, data->sample_rate);
}

static float _rate_from_position(const struct spa_io_position *position) {
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

static int _load_preset_into_runtime(struct data *data, const char *requested_preset, bool persist_state) {
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
    } else if (read_current_preset_name(data->state_dir, from_state, sizeof(from_state)) == 0) {
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
    _redesign_eq_bank_data(data, sample_rate);

    if (persist_state)
        write_current_preset(data->state_dir, selected);

    fprintf(stderr, "active preset '%s': %u PK bands from %s (preamp %.2f dB)\n", selected, new_n_bands,
            preset->filters_path, preamp_db);

    free_config(&cfg);
    return 0;
}

static void _on_process(void *userdata, struct spa_io_position *position) {
    struct data *data = userdata;
    u32 n_samples = position->clock.duration;
    float graph_rate = _rate_from_position(position);
    float *in[MAX_CHANNELS] = { 0 };
    float *out[MAX_CHANNELS] = { 0 };

    if (data->reload_requested) {
        int rc = _load_preset_into_runtime(data, NULL, false);
        if (rc < 0)
            fprintf(stderr, "reload failed: %s\n", strerror(-rc));
        data->reload_requested = false;
    }

    if (graph_rate > 0.0f) {
        if (!data->have_graph_rate) {
            _redesign_eq_bank_data(data, graph_rate);
            data->have_graph_rate = true;
        } else {
            float rel = fabsf(graph_rate - data->sample_rate) / data->sample_rate;
            if (rel > 0.05f)
                _redesign_eq_bank_data(data, graph_rate);
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
                struct biquad *biq = band_state(data->eq, data->n_bands, ch, b);
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
    .process = _on_process,
};

static void _do_quit(void *userdata, int signal_number) {
    struct data *data = userdata;
    (void)signal_number;
    pw_main_loop_quit(data->loop);
}

static void _do_reload(void *userdata, int signal_number) {
    struct data *data = userdata;
    (void)signal_number;
    data->reload_requested = true;
}

int run_daemon(const char *config_path) {
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

    rc = _load_preset_into_runtime(&data, NULL, true);
    if (rc < 0) {
        fprintf(stderr, "failed to load preset: %s\n", strerror(-rc));
        return 1;
    }

    data.loop = pw_main_loop_new(NULL);
    if (data.loop == NULL)
        return 1;

    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, _do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, _do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGUSR1, _do_reload, &data);

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

    if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, params, n_params) < 0) {
        fprintf(stderr, "can't connect\n");
        return 1;
    }

    write_daemon_pid(data.state_dir);
    pw_main_loop_run(data.loop);
    remove_daemon_pid(data.state_dir);

    pw_filter_destroy(data.filter);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    free(data.eq);
    free(data.bands);
    return 0;
}
