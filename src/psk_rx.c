/*
 * fldigi-psk-core: BPSK receiver for PSK31/63/125 after fldigi 4.2.13 psk::rx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/psk/psk.cxx and src/psk/pskcoeff.cxx, Copyright (C) Dave Freese W1HKJ and the
 * fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A C translation of wave-owl's Python port (src/modes/psk/demod.py):
 *
 * 1. Complex NCO at the carrier, z = x * exp(j phi) (psk.cxx:2020-2035).
 * 2. fir1: low-pass and decimate to 16 samples per symbol (psk.cxx:2036-2044).
 *    PSK31/63 use the raised-cosine fir1 and the PSKCore fir2, PSK125 the
 *    Blackman-windowed sinc pair (psk.cxx:383-397, psk.cxx:940-958).
 * 3. fir2: matched filter at 16 samples per symbol.
 * 4. Bit clock from the amplitude envelope: syncbuf early/late balance
 *    (psk.cxx:2061-2104), one symbol each time bitclk wraps (psk.cxx:2143-2158).
 * 5. Differential phase decision between consecutive symbols: no reversal = bit 1,
 *    reversal = bit 0 (psk.cxx:1490-1512, rx_bit(!bits) at psk.cxx:1829).
 * 6. Optional AFC from the residual phase error (psk::phaseafc, psk.cxx:1442-1468).
 *
 * The arithmetic follows the Python reference operation by operation (doubles, the same
 * NCO phase formula, numpy's pairwise sums and complex magnitude), so the decided bits are
 * identical (wave-owl INV-750).
 */
#include "fldigi_psk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FPSK_VERSION "0.2.0"

static const double PI = 3.14159265358979323846;

/* fldigi's FIR length (FIRLEN = 64, src/include/pskcoeff.h:24). */
#define FIRLEN 64
/* Samples per symbol after fir1 (bitsteps, psk.cxx:2071). */
#define BITSTEPS 16
/* Longest filter: FIRLEN + 1 taps. */
#define MAXTAPS (FIRLEN + 1)

/* PSKCore matched filter at 16 samples per symbol, fldigi src/psk/pskcoeff.cxx:203-269. */
static const double PSKCORE_FILTER[FIRLEN + 1] = {
    4.3453566e-005, -0.00049122414, -0.00078771292, -0.0013507826, -0.0021287814,
    -0.003133466, -0.004366817, -0.0058112187, -0.0074249976, -0.0091398882,
    -0.010860157, -0.012464086, -0.013807772, -0.014731191, -0.015067057,
    -0.014650894, -0.013333425, -0.01099166, -0.0075431246, -0.0029527849,
    0.0027546292, 0.0094932775, 0.017113308, 0.025403511, 0.034099681,
    0.042895839, 0.051458575, 0.059444853, 0.066521003, 0.072381617,
    0.076767694, 0.079481619, 0.080420311, 0.079481619, 0.076767694,
    0.072381617, 0.066521003, 0.059444853, 0.051458575, 0.042895839,
    0.034099681, 0.025403511, 0.017113308, 0.0094932775, 0.0027546292,
    -0.0029527849, -0.0075431246, -0.01099166, -0.013333425, -0.014650894,
    -0.015067057, -0.014731191, -0.013807772, -0.012464086, -0.010860157,
    -0.0091398882, -0.0074249976, -0.0058112187, -0.004366817, -0.003133466,
    -0.0021287814, -0.0013507826, -0.00078771292, -0.00049122414, 4.3453566e-005,
};

typedef struct fir {
    size_t ntaps;
    double taps[MAXTAPS];
    /* Input history: the last ntaps - 1 samples (oldest first), then the current block. */
    double *re, *im;
    size_t cap;     /* capacity of re/im */
} fir;

struct fpsk_rx {
    fpsk_config cfg;
    int symbollen;
    int dcdbits;
    int decim;
    double afc_range_hz;
    fir fir1, fir2;
    double *dec_re, *dec_im;   /* decimated fir1 outputs of one block */
    double *z2_re, *z2_im;     /* fir2 outputs of one block */

