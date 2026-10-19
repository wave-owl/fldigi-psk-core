/*
 * fldigi-psk-core: receiver of the PSK variants, fldigi 4.2.13 psk::rx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/psk/psk.cxx, src/psk/pskcoeff.cxx, src/include/psk.h, Copyright (C) Dave Freese
 * W1HKJ, John Douyere VK2ETA, John Phelps KL4YFD and the fldigi developers, after gmfsk
 * (C) Tomi Manninen OH2BNS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A line-by-line port of the receive half of fldigi's psk class for every psk mode:
 *
 * 1. Per carrier: complex NCO, fir1 low pass and decimation to 16 samples per symbol, fir2
 *    matched filter (psk.cxx:2011-2044; filters psk.cxx:926-959, pskcoeff.cxx:203-312).
 * 2. Bit clock from the mean envelope of all carriers in syncbuf (psk.cxx:2061-2150).
 * 3. Per carrier and symbol rx_symbol (psk.cxx:1480-1831): differential phase, QPSK/8PSK/xPSK
 *    /BPSK decisions, PSK-R soft bits, quality, DCD shift register and its patterns.
 * 4. FEC: rx_qpsk (K=5, psk.cxx:1191-1217) and rx_pskr (two de-interleavers and Viterbi
 *    decoders voting by metric, psk.cxx:1220-1293) on the pinned fldigi-dsp-core.
 * 5. Varicode: rx_bit / rx_bit2 (psk.cxx:1116-1189), PSK varicode or MFSK varicode.
 * 6. AFC: phaseafc / vestigial_afc / afc (psk.cxx:1389-1478).
 *
 * As in fldigi, the carrier frequencies are taken once per rx_process call
 * (psk.cxx:2011-2016); the audio is cut into fldigi's 512-sample sound card blocks.
 * Not ported (display or PSKmail only): viewer, signal search, S/N and IMD Goertzel bins,
 * phase scope. Built with -ffp-contract=on like fldigi's reference build; std::complex
 * products are separate statements as in libc++ (never fused).
 */
#include "psk_v_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#define TWOPI (2.0 * M_PI)          /* include/modem.h */
#define FIRLEN 64                   /* include/pskcoeff.h */
#define SQLDECAY 50                 /* psk.cxx:62 */
#define NUM_SFFT_BINS 11
#define SOUNDCARD_BLOCK 512         /* trx loop / bench block size */
/* C_FIR_filter history buffer; fldigi's 4096 (FIRBufferLen) only changes how often it is
 * moved, not the output (fdsp_firh) */
#define FIR_BUFLEN 256

/* PSKCore matched filter at 16 samples per symbol, pskcoeff.cxx:203-269. */
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

/* graymapped_8psk_softbits, psk.cxx:115-124 */
static const unsigned char GRAY8_SOFTBITS[8][3] = {
    {25, 0, 25}, {0, 25, 230}, {0, 230, 230}, {25, 255, 25},
    {230, 255, 25}, {255, 230, 230}, {255, 25, 230}, {230, 0, 25},
};

/* graymapped_xpsk_softbits, psk.cxx:140-145 */
static const unsigned char GRAYX_SOFTBITS[4][2] = {
    {0, 0}, {0, 255}, {255, 255}, {255, 0},
};

struct fpsk_v_rx {
    pskv_mode m;
    fpsk_v_rx_config cfg;
    double frequency;
    double phaseacc[PSKV_MAX_CARRIERS];
    fdsp_cmpx prevsymbol[PSKV_MAX_CARRIERS];
    fdsp_cmpx z2[PSKV_MAX_CARRIERS];
    double delta[PSKV_MAX_CARRIERS];
    fdsp_firh *fir1, *fir2;         /* numcarriers each */
    double fir1c[FIRLEN + 1], fir2c[FIRLEN + 1];
    int fir_len;
    int fir_dec;
    /* FEC */
    fdsp_viterbi dec, dec2;
    fdsp_interleave rxinlv, rxinlv2;
    int has_dec, has_dec2;
    unsigned int shreg, shreg2;
    int rxbitstate;
    double fecmet, fecmet2;
    unsigned char symbolpair[2];
    /* symbol timing and decisions */
    double bitclk;
    double syncbuf[16];
    double phase;
    int bits;
    unsigned int dcdshreg;
    int dcd;
    fdsp_cmpx quality;
    double metric;
    double afcmetric;
    int acquire;
    /* function statics of fldigi, per instance here */
    double averageamp;              /* rx_symbol, psk.cxx:1488 */
    int dcd_off_counter;            /* rx_symbol, psk.cxx:1614 */
    double lastpq8, lastpqx;        /* rx_symbol, psk.cxx:1721, 1752 */
    int previous1, previous2;       /* vestigial_afc, psk.cxx:1398-1399 */
    /* vestigial carrier */
    fdsp_sfft vsfft;
    int has_sfft;
    int sfft_size;
    fdsp_cmpx sfft_bins[NUM_SFFT_BINS];
    /* sound card block and text output */
    size_t block_pos;
    unsigned char *text;
    size_t max_text, ntext, dropped;
    size_t footprint;
};

