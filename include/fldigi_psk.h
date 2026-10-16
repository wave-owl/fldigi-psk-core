/*
 * fldigi-psk-core: BPSK receiver for PSK31/63/125 after fldigi 4.2.13 psk::rx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (src/psk/psk.cxx, src/psk/pskcoeff.cxx),
 * Copyright (C) Dave Freese W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contract "psk-c1", ABI 1. Audio in (doubles), decided bits out (1 = no phase reversal,
 * 0 = reversal); the varicode step is left to the caller.
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

/* "fldigi-psk-core <x.y.z> contract=psk-c1 abi=1" */
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

#ifdef __cplusplus
}
#endif

#endif
