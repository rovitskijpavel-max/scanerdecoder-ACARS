#include "coherent_am.h"
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double goertzel_power(const float *x, size_t n, int sample_rate, double freq_hz) {
    double k = 0.5 + ((double)n * freq_hz) / (double)sample_rate;
    double w = 2.0 * M_PI * k / (double)n;
    double coeff = 2.0 * cos(w);

    double s_prev = 0.0, s_prev2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s = (double)x[i] + coeff * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev = s;
    }
    return s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
}

double coherent_am_estimate_carrier_hz(const float *x, size_t n, int sample_rate,
                                        double f_lo, double f_hi, double coarse_step_hz,
                                        double *out_confidence) {
    if (n == 0 || f_hi <= f_lo || coarse_step_hz <= 0.0) {
        if (out_confidence) *out_confidence = 0.0;
        return f_lo;
    }

    double best_freq = f_lo;
    double best_power = -1.0;
    double sum_power = 0.0;
    int count = 0;

    for (double f = f_lo; f <= f_hi; f += coarse_step_hz) {
        double p = goertzel_power(x, n, sample_rate, f);
        sum_power += p;
        count++;
        if (p > best_power) {
            best_power = p;
            best_freq = f;
        }
    }

    /* Уточняем вокруг грубого максимума с шагом в 10 раз мельче. */
    double fine_step = coarse_step_hz / 10.0;
    double fine_lo = best_freq - coarse_step_hz;
    double fine_hi = best_freq + coarse_step_hz;
    if (fine_lo < f_lo) fine_lo = f_lo;
    if (fine_hi > f_hi) fine_hi = f_hi;

    for (double f = fine_lo; f <= fine_hi; f += fine_step) {
        double p = goertzel_power(x, n, sample_rate, f);
        if (p > best_power) {
            best_power = p;
            best_freq = f;
        }
    }

    if (out_confidence) {
        double avg_power = (count > 0) ? (sum_power / (double)count) : 1.0;
        *out_confidence = (avg_power > 1e-12) ? (best_power / avg_power) : 0.0;
    }

    return best_freq;
}

/* Биквад "constant 0 dB peak gain" полоснопропускающий фильтр
 * (формулы RBJ Audio EQ Cookbook), Direct Form I. */
typedef struct {
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} Biquad;

static void biquad_init_bandpass(Biquad *bq, double f0, double q, int sample_rate) {
    double w0 = 2.0 * M_PI * f0 / (double)sample_rate;
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;

    bq->b0 = alpha / a0;
    bq->b1 = 0.0;
    bq->b2 = -alpha / a0;
    bq->a1 = (-2.0 * cos(w0)) / a0;
    bq->a2 = (1.0 - alpha) / a0;
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0;
}

static double biquad_process(Biquad *bq, double x0) {
    double y0 = bq->b0 * x0 + bq->b1 * bq->x1 + bq->b2 * bq->x2
                - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    bq->x2 = bq->x1;
    bq->x1 = x0;
    bq->y2 = bq->y1;
    bq->y1 = y0;
    return y0;
}

static void onepole_lowpass_inplace(double *x, size_t n, int sample_rate, double fc_hz) {
    double dt = 1.0 / (double)sample_rate;
    double rc = 1.0 / (2.0 * M_PI * fc_hz);
    double alpha = dt / (rc + dt);
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        acc += alpha * (x[i] - acc);
        x[i] = acc;
    }
}

static void onepole_highpass_inplace(float *x, size_t n, int sample_rate, double fc_hz) {
    double dt = 1.0 / (double)sample_rate;
    double rc = 1.0 / (2.0 * M_PI * fc_hz);
    double alpha = rc / (rc + dt);
    double y_prev = 0.0, x_prev = 0.0;
    for (size_t i = 0; i < n; i++) {
        double xi = (double)x[i];
        double y = alpha * (y_prev + xi - x_prev);
        x[i] = (float)y;
        y_prev = y;
        x_prev = xi;
    }
}

void coherent_am_envelope_inplace(float *x, size_t n, int sample_rate,
                                   double carrier_hz, double bandwidth_hz) {
    if (n == 0 || carrier_hz <= 0.0)
        return;

    /* 1. Полосовой фильтр вокруг несущей (2 каскада биквад для более
     *    крутого ската, чем один порядок). Q подобран так, чтобы -3дБ
     *    полоса была примерно bandwidth_hz. */
    double q = carrier_hz / (bandwidth_hz > 1.0 ? bandwidth_hz : 1.0);
    Biquad bq1, bq2;
    biquad_init_bandpass(&bq1, carrier_hz, q, sample_rate);
    biquad_init_bandpass(&bq2, carrier_hz, q, sample_rate);

    double *filtered = (double *)malloc(n * sizeof(double));
    if (!filtered)
        return;
    for (size_t i = 0; i < n; i++) {
        double v = biquad_process(&bq1, (double)x[i]);
        v = biquad_process(&bq2, v);
        filtered[i] = v;
    }

    /* 2. Квадратурный перенос несущей на 0 Гц. */
    double *I = (double *)malloc(n * sizeof(double));
    double *Q = (double *)malloc(n * sizeof(double));
    if (!I || !Q) {
        free(filtered);
        free(I);
        free(Q);
        return;
    }

    double phase = 0.0;
    double step = 2.0 * M_PI * carrier_hz / (double)sample_rate;
    for (size_t i = 0; i < n; i++) {
        I[i] = filtered[i] * cos(phase);
        Q[i] = -filtered[i] * sin(phase);
        phase += step;
        if (phase > 2.0 * M_PI)
            phase -= 2.0 * M_PI;
    }
    free(filtered);

    /* 3. ФНЧ по I и Q (полоса до ~3 кГц - с запасом покрывает тоны
     *    ACARS 1200/2400 Гц). Два каскада для крутизны. */
    double lp_cutoff = 3000.0;
    onepole_lowpass_inplace(I, n, sample_rate, lp_cutoff);
    onepole_lowpass_inplace(I, n, sample_rate, lp_cutoff);
    onepole_lowpass_inplace(Q, n, sample_rate, lp_cutoff);
    onepole_lowpass_inplace(Q, n, sample_rate, lp_cutoff);

    /* 4. Огибающая = |I+jQ| - не зависит от фазы несущей, только от
     *    точности оценки её частоты. */
    for (size_t i = 0; i < n; i++)
        x[i] = (float)sqrt(I[i] * I[i] + Q[i] * Q[i]);

    free(I);
    free(Q);

    /* 5. Подавление постоянной составляющей, чтобы получить
     *    двухполярный сигнал, ожидаемый demod.c. */
    onepole_highpass_inplace(x, n, sample_rate, 200.0);
}