/* ---- helpers with the evaluation order of fldigi's reference build ---------------- */

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

/* std::norm of libc++ (one expression, contracted as in the reference build). */
static double cnorm(fdsp_cmpx z)
{
    return z.re * z.re + z.im * z.im;
}

/* std::abs of libc++: hypot. */
static double cabs_(fdsp_cmpx z)
{
    return hypot(z.re, z.im);
}

/* clamp, include/misc.h:53-56 */
static double clampd(double x, double mn, double mx)
{
    return (x < mn) ? mn : ((x > mx) ? mx : x);
}

/* raisedcosfilt, pskcoeff.cxx:274-281 */
static void raisedcosfilt(double *firc, int len)
{
    double k1 = M_PI * 2.0 / len;
    double k2 = 2.0 * len;
    int i;
    for (i = 0; i <= len; i++)
        firc[i] = (1.0 - cos(k1 * i)) / k2;
}

/* wsincfilt with the Blackman window, pskcoeff.cxx:285-312 */
static void wsincfilt(double *firc, double fc, int len)
{
    double normalize = 0;
    double k1 = 2.0 * M_PI / len;
    double k2 = 2.0 * M_PI * fc;
    double l2 = len / 2.0;
    int i;
    for (i = 0; i <= len; i++)
        if (i == l2)
            firc[i] = 1.0;
        else
            firc[i] = sin(k2 * (i - l2)) / (k2 * (i - l2));
    for (i = 0; i <= len; i++)
        firc[i] = firc[i] * (0.42 - 0.5 * cos(k1 * i) + 0.08 * cos(2.0 * k1 * i));
    for (i = 0; i <= len; i++)
        normalize += firc[i];
    for (i = 0; i <= len; i++)
        firc[i] /= normalize;
}

/* ---- output ------------------------------------------------------------------------ */

/* put_rx_char in the bench build, dialogs/fl_digi.cxx:9516-9523: the raw byte */
static void put_rx_char(fpsk_v_rx *rx, int c)
{
    if (rx->ntext < rx->max_text)
        rx->text[rx->ntext++] = (unsigned char)c;
    else
        rx->dropped++;
}

/* ---- rx_init / reset ---------------------------------------------------------------- */


/* rx_init, psk.cxx:236-271, plus the constructor state of psk.cxx:1042-1043, 1071-1074 and
 * fresh filters (a new psk object). */
static void rx_restart(fpsk_v_rx *rx)
{
    const pskv_mode *m = &rx->m;
    int car;
    for (car = 0; car < m->numcarriers; car++) {
        rx->phaseacc[car] = 0;
        rx->prevsymbol[car].re = 1.0;
        rx->prevsymbol[car].im = 0.0;
        rx->z2[car].re = rx->z2[car].im = 0.0;
        fdsp_firh_clear(&rx->fir1[car]);
        fdsp_firh_clear(&rx->fir2[car]);
    }
    rx->quality.re = rx->quality.im = 0.0;
    if (m->pskr || m->xpsk || m->x8psk) {
        rx->shreg = 1;              /* MFSK varicode instead of psk */
        rx->shreg2 = 1;
    } else {
        rx->shreg = 0;
        rx->shreg2 = 0;
    }
    rx->dcdshreg = 0;
    rx->dcd = 0;
    rx->bitclk = 0;
    rx->afcmetric = 0.0;
    rx->rxbitstate = 0;
    rx->fecmet = rx->fecmet2 = 0;
    if (m->k) {
        fdsp_interleave_flush(&rx->rxinlv);
        fdsp_viterbi_reset(&rx->dec);
    }
    if (rx->has_dec2) {
        fdsp_interleave_flush(&rx->rxinlv2);
        fdsp_viterbi_reset(&rx->dec2);
    }
    rx->symbolpair[0] = rx->symbolpair[1] = 0;
    memset(rx->syncbuf, 0, sizeof rx->syncbuf);
    rx->acquire = 0;
    rx->phase = 0;
    rx->bits = 0;
    rx->metric = 0;
    rx->averageamp = 0;
    rx->dcd_off_counter = 0;
    rx->lastpq8 = rx->lastpqx = 0;
    rx->previous1 = -1;
    rx->previous2 = -2;
    if (rx->has_sfft)
        fdsp_sfft_reset(&rx->vsfft);
    memset(rx->sfft_bins, 0, sizeof rx->sfft_bins);
    rx->block_pos = 0;
}

