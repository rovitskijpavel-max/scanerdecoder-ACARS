#include "demod.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void msk_init(MskDemod *d, msk_bit_cb cb, void *ctx) {
    d->lo_phase = 0.0;
    d->lo_step = 2.0 * M_PI * 1800.0 / (double)MSK_TARGET_FS;
    d->lpf_I = d->lpf_Q = 0.0;
    /* Полоса ФНЧ ~ baud/2, чтобы подавить сумму частот (~3.6-4.2 кГц) но
     * пропустить полезное отклонение +-600 Гц. Однополюсный IIR. */
    d->lpf_alpha = 0.35;
    d->I_prev = d->Q_prev = 0.0;
    d->acc = 0.0;
    d->samp_in_bit = 0;
    /* Преамбула ACARS - это продолжительный тон 2400 Гц (см. README),
     * поэтому логично считать "предыдущим" тоном именно 2400 Гц (у нас
     * это "1") - тогда первые биты потока (внутри преамбулы) корректно
     * декодируются в NRZI как единицы, что и ожидается. */
    d->prev_tone_bit = 1;
    d->cb = cb;
    d->cb_ctx = ctx;
}

void msk_process(MskDemod *d, const float *samples, int n) {
    for (int i = 0; i < n; i++) {
        double s = (double)samples[i];

        double c = cos(d->lo_phase);
        double sn = sin(d->lo_phase);
        double I = s * c;
        double Q = -s * sn;

        d->lo_phase += d->lo_step;
        if (d->lo_phase > 2.0 * M_PI)
            d->lo_phase -= 2.0 * M_PI;

        d->lpf_I += d->lpf_alpha * (I - d->lpf_I);
        d->lpf_Q += d->lpf_alpha * (Q - d->lpf_Q);

        /* дифференциальный частотный дискриминатор (cross/dot product) */
        double cross = d->I_prev * d->lpf_Q - d->Q_prev * d->lpf_I;
        double dot = d->I_prev * d->lpf_I + d->Q_prev * d->lpf_Q;
        double dphi = atan2(cross, dot);

        d->I_prev = d->lpf_I;
        d->Q_prev = d->lpf_Q;

        d->acc += dphi;
        d->samp_in_bit++;

        if (d->samp_in_bit >= MSK_SAMPLES_PER_BIT) {
            int tone_bit = (d->acc > 0.0) ? 1 : 0; /* 1 = тон 2400 Гц, 0 = тон 1200 Гц */
            d->acc = 0.0;
            d->samp_in_bit = 0;

            /* NRZI-декодирование: тон совпадает с предыдущим -> бит "1",
             * отличается -> бит "0" (см. demod.h). */
            int bit = (tone_bit == d->prev_tone_bit) ? 1 : 0;
            d->prev_tone_bit = tone_bit;

            if (d->cb)
                d->cb(bit, d->cb_ctx);
        }
    }
}
