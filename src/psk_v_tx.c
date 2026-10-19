/*
 * fldigi-psk-core: transmitter of the PSK variants, fldigi 4.2.13 psk::tx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/psk/psk.cxx, Copyright (C) Dave Freese W1HKJ, John Douyere VK2ETA, John Phelps KL4YFD
 * and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A line-by-line port of the transmit half of fldigi's psk class for every psk mode:
 * tx_init (psk.cxx:196-234), the raised-cosine shape (psk.cxx:1045-1057), tx_carriers with
 * the Gray-mapped 8PSK/xPSK and the 16-point constellations and the optional vestigial pilot
 * (psk.cxx:2193-2311), tx_symbol (psk.cxx:2313-2322), tx_bit / tx_xpsk (psk.cxx:2324-2466),
 * tx_char (psk.cxx:2470-2493), tx_flush (psk.cxx:2496-2543), clearbits (psk.cxx:2546-2551)
 * and the preamble / character / idle branches of tx_process (psk.cxx:2553-2652).
 *
 * fldigi keeps some transmitter state across overs (the static bit accumulators of tx_bit and
 * tx_xpsk, the QPSK encoder); here every over starts from the state of a fresh fldigi
 * (fpsk_v_tx_begin), which is what fldigi's first over after start-up sends. Not ported:
 * the IMD test shape (test signal window), soft start (softPSK, off by default).
 */
#include "psk_v_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWOPI (2.0 * M_PI)
#define SVP_MASK 0xF
#define FLUSH_ZEROS 2048            /* psk.cxx:2541-2542 */
#define MAX_CODE_BITS 16            /* longest PSK / MFSK varicode (12) plus slack */

/* graymapped_8psk_pos, psk.cxx:100-110 */
static const fdsp_cmpx GRAY8_POS[8] = {
    {1.0, 0.0}, {0.7071, 0.7071}, {-0.7071, 0.7071}, {0.0, 1.0},
    {0.7071, -0.7071}, {0.0, -1.0}, {-1.0, 0.0}, {-0.7071, -0.7071},
};

/* graymapped_xpsk_pos, psk.cxx:129-135 */
static const fdsp_cmpx GRAYX_POS[4] = {
    {0.7071, 0.7071}, {-0.7071, 0.7071}, {0.7071, -0.7071}, {-0.7071, -0.7071},
};

/* sym_vec_pos, psk.cxx:2196-2213 */
static const fdsp_cmpx SYM_VEC_POS[SVP_MASK + 1] = {
    {-1.0, 0.0}, {-0.9238, -0.3826}, {-0.7071, -0.7071}, {-0.3826, -0.9238},
    {0.0, -1.0}, {0.3826, -0.9238}, {0.7071, -0.7071}, {0.9238, -0.3826},
    {1.0, 0.0}, {0.9238, 0.3826}, {0.7071, 0.7071}, {0.3826, 0.9238},
    {0.0, 1.0}, {-0.3826, 0.9238}, {-0.7071, 0.7071}, {-0.9238, 0.3826},
};

struct fpsk_v_tx {
    pskv_mode m;
    fpsk_v_tx_config cfg;
    int rate;                       /* output rate */
    int symlen;                     /* output samples per symbol */
    size_t flush_zeros;
    double frequency;
    double *tx_shape;               /* symlen */
    double *outbuf, *outim;         /* symlen */
    double phaseacc[PSKV_MAX_CARRIERS];
    fdsp_cmpx prevsymbol[PSKV_MAX_CARRIERS];
    int txsymbols[PSKV_MAX_CARRIERS];
    int symbols, acc_symbols;
    int preamble;
    int disablefec;                 /* _disablefec, toggled by the 8PSK FEC preamble */
    fdsp_encoder enc;
    fdsp_interleave txinlv;
    int has_enc;
    unsigned int bitshreg;
    /* function statics of fldigi (tx_bit psk.cxx:2327-2328, tx_xpsk psk.cxx:2365-2366) */
    int bit_bitcount, bit_xpsk_sym;
    int x_bitcount;
    unsigned int x_xpsk_sym;
    double vphase, maxamp;
    size_t bound;
    /* output cursor of the current call */
    double *re, *im;
    size_t pos;
};

