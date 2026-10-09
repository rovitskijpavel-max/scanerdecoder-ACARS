#include "bitsync.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Мощность одного тона freq_hz в окне x[offset..offset+window_len-1]
 * методом Гёрцеля. */
static double goertzel_power(const float *x, size_t offset, int window_len,
                              int sample_rate, double freq_hz) {
    double k = 0.5 + ((double)window_len * freq_hz) / (double)sample_rate;
    double w = 2.0 * M_PI * k / (double)window_len;
    double coeff = 2.0 * cos(w);

    double s_prev = 0.0, s_prev2 = 0.0;
    for (int n = 0; n < window_len; n++) {
        double s = (double)x[offset + (size_t)n] + coeff * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev = s;
    }
    return s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
}

int bitsync_best_phase(const float *x, size_t n, int sample_rate,
                        int samples_per_bit, double baud) {
    if (samples_per_bit <= 0 || n < (size_t)samples_per_bit * 2)
        return 0;

    int best_phase = 0;
    double best_score = -1.0;

    for (int phase = 0; phase < samples_per_bit; phase++) {
        double score = 0.0;
        size_t pos = (size_t)phase;
        while (pos + (size_t)samples_per_bit <= n) {
            double e_mark = goertzel_power(x, pos, samples_per_bit, sample_rate, baud);       /* 2400 Гц */
            double e_space = goertzel_power(x, pos, samples_per_bit, sample_rate, baud / 2.0); /* 1200 Гц */
            score += fabs(e_mark - e_space);
            pos += (size_t)samples_per_bit;
        }
        if (score > best_score) {
            best_score = score;
            best_phase = phase;
        }
    }

    return best_phase;
}
