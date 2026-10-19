/*
 * fldigi-psk-core: the PSK family of fldigi 4.2.13 (psk::rx_process, psk::tx_process):
 * BPSK31/63/125 bit API and the PSK variants text API.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (src/psk/psk.cxx, src/psk/pskcoeff.cxx, src/psk/pskvaricode.cxx),
 * Copyright (C) Dave Freese W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contract "psk-c1", ABI 1, capability tags "tx=1" and "variants=1".
 * BPSK31/63/125 bit API: RX audio in (doubles), decided bits out (1 = no phase reversal,
 * 0 = reversal); TX bits in, audio or analytic IQ out; varicode is left to the caller.
 * PSK variants (fpsk_v_*): text in and out, see below.
 */
#ifndef FLDIGI_PSK_H
#define FLDIGI_PSK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FPSK_CONTRACT "psk-c1"
#define FPSK_ABI 1

/* Which filter fpsk_rx_taps() returns. */
#define FPSK_FIR1 1
#define FPSK_FIR2 2

typedef struct fpsk_config {
    double sample_rate;   /* audio rate in Hz; rate / baud must be 256, 128 or 64 (8000 Hz) */
    double baud;          /* 31.25, 62.5 or 125 */
    double center_hz;     /* carrier frequency in the audio */
    int afc;              /* nonzero: track the carrier (fldigi phaseafc) */
    double afc_range_hz;  /* maximum AFC pull from center_hz; <= 0 means one symbol rate */
} fpsk_config;

typedef struct fpsk_rx fpsk_rx;

/* "fldigi-psk-core <x.y.z> contract=psk-c1 abi=1 tx=1 variants=1 dsp=<a.b.c>" */
const char *fpsk_version(void);

/* NULL on invalid configuration or out of memory. */
fpsk_rx *fpsk_rx_create(const fpsk_config *cfg);
void fpsk_rx_destroy(fpsk_rx *rx);
void fpsk_rx_reset(fpsk_rx *rx);

/*
 * Process n samples; writes at most max_bits bits and returns the number of bits decided
 * (bits beyond max_bits are counted but dropped). The input is processed in blocks of one
 * symbol starting at x[0], so an AFC correction takes effect within a symbol; with AFC on
 * the result therefore depends on how the audio is split into calls, exactly as in the
 * reference implementation. The bit clock emits at most one bit per decimated sample,
 * so at most n / (rate / baud / 16) + 2 bits are returned; size bits for that bound.
 */
size_t fpsk_rx_process(fpsk_rx *rx, const double *x, size_t n,
                       uint8_t *bits, size_t max_bits);

/* Retune the carrier (and the AFC centre); the receiver state is kept. */
void fpsk_rx_set_freq(fpsk_rx *rx, double center_hz);

/* Current carrier frequency in Hz (moves with AFC). */
double fpsk_rx_freq_hz(const fpsk_rx *rx);
/* Phase-scatter SNR estimate in dB (0 before 10 symbols). */
double fpsk_rx_snr_db(const fpsk_rx *rx);
/* fldigi's norm(quality): 1 for a clean BPSK signal, near 0 for noise. */
double fpsk_rx_quality(const fpsk_rx *rx);

/* Copy the taps of FPSK_FIR1 or FPSK_FIR2 into out (up to max); returns the tap count. */
size_t fpsk_rx_taps(const fpsk_rx *rx, int which, double *out, size_t max);

/*
 * ---- Transmitter (capability tag "tx=1", fldigi 4.2.13 psk::tx_process) ----
 *
 * An over is fpsk_tx_preamble + fpsk_tx_bits (varicode bits, each codeword followed by
 * "00") + fpsk_tx_postamble. Calls can be repeated; phase and previous symbol carry over,
 * so splitting the bits over several calls gives the same samples as one call.
 *
 * Output modes of the rendering calls:
 *   im == NULL: real audio exactly as fldigi tx_carriers, ival*cos(phi) + qval*sin(phi),
 *               each symbol divided by its own peak |sample| (maxamp), peak 1.
 *   im != NULL: analytic IQ, re = ival*cos(phi) + qval*sin(phi),
 *               im = ival*sin(phi) - qval*cos(phi); no maxamp division (envelope peak 1),
 *               so re is the real-mode output before the division. Only positive
 *               frequencies (the carrier above 0 Hz).
 * Each call writes whole symbols and returns the number of samples written; if max is too
 * small for the complete request (or re is NULL) it writes nothing, returns 0 and leaves
 * the state unchanged.
 */