    double frequency;
    double phase;
    int dec_count;
    double syncbuf[BITSTEPS];
    double bitclk;
    double prev_re, prev_im;
    double afcmetric;
    double q_re, q_im;
    double sig, noise;
    long nsym;
};

const char *fpsk_version(void)
{
    return "fldigi-psk-core " FPSK_VERSION " contract=" FPSK_CONTRACT " abi=1 tx=1";
}

/*
 * numpy's pairwise sum (numpy/_core/src/umath/loops_utils.h.src) for n <= 128: eight
 * accumulators, then the remainder. Reproduces ndarray.sum() bit for bit.
 */
static double np_sum(const double *a, size_t n)
{
    size_t i;
    double res, r[8];
    if (n < 8) {
        res = 0.0;
        for (i = 0; i < n; i++)
            res += a[i];
        return res;
    }
    for (i = 0; i < 8; i++)
        r[i] = a[i];
    for (i = 8; i < n - (n % 8); i += 8) {
        r[0] += a[i + 0]; r[1] += a[i + 1]; r[2] += a[i + 2]; r[3] += a[i + 3];
        r[4] += a[i + 4]; r[5] += a[i + 5]; r[6] += a[i + 6]; r[7] += a[i + 7];
    }
    res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; i++)
        res += a[i];
    return res;
}

/* numpy's complex absolute value: larger * sqrt(1 + (smaller / larger)^2), fused. */
static double np_cabs(double re, double im)
{
    double a = fabs(re), b = fabs(im);
    double larger = a > b ? a : b, smaller = a > b ? b : a, d;
    if (larger == 0.0)
        return 0.0;
    if (isinf(larger))
        return INFINITY;
    d = smaller / larger;
    return larger * sqrt(fma(d, d, 1.0));
}

/* fldigi raisedcosfilt (src/psk/pskcoeff.cxx:274-283): length + 1 taps. */
static void raised_cosine_fir(double *h, int length)
{
    int i;
    for (i = 0; i <= length; i++)
        h[i] = (1.0 - cos(2.0 * PI * (double)i / length)) / (2.0 * length);
}

/*
 * fldigi wsincfilt with Blackman window (src/psk/pskcoeff.cxx:285-312). fldigi fills
 * length + 1 coefficients but runs the filter with length taps (psk.cxx:955-956); the
 * normalisation uses all length + 1, the last one is then dropped.
 */
static void windowed_sinc_fir(double *h, double fc, int length)
{
    double tmp[FIRLEN + 1], sum;
    double l2 = length / 2.0, k2 = 2.0 * PI * fc, k1 = 2.0 * PI / length;
    int i;
    for (i = 0; i <= length; i++) {
        double fi = (double)i;
        tmp[i] = (fi == l2) ? 1.0 : sin(k2 * (fi - l2)) / (k2 * (fi - l2));
        tmp[i] = tmp[i] * (0.42 - 0.5 * cos(k1 * fi) + 0.08 * cos(2.0 * k1 * fi));
    }
    sum = np_sum(tmp, (size_t)length + 1);
    for (i = 0; i < length; i++)
        h[i] = tmp[i] / sum;
}

static int fir_init(fir *f, size_t block)
{
    f->cap = f->ntaps - 1 + block;
    f->re = calloc(f->cap, sizeof(double));
    f->im = calloc(f->cap, sizeof(double));
    return f->re && f->im;
}

static void fir_free(fir *f)
{
    free(f->re);
    free(f->im);
}

static void fir_clear(fir *f)
{
    memset(f->re, 0, f->cap * sizeof(double));
    memset(f->im, 0, f->cap * sizeof(double));
}