/* fir1/fir2 per carrier, psk.cxx:930-959 */
static int rx_filters(fpsk_v_rx *rx)
{
    int car;
    for (car = 0; car < rx->m.numcarriers; car++) {
        if (fdsp_firh_init(&rx->fir1[car], rx->fir_len, rx->fir_dec, rx->fir1c, rx->fir1c,
                           FIR_BUFLEN) < 0)
            return -1;
        if (fdsp_firh_init(&rx->fir2[car], rx->fir_len, 1, rx->fir2c, rx->fir2c, FIR_BUFLEN) < 0)
            return -1;
    }
    return 0;
}

/* psk::psk (psk.cxx:362-1103) and psk::init (psk.cxx:291-324) */
fpsk_v_rx *fpsk_v_rx_create(const fpsk_v_rx_config *cfg)
{
    const fpsk_v_submode *sm;
    fpsk_v_rx *rx;
    pskv_mode *m;

    if (!cfg || !(sm = fpsk_v_submode_find(cfg->submode)))
        return NULL;
    rx = calloc(1, sizeof *rx);
    if (!rx)
        return NULL;
    m = &rx->m;
    pskv_mode_init(m, sm);
    rx->cfg = *cfg;
    if (!(cfg->carrier_hz == pskv_clamp_freq(m, cfg->carrier_hz))) {
        free(rx);
        return NULL;
    }
    rx->frequency = cfg->carrier_hz;
    rx->footprint = sizeof *rx;
    rx->fir1 = calloc((size_t)m->numcarriers, sizeof *rx->fir1);
    rx->fir2 = calloc((size_t)m->numcarriers, sizeof *rx->fir2);
    if (!rx->fir1 || !rx->fir2)
        goto fail;
    rx->footprint += 2 * (size_t)m->numcarriers * sizeof *rx->fir1;
    rx->footprint += 2 * (size_t)m->numcarriers * 2 * FIR_BUFLEN * sizeof(double);
    /* psk.cxx:940-959 */
    rx->fir_dec = m->symbollen > 15 ? m->symbollen / 16 : 1;
    if (!m->sinc) {
        int i;
        raisedcosfilt(rx->fir1c, FIRLEN);
        for (i = 0; i <= FIRLEN; i++)
            rx->fir2c[i] = PSKCORE_FILTER[i];
        rx->fir_len = FIRLEN + 1;
    } else {
        wsincfilt(rx->fir1c, 1.0 / m->symbollen, FIRLEN);
        wsincfilt(rx->fir2c, 1.0 / 16.0, FIRLEN);
        rx->fir_len = FIRLEN;
    }
    if (rx_filters(rx) < 0)
        goto fail;
    rx->footprint += 4 * (size_t)m->numcarriers * (size_t)rx->fir_len * sizeof(double);
    /* psk.cxx:977-1040 */
    if (m->k) {
        if (fdsp_viterbi_init(&rx->dec, m->k, m->poly1, m->poly2) < 0)
            goto fail;
        rx->has_dec = 1;
        if (m->chunksize)
            fdsp_viterbi_set_chunksize(&rx->dec, m->chunksize);
        if (m->traceback)
            fdsp_viterbi_set_traceback(&rx->dec, m->traceback);
        if (fdsp_interleave_init(&rx->rxinlv, 2, m->idepth, FDSP_INTERLEAVE_REV) < 0)
            goto fail;
        rx->footprint += fdsp_viterbi_footprint(&rx->dec) + 4 * (size_t)m->idepth;
        if (m->dec2) {
            if (fdsp_viterbi_init(&rx->dec2, m->k, m->poly1, m->poly2) < 0)
                goto fail;
            rx->has_dec2 = 1;
            fdsp_viterbi_set_chunksize(&rx->dec2, m->chunksize);
            if (m->traceback)
                fdsp_viterbi_set_traceback(&rx->dec2, m->traceback);
            if (fdsp_interleave_init(&rx->rxinlv2, 2, m->idepth, FDSP_INTERLEAVE_REV) < 0)
                goto fail;
            rx->footprint += fdsp_viterbi_footprint(&rx->dec2) + 4 * (size_t)m->idepth;
        }
    }
    /* psk.cxx:1082-1091; the sliding DFT is only fed for a pilot (single carrier) or on
     * carrier 0 of the multi-carrier OFDM modes (psk.cxx:2025-2030), so it is only
     * allocated then */
    if (m->vestigial && (m->numcarriers > 1 || cfg->pilot)) {
        int bin;
        rx->sfft_size = m->samplerate == 16000 ? 16384 : 8192;
        bin = (int)(m->sc_bw * rx->sfft_size / m->samplerate);
        if (fdsp_sfft_init(&rx->vsfft, rx->sfft_size, bin - 5, bin + 6) < 0)
            goto fail;
        rx->has_sfft = 1;
        rx->footprint += 3 * (size_t)rx->sfft_size * sizeof(fdsp_cmpx);
    }
    rx_restart(rx);
    return rx;
fail:
    fpsk_v_rx_destroy(rx);
    return NULL;
}