/* std::complex<double> operator* of libc++: four products as separate statements. */
static fdsp_cmpx cmul(fdsp_cmpx x, fdsp_cmpx y)
{
    fdsp_cmpx r;
    double ac = x.re * y.re;
    double bd = x.im * y.im;
    double ad = x.re * y.im;
    double bc = x.im * y.re;
    r.re = ac - bd;
    r.im = ad + bc;
    return r;
}

static void emit(fpsk_v_tx *tx, const double *re, const double *im, size_t n)
{
    memcpy(tx->re + tx->pos, re, n * sizeof *re);
    if (tx->im)
        memcpy(tx->im + tx->pos, im, n * sizeof *im);
    tx->pos += n;
}

/* tx_carriers, psk.cxx:2215-2311 */
static void tx_carriers(fpsk_v_tx *tx)
{
    const pskv_mode *m = &tx->m;
    double delta[PSKV_MAX_CARRIERS], frequencies[PSKV_MAX_CARRIERS];
    double ival, qval, shapeA, shapeB, amp = 0;
    double *outbuf = tx->outbuf, *outim = tx->outim;
    int car, sym, i, n = tx->symlen, iq = tx->im != NULL, tx_vestigial = 0;
    fdsp_cmpx symbol;

    frequencies[0] = tx->frequency + ((-1 * m->numcarriers) + 1) * m->inter_carrier / 2;
    delta[0] = TWOPI * frequencies[0] / tx->rate;
    for (car = 1; car < tx->symbols; car++) {
        frequencies[car] = frequencies[car - 1] + m->inter_carrier;
        delta[car] = TWOPI * frequencies[car] / tx->rate;
    }

    for (car = 0; car < tx->symbols; car++) {
        sym = tx->txsymbols[car];
        if (m->xpsk && !tx->disablefec) {           /* Gray-mapped xpsk */
            symbol = cmul(tx->prevsymbol[car], GRAYX_POS[sym & 3]);
        } else if (m->x8psk && !tx->disablefec) {   /* Gray-mapped 8psk */
            symbol = cmul(tx->prevsymbol[car], GRAY8_POS[sym & 7]);
        } else {                                    /* the 16psk constellation */
            if (m->xpsk) {
                sym = sym * 4 + 2;
            } else if (m->x8psk) {
                sym *= 2;
            } else {                                /* BPSK and QPSK */
                if (m->qpsk && !tx->cfg.reverse)
                    sym = (4 - sym) & 3;
                sym *= 4;
            }
            symbol = cmul(tx->prevsymbol[car], SYM_VEC_POS[sym & SVP_MASK]);
        }
        for (i = 0; i < n; i++) {
            double s, c;
            shapeA = tx->tx_shape[i];
            shapeB = (1.0 - shapeA);
            ival = shapeA * tx->prevsymbol[car].re + shapeB * symbol.re;
            qval = shapeA * tx->prevsymbol[car].im + shapeB * symbol.im;
            fdsp_sincos(tx->phaseacc[car], &s, &c);
            if (car != 0)
                outbuf[i] += (ival * c + qval * s) / m->numcarriers;
            else
                outbuf[i] = (ival * c + qval * s) / m->numcarriers;
            if (iq) {
                if (car != 0)
                    outim[i] += (ival * s - qval * c) / m->numcarriers;
                else
                    outim[i] = (ival * s - qval * c) / m->numcarriers;
            }
            tx->phaseacc[car] += delta[car];
            if (tx->phaseacc[car] > TWOPI)
                tx->phaseacc[car] -= TWOPI;
        }
        tx->prevsymbol[car] = symbol;
    }

    if (m->vestigial && tx->cfg.pilot) {
        tx_vestigial = 1;
        amp = pow(10, -30.0 / 20.0) * tx->maxamp;   /* pilot_power default -30 dB */
    }
    if (tx_vestigial) {
        double dvp = TWOPI * (frequencies[0] - m->sc_bw) / tx->rate;
        for (i = 0; i < n; i++) {
            if (iq) {
                double s, c;
                fdsp_sincos(tx->vphase, &s, &c);
                outbuf[i] += amp * c;
                outim[i] += amp * s;
                outim[i] /= (1 + amp);
            } else {
                outbuf[i] += amp * cos(tx->vphase);
            }
            outbuf[i] /= (1 + amp);
            tx->vphase += dvp;
            if (tx->vphase > TWOPI)
                tx->vphase -= TWOPI;
        }
    }

    tx->maxamp = 0;
    for (i = 0; i < n; i++)
        if (tx->maxamp < fabs(outbuf[i]))
            tx->maxamp = fabs(outbuf[i]);
    if (tx->maxamp && !iq)
        for (i = 0; i < n; i++)
            outbuf[i] /= tx->maxamp;
    emit(tx, outbuf, outim, (size_t)n);
}

