/*
 * C tests of the fldigi-psk-core transmitter against the fldigi 4.2.13 reference vectors.
 *
 * Usage: test_psk_tx <golden dir>   (reads <dir>/tx/<name>.{s16,bits})
 *
 * <name>.s16 is the headless fldigi TX output (8000 Hz, carrier 1500 Hz, txlevel -12 dB,
 * int16); <name>.bits is the varicode bit stream of the same text (make_tx_golden.py).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fldigi_psk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    double baud;
} tx_case;

static const tx_case CASES[] = {
    {"psk31_cq", 31.25},
    {"psk63_cq", 62.5},
    {"psk125_pangram", 125.0},
};
#define NCASES (sizeof CASES / sizeof CASES[0])

static const char *g_dir;
static int g_failures, g_passes;

static void report(const char *test, int ok, const char *detail)
{
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", test, detail[0] ? ": " : "", detail);
    if (ok)
        g_passes++;
    else
        g_failures++;
}

static FILE *open_golden(const char *name, const char *ext, const char *mode)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/tx/%s.%s", g_dir, name, ext);
    return fopen(path, mode);
}

/* int16 little-endian samples; NULL on error. */
static short *load_s16(const char *name, size_t *n)
{
    FILE *f = open_golden(name, "s16", "rb");
    unsigned char *raw;
    short *pcm;
    long len;
    size_t i;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    raw = malloc((size_t)len);
    pcm = malloc((size_t)len);
    if (!raw || !pcm || fread(raw, 1, (size_t)len, f) != (size_t)len) {
        free(raw);
        free(pcm);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len / 2;
    for (i = 0; i < *n; i++)
        pcm[i] = (short)(raw[2 * i] | (raw[2 * i + 1] << 8));
    free(raw);
    return pcm;
}

/* The 0/1 line after the '#' comment lines; NULL on error. */
static unsigned char *load_bits(const char *name, size_t *n)
{
    FILE *f = open_golden(name, "bits", "r");
    static char line[65536];
    unsigned char *bits = NULL;
    size_t i;

    if (!f)
        return NULL;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        *n = strcspn(line, "\r\n");
        bits = malloc(*n ? *n : 1);
        for (i = 0; bits && i < *n; i++)
            bits[i] = (unsigned char)(line[i] == '1');
        break;
    }
    fclose(f);
    return bits;
}

static fpsk_tx *make_tx(int rate, double baud, double carrier_hz)
{
    fpsk_tx_config cfg;
    cfg.rate = rate;
    cfg.baud = baud;
    cfg.carrier_hz = carrier_hz;
    return fpsk_tx_create(&cfg);
}

/* Sample count of preamble + bits + postamble. */
static size_t over_len(const fpsk_tx *tx, int rate, size_t nbits)
{
    size_t dcd = fpsk_tx_preamble_symbols(tx), sl = fpsk_tx_symbol_len(tx);
    return (2 * dcd + nbits) * sl + 2048 * (size_t)rate / 8000;
}

/*
 * Render a whole over; bits are passed in chunks of `chunk` (0 = one call).
 * Returns the sample count, 0 on failure. im may be NULL (real mode).
 */
static size_t render(fpsk_tx *tx, const unsigned char *bits, size_t nbits, size_t chunk,
                     double *re, double *im, size_t max)
{
    size_t pos = 0, k = 0, got;

    got = fpsk_tx_preamble(tx, re, im, max);
    if (!got)
        return 0;
    pos += got;
    if (!chunk)
        chunk = nbits ? nbits : 1;
    while (k < nbits) {
        size_t m = nbits - k < chunk ? nbits - k : chunk;
        got = fpsk_tx_bits(tx, bits + k, m, re + pos, im ? im + pos : NULL, max - pos);
        if (got != m * fpsk_tx_symbol_len(tx))
            return 0;
        pos += got;
        k += m;
    }
    got = fpsk_tx_postamble(tx, re + pos, im ? im + pos : NULL, max - pos);
    if (!got)
        return 0;
    return pos + got;
}