/* psk::~psk, psk.cxx:326-360 */
void fpsk_v_rx_destroy(fpsk_v_rx *rx)
{
    int car;
    if (!rx)
        return;
    if (rx->fir1 && rx->fir2)
        for (car = 0; car < rx->m.numcarriers; car++) {
            fdsp_firh_free(&rx->fir1[car]);
            fdsp_firh_free(&rx->fir2[car]);
        }
    free(rx->fir1);
    free(rx->fir2);
    if (rx->has_dec) {
        fdsp_viterbi_free(&rx->dec);
        fdsp_interleave_free(&rx->rxinlv);
    }
    if (rx->has_dec2) {
        fdsp_viterbi_free(&rx->dec2);
        fdsp_interleave_free(&rx->rxinlv2);
    }
    if (rx->has_sfft)
        fdsp_sfft_free(&rx->vsfft);
    free(rx);
}

/* rx_init, psk.cxx:236-271, on a fresh receiver */
void fpsk_v_rx_reset(fpsk_v_rx *rx)
{
    if (!rx)
        return;
    rx->frequency = rx->cfg.carrier_hz;
    rx_restart(rx);
}

/* ---- varicode and FEC ------------------------------------------------------------- */

/* rx_bit, psk.cxx:1116-1156 (the PSKmail s/n report is not ported) */
static void rx_bit(fpsk_v_rx *rx, int bit)
{
    const pskv_mode *m = &rx->m;
    int c;
    rx->shreg = (rx->shreg << 1) | !!bit;
    if (m->pskr || m->xpsk || m->x8psk) {
        /* MFSK varicode instead of PSK Varicode */
        if ((rx->shreg & 7) == 1) {
            c = fdsp_mfsk_varidec(rx->shreg >> 1);
            /* Voting at the character level */
            if (rx->fecmet >= rx->fecmet2 || m->disablefec) {
                if ((c != -1) && (c != 0) && rx->dcd)
                    put_rx_char(rx, c);
            }
            rx->shreg = 1;
        }
    } else {
        /* PSK varicode */
        if ((rx->shreg & 3) == 0) {
            c = pskv_varicode_decode(rx->shreg >> 2);
            if ((c != -1) && rx->dcd)
                put_rx_char(rx, c);
            rx->shreg = 0;
        }
    }
}

/* rx_bit2, psk.cxx:1161-1189 */
static void rx_bit2(fpsk_v_rx *rx, int bit)
{
    const pskv_mode *m = &rx->m;
    int c;
    rx->shreg2 = (rx->shreg2 << 1) | !!bit;
    if ((rx->shreg2 & 7) == 1) {
        c = fdsp_mfsk_varidec(rx->shreg2 >> 1);
        /* Voting at the character level for only PSKR modes */
        if (rx->fecmet < rx->fecmet2 || m->disablefec) {
            if ((c != -1) && (c != 0) && rx->dcd)
                put_rx_char(rx, c);
        }
        rx->shreg2 = 1;
    }
}

/* rx_qpsk, psk.cxx:1191-1217 */
static void rx_qpsk(fpsk_v_rx *rx, int bits)
{
    unsigned char sym[2];
    int c;
    if (!rx->cfg.reverse)
        bits = (4 - bits) & 3;
    sym[0] = (bits & 1) ? 255 : 0;
    sym[1] = (bits & 2) ? 0 : 255;  /* top bit is flipped */
    c = fdsp_viterbi_decode(&rx->dec, sym, NULL);
    if (c != -1) {
        rx_bit(rx, c & 0x80);
        rx_bit(rx, c & 0x40);
        rx_bit(rx, c & 0x20);
        rx_bit(rx, c & 0x10);
        rx_bit(rx, c & 0x08);
        rx_bit(rx, c & 0x04);
        rx_bit(rx, c & 0x02);
        rx_bit(rx, c & 0x01);
    }
}

