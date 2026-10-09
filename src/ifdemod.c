#include "ifdemod.h"
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double estimate_zero_crossing_hz(const float *x, size_t n, int sample_rate) {
    if (n < 2 || sample_rate <= 0)
        return 0.0;

    /* Убираем постоянную составляющую перед подсчётом переходов через
     * ноль, иначе смещённый по DC сигнал даст заниженную оценку. */
    double mean = 0.0;
    for (size_t i = 0; i < n; i++)
        mean += (double)x[i];
    mean /= (double)n;

    size_t crossings = 0;
    int prev_sign = ((double)x[0] - mean) >= 0.0 ? 1 : -1;
    for (size_t i = 1; i < n; i++) {
        int sign = ((double)x[i] - mean) >= 0.0 ? 1 : -1;
        if (sign != prev_sign)
            crossings++;
        prev_sign = sign;
    }

    double duration_s = (double)n / (double)sample_rate;
    if (duration_s <= 0.0)
        return 0.0;

    /* Один период сигнала на данной "средней" частоте даёт 2 перехода
     * через ноль, поэтому делим на 2. */
    return ((double)crossings / duration_s) / 2.0;
}

static void onepole_lowpass_inplace(float *x, size_t n, int sample_rate, double fc_hz) {
    double dt = 1.0 / (double)sample_rate;
    double rc = 1.0 / (2.0 * M_PI * fc_hz);
    double alpha = dt / (rc + dt);
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        acc += alpha * ((double)x[i] - acc);
        x[i] = (float)acc;
    }
}

static void onepole_highpass_inplace(float *x, size_t n, int sample_rate, double fc_hz) {
    double dt = 1.0 / (double)sample_rate;
    double rc = 1.0 / (2.0 * M_PI * fc_hz);
    double alpha = rc / (rc + dt);
    double y_prev = 0.0;
    double x_prev = 0.0;
    for (size_t i = 0; i < n; i++) {
        double xi = (double)x[i];
        double y = alpha * (y_prev + xi - x_prev);
        x[i] = (float)y;
        y_prev = y;
        x_prev = xi;
    }
}

void am_envelope_detect_inplace(float *x, size_t n, int sample_rate) {
    if (n == 0)
        return;

    /* 1. Убираем DC перед выпрямлением, иначе смещение "спрямляется"
     *    в выпрямителе и портит огибающую. */
    double mean = 0.0;
    for (size_t i = 0; i < n; i++)
        mean += (double)x[i];
    mean /= (double)n;
    for (size_t i = 0; i < n; i++)
        x[i] = (float)((double)x[i] - mean);

    /* 2. Двухполупериодное выпрямление (детектор огибающей). */
    for (size_t i = 0; i < n; i++)
        x[i] = (float)fabs((double)x[i]);

    /* 3. ФНЧ (каскад из двух однополюсных звеньев для более крутого
     *    спада, чем у одного звена) - оставляем полосу до ~3 кГц, этого
     *    достаточно для тонов ACARS 1200/2400 Гц с запасом. */
    onepole_lowpass_inplace(x, n, sample_rate, 3000.0);
    onepole_lowpass_inplace(x, n, sample_rate, 3000.0);

    /* 4. Подавление постоянной составляющей (аналог разделительного
     *    конденсатора на выходе классического АМ-детектора), чтобы
     *    получить двухполярный сигнал, ожидаемый demod.c. */
    onepole_highpass_inplace(x, n, sample_rate, 200.0);
}

int ifdemod_decide_mode(const float *x, size_t n, int sample_rate, int force_mode,
                         double *out_zcr_hz) {
    double zcr = estimate_zero_crossing_hz(x, n, sample_rate);
    if (out_zcr_hz)
        *out_zcr_hz = zcr;

    if (force_mode > 0) return 1;
    if (force_mode < 0) return 0;
    return (zcr > IFDEMOD_ZCR_THRESHOLD_HZ);
}

int ifdemod_auto_process(float *x, size_t n, int sample_rate, int force_mode,
                          double *out_zcr_hz) {
    double zcr = estimate_zero_crossing_hz(x, n, sample_rate);
    if (out_zcr_hz)
        *out_zcr_hz = zcr;

    int should_apply;
    if (force_mode > 0)
        should_apply = 1;
    else if (force_mode < 0)
        should_apply = 0;
    else
        should_apply = (zcr > IFDEMOD_ZCR_THRESHOLD_HZ);

    if (should_apply)
        am_envelope_detect_inplace(x, n, sample_rate);

    return should_apply;
}