/* Output k of the current block, whose first input sample is at hist + ntaps - 1. */
static void fir_out(const fir *f, size_t k, double *yr, double *yi)
{
    const double *xr = f->re + k + f->ntaps - 1, *xi = f->im + k + f->ntaps - 1;
    double sr = 0.0, si = 0.0;
    size_t j;
    for (j = 0; j < f->ntaps; j++) {
        sr += f->taps[j] * xr[-(long)j];
        si += f->taps[j] * xi[-(long)j];
    }
    *yr = sr;
    *yi = si;
}

/* Keep the last ntaps - 1 inputs after a block of n samples. */
static void fir_shift(fir *f, size_t n)
{
    size_t keep = f->ntaps - 1;
    memmove(f->re, f->re + n, keep * sizeof(double));
    memmove(f->im, f->im + n, keep * sizeof(double));
}

fpsk_rx *fpsk_rx_create(const fpsk_config *cfg)
{
    fpsk_rx *rx;
    double symlen;
    int symbollen, dcdbits, sinc;
    size_t dec_cap;

    if (!cfg || cfg->sample_rate <= 0.0 || cfg->baud <= 0.0)
        return NULL;
    /* Samples per symbol at 8 kHz and DCD/AFC averaging length (psk.cxx:383-397). */
    if (cfg->baud == 31.25) {
        symbollen = 256; dcdbits = 32; sinc = 0;
    } else if (cfg->baud == 62.5) {
        symbollen = 128; dcdbits = 64; sinc = 0;
    } else if (cfg->baud == 125.0) {
        symbollen = 64; dcdbits = 128; sinc = 1;
    } else {
        return NULL;
    }
    symlen = cfg->sample_rate / cfg->baud;
    if (fabs(symlen - symbollen) > 1e-9)
        return NULL;

    rx = calloc(1, sizeof(*rx));
    if (!rx)
        return NULL;
    rx->cfg = *cfg;
    rx->symbollen = symbollen;
    rx->dcdbits = dcdbits;
    rx->decim = symbollen / BITSTEPS;
    rx->afc_range_hz = cfg->afc_range_hz > 0.0 ? cfg->afc_range_hz : cfg->baud;

    /* Filters per submode (psk.cxx:940-958). */
    if (sinc) {
        windowed_sinc_fir(rx->fir1.taps, 1.0 / symbollen, FIRLEN);
        rx->fir1.ntaps = FIRLEN;
        windowed_sinc_fir(rx->fir2.taps, 1.0 / 16.0, FIRLEN);
        rx->fir2.ntaps = FIRLEN;
    } else {
        raised_cosine_fir(rx->fir1.taps, FIRLEN);
        rx->fir1.ntaps = FIRLEN + 1;
        memcpy(rx->fir2.taps, PSKCORE_FILTER, sizeof(PSKCORE_FILTER));
        rx->fir2.ntaps = FIRLEN + 1;
    }
    dec_cap = (size_t)BITSTEPS + 2;
    if (!fir_init(&rx->fir1, (size_t)symbollen) || !fir_init(&rx->fir2, dec_cap)) {
        fpsk_rx_destroy(rx);
        return NULL;
    }
    rx->dec_re = rx->fir2.re + rx->fir2.ntaps - 1;
    rx->dec_im = rx->fir2.im + rx->fir2.ntaps - 1;
    rx->z2_re = calloc(dec_cap, sizeof(double));
    rx->z2_im = calloc(dec_cap, sizeof(double));
    if (!rx->z2_re || !rx->z2_im) {
        fpsk_rx_destroy(rx);
        return NULL;
    }
    fpsk_rx_reset(rx);
    return rx;
}

void fpsk_rx_destroy(fpsk_rx *rx)
{
    if (!rx)
        return;
    fir_free(&rx->fir1);
    fir_free(&rx->fir2);
    free(rx->z2_re);
    free(rx->z2_im);
    free(rx);
}