/* rx_pskr, psk.cxx:1220-1293 */
static void rx_pskr(fpsk_v_rx *rx, unsigned char symbol)
{
    const pskv_mode *m = &rx->m;
    int met;
    unsigned char twosym[2];
    unsigned char tempc;
    int c;

    rx->symbolpair[1] = rx->symbolpair[0];
    rx->symbolpair[0] = symbol;

    if (rx->rxbitstate == 0) {
        rx->rxbitstate++;
        /* xPSK and punctured 8PSK: bit order known, one decoder (psk.cxx:1246-1252) */
        if (m->xpsk || (m->x8psk && m->puncturing)) {
            rx->fecmet2 = -9999.0;
            return;
        }
        twosym[0] = rx->symbolpair[0];
        twosym[1] = rx->symbolpair[1];
        if (!m->psk63f)
            fdsp_interleave_symbols(&rx->rxinlv2, twosym);
        tempc = twosym[1];
        twosym[1] = twosym[0];
        twosym[0] = tempc;
        c = fdsp_viterbi_decode(&rx->dec2, twosym, &met);
        if (c != -1) {
            rx->fecmet2 = fdsp_decayavg(rx->fecmet2, met, 20);
            rx_bit2(rx, c & 0x08);
            rx_bit2(rx, c & 0x04);
            rx_bit2(rx, c & 0x02);
            rx_bit2(rx, c & 0x01);
        }
    } else {
        rx->rxbitstate = 0;
        twosym[0] = rx->symbolpair[0];
        twosym[1] = rx->symbolpair[1];
        if (!m->psk63f)
            fdsp_interleave_symbols(&rx->rxinlv, twosym);
        tempc = twosym[1];
        twosym[1] = twosym[0];
        twosym[0] = tempc;
        c = fdsp_viterbi_decode(&rx->dec, twosym, &met);
        if (c != -1) {
            rx->fecmet = fdsp_decayavg(rx->fecmet, met, 20);
            rx_bit(rx, c & 0x08);
            rx_bit(rx, c & 0x04);
            rx_bit(rx, c & 0x02);
            rx_bit(rx, c & 0x01);
        }
    }
}

/* ---- AFC ---------------------------------------------------------------------------- */

/* modem::set_freq, trx/modem.cxx:319-333 */
static void set_freq(fpsk_v_rx *rx, double f)
{
    rx->frequency = pskv_clamp_freq(&rx->m, f);
}

/* vestigial_afc, psk.cxx:1390-1438 */
static void vestigial_afc(fpsk_v_rx *rx)
{
    const pskv_mode *m = &rx->m;
    double avg = 0, max = 0;
    int i = -1, k;

    if (!m->vestigial)
        return;
    if (!rx->has_sfft || !(rx->vsfft.count >= rx->vsfft.fftlen))  /* is_stable, filters.cxx:438-441 */
        return;
    for (i = 0; i < NUM_SFFT_BINS; i++)
        avg += cabs_(rx->sfft_bins[i]);
    avg /= 11.0;
    /* No real signal present: ignore AFC, reset, and return */
    if (avg == 0.0) {
        fdsp_sfft_reset(&rx->vsfft);
        return;
    }
    for (k = 0; k < NUM_SFFT_BINS; k++) {
        if (cabs_(rx->sfft_bins[k]) > max) {
            max = cabs_(rx->sfft_bins[k]);
            i = k;
        }
    }
    /* must see the same tone twice in a row, previous tones within 1 bin */
    if (i != rx->previous1 || abs(rx->previous1 - rx->previous2) > 1) {
        rx->previous2 = rx->previous1;
        rx->previous1 = i;
        fdsp_sfft_reset(&rx->vsfft);
        return;
    }
    if (i < 11 && i > -1) {
        if (i != 5) {
            double f = rx->frequency;
            f -= 1.0 * (i - 5) * m->samplerate / rx->sfft_size;
            set_freq(rx, f);
        }
    }
    rx->previous2 = rx->previous1;
    rx->previous1 = i;
    fdsp_sfft_reset(&rx->vsfft);
}

/* phaseafc, psk.cxx:1442-1468 */
static void phaseafc(fpsk_v_rx *rx)
{
    const pskv_mode *m = &rx->m;
    double error;

    if (m->vestigial) {
        vestigial_afc(rx);
        return;
    }
    if (rx->afcmetric < 0.05 || m->bpsk500 || m->numcarriers > 1)
        return;
    error = (rx->phase - rx->bits * M_PI / 2.0);
    if (error < -M_PI / 2.0 || error > M_PI / 2.0)
        return;
    error *= m->samplerate / (TWOPI * m->symbollen);
    if (fabs(error) < m->sc_bw) {
        double freqerr = error / m->dcdbits;
        set_freq(rx, rx->frequency - freqerr);
    }
    if (rx->acquire)
        rx->acquire--;
}

/* afc, psk.cxx:1470-1478 */
static void afc(fpsk_v_rx *rx)
{
    if (rx->m.ofdm)
        vestigial_afc(rx);
    else if (!rx->cfg.afc)
        return;
    else if (rx->dcd || rx->acquire)
        phaseafc(rx);
}

/* ---- rx_symbol, psk.cxx:1480-1831 ------------------------------------------------------ */

