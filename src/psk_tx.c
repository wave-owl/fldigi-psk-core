/*
 * fldigi-psk-core: BPSK transmitter for PSK31/63/125 after fldigi 4.2.13 psk::tx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/psk/psk.cxx, Copyright (C) Dave Freese W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single-carrier BPSK only (no QPSK, PSK-R, xPSK, 8PSK, vestigial pilot, IMD test shape):
 *
 * 1. tx_init: phase accumulator 0, previous symbol (1,0) (psk.cxx:196-234).
 * 2. Symbol length and dcdbits per submode at 8 kHz: 256/128/64 samples and 32/64/128
 *    bits (psk.cxx:372-397); here symbol length = rate / baud, so 48 kHz gives 1536/768/384.
 * 3. Raised-cosine shape tx_shape[i] = 0.5 cos(i pi / symlen) + 0.5 (psk.cxx:1045-1052).
 * 4. tx_carriers for one carrier (psk.cxx:2215-2311): symbol = prev * sym_vec_pos[(sym*4)
 *    & 15] (psk.cxx:2196-2213), per sample ival/qval mixed by the shape, out = ival cos(phi)
 *    + qval sin(phi), phi += 2 pi f / fs wrapped once above 2 pi, then the symbol divided
 *    by its own peak (maxamp).
 * 5. tx_bit for BPSK: sym = bit << 1, bit 1 keeps the phase, 0 reverses it
 *    (psk.cxx:2324-2361).
 * 6. Preamble: dcdbits x tx_symbol(0), phase reversals (psk.cxx:2612-2620).
 * 7. Postamble, BPSK tx_flush: dcdbits x tx_symbol(2), steady carrier, then 2048 zero
 *    samples (psk.cxx:2535-2542); the zeros are scaled to the rate (2048 * rate / 8000).
 *
 * Varicode is not part of this file (wave-owl INV-781): the caller passes bits.
 * The IQ mode (im != NULL) emits the analytic signal of the same ival/qval and phase,
 * without the maxamp division (wave-owl D-782).
 */
#include "fldigi_psk.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static const double PI = 3.14159265358979323846;

/* TWOPI, fldigi src/include/modem.h:45. */
#define TWOPI (2.0 * PI)
/* Zero samples after the BPSK postamble at 8 kHz, psk.cxx:2541-2542. */
#define FLUSH_ZEROS_8K 2048
#define SVP_MASK 0xF

/* psk.cxx:2193-2213 */
static const double SYM_VEC_POS[SVP_MASK + 1][2] = {
    {-1.0, 0.0},       /* 180 degrees */
    {-0.9238, -0.3826}, /* 202.5 degrees */
    {-0.7071, -0.7071}, /* 225 degrees */
    {-0.3826, -0.9238}, /* 247.5 degrees */
    {0.0, -1.0},       /* 270 degrees */
    {0.3826, -0.9238},  /* 292.5 degrees */
    {0.7071, -0.7071},  /* 315 degrees */
    {0.9238, -0.3826},  /* 337.5 degrees */
    {1.0, 0.0},        /* 0 degrees */
    {0.9238, 0.3826},   /* 22.5 degrees */
    {0.7071, 0.7071},   /* 45 degrees */
    {0.3826, 0.9238},   /* 67.5 degrees */
    {0.0, 1.0},        /* 90 degrees */
    {-0.3826, 0.9238},  /* 112.5 degrees */
    {-0.7071, 0.7071},  /* 135 degrees */
    {-0.9238, 0.3826},  /* 157.5 degrees */
};

struct fpsk_tx {
    fpsk_tx_config cfg;
    size_t symbollen;
    size_t dcdbits;
    size_t flush_zeros;
    double delta;        /* phase increment per sample */
    double *tx_shape;    /* symbollen raised-cosine weights */
    double phaseacc;
    double prev_re, prev_im;
};

fpsk_tx *fpsk_tx_create(const fpsk_tx_config *cfg)
{
    fpsk_tx *tx;
    size_t i, dcdbits;

    if (!cfg || (cfg->rate != 8000 && cfg->rate != 48000))
        return NULL;
    /* psk.cxx:386-397: dcdbits per submode. */
    if (cfg->baud == 31.25)
        dcdbits = 32;
    else if (cfg->baud == 62.5)
        dcdbits = 64;
    else if (cfg->baud == 125.0)
        dcdbits = 128;
    else
        return NULL;
    if (!(cfg->carrier_hz >= 0.0 && cfg->carrier_hz < cfg->rate / 2.0))
        return NULL;
    tx = calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    tx->cfg = *cfg;
    tx->dcdbits = dcdbits;
    /* psk.cxx:388-397: symbollen 256/128/64 at 8 kHz = rate / baud (exact for both rates). */
    tx->symbollen = (size_t)(cfg->rate / cfg->baud);
    tx->flush_zeros = (size_t)FLUSH_ZEROS_8K * (size_t)cfg->rate / 8000;
    /* psk.cxx:2224-2225: delta = TWOPI * frequency / samplerate. */
    tx->delta = TWOPI * cfg->carrier_hz / cfg->rate;
    tx->tx_shape = malloc(tx->symbollen * sizeof *tx->tx_shape);
    if (!tx->tx_shape) {
        free(tx);
        return NULL;
    }
    /* psk.cxx:1045-1052: raised cosine shape for the transmitter. */
    for (i = 0; i < tx->symbollen; i++) {
        double sym_ph = (double)i * PI / (double)tx->symbollen;
        tx->tx_shape[i] = 0.5 * cos(sym_ph) + 0.5;
    }
    fpsk_tx_reset(tx);
    return tx;
}

