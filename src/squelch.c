#include "squelch.h"
#include <math.h>
#include <stdlib.h>

/* Огибающая |x| со сглаживанием (быстрая, чтобы ловить всплески). */
#define ENV_TAU_MS 3.0
/* Слежение за фоновым шумом: быстро "опускается" при затишье, но
 * медленно "поднимается", чтобы не подстроиться под сам полезный
 * сигнал во время длинного всплеска. */
#define FLOOR_FALL_TAU_MS 40.0
#define FLOOR_RISE_TAU_MS 1500.0
/* Запас по краям найденного всплеска (мс), чтобы не обрезать фронты
 * (нарастание преамбулы, хвост сообщения). Увеличен с 25 до 60 мс:
 * на когерентном демоде (в отличие от диодного) короткие сообщения
 * при слишком плотном обрезании границ всплеска могут терять пару
 * бит преамбулы/CRC на самом краю. */
#define BURST_PAD_MS 60.0

static double alpha_for_tau(double tau_ms, int sample_rate) {
    if (tau_ms <= 0.0)
        return 1.0;
    double tau_s = tau_ms / 1000.0;
    double dt = 1.0 / (double)sample_rate;
    return dt / (tau_s + dt);
}

int squelch_find_bursts(const float *x, size_t n, int sample_rate,
                         double threshold_factor,
                         double min_burst_ms, double min_gap_ms,
                         SquelchBurst *out_bursts, int max_bursts) {
    if (n == 0 || sample_rate <= 0 || max_bursts <= 0)
        return 0;

    double a_env = alpha_for_tau(ENV_TAU_MS, sample_rate);
    double a_floor_fall = alpha_for_tau(FLOOR_FALL_TAU_MS, sample_rate);
    double a_floor_rise = alpha_for_tau(FLOOR_RISE_TAU_MS, sample_rate);

    size_t min_burst_samples = (size_t)((min_burst_ms / 1000.0) * sample_rate);
    size_t min_gap_samples = (size_t)((min_gap_ms / 1000.0) * sample_rate);
    size_t pad_samples = (size_t)((BURST_PAD_MS / 1000.0) * sample_rate);
    if (min_burst_samples < 1) min_burst_samples = 1;
    if (min_gap_samples < 1) min_gap_samples = 1;

    double env = fabs((double)x[0]);
    double floor_est = env > 1e-9 ? env : 1e-9;

    int found = 0;
    int in_burst = 0;
    size_t consecutive_high = 0;
    size_t consecutive_low = 0;
    size_t cur_start = 0;

    for (size_t i = 0; i < n; i++) {
        double sample = fabs((double)x[i]);
        env += a_env * (sample - env);

        if (env < floor_est)
            floor_est += a_floor_fall * (env - floor_est);
        else
            floor_est += a_floor_rise * (env - floor_est);
        if (floor_est < 1e-9)
            floor_est = 1e-9;

        double threshold = floor_est * threshold_factor;
        int above = (env > threshold);

        if (!in_burst) {
            if (above) {
                consecutive_high++;
                if (consecutive_high >= min_burst_samples) {
                    in_burst = 1;
                    size_t s = (i + 1 >= consecutive_high) ? (i + 1 - consecutive_high) : 0;
                    cur_start = (s > pad_samples) ? (s - pad_samples) : 0;
                    consecutive_low = 0;
                }
            } else {
                consecutive_high = 0;
            }
        } else {
            if (!above) {
                consecutive_low++;
                if (consecutive_low >= min_gap_samples) {
                    size_t e = (i + 1 >= consecutive_low) ? (i + 1 - consecutive_low) : i + 1;
                    size_t end = e + pad_samples;
                    if (end > n) end = n;
                    if (found < max_bursts) {
                        out_bursts[found].start = cur_start;
                        out_bursts[found].end = end;
                    }
                    found++;
                    in_burst = 0;
                    consecutive_high = 0;
                }
            } else {
                consecutive_low = 0;
            }
        }
    }

    if (in_burst) {
        size_t end = n;
        if (found < max_bursts) {
            out_bursts[found].start = cur_start;
            out_bursts[found].end = end;
        }
        found++;
    }

    return found;
}