static void rx_symbol(fpsk_v_rx *rx, fdsp_cmpx symbol, int car)
{
    const pskv_mode *m = &rx->m;
    int n, set_dcd;
    unsigned char softbit = 0;
    double softangle, softamp, sigamp = cnorm(symbol);
    double phase, decay, attack, cval, sval;
    fdsp_cmpx cprev;

    /* phase = arg(conj(prevsymbol[car]) * symbol) */
    cprev.re = rx->prevsymbol[car].re;
    cprev.im = -rx->prevsymbol[car].im;
    {
        fdsp_cmpx d = cmul(cprev, symbol);
        phase = atan2(d.im, d.re);
    }
    rx->prevsymbol[car] = symbol;

    /* align the RX constellation to the TX constellation, for Non-FEC modes */
    if (m->disablefec && (m->x8psk || m->xpsk))
        phase -= M_PI;
    if (phase < 0)
        phase += TWOPI;
    rx->phase = phase;

    if (m->qpsk) {
        n = 4;
        rx->bits = ((int)(phase / M_PI_2 + 0.5)) & (n - 1);
    } else if (m->xpsk) {
        n = 4;
        rx->bits = ((int)(phase / M_PI_2)) & (n - 1);
    } else if (m->x8psk) {
        n = 8;
        rx->bits = ((int)(phase / (M_PI / 4.0) + 0.5)) & (n - 1);
    } else {
        double alpha;
        n = 2;
        rx->bits = (((int)(phase / M_PI + 0.5)) & (n - 1)) << 1;
        rx->averageamp = fdsp_decayavg(rx->averageamp, sigamp, SQLDECAY);
        if (sigamp > 0 && rx->averageamp > 0) {
            if (sigamp > rx->averageamp)
                softamp = clampd(sqrt(sigamp / rx->averageamp), 1.0, 1e6);
            else
                softamp = clampd(sqrt(rx->averageamp / sigamp), 1.0, 1e6);
        } else {
            softamp = 2;            /* arbitrary number (50% impact) */
        }
        /* values between -128 and +127 for the phase only */
        alpha = phase / M_PI;
        if (alpha > 1.0)
            alpha = 2.0 - alpha;
        softangle = 127.0 - 255.0 * alpha;
        softbit = (unsigned char)((softangle / (1.0 + softamp / 2.0)) + 128);
    }

    /* (phase_quality every dcdbits/4 calls is display only, psk.cxx:1534-1559) */

    /* simple low pass filter for quality of signal, psk.cxx:1561-1583 */
    decay = SQLDECAY;
    attack = SQLDECAY;
    fdsp_sincos(n * phase, &sval, &cval);
    if (m->x8psk) {
        attack *= 2;
        decay *= 4;
    }
    if (m->pskr) {
        fdsp_cmpx q;
        decay *= 10;
        q.re = fdsp_decayavg(rx->quality.re, cval, (int)decay);
        q.im = fdsp_decayavg(rx->quality.im, sval, (int)decay);
        rx->quality = q;
    } else {
        fdsp_cmpx q;
        q.re = fdsp_decayavg(rx->quality.re, cval,
                             (int)(cval > rx->quality.re ? attack : decay));
        q.im = fdsp_decayavg(rx->quality.im, sval,
                             (int)(sval > rx->quality.re ? attack : decay));
        rx->quality = q;
    }
    rx->metric = 100.0 * cnorm(rx->quality);
    if (m->pskr && (rx->averageamp < 3e-5))
        rx->metric = 0;
    if (rx->metric > 100)
        rx->metric = 100;

    rx->afcmetric = fdsp_decayavg(rx->afcmetric, cnorm(rx->quality), 50);

    /* DCD, psk.cxx:1611-1701 */
    rx->dcdshreg = (rx->dcdshreg << (m->symbits + 1)) | (unsigned int)rx->bits;
    set_dcd = -1;
    switch (rx->dcdshreg) {
    case 0xAAAAAAAAu:               /* bpsk DCD ON */
        if (m->xpsk || m->x8psk)
            break;
        if (m->pskr)
            break;
        set_dcd = 1;
        break;
    case 0x0A0A0A0Au:               /* pskr DCD ON */
    case 0x28282828u:
    case 0xA0A0A0A0u:
    case 0x82828282u:
        if (!m->pskr)
            break;
        set_dcd = 1;
        break;
    case 0x92492492u:               /* xpsk DCD OFF */
        if (m->qpsk)
            break;
        if (!m->xpsk)
            break;
        if (!m->disablefec)
            break;
        set_dcd = 0;
        break;
    case 0x44444444u:               /* 8psk DCD OFF */
        if (!m->x8psk)
            break;
        if (!m->disablefec)
            break;
        set_dcd = 0;
        break;
    case 0x00000000u:               /* bpsk DCD OFF; 8psk & xpsk DCD ON */
        if (m->pskr)
            break;
        if (m->x8psk || m->xpsk) {
            set_dcd = 1;
            break;
        }
        set_dcd = 0;
        break;
    default:
        if (rx->metric > rx->cfg.squelch || !rx->cfg.squelch_on)
            rx->dcd = 1;
        else if (!m->ofdm_fec)      /* OFDM FEC modes have no DCD off (psk.cxx:1672-1675) */
            rx->dcd = 0;
        rx->dcd_off_counter -= 1;
        if (rx->dcd_off_counter < 0)
            rx->dcd_off_counter = 0;
        break;
    }
    if (set_dcd == 1) {
        rx->dcd_off_counter = 0;
        rx->dcd = 1;
        rx->acquire = 0;
        rx->quality.re = 1.0;
        rx->quality.im = 0.0;
    } else if (set_dcd == 0) {
        if (++rx->dcd_off_counter > 5) {
            rx->dcd_off_counter = 0;
            rx->dcd = 0;
            rx->acquire = 0;
            rx->quality.re = 0.0;
            rx->quality.im = 0.0;
        }
    }

    /* psk.cxx:1703-1830 */
    if (m->pskr) {
        rx_pskr(rx, softbit);
    } else if (rx->dcd) {
        if (!m->disablefec && (m->x8psk || m->xpsk)) {
            int i;
            if (m->x8psk) {
                /* Soft-decode of Gray-mapped 8psk, psk.cxx:1719-1747 */
                int softpuncture = 0, soft_qualityerror;
                double phasequality = fabs(cos(n / 2 * phase));
                phasequality = (phasequality + rx->lastpq8) / 2;
                rx->lastpq8 = phasequality;
                soft_qualityerror = (int)(128 - (128 * phasequality));
                if (soft_qualityerror > 255 - 25)
                    softpuncture = 1;
                else if (soft_qualityerror < 128 / 3)
                    soft_qualityerror = 0;
                else if (soft_qualityerror > 128 - (128 / 8))
                    softpuncture = 1;
                else
                    soft_qualityerror /= 2;
                if (softpuncture) {
                    for (i = 0; i < m->symbits; i++)
                        rx_pskr(rx, 128);
                } else {
                    int bitindex = rx->bits;
                    for (i = m->symbits - 1; i >= 0; i--) {
                        if (GRAY8_SOFTBITS[bitindex][i] > 128)
                            rx_pskr(rx, (unsigned char)(GRAY8_SOFTBITS[bitindex][i]
                                                        - soft_qualityerror));
                        else
                            rx_pskr(rx, (unsigned char)(GRAY8_SOFTBITS[bitindex][i]
                                                        + soft_qualityerror));
                    }
                }
            } else {
                /* xPSK, psk.cxx:1749-1776 */
                int softpuncture = 0, soft_qualityerror;
                double phasequality = fabs(cos(n / 2 * phase));
                phasequality = (phasequality + rx->lastpqx) / 2;
                rx->lastpqx = phasequality;
                soft_qualityerror = (int)(128 * phasequality);
                if (soft_qualityerror > 255 - 12)
                    softpuncture = 1;
                else if (soft_qualityerror < 128 / 3)
                    soft_qualityerror = 0;
                else if (soft_qualityerror > 128 - (128 / 8))
                    softpuncture = 1;
                else
                    soft_qualityerror /= 2;
                if (softpuncture) {
                    for (i = 0; i < m->symbits; i++)
                        rx_pskr(rx, 128);
                } else {
                    int bitindex = rx->bits;
                    for (i = m->symbits - 1; i >= 0; i--) {
                        if (GRAYX_SOFTBITS[bitindex][i] > 128)
                            rx_pskr(rx, (unsigned char)(GRAYX_SOFTBITS[bitindex][i]
                                                        - soft_qualityerror));
                        else
                            rx_pskr(rx, (unsigned char)(GRAYX_SOFTBITS[bitindex][i]
                                                        + soft_qualityerror));
                    }
                }
            }
            if (m->puncturing)
                rx_pskr(rx, 128);   /* Recover punctured high bit */
        } else if (m->x8psk || m->xpsk) {
            /* Decode symbol one bit at a time, psk.cxx:1817-1823 */
            int bitmask = 1, i;
            for (i = 0; i < m->symbits; i++) {
                rx_bit(rx, rx->bits & bitmask);
                bitmask = bitmask << 1;
            }
        } else if (m->qpsk) {
            rx_qpsk(rx, rx->bits);
        } else {
            rx_bit(rx, !rx->bits);  /* default to BPSK */
        }
    }
}

