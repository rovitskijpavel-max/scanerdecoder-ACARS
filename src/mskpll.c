#include "mskpll.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Совпадает с PLLG/PLLC в msk.c оригинального acarsdec. */
#define PLL_GAIN 38e-4f
#define PLL_COEF 0.52f

void mskpll_init(MskPll *d, mskpll_bit_cb cb, void *ctx) {
    memset(d, 0, sizeof(*d));
    d->cb = cb;
    d->cb_ctx = ctx;

    /* h[i] = cos(2*pi*600/IF_RATE/MFLTOVER*(i-center)), обрезанный по
     * нулю снизу (отрицательные значения зануляются) - "половинка"
     * косинуса, работающая как согласованный фильтр под один бит на
     * 1200 Гц (самый узкий возможный "лепесток" тона в спектре сигнала). */
    int center = (MSKPLL_FLENO - 1) / 2;
    for (int i = 0; i < MSKPLL_FLENO; i++) {
        int n = i - center;
        double arg = 2.0 * M_PI * 600.0 / (double)MSKPLL_IF_RATE_HZ / (double)MSKPLL_MFLTOVER * (double)n;
        float c = (float)cos(arg);
        d->h[i] = (c < 0.0f) ? 0.0f : c;
    }
}

void mskpll_process(MskPll *d, const float *samples, int n) {
    int idx = d->idx;
    double p = d->msk_phi;

    for (int k = 0; k < n; k++) {
        float in_sample = samples[k];

        /* ГУН на ~1800 Гц (с поправкой ФАПЧ msk_df). */
        double s = 1800.0 / (double)MSKPLL_IF_RATE_HZ * 2.0 * M_PI + d->msk_df;
        p += s;
        if (p >= 2.0 * M_PI)
            p -= 2.0 * M_PI;

        float cos_p = (float)cos(p);
        float sin_p = (float)sin(p);
        d->inb[idx].re = in_sample * cos_p;
        d->inb[idx].im = -in_sample * sin_p;
        idx = (idx + 1) % MSKPLL_FLEN;

        d->msk_clk = (float)((double)d->msk_clk + s);

        double threshold = 3.0 * M_PI / 2.0 - s / 2.0;
        if ((double)d->msk_clk >= threshold) {
            d->msk_clk = (float)((double)d->msk_clk - 3.0 * M_PI / 2.0);

            int o = (int)((double)MSKPLL_MFLTOVER * ((double)d->msk_clk / s + 0.5));
            if (o > MSKPLL_MFLTOVER)
                o = MSKPLL_MFLTOVER;

            float v_re = 0.0f, v_im = 0.0f;
            for (int j = 0; j < MSKPLL_FLEN; j++) {
                int idx2 = (j + idx) % MSKPLL_FLEN;
                float hv = d->h[o];
                v_re += d->inb[idx2].re * hv;
                v_im += d->inb[idx2].im * hv;
                o += MSKPLL_MFLTOVER;
            }

            float lvl = (float)sqrt((double)v_re * v_re + (double)v_im * v_im);
            v_re /= (lvl + 1e-8f);
            v_im /= (lvl + 1e-8f);
            d->lvl_sum += (double)lvl * (double)lvl / 4.0;
            d->bit_count++;

            float vo, dphi;
            if (d->msk_s & 1u) {
                vo = v_im;
                dphi = (vo >= 0.0f) ? -v_re : v_re;
            } else {
                vo = v_re;
                dphi = (vo >= 0.0f) ? v_im : -v_im;
            }

            float out_val = (d->msk_s & 2u) ? -vo : vo;
            int bit = (out_val > 0.0f) ? 1 : 0;

            if (d->cb)
                d->cb(bit, d->cb_ctx);

            d->msk_s = d->msk_s + 1;

            d->msk_df = (double)PLL_COEF * d->msk_df +
                        (1.0 - (double)PLL_COEF) * (double)PLL_GAIN * (double)dphi;
        }
    }

    d->idx = idx;
    d->msk_phi = p;
}