/* tx_symbol, psk.cxx:2313-2322 */
static void tx_symbol(fpsk_v_tx *tx, int sym)
{
    tx->acc_symbols++;
    tx->txsymbols[tx->symbols] = sym;
    if (++tx->symbols < tx->m.numcarriers)
        return;
    tx_carriers(tx);
    tx->symbols = 0;
}

/* tx_xpsk, psk.cxx:2363-2466 (8PSK and xPSK with FEC; 16PSK is not a fldigi mode) */
static void tx_xpsk(fpsk_v_tx *tx, int bit)
{
    const pskv_mode *m = &tx->m;
    int fecbits;

    /* If invalid value of bitcount, reset to 0 */
    if ((m->x8psk && m->puncturing) || m->xpsk)
        if ((tx->x_bitcount & 0x1))
            tx->x_bitcount = 0;
    /* Pass one bit and return two bits */
    tx->bitshreg = (unsigned int)fdsp_encoder_encode(&tx->enc, bit);
    fdsp_interleave_bits(&tx->txinlv, &tx->bitshreg);
    fecbits = (int)tx->bitshreg;

    if (m->xpsk) {                  /* 2 bits-per-symbol */
        tx->x_xpsk_sym = (unsigned int)fecbits;
        tx_symbol(tx, (int)tx->x_xpsk_sym);
        return;
    } else if (m->x8psk && m->puncturing) {     /* @ 2/3 rate */
        if (0 == tx->x_bitcount) {
            tx->x_xpsk_sym = (unsigned int)fecbits;
            tx->x_bitcount = 2;
            return;
        } else if (2 == tx->x_bitcount) {
            tx->x_xpsk_sym |= ((unsigned int)fecbits & 1) << 2;
            tx_symbol(tx, (int)(tx->x_xpsk_sym & 7));   /* puncture the high bit */
            tx->x_xpsk_sym = 0;
            tx->x_bitcount = 0;
            return;
        }
    } else if (m->x8psk) {          /* 3 bits-per-symbol */
        if (0 == tx->x_bitcount) {
            tx->x_xpsk_sym = (unsigned int)fecbits;
            tx->x_bitcount = 2;
            return;
        } else if (1 == tx->x_bitcount) {
            tx->x_xpsk_sym |= ((unsigned int)fecbits & 1) << 1;
            tx->x_xpsk_sym |= ((unsigned int)fecbits & 2) << 1;
            tx_symbol(tx, (int)tx->x_xpsk_sym);
            tx->x_xpsk_sym = 0;
            tx->x_bitcount = 0;
            return;
        } else if (2 == tx->x_bitcount) {
            tx->x_xpsk_sym |= ((unsigned int)fecbits & 1) << 2;
            tx_symbol(tx, (int)tx->x_xpsk_sym);
            tx->x_xpsk_sym = 0;
            tx->x_bitcount = 0;
            tx->x_xpsk_sym |= ((unsigned int)fecbits & 2) >> 1;
            tx->x_bitcount = 1;
            return;
        }
    }
}

