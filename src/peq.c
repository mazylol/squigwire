#include "include/peq.h"
#include "include/util.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int _append_band(struct peq_band **bands, u32 *n_bands, struct peq_band band) {
    struct peq_band *new_bands = realloc(*bands, (size_t)(*n_bands + 1) * sizeof(*new_bands));
    if (new_bands == NULL)
        return -ENOMEM;
    *bands = new_bands;
    (*bands)[*n_bands] = band;
    (*n_bands)++;
    return 0;
}

int parse_filters_file(const char *filters_path, struct peq_band **bands, u32 *n_bands, float *preamp_db) {
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
                if (_append_band(bands, n_bands, band) < 0) {
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

static void _biquad_peaking(struct biquad *b, float fs, float f0, float Q, float gain_db) {
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

void redesign_eq_bank(struct biquad *eq, const struct peq_band *bands, u32 n_channels, u32 n_bands, float sample_rate) {
    for (u32 ch = 0; ch < n_channels; ++ch) {
        for (u32 b = 0; b < n_bands; ++b) {
            struct peq_band band = bands[b];
            _biquad_peaking(band_state(eq, n_bands, ch, b), sample_rate, band.freq_hz, band.q, band.gain_db);
        }
    }
}