typedef struct fpsk_tx fpsk_tx;
typedef struct { int rate; double baud; double carrier_hz; } fpsk_tx_config;

/* rate 8000|48000, baud 31.25|62.5|125, 0 <= carrier_hz < rate/2; otherwise NULL. */
fpsk_tx *fpsk_tx_create(const fpsk_tx_config *cfg);
void     fpsk_tx_destroy(fpsk_tx *tx);
void     fpsk_tx_reset(fpsk_tx *tx);                /* fldigi tx_init: phase 0, prev (1,0) */
size_t   fpsk_tx_symbol_len(const fpsk_tx *tx);     /* rate / baud */
size_t   fpsk_tx_preamble_symbols(const fpsk_tx *tx);/* dcdbits: 32, 64, 128 */
/* dcdbits phase reversals: dcdbits * symbol_len samples. */
size_t   fpsk_tx_preamble(fpsk_tx *tx, double *re, double *im, size_t max);
/* One symbol per bit (nonzero = 1 = keep phase, 0 = reverse): n * symbol_len samples. */
size_t   fpsk_tx_bits(fpsk_tx *tx, const unsigned char *bits, size_t n,
                      double *re, double *im, size_t max);
/* dcdbits steady symbols + 2048*rate/8000 zeros. */
size_t   fpsk_tx_postamble(fpsk_tx *tx, double *re, double *im, size_t max);

/*
 * ---- fldigi PSK variants (capability tag "variants=1", since 0.3.0) ----
 *
 * The whole PSK family of fldigi 4.2.13's psk modem class (src/psk/psk.cxx,
 * src/include/psk.h), receiver and transmitter, as text: QPSK, 8PSK (with and without FEC),
 * PSK-R, multi-carrier BPSK and PSK-R, BPSK63F/250/500/1000 and the OFDM modes. The core owns
 * the PSK and MFSK varicode, the convolutional code and the interleaver of these submodes
 * (shared DSP from the pinned fldigi-dsp-core submodule dsp/). BPSK31/63/125 are in the
 * table as well (fldigi's own receiver and transmitter); their bit API above is unchanged.
 *
 * Every submode runs at its fldigi modem rate (8000 Hz, or 16000 Hz for 8PSK and OFDM).
 */
#define FPSK_V_BPSK 0           /* constellation of a submode (fpsk_v_submode.kind) */
#define FPSK_V_QPSK 1
#define FPSK_V_PSKR 2           /* BPSK + K=7 FEC + interleaver + MFSK varicode */
#define FPSK_V_8PSK 3
#define FPSK_V_XPSK 4           /* the 4-point "X" constellation of OFDM-500F */

typedef struct fpsk_v_submode {
    const char *name;        /* fldigi's short name: "QPSK31", "8PSK125F", "PSK63RC4", ... */
    const char *label;       /* fldigi's long name: "QPSK-31", "8PSK-125F", "4xPSK63R", ... */
    const char *adif;        /* fldigi's ADIF submode name (mode_info adif_name) */
    int fldigi_id;           /* trx_mode of fldigi 4.2.13 (index into mode_info[]) */
    int rate;                /* modem rate, Hz: 8000 or 16000 */
    int symbollen;           /* samples per symbol at rate */
    int dcdbits;
    int carriers;            /* number of carriers */
    double separation;       /* carrier spacing in units of sc_bw */
    int kind;                /* FPSK_V_BPSK ... FPSK_V_XPSK */
    int fec_k;               /* constraint length of the code, 0 = none */
    int puncturing;          /* 1: 2/3 rate punctured 8PSK */
    int idepth;              /* interleaver depth (2 x 2 x idepth), 0 = none */
    int flushlength;         /* NUL characters of the FEC 8PSK/xPSK postamble */
    int vestigial;           /* 1: fldigi's vestigial-carrier AFC (pilot) */
    double sc_bw;            /* fldigi's single-carrier bandwidth: rate / symbollen (int) */
    double bandwidth;        /* fldigi's bandwidth: sc_bw * (1 + separation * (carriers-1)) */
    double baud;             /* rate / symbollen */
} fpsk_v_submode;

int fpsk_v_submode_count(void);
const fpsk_v_submode *fpsk_v_submode_at(int index);     /* NULL outside 0 .. count-1 */
/* Case-insensitive by name (or label); NULL when unknown. */
const fpsk_v_submode *fpsk_v_submode_find(const char *name);

