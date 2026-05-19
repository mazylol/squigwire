#ifndef PEQ_H
#define PEQ_H

#include "global.h"

#include <stddef.h>

struct biquad {
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;
};

struct peq_band {
    float freq_hz;
    float q;
    float gain_db;
};

static inline struct biquad *band_state(struct biquad *eq, u32 n_bands, u32 ch, u32 band) {
    return &eq[(size_t)ch * n_bands + band];
}

int parse_filters_file(const char *filters_path, struct peq_band **bands, u32 *n_bands, float *preamp_db);

void redesign_eq_bank(struct biquad *eq, const struct peq_band *bands, u32 n_channels, u32 n_bands, float sample_rate);

#endif // PEQ_H