/* tx_bit, psk.cxx:2324-2361 */
static void tx_bit(fpsk_v_tx *tx, int bit)
{
    const pskv_mode *m = &tx->m;
    unsigned int sym;

    if (m->qpsk) {
        sym = (unsigned int)fdsp_encoder_encode(&tx->enc, bit);
        sym = sym & 3;
        tx_symbol(tx, (int)sym);
    } else if (m->pskr) {
        tx->bitshreg = (unsigned int)fdsp_encoder_encode(&tx->enc, bit);
        if (!m->psk63f)
            fdsp_interleave_bits(&tx->txinlv, &tx->bitshreg);
        /* Send low bit first. tx_symbol expects 0 or 2 for BPSK */
        sym = (tx->bitshreg & 1) << 1;
        tx_symbol(tx, (int)sym);
        sym = tx->bitshreg & 2;
        tx_symbol(tx, (int)sym);
    } else if (m->x8psk || m->xpsk) {
        if (tx->disablefec) {
            /* Accumulate tx bits until the correct number for symbol-size is reached */
            tx->bit_xpsk_sym |= bit << tx->bit_bitcount++;
            if (tx->bit_bitcount == m->symbits) {
                tx_symbol(tx, tx->bit_xpsk_sym);
                tx->bit_xpsk_sym = tx->bit_bitcount = 0;
            }
        } else {
            tx_xpsk(tx, bit);
        }
    } else {
        sym = (unsigned int)bit << 1;
        tx_symbol(tx, (int)sym);
    }
}

/* tx_char, psk.cxx:2470-2493 */
static void tx_char(fpsk_v_tx *tx, unsigned char c)
{
    const pskv_mode *m = &tx->m;
    if (m->pskr || m->xpsk || m->x8psk) {
        const char *code = fdsp_mfsk_varienc(c);
        while (*code) {
            tx_bit(tx, (*code - '0'));
            code++;
        }
    } else {
        unsigned char bits[MAX_CODE_BITS];
        int nb = pskv_varicode_encode(c, bits), i;
        for (i = 0; i < nb; i++)
            tx_bit(tx, bits[i]);
        /* Insert PSK varicode character-delimiting bits */
        tx_bit(tx, 0);
        tx_bit(tx, 0);
    }
}

/* tx_flush, psk.cxx:2496-2543 */
static void tx_flush(fpsk_v_tx *tx)
{
    const pskv_mode *m = &tx->m;
    int i;
    if (m->pskr) {
        int ovhd_symbols = ((m->numcarriers - tx->symbols) % m->numcarriers);
        for (i = 0; i < ovhd_symbols / 2; i++)
            tx_bit(tx, 0);
        for (i = 0; i < m->dcdbits / 2; i++) {
            tx_bit(tx, 1);
            tx_bit(tx, 1);
        }
    } else if (m->qpsk) {           /* flush the encoder */
        for (i = 0; i < m->dcdbits; i++)
            tx_bit(tx, 0);
    } else if (!tx->disablefec && (m->xpsk || m->x8psk)) {
        for (i = 0; i < m->flushlength; i++)
            tx_char(tx, 0);         /* <NUL> clears the bit accumulators */
    } else if (tx->disablefec && (m->xpsk || m->x8psk)) {
        int symbol;
        for (i = 0; i < m->symbits; i++)
            tx_char(tx, 0);
        if (m->x8psk)
            symbol = 4;
        else
            symbol = 2;             /* xpsk */
        for (i = 0; i <= 96; i++)   /* DCD window is only 32-bits wide. Send 3-times */
            tx_symbol(tx, symbol);
    } else {                        /* Standard BPSK postamble */
        for (i = 0; i < m->dcdbits; i++)
            tx_symbol(tx, 2);       /* 0 degrees */
    }
    memset(tx->re + tx->pos, 0, tx->flush_zeros * sizeof *tx->re);
    if (tx->im)
        memset(tx->im + tx->pos, 0, tx->flush_zeros * sizeof *tx->im);
    tx->pos += tx->flush_zeros;
}

/* clearbits, psk.cxx:2546-2551 */
static void clearbits(fpsk_v_tx *tx)
{
    tx->bitshreg = 0;
    tx->enc.shreg = 0;          /* encoder::init, viterbi.cxx:241-257 */
    fdsp_interleave_flush(&tx->txinlv);
}