void fpsk_rx_reset(fpsk_rx *rx)
{
    if (!rx)
        return;
    rx->frequency = rx->cfg.center_hz;
    rx->phase = 0.0;
    fir_clear(&rx->fir1);
    fir_clear(&rx->fir2);
    rx->dec_count = 0;
    memset(rx->syncbuf, 0, sizeof(rx->syncbuf));
    rx->bitclk = 0.0;
    rx->prev_re = 1.0;
    rx->prev_im = 0.0;
    rx->afcmetric = 0.0;
    rx->q_re = 0.0;
    rx->q_im = 0.0;
    rx->sig = 0.0;
    rx->noise = 0.0;
    rx->nsym = 0;
}

void fpsk_rx_set_freq(fpsk_rx *rx, double center_hz)
{
    if (!rx)
        return;
    rx->cfg.center_hz = center_hz;
    rx->frequency = center_hz;
}

double fpsk_rx_freq_hz(const fpsk_rx *rx)
{
    return rx ? rx->frequency : 0.0;
}

double fpsk_rx_snr_db(const fpsk_rx *rx)
{
    if (!rx || rx->nsym < 10 || rx->sig <= 0.0)
        return 0.0;
    if (rx->noise <= 1e-15)
        return 30.0;
    return 10.0 * log10(rx->sig / rx->noise);
}

double fpsk_rx_quality(const fpsk_rx *rx)
{
    if (!rx)
        return 0.0;
    return pow(hypot(rx->q_re, rx->q_im), 2.0);
}

size_t fpsk_rx_taps(const fpsk_rx *rx, int which, double *out, size_t max)
{
    const fir *f;
    size_t i;
    if (!rx || (which != FPSK_FIR1 && which != FPSK_FIR2))
        return 0;
    f = which == FPSK_FIR1 ? &rx->fir1 : &rx->fir2;
    for (i = 0; out && i < f->ntaps && i < max; i++)
        out[i] = f->taps[i];
    return f->ntaps;
}

/*
 * fldigi psk::phaseafc (psk.cxx:1442-1468). The residual phase error per symbol is a
 * frequency error of err * fs / (2 pi symbollen) Hz; 1/dcdbits of it is removed per
 * symbol. fldigi derives the error without wrapping, so it ignores negative errors on
 * non-reversals; here it is wrapped to (-pi, pi] and both signs are used.
 */
static void phase_afc(fpsk_rx *rx, double err)
{
    double hz, f, lo, hi;
    double rate = rx->cfg.sample_rate;
    if (rx->afcmetric < 0.05 || fabs(err) > PI / 2.0)
        return;
    hz = err * rate / (2.0 * PI * rx->symbollen);
    if (fabs(hz) >= rate / rx->symbollen)
        return;
    f = rx->frequency - hz / rx->dcdbits;
    lo = rx->cfg.center_hz - rx->afc_range_hz;
    hi = rx->cfg.center_hz + rx->afc_range_hz;
    if (f < lo)
        f = lo;
    if (f > hi)
        f = hi;
    rx->frequency = f;
}

/* Differential decision (psk.cxx:1490-1512): phase of sym relative to the last one. */
static void symbol(fpsk_rx *rx, double sr, double si, uint8_t *bits, size_t max_bits,
                   size_t *nbits)
{
    /* conj(prev) * sym */
    double ar = rx->prev_re, ai = -rx->prev_im;
    double re = ar * sr - ai * si, im = ar * si + ai * sr;
    double phase = atan2(im, re), c2, s2, quality, err;
    int reversal;

    rx->prev_re = sr;
    rx->prev_im = si;
    reversal = fabs(phase) > PI / 2.0;
    if (*nbits < max_bits)
        bits[*nbits] = reversal ? 0 : 1;
    (*nbits)++;
    /* Signal quality (psk.cxx:1561-1580, SQLDECAY = 50) and a phase-scatter SNR. */
    c2 = cos(2.0 * phase);
    s2 = sin(2.0 * phase);
    rx->q_re = rx->q_re + (c2 - rx->q_re) / 50.0;
    rx->q_im = rx->q_im + (s2 - rx->q_im) / 50.0;
    quality = fpsk_rx_quality(rx);
    rx->afcmetric += (quality - rx->afcmetric) / 50.0;
    err = phase - (reversal ? PI : 0.0);
    if (err > PI)
        err -= 2.0 * PI;
    else if (err < -PI)
        err += 2.0 * PI;
    rx->sig += pow(cos(err), 2.0);
    rx->noise += pow(sin(err), 2.0);
    rx->nsym++;
    if (rx->cfg.afc)
        phase_afc(rx, err);
}

