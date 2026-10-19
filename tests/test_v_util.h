/*
 * fldigi-psk-core test helpers for the PSK variants (header-only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSK_V_TEST_UTIL_H
#define PSK_V_TEST_UTIL_H

#include "fldigi_psk.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures __attribute__((unused));
static int passes __attribute__((unused));

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                \
            fputc('\n', stderr);                                                         \
        } else {                                                                         \
            passes++;                                                                    \
        }                                                                                \
    } while (0)

/* The platform the goldens were made on (fldigi 4.2.13 bench, macOS arm64, Apple clang -O2
 * with its default FP contraction): there the port must reproduce fldigi bit for bit. The
 * sanitizer build (-O1) defines PSKV_NOT_REFERENCE. */
#if defined(__APPLE__) && defined(__aarch64__) && !defined(PSKV_NOT_REFERENCE)
#define REFERENCE_PLATFORM 1
#else
#define REFERENCE_PLATFORM 0
#endif

static inline void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* A whole over as fldigi's bench sends it: begin, the text, end (re only, or re + im).
 * Returns malloc'ed buffers of *n samples. */
static inline double *render_over_iq(const char *submode, const char *text, double carrier,
                                     int rate, int pilot, double **im_out, size_t *n)
{
    fpsk_v_tx_config c;
    fpsk_v_tx *tx;
    size_t bound, cap, pos = 0, len = strlen(text), i;
    double *re, *im = NULL;

    memset(&c, 0, sizeof c);
    c.submode = submode;
    c.carrier_hz = carrier;
    c.rate = rate;
    c.pilot = pilot;
    tx = fpsk_v_tx_create(&c);
    if (!tx) {
        *n = 0;
        return NULL;
    }
    bound = fpsk_v_tx_bound(tx);
    cap = bound * (len + 2);
    re = (double *)malloc(cap * sizeof(double));
    if (im_out)
        im = (double *)malloc(cap * sizeof(double));
    pos += fpsk_v_tx_begin(tx, re + pos, im ? im + pos : NULL, cap - pos);
    for (i = 0; i < len; i++)
        pos += fpsk_v_tx_put(tx, (unsigned char)text[i], re + pos, im ? im + pos : NULL,
                             cap - pos);
    pos += fpsk_v_tx_end(tx, re + pos, im ? im + pos : NULL, cap - pos);
    fpsk_v_tx_destroy(tx);
    if (im_out)
        *im_out = im;
    *n = pos;
    return re;
}

static inline double *render_over(const char *submode, const char *text, double carrier,
                                  int pilot, size_t *n)
{
    return render_over_iq(submode, text, carrier, 0, pilot, NULL, n);
}

static inline fpsk_v_rx *make_rx(const char *submode, double carrier, int afc, int pilot)
{
    fpsk_v_rx_config c;
    memset(&c, 0, sizeof c);
    c.submode = submode;
    c.carrier_hz = carrier;
    c.afc = afc;
    c.pilot = pilot;
    return fpsk_v_rx_create(&c);
}

/* Decode x in calls of chunk samples (0: one call); malloc'ed text of *len bytes. */
static inline unsigned char *decode(fpsk_v_rx *rx, const double *x, size_t n, size_t chunk,
                                    size_t *len)
{
    size_t cap = 1 << 16, got = 0, i, step;
    unsigned char *out = (unsigned char *)malloc(cap);
    if (!chunk)
        chunk = n ? n : 1;
    for (i = 0; i < n; i += step) {
        step = n - i < chunk ? n - i : chunk;
        got += fpsk_v_rx_process(rx, x + i, step, out + got, cap - got);
    }
    *len = got;
    return out;
}

static inline int contains(const unsigned char *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    for (i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0)
            return 1;
    return 0;
}

/* Smallest edit distance of the text to any window of the output (CER numerator). */
static inline size_t window_distance(const unsigned char *t, size_t nt, const unsigned char *o,
                                     size_t no)
{
    size_t *prev = (size_t *)malloc((no + 1) * sizeof(size_t));
    size_t *cur = (size_t *)malloc((no + 1) * sizeof(size_t));
    size_t i, j, best;
    for (j = 0; j <= no; j++)
        prev[j] = 0;
    for (i = 1; i <= nt; i++) {
        cur[0] = i;
        for (j = 1; j <= no; j++) {
            size_t a = prev[j - 1] + (t[i - 1] != o[j - 1]), b = prev[j] + 1, c = cur[j - 1] + 1;
            cur[j] = a < b ? (a < c ? a : c) : (b < c ? b : c);
        }
        memcpy(prev, cur, (no + 1) * sizeof(size_t));
    }
    best = prev[0];
    for (j = 1; j <= no; j++)
        if (prev[j] < best)
            best = prev[j];
    free(prev);
    free(cur);
    return best;
}

#endif