/* AC-784.1: real mode at 8 kHz equals the fldigi vector within 1 LSB, same length. */
static void test_fldigi_vector(const tx_case *c)
{
    char test[128], detail[256];
    size_t npcm = 0, nbits = 0, n, i, ndiff = 0;
    short *pcm = load_s16(c->name, &npcm);
    unsigned char *bits = load_bits(c->name, &nbits);
    fpsk_tx *tx = make_tx(8000, c->baud, 1500.0);
    double *re = NULL, gain = pow(10.0, -12.0 / 20.0);
    long maxdiff = 0;

    snprintf(test, sizeof test, "fldigi vector %s", c->name);
    if (!pcm || !bits || !tx) {
        report(test, 0, "setup failed");
        goto out;
    }
    n = over_len(tx, 8000, nbits);
    re = malloc(n * sizeof *re);
    if (!re || render(tx, bits, nbits, 0, re, NULL, n) != n) {
        report(test, 0, "render failed");
        goto out;
    }
    if (n != npcm) {
        snprintf(detail, sizeof detail, "length %zu, fldigi %zu", n, npcm);
        report(test, 0, detail);
        goto out;
    }
    for (i = 0; i < n; i++) {
        long d = labs(lrint(re[i] * gain * 32767.0) - (long)pcm[i]);
        if (d > maxdiff)
            maxdiff = d;
        ndiff += d != 0;
    }
    snprintf(detail, sizeof detail, "%zu samples, %zu bits, max diff %ld LSB (%zu samples off)",
             n, nbits, maxdiff, ndiff);
    report(test, maxdiff <= 1, detail);
out:
    free(re);
    free(pcm);
    free(bits);
    fpsk_tx_destroy(tx);
}

/*
 * AC-784.2: at 48 kHz the IQ real part divided by each symbol's peak equals the real mode;
 * the IQ envelope never exceeds 1 and the zero tail matches.
 */
static void test_iq_matches_real(const tx_case *c)
{
    char test[128], detail[256];
    size_t nbits = 0, n, i, sl, s;
    unsigned char *bits = load_bits(c->name, &nbits);
    fpsk_tx *txr = make_tx(48000, c->baud, 1000.0), *txi = make_tx(48000, c->baud, 1000.0);
    double *real = NULL, *re = NULL, *im = NULL, maxerr = 0.0, peak = 0.0;

    snprintf(test, sizeof test, "iq vs real 48k %s", c->name);
    if (!bits || !txr || !txi) {
        report(test, 0, "setup failed");
        goto out;
    }
    n = over_len(txr, 48000, nbits);
    sl = fpsk_tx_symbol_len(txr);
    real = malloc(n * sizeof *real);
    re = malloc(n * sizeof *re);
    im = malloc(n * sizeof *im);
    if (!real || !re || !im || render(txr, bits, nbits, 0, real, NULL, n) != n
        || render(txi, bits, nbits, 0, re, im, n) != n) {
        report(test, 0, "render failed");
        goto out;
    }
    for (s = 0; s * sl < n; s++) {
        size_t a = s * sl, b = a + sl < n ? a + sl : n;
        double maxamp = 0.0;
        for (i = a; i < b; i++)
            if (maxamp < fabs(re[i]))
                maxamp = fabs(re[i]);
        for (i = a; i < b; i++) {
            double want = maxamp != 0.0 ? re[i] / maxamp : re[i];
            double e = fabs(want - real[i]);
            double env = sqrt(re[i] * re[i] + im[i] * im[i]);
            if (e > maxerr)
                maxerr = e;
            if (env > peak)
                peak = env;
        }
    }
    snprintf(detail, sizeof detail, "max error %.3g, envelope peak %.15f", maxerr, peak);
    report(test, maxerr <= 1e-12 && peak <= 1.0 + 1e-12, detail);
out:
    free(real);
    free(re);
    free(im);
    free(bits);
    fpsk_tx_destroy(txr);
    fpsk_tx_destroy(txi);
}

/* Splitting the bits over several calls gives the same samples as one call. */
static void test_chunk_invariance(const tx_case *c)
{
    static const size_t CHUNKS[] = {1, 3, 7, 64};
    char test[128], detail[128];
    size_t nbits = 0, n, k;
    unsigned char *bits = load_bits(c->name, &nbits);
    fpsk_tx *tx = make_tx(48000, c->baud, 1000.0);
    double *re1 = NULL, *im1 = NULL, *re2 = NULL, *im2 = NULL;
    int same = 1;

    snprintf(test, sizeof test, "chunk invariance %s", c->name);
    if (!bits || !tx) {
        report(test, 0, "setup failed");
        goto out;
    }
    n = over_len(tx, 48000, nbits);
    re1 = malloc(n * sizeof *re1);
    im1 = malloc(n * sizeof *im1);
    re2 = malloc(n * sizeof *re2);
    im2 = malloc(n * sizeof *im2);
    if (!re1 || !im1 || !re2 || !im2 || render(tx, bits, nbits, 0, re1, im1, n) != n) {
        report(test, 0, "render failed");
        goto out;
    }
    for (k = 0; k < sizeof CHUNKS / sizeof CHUNKS[0] && same; k++) {
        fpsk_tx_reset(tx);
        same = render(tx, bits, nbits, CHUNKS[k], re2, im2, n) == n
               && !memcmp(re1, re2, n * sizeof *re1) && !memcmp(im1, im2, n * sizeof *im1);
        snprintf(detail, sizeof detail, "chunk %zu", CHUNKS[k]);
    }
    report(test, same, same ? "" : detail);
out:
    free(re1);
    free(im1);
    free(re2);
    free(im2);
    free(bits);
    fpsk_tx_destroy(tx);
}