/* ---- rx_process, psk.cxx:1996-2182 ------------------------------------------------------ */

/* The carrier frequencies of one rx_process call, psk.cxx:2011-2016. */
static void set_deltas(fpsk_v_rx *rx)
{
    const pskv_mode *m = &rx->m;
    double frequencies[PSKV_MAX_CARRIERS];
    int car;
    frequencies[0] = rx->frequency + ((-1 * m->numcarriers) + 1) * m->inter_carrier / 2;
    rx->delta[0] = TWOPI * frequencies[0] / m->samplerate;
    for (car = 1; car < m->numcarriers; car++) {
        frequencies[car] = frequencies[car - 1] + m->inter_carrier;
        rx->delta[car] = TWOPI * frequencies[car] / m->samplerate;
    }
}

/* One input sample of rx_process, psk.cxx:2018-2161 */
static void rx_sample(fpsk_v_rx *rx, double x)
{
    const pskv_mode *m = &rx->m;
    int can_rx_symbol = 0, car;

    for (car = 0; car < m->numcarriers; car++) {
        fdsp_cmpx z;
        double s, c;
        /* Mix with the internal NCO */
        fdsp_sincos(rx->phaseacc[car], &s, &c);
        z.re = x * c;
        z.im = x * s;
        if (m->numcarriers == 1 && m->vestigial && rx->cfg.pilot)
            fdsp_sfft_run(&rx->vsfft, z, rx->sfft_bins, 1);
        else if (m->numcarriers > 1 && m->vestigial && car == 0)
            fdsp_sfft_run(&rx->vsfft, z, rx->sfft_bins, 1);
        rx->phaseacc[car] += rx->delta[car];
        if (rx->phaseacc[car] > TWOPI)
            rx->phaseacc[car] -= TWOPI;

        /* Filter and downsample */
        if (fdsp_firh_run(&rx->fir1[car], z, &z)) {
            fdsp_firh_run(&rx->fir2[car], z, &rx->z2[car]);
            if (car == m->numcarriers - 1) {
                /* symbol timing recovery, psk.cxx:2061-2150 */
                int idx = (int)rx->bitclk, ii, i, symsteps;
                double sum = 0.0, ampsum = 0.0, bitsteps;
                for (ii = 0; ii < m->numcarriers; ii++)
                    sum += cabs_(rx->z2[ii]) / m->numcarriers;
                rx->syncbuf[idx] = 0.8 * rx->syncbuf[idx] + 0.2 * sum;
                sum = 0.0;
                bitsteps = (m->symbollen >= 16 ? 16 : m->symbollen);
                symsteps = (int)(bitsteps / 2);
                for (i = 0; i < symsteps; i++) {
                    sum += (rx->syncbuf[i] - rx->syncbuf[i + symsteps]);
                    ampsum += (rx->syncbuf[i] + rx->syncbuf[i + symsteps]);
                }
                sum = (ampsum == 0 ? 0 : sum / ampsum);
                rx->bitclk -= sum / (5.0 * 16 / bitsteps);
                rx->bitclk += 1;
                if (rx->bitclk < 0)
                    rx->bitclk += bitsteps;
                if (rx->bitclk >= bitsteps) {
                    rx->bitclk -= bitsteps;
                    can_rx_symbol = 1;
                    afc(rx);
                }
            }
        }
    }
    if (can_rx_symbol)
        for (car = 0; car < m->numcarriers; car++)
            rx_symbol(rx, rx->z2[car], car);
}