/* tx_init, psk.cxx:196-234, from the state of a fresh psk object */
static void tx_init(fpsk_v_tx *tx)
{
    const pskv_mode *m = &tx->m;
    int car;
    for (car = 0; car < m->numcarriers; car++) {
        tx->phaseacc[car] = 0;
        tx->prevsymbol[car].re = 1.0;
        tx->prevsymbol[car].im = 0.0;
    }
    tx->preamble = m->dcdbits;
    tx->bitshreg = 0;
    tx->symbols = 0;
    tx->acc_symbols = 0;
    tx->disablefec = m->disablefec;
    tx->bit_bitcount = tx->bit_xpsk_sym = 0;
    tx->x_bitcount = 0;
    tx->x_xpsk_sym = 0;
    if (tx->has_enc)
        clearbits(tx);
    tx->vphase = 0;
    tx->maxamp = 0;
}

/* The preamble branch of tx_process, psk.cxx:2557-2621 */
static void tx_preamble(fpsk_v_tx *tx)
{
    const pskv_mode *m = &tx->m;
    int i;
    if (m->pskr) {
        if (!m->psk63f)
            clearbits(tx);
        /* FEC prep the encoder with one/zero sequences of bits */
        for (i = 0; i < tx->preamble / 2; i++) {
            tx_bit(tx, 1);
            tx_bit(tx, 0);
        }
        /* FEC: Mark start of first character with a double zero */
        while (tx->acc_symbols % m->numcarriers)
            tx_bit(tx, 0);
        tx_char(tx, 0);
    } else if (m->ofdm_fec) {
        clearbits(tx);
        for (i = 0; i < 5; i++)
            tx_char(tx, 0);         /* sacrificial header, ramps up the FEC */
        tx_char(tx, 10);            /* <LF> newline as first character */
    } else if (m->x8psk) {
        if (!tx->disablefec)
            clearbits(tx);
        if (tx->disablefec) {
            for (i = 0; i < tx->preamble; i++)
                tx_symbol(tx, 0);
            tx_char(tx, 0);
        } else {
            tx->disablefec = 1;
            for (i = 0; i < tx->preamble / 2; i++)
                tx_symbol(tx, 0);
            tx->disablefec = 0;
            for (i = 0; i < tx->preamble; i += 2) {
                tx_bit(tx, 0);
                tx_bit(tx, 0);
            }
            tx_char(tx, 0);
        }
    } else {
        /* Standard BPSK/QPSK/xPSK preamble: phase reversals */
        for (i = 0; i < tx->preamble; i++)
            tx_symbol(tx, 0);
    }
    tx->preamble = 0;
}

/* Upper bound of the samples of one call (see fpsk_v_tx_bound). */
static size_t step_bound(const pskv_mode *m, int symlen, size_t flush_zeros)
{
    /* symbols per input bit (upper bound) and the longest code */
    size_t spb = m->pskr ? 2 : 1, code = MAX_CODE_BITS + 2, syms, worst = 0;
    size_t nc = (size_t)m->numcarriers, d = (size_t)m->dcdbits;
    /* preamble */
    syms = 2 * d + 2 * spb * nc + spb * code + 6 * spb * code + d;
    if (syms > worst)
        worst = syms;
    /* idle: NUL, 1, 32 zeros */
    syms = spb * (code + 33);
    if (syms > worst)
        worst = syms;
    /* end */
    syms = 2 * (nc + 2 * d) + d + (size_t)m->flushlength * spb * code
           + (size_t)m->symbits * spb * code + 97;
    if (syms > worst)
        worst = syms;
    return (worst / nc + 2) * (size_t)symlen + flush_zeros;
}