static void test_create_rejects(void)
{
    int ok = !make_tx(44100, 31.25, 1000.0) && !make_tx(8000, 50.0, 1000.0)
             && !make_tx(48000, 31.25, -1.0) && !make_tx(8000, 31.25, 4000.0)
             && !fpsk_tx_create(NULL);
    report("create rejects rate 44100, baud 50, carrier out of range, NULL", ok, "");
}

static void test_sizes(void)
{
    static const double BAUDS[] = {31.25, 62.5, 125.0};
    static const size_t SL8[] = {256, 128, 64}, SL48[] = {1536, 768, 384}, DCD[] = {32, 64, 128};
    int ok = 1;
    size_t i;

    for (i = 0; i < 3; i++) {
        fpsk_tx *a = make_tx(8000, BAUDS[i], 1500.0), *b = make_tx(48000, BAUDS[i], 1000.0);
        ok = ok && a && b && fpsk_tx_symbol_len(a) == SL8[i] && fpsk_tx_symbol_len(b) == SL48[i]
             && fpsk_tx_preamble_symbols(a) == DCD[i] && fpsk_tx_preamble_symbols(b) == DCD[i];
        fpsk_tx_destroy(a);
        fpsk_tx_destroy(b);
    }
    report("symbol length 256/128/64 (8k), 1536/768/384 (48k), dcdbits 32/64/128", ok, "");
}

/* max too small (or re NULL): returns 0 and leaves the state unchanged. */
static void test_max_too_small(void)
{
    static const unsigned char BITS[] = {1, 0, 1, 1, 0, 0};
    fpsk_tx *a = make_tx(8000, 31.25, 1500.0), *b = make_tx(8000, 31.25, 1500.0);
    size_t n, sl;
    double *ra = NULL, *rb = NULL;
    int ok;

    if (!a || !b) {
        report("max too small", 0, "setup failed");
        goto out;
    }
    sl = fpsk_tx_symbol_len(a);
    n = over_len(a, 8000, sizeof BITS);
    ra = malloc(n * sizeof *ra);
    rb = malloc(n * sizeof *rb);
    if (!ra || !rb) {
        report("max too small", 0, "setup failed");
        goto out;
    }
    ok = fpsk_tx_preamble(a, ra, NULL, 32 * sl - 1) == 0
         && fpsk_tx_preamble(a, NULL, NULL, n) == 0
         && fpsk_tx_preamble(a, ra, NULL, n) == 32 * sl
         && fpsk_tx_bits(a, BITS, sizeof BITS, ra + 32 * sl, NULL, sizeof BITS * sl - 1) == 0
         && fpsk_tx_bits(a, BITS, sizeof BITS, ra + 32 * sl, NULL, n) == sizeof BITS * sl
         && fpsk_tx_postamble(a, ra, NULL, 32 * sl + 2047) == 0
         && fpsk_tx_postamble(a, ra + (32 + sizeof BITS) * sl, NULL, n) == 32 * sl + 2048
         && render(b, BITS, sizeof BITS, 0, rb, NULL, n) == n
         && !memcmp(ra, rb, n * sizeof *ra);
    report("max too small returns 0 without a state change", ok, "");
out:
    free(ra);
    free(rb);
    fpsk_tx_destroy(a);
    fpsk_tx_destroy(b);
}

static void test_version(void)
{
    const char *v = fpsk_version();
    report("version has contract=psk-c1 abi=1 tx=1",
           v && strstr(v, "contract=psk-c1") && strstr(v, "abi=1") && strstr(v, "tx=1"),
           v ? v : "NULL");
}

int main(int argc, char **argv)
{
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <golden dir>\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    test_version();
    test_create_rejects();
    test_sizes();
    test_max_too_small();
    for (i = 0; i < NCASES; i++) {
        test_fldigi_vector(&CASES[i]);
        test_iq_matches_real(&CASES[i]);
        test_chunk_invariance(&CASES[i]);
    }
    printf("%s: %d passed, %d failure(s)\n", g_failures ? "FAILED" : "OK", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