void fpsk_tx_destroy(fpsk_tx *tx)
{
    if (!tx)
        return;
    free(tx->tx_shape);
    free(tx);
}

/* psk.cxx:196-234 (tx_init), BPSK part. */
void fpsk_tx_reset(fpsk_tx *tx)
{
    if (!tx)
        return;
    tx->phaseacc = 0.0;
    tx->prev_re = 1.0;
    tx->prev_im = 0.0;
}

size_t fpsk_tx_symbol_len(const fpsk_tx *tx)
{
    return tx ? tx->symbollen : 0;
}

size_t fpsk_tx_preamble_symbols(const fpsk_tx *tx)
{
    return tx ? tx->dcdbits : 0;
}

/*
 * One symbol, psk.cxx:2215-2311 (tx_carriers, single carrier, no vestigial pilot).
 * sym is the BPSK tx_symbol argument: 0 = reversal, 2 = keep (psk.cxx:2357-2358).
 */
static void tx_symbol(fpsk_tx *tx, int sym, double *re, double *im)
{
    const double *svp;
    double sym_re, sym_im, maxamp;
    size_t i, n = tx->symbollen;

    /* psk.cxx:2259: BPSK and QPSK map to the 16-point constellation. */
    sym *= 4;
    svp = SYM_VEC_POS[sym & SVP_MASK];
    /* psk.cxx:2261: symbol = prevsymbol * sym_vec_pos[...] (complex multiplication). */
    sym_re = tx->prev_re * svp[0] - tx->prev_im * svp[1];
    sym_im = tx->prev_re * svp[1] + tx->prev_im * svp[0];

    for (i = 0; i < n; i++) {
        /* psk.cxx:2267-2277 */
        double shapeA = tx->tx_shape[i];
        double shapeB = (1.0 - shapeA);
        double ival = shapeA * tx->prev_re + shapeB * sym_re;
        double qval = shapeA * tx->prev_im + shapeB * sym_im;
        double c = cos(tx->phaseacc), s = sin(tx->phaseacc);

        /* numcarriers = 1, so the "/ numcarriers" of psk.cxx:2276 is the identity. */
        re[i] = ival * c + qval * s;
        if (im)
            im[i] = ival * s - qval * c;
        /* psk.cxx:2279-2280 */
        tx->phaseacc += tx->delta;
        if (tx->phaseacc > TWOPI)
            tx->phaseacc -= TWOPI;
    }
    /* psk.cxx:2283 */
    tx->prev_re = sym_re;
    tx->prev_im = sym_im;

    if (im)
        return;
    /* psk.cxx:2302-2307: normalise the symbol to its own peak (real mode only). */
    maxamp = 0.0;
    for (i = 0; i < n; i++)
        if (maxamp < fabs(re[i]))
            maxamp = fabs(re[i]);
    if (maxamp != 0.0)
        for (i = 0; i < n; i++)
            re[i] /= maxamp;
}

/* Samples for nsym symbols plus extra; SIZE_MAX on overflow. */
static size_t need_samples(const fpsk_tx *tx, size_t nsym, size_t extra)
{
    if (nsym > (SIZE_MAX - extra) / tx->symbollen)
        return SIZE_MAX;
    return nsym * tx->symbollen + extra;
}

/* psk.cxx:2612-2620: the standard BPSK preamble, dcdbits phase reversals. */
size_t fpsk_tx_preamble(fpsk_tx *tx, double *re, double *im, size_t max)
{
    size_t k, need;

    if (!tx || !re)
        return 0;
    need = need_samples(tx, tx->dcdbits, 0);
    if (need > max)
        return 0;
    for (k = 0; k < tx->dcdbits; k++)
        tx_symbol(tx, 0, re + k * tx->symbollen, im ? im + k * tx->symbollen : NULL);
    return need;
}

/* psk.cxx:2324-2361 (tx_bit), BPSK branch :2356-2359: sym = bit << 1. */
size_t fpsk_tx_bits(fpsk_tx *tx, const unsigned char *bits, size_t n,
                    double *re, double *im, size_t max)
{
    size_t k, need;

    if (!tx || !re || (n > 0 && !bits))
        return 0;
    need = need_samples(tx, n, 0);
    if (need > max)
        return 0;
    for (k = 0; k < n; k++)
        tx_symbol(tx, (bits[k] ? 1 : 0) << 1, re + k * tx->symbollen,
                  im ? im + k * tx->symbollen : NULL);
    return need;
}

/* psk.cxx:2535-2542: BPSK tx_flush, dcdbits x tx_symbol(2), then 2048 zeros at 8 kHz. */
size_t fpsk_tx_postamble(fpsk_tx *tx, double *re, double *im, size_t max)
{
    size_t k, need, start;

    if (!tx || !re)
        return 0;
    need = need_samples(tx, tx->dcdbits, tx->flush_zeros);
    if (need > max)
        return 0;
    for (k = 0; k < tx->dcdbits; k++)
        tx_symbol(tx, 2, re + k * tx->symbollen, im ? im + k * tx->symbollen : NULL);
    start = tx->dcdbits * tx->symbollen;
    for (k = start; k < need; k++) {
        re[k] = 0.0;
        if (im)
            im[k] = 0.0;
    }
    return need;
}