/* psk::psk, psk.cxx:362-1103 (transmitter part) */
fpsk_v_tx *fpsk_v_tx_create(const fpsk_v_tx_config *cfg)
{
    const fpsk_v_submode *sm;
    fpsk_v_tx *tx;
    pskv_mode *m;
    int i;

    if (!cfg || !(sm = fpsk_v_submode_find(cfg->submode)))
        return NULL;
    if (!(cfg->rate == 0 || cfg->rate == sm->rate || cfg->rate == 48000))
        return NULL;
    tx = calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    m = &tx->m;
    pskv_mode_init(m, sm);
    tx->cfg = *cfg;
    if (!(cfg->carrier_hz == pskv_clamp_freq(m, cfg->carrier_hz))) {
        free(tx);
        return NULL;
    }
    tx->frequency = cfg->carrier_hz;
    tx->rate = cfg->rate ? cfg->rate : sm->rate;
    tx->symlen = m->symbollen * (tx->rate / sm->rate);
    tx->flush_zeros = (size_t)FLUSH_ZEROS * (size_t)(tx->rate / sm->rate);
    tx->tx_shape = malloc((size_t)tx->symlen * sizeof *tx->tx_shape);
    tx->outbuf = malloc((size_t)tx->symlen * sizeof *tx->outbuf);
    tx->outim = malloc((size_t)tx->symlen * sizeof *tx->outim);
    if (!tx->tx_shape || !tx->outbuf || !tx->outim)
        goto fail;
    /* raised cosine shape for the transmitter, psk.cxx:1048-1057 */
    for (i = 0; i < tx->symlen; i++) {
        double sym_ph = i * M_PI / tx->symlen;
        tx->tx_shape[i] = 0.5 * cos(sym_ph) + 0.5;
    }
    if (m->k) {
        if (fdsp_encoder_init(&tx->enc, m->k, m->poly1, m->poly2) < 0)
            goto fail;
        tx->has_enc = 1;
        if (fdsp_interleave_init(&tx->txinlv, 2, m->idepth, FDSP_INTERLEAVE_FWD) < 0)
            goto fail;
    }
    tx->bound = step_bound(m, tx->symlen, tx->flush_zeros);
    tx_init(tx);
    return tx;
fail:
    fpsk_v_tx_destroy(tx);
    return NULL;
}

/* psk::~psk, psk.cxx:326-360 */
void fpsk_v_tx_destroy(fpsk_v_tx *tx)
{
    if (!tx)
        return;
    if (tx->has_enc) {
        fdsp_encoder_free(&tx->enc);
        fdsp_interleave_free(&tx->txinlv);
    }
    free(tx->tx_shape);
    free(tx->outbuf);
    free(tx->outim);
    free(tx);
}

size_t fpsk_v_tx_bound(const fpsk_v_tx *tx)
{
    return tx ? tx->bound : 0;
}

int fpsk_v_tx_rate(const fpsk_v_tx *tx)
{
    return tx ? tx->rate : 0;
}

static int start(fpsk_v_tx *tx, double *re, double *im, size_t max)
{
    if (!tx || !re || max < tx->bound)
        return 0;
    tx->re = re;
    tx->im = im;
    tx->pos = 0;
    return 1;
}

static size_t finish(fpsk_v_tx *tx)
{
    tx->re = tx->im = NULL;
    return tx->pos;
}

/* tx_init (psk.cxx:196-234) and the first tx_process step, psk.cxx:2553-2621 */
size_t fpsk_v_tx_begin(fpsk_v_tx *tx, double *re, double *im, size_t max)
{
    if (!start(tx, re, im, max))
        return 0;
    tx_init(tx);
    tx_preamble(tx);
    return finish(tx);
}

/* a tx_process step with a character, psk.cxx:2647-2650 */
size_t fpsk_v_tx_put(fpsk_v_tx *tx, unsigned char c, double *re, double *im, size_t max)
{
    if (!start(tx, re, im, max))
        return 0;
    tx_char(tx, c);
    return finish(tx);
}

/* GET_TX_CHAR_NODATA, psk.cxx:2636-2646 */
size_t fpsk_v_tx_idle(fpsk_v_tx *tx, double *re, double *im, size_t max)
{
    const pskv_mode *m;
    int i;
    if (!start(tx, re, im, max))
        return 0;
    m = &tx->m;
    if (m->pskr || m->xpsk || m->x8psk) {
        tx_char(tx, 0);             /* <NUL> */
        tx_bit(tx, 1);
        for (i = 0; i < 32; i++)    /* extended zero bit stream */
            tx_bit(tx, 0);
    } else {
        tx_bit(tx, 0);
    }
    return finish(tx);
}

/* the tx_process step at ETX, psk.cxx:2625-2634 */
size_t fpsk_v_tx_end(fpsk_v_tx *tx, double *re, double *im, size_t max)
{
    if (!start(tx, re, im, max))
        return 0;
    tx_flush(tx);
    return finish(tx);
}