/* rx_process, psk.cxx:1996-2182, in 512-sample calls */
size_t fpsk_v_rx_process(fpsk_v_rx *rx, const double *x, size_t n, unsigned char *text,
                         size_t max_text)
{
    size_t i;
    if (!rx || (!x && n))
        return 0;
    rx->text = text;
    rx->max_text = text ? max_text : 0;
    rx->ntext = 0;
    for (i = 0; i < n; i++) {
        if (rx->block_pos == 0)
            set_deltas(rx);
        rx_sample(rx, x[i]);
        if (++rx->block_pos == SOUNDCARD_BLOCK)
            rx->block_pos = 0;
    }
    rx->text = NULL;
    return rx->ntext;
}

double fpsk_v_rx_carrier_hz(const fpsk_v_rx *rx)
{
    return rx ? rx->frequency : 0.0;
}

void fpsk_v_rx_set_carrier(fpsk_v_rx *rx, double carrier_hz)
{
    if (!rx)
        return;
    set_freq(rx, carrier_hz);
    rx->cfg.carrier_hz = rx->frequency;
}

double fpsk_v_rx_metric(const fpsk_v_rx *rx)
{
    return rx ? rx->metric : 0.0;
}

int fpsk_v_rx_dcd(const fpsk_v_rx *rx)
{
    return rx ? rx->dcd : 0;
}

size_t fpsk_v_rx_dropped(const fpsk_v_rx *rx)
{
    return rx ? rx->dropped : 0;
}

size_t fpsk_v_rx_footprint(const fpsk_v_rx *rx)
{
    return rx ? rx->footprint : 0;
}