/*
 * Bit clock (psk.cxx:2061-2104): average the envelope into syncbuf and steer bitclk so
 * the early and late halves balance; one symbol each time bitclk wraps
 * (psk.cxx:2143-2158).
 */
static void sample(fpsk_rx *rx, double zr, double zi, double mag, uint8_t *bits,
                   size_t max_bits, size_t *nbits)
{
    double *sb = rx->syncbuf, early, late, amp, err;
    int idx = (int)rx->bitclk;
    sb[idx] = 0.8 * sb[idx] + 0.2 * mag;
    early = np_sum(sb, BITSTEPS / 2);
    late = np_sum(sb + BITSTEPS / 2, BITSTEPS / 2);
    amp = early + late;
    err = amp == 0.0 ? 0.0 : (early - late) / amp;
    rx->bitclk -= err / 5.0;
    rx->bitclk += 1.0;
    if (rx->bitclk < 0.0)
        rx->bitclk += BITSTEPS;
    if (rx->bitclk >= BITSTEPS) {
        rx->bitclk -= BITSTEPS;
        symbol(rx, zr, zi, bits, max_bits, nbits);
    }
}

/* One block of at most one symbol: NCO, fir1 + decimation, fir2, bit clock. */
static void block(fpsk_rx *rx, const double *x, size_t n, uint8_t *bits, size_t max_bits,
                  size_t *nbits)
{
    fir *f1 = &rx->fir1, *f2 = &rx->fir2;
    double step = 2.0 * PI * rx->frequency / rx->cfg.sample_rate;
    double *zr = f1->re + f1->ntaps - 1, *zi = f1->im + f1->ntaps - 1;
    size_t k, first, ndec = 0;

    /* Complex NCO (psk.cxx:2020-2035): the block's phases are phase + step * k. */
    for (k = 0; k < n; k++) {
        double ph = rx->phase + step * (double)k;
        zr[k] = x[k] * cos(ph);
        zi[k] = x[k] * sin(ph);
    }
    rx->phase = fmod(rx->phase + step * (double)n, 2.0 * PI);

    /* fir1 and decimation to BITSTEPS samples per symbol (psk.cxx:2036-2044). */
    first = (size_t)(rx->decim - 1 - rx->dec_count);
    rx->dec_count = (int)((rx->dec_count + n) % (size_t)rx->decim);
    for (k = first; k < n; k += (size_t)rx->decim) {
        fir_out(f1, k, &rx->dec_re[ndec], &rx->dec_im[ndec]);
        ndec++;
    }
    fir_shift(f1, n);
    if (ndec == 0)
        return;

    /* fir2: matched filter at 16 samples per symbol. */
    for (k = 0; k < ndec; k++)
        fir_out(f2, k, &rx->z2_re[k], &rx->z2_im[k]);
    fir_shift(f2, ndec);

    for (k = 0; k < ndec; k++)
        sample(rx, rx->z2_re[k], rx->z2_im[k], np_cabs(rx->z2_re[k], rx->z2_im[k]),
               bits, max_bits, nbits);
}

size_t fpsk_rx_process(fpsk_rx *rx, const double *x, size_t n,
                       uint8_t *bits, size_t max_bits)
{
    size_t start, nbits = 0, len;
    if (!rx || !x)
        return 0;
    /* One symbol per block, so an AFC correction takes effect within a symbol. */
    for (start = 0; start < n; start += (size_t)rx->symbollen) {
        len = n - start < (size_t)rx->symbollen ? n - start : (size_t)rx->symbollen;
        block(rx, x + start, len, bits, max_bits, &nbits);
    }
    return nbits;
}