typedef struct fpsk_v_rx_config {
    const char *submode;     /* see fpsk_v_submode_find */
    double carrier_hz;       /* centre of the carriers (fldigi "frequency") */
    int afc;                 /* 1: fldigi's AFC (progStatus.afconoff); 0: off (bench) */
    int squelch_on;          /* 1: fldigi's squelch (progStatus.sqlonoff); 0: off (bench) */
    double squelch;          /* squelch level in fldigi metric units 0 .. 100 */
    int reverse;             /* 1: fldigi's REV (QPSK) */
    int pilot;               /* 1: fldigi's pskpilot (single-carrier vestigial AFC) */
} fpsk_v_rx_config;

typedef struct fpsk_v_rx fpsk_v_rx;

/* NULL for an unknown submode, a carrier outside fldigi's range (bandwidth/2 .. 3000 Hz -
 * bandwidth/2) or out of memory. All memory is allocated here. */
fpsk_v_rx *fpsk_v_rx_create(const fpsk_v_rx_config *cfg);
void fpsk_v_rx_destroy(fpsk_v_rx *rx);
void fpsk_v_rx_reset(fpsk_v_rx *rx);
/* Feed n samples at the submode rate (fldigi's sound card scale: int16 / 32768). Writes the
 * bytes fldigi hands to put_rx_char() to text and returns their number; bytes beyond
 * max_text are dropped and counted (fpsk_v_rx_dropped). The audio is processed in fldigi's
 * 512-sample sound card blocks, counted across calls, so the result does not depend on how
 * the audio is split into calls. */
size_t fpsk_v_rx_process(fpsk_v_rx *rx, const double *x, size_t n, unsigned char *text,
                         size_t max_text);
double fpsk_v_rx_carrier_hz(const fpsk_v_rx *rx);   /* current centre (moves with AFC) */
void fpsk_v_rx_set_carrier(fpsk_v_rx *rx, double carrier_hz);   /* clamped as fldigi */
double fpsk_v_rx_metric(const fpsk_v_rx *rx);       /* fldigi's metric 0 .. 100 */
int fpsk_v_rx_dcd(const fpsk_v_rx *rx);
size_t fpsk_v_rx_dropped(const fpsk_v_rx *rx);
size_t fpsk_v_rx_footprint(const fpsk_v_rx *rx);    /* bytes allocated at create */

/*
 * Transmitter: psk::tx_process one state step per call. Output: the samples fldigi hands to
 * modem::ModulateXmtr (each symbol divided by its own peak), before the TX level (multiply
 * by 10^(txlevel/20), fldigi's -12 dB, to reproduce fldigi's WAV). With im != NULL the
 * analytic signal of the same symbols is written instead (re + j im, no peak division,
 * envelope peak <= 1), as fpsk_tx does. rate is the submode rate or 48000 (symbols
 * lengthened by 48000 / rate). Every call needs max >= fpsk_v_tx_bound(); otherwise it
 * writes nothing, returns 0 and leaves the state unchanged.
 */
typedef struct fpsk_v_tx_config {
    const char *submode;
    int rate;                /* 0 or the submode rate: fldigi's rate; or 48000 */
    double carrier_hz;       /* centre of the carriers, clamped as for RX */
    int reverse;             /* 1: fldigi's REV (QPSK) */
    int pilot;               /* 1: fldigi's pskpilot vestigial carrier (8PSK, OFDM) */
} fpsk_v_tx_config;

typedef struct fpsk_v_tx fpsk_v_tx;

fpsk_v_tx *fpsk_v_tx_create(const fpsk_v_tx_config *cfg);
void fpsk_v_tx_destroy(fpsk_v_tx *tx);
size_t fpsk_v_tx_bound(const fpsk_v_tx *tx);      /* largest output of one call, samples */
int fpsk_v_tx_rate(const fpsk_v_tx *tx);
/* tx_init and the preamble (psk.cxx:196-234, 2557-2621). */
size_t fpsk_v_tx_begin(fpsk_v_tx *tx, double *re, double *im, size_t max);
/* One character (tx_char, psk.cxx:2470-2493). */
size_t fpsk_v_tx_put(fpsk_v_tx *tx, unsigned char c, double *re, double *im, size_t max);
/* fldigi with an empty TX buffer (GET_TX_CHAR_NODATA, psk.cxx:2636-2646). */
size_t fpsk_v_tx_idle(fpsk_v_tx *tx, double *re, double *im, size_t max);
/* End of the over (tx_flush, psk.cxx:2496-2543), including fldigi's 2048 zero samples. */
size_t fpsk_v_tx_end(fpsk_v_tx *tx, double *re, double *im, size_t max);

#ifdef __cplusplus
}
#endif

#endif
