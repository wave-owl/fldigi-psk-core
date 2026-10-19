/*
 * fldigi-psk-core: internals of the PSK variants (fldigi 4.2.13 psk modem class).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSK_V_INTERNAL_H
#define PSK_V_INTERNAL_H

#include "fldigi_psk.h"
#include "fldigi_dsp.h"

#define PSKV_MAX_CARRIERS 32        /* MAX_CARRIERS, include/psk.h:49 */

/* Per-submode flags of the psk constructor (psk.cxx:362-1103). */
typedef struct pskv_mode {
    const fpsk_v_submode *sm;
    int samplerate;
    int symbollen;
    int dcdbits;
    int numcarriers;
    int qpsk, pskr, x8psk, xpsk, disablefec, puncturing;
    int psk63f;                     /* mode == MODE_PSK63F: no interleaver */
    int ofdm;                       /* MODE_OFDM_500F .. MODE_OFDM_3500 */
    int ofdm_fec;                   /* MODE_OFDM_500F, MODE_OFDM_750F */
    int bpsk500;                    /* MODE_PSK500 or MODE_QPSK500: no phase AFC */
    int symbits;
    int sinc;                       /* fir_type SINC (else PSK_CORE) */
    int idepth;
    int flushlength;
    int vestigial;
    int psk_viterbi;
    /* FEC (psk.cxx:977-1031) */
    int k, poly1, poly2;
    int dec2;                       /* second decoder and de-interleaver */
    int chunksize;                  /* 0: fldigi's default (8) */
    int traceback;                  /* 0: fldigi's default (k * 12) */
    double sc_bw, inter_carrier, bandwidth;
} pskv_mode;

int pskv_mode_init(pskv_mode *m, const fpsk_v_submode *sm);
/* modem::set_freq clamp with the default waterfall limits (modem.cxx:319-333). */
double pskv_clamp_freq(const pskv_mode *m, double f);

int pskv_varicode_decode(unsigned int symbol);
int pskv_varicode_encode(unsigned char c, unsigned char *bits);

#endif
