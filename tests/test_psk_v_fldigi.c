/*
 * fldigi-psk-core PSK variants against fldigi 4.2.13 itself (goldens from
 * tests/golden/variants/make_fldigi_golden.py, the headless fldigi bench build).
 *
 * TX, all 56 psk modes of fldigi (the 53 variants and BPSK31/63/125), a CQ over and a
 * pangram over each, plus AFC and vestigial-pilot overs: the port renders fldigi's TX vector
 * (same length; FNV-1a hash of the int16 output identical on the reference platform).
 * RX, the same overs clean and at -5 / -10 / -15 dB (2500 Hz), the AFC/pilot overs with
 * fldigi's AFC: the decoded bytes equal fldigi's own decode byte for byte on the reference
 * platform. Elsewhere (another libm, no FMA) the sent text must be recovered with at most
 * 1 + 5 % more errors than fldigi's own decode on the clean overs, and in total on the noisy
 * ones within 20 % + 10 errors.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_v_util.h"

static const char *golden = "tests/golden/variants";

static uint64_t fnv1a64(const int16_t *pcm, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        uint16_t u = (uint16_t)pcm[i];
        h ^= (uint8_t)(u & 0xFF);
        h *= 0x100000001B3ULL;
        h ^= (uint8_t)(u >> 8);
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* fldigi's sound path: modem::ModulateXmtr scales by 10^(txlevel/20), txlevel -12 dB, and
 * clips at +-0.95 (trx/modem.cxx); libsndfile writes PCM16 as lrint(x * 0x7FFF). */
static void quantize(const double *x, size_t n, int16_t *pcm)
{
    double mult = pow(10, -12.0 / 20.0);
    size_t i;
    for (i = 0; i < n; i++) {
        double v = x[i] * mult;
        if (v < -0.95)
            v = -0.95;
        if (v > 0.95)
            v = 0.95;
        pcm[i] = (int16_t)lrint(v * 32767.0);
    }
}

/* Same as noisy() in make_fldigi_golden.py; this file is compiled without FP contraction,
 * so every step rounds like Python. */
static uint32_t lcg;
static double lcg_uniform(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return lcg / 4294967296.0;
}

static double *noisy(const int16_t *pcm, size_t n, int sr, int snr_step, uint32_t seed,
                     size_t *out_n)
{
    size_t lead = (size_t)sr, tail = 12 * (size_t)sr, total = lead + n + tail, i;
    double *x = (double *)calloc(total, sizeof(double));
    double ps = 0, inv, s2, sd, gain;
    int k;
    for (i = 0; i < n; i++)
        x[lead + i] = (double)pcm[i];
    *out_n = total;
    if (snr_step == 0) {
        for (i = 0; i < total; i++)
            x[i] /= 32768.0;
        return x;
    }
    for (i = 0; i < n; i++)
        ps += (double)pcm[i] * (double)pcm[i];
    ps = ps / (double)n;
    {
        double sq = sqrt(10.0);
        inv = snr_step == 1 ? 1.0 / sq : snr_step == 2 ? 1.0 / 10.0 : 1.0 / (10.0 * sq);
    }
    s2 = ps / inv * (sr / 2.0) / 2500.0;
    sd = sqrt(s2);
    gain = 3276.7 / sqrt(ps * (double)n / (double)total + s2);
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, y, q;
        for (k = 0; k < 12; k++)
            z += lcg_uniform();
        z -= 6.0;
        y = (x[i] + sd * z) * gain;
        q = floor(y + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;  /* libsndfile sf_readf_double: int16 / 32768 */
    }
    return x;
}

static char texts[2][160];

static const char *text_for(const char *key)
{
    return strcmp(key, "cq") == 0 ? texts[0] : texts[1];
}

static void load_texts(void)
{
    char path[512], *buf, *line;
    size_t len;
    snprintf(path, sizeof path, "%s/texts.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (strncmp(line, "cq ", 3) == 0)
            snprintf(texts[0], sizeof texts[0], "%s", line + 3);
        else if (strncmp(line, "pangram ", 8) == 0)
            snprintf(texts[1], sizeof texts[1], "%s", line + 8);
    }
    free(buf);
}

static int vectors_done, tx_identical, rx_exact, rx_total;
static size_t noisy_port, noisy_fldigi;
static int full_port, full_fldigi;
static const char *only;

static void check_vector(const char *stem, const char *sname, const char *key, int idx,
                         int sr, size_t samples, double ctx, double crx, int afc, int pilot,
                         uint64_t hash)
{
    static const char *snr_names[4] = {"clean", "m05", "m10", "m15"};
    const fpsk_v_submode *m = fpsk_v_submode_find(sname);
    const char *text = text_for(key);
    double *audio;
    int16_t *pcm;
    size_t n, ns = strlen(text);
    char path[512];
    int s, same;

    if (only && strcmp(only, stem) != 0)
        return;
    CHECK(m != NULL && sr == m->rate, "%s: submode %s at %d Hz", stem, sname, sr);
    if (!m)
        return;
    audio = render_over(m->name, text, ctx, pilot, &n);
    CHECK(audio != NULL, "%s: tx_create", stem);
    if (!audio)
        return;
    pcm = (int16_t *)malloc((n + 1) * sizeof(int16_t));
    quantize(audio, n, pcm);
    CHECK(n == samples, "%s: %zu samples, fldigi %zu", stem, n, samples);
    same = n == samples && fnv1a64(pcm, n) == hash;
    tx_identical += same;
    if (REFERENCE_PLATFORM)
        CHECK(same, "%s: TX hash %016llx, fldigi %016llx", stem,
              (unsigned long long)fnv1a64(pcm, n), (unsigned long long)hash);

    for (s = 0; s < 4; s++) {
        fpsk_v_rx *rx;
        size_t nx, got, glen;
        /* the inputs are built from the port's TX, identical to fldigi's (hash above) */
        double *x = noisy(pcm, n, sr, s, (uint32_t)(1000 * idx + s), &nx);
        unsigned char *out, *want;
        rx = make_rx(m->name, crx, afc, pilot);
        CHECK(rx != NULL, "%s: rx_create", stem);
        if (!rx) {
            free(x);
            continue;
        }
        /* odd call sizes: the result does not depend on the chunking */
        out = decode(rx, x, nx, 1000 + 37 * (size_t)s, &got);
        snprintf(path, sizeof path, "%s/rx/%s_%s.txt", golden, stem, snr_names[s]);
        want = (unsigned char *)read_file(path, &glen);
        CHECK(want != NULL, "%s: missing %s", stem, path);
        if (want) {
            int exact = got == glen && memcmp(out, want, glen) == 0;
            rx_total++;
            rx_exact += exact;
            if (REFERENCE_PLATFORM) {
                CHECK(exact, "%s %s: RX differs from fldigi (%zu vs %zu bytes)", stem,
                      snr_names[s], got, glen);
            } else {
                size_t dp = window_distance((const unsigned char *)text, ns, out, got);
                size_t df = window_distance((const unsigned char *)text, ns, want, glen);
                if (!exact)
                    printf("  %s %s: not identical; text errors port %zu, fldigi %zu\n", stem,
                           snr_names[s], dp, df);
                if (s == 0) {
                    CHECK(dp <= df + 1 + ns / 20, "%s clean: %zu text errors, fldigi %zu", stem,
                          dp, df);
                } else {
                    noisy_port += dp;
                    noisy_fldigi += df;
                    full_port += contains(out, got, text);
                    full_fldigi += contains(want, glen, text);
                }
            }
        }
        free(want);
        free(out);
        free(x);
        fpsk_v_rx_destroy(rx);
    }
    vectors_done++;
    free(pcm);
    free(audio);
}

int main(int argc, char **argv)
{
    char path[512], *buf, *line, *save = NULL;
    size_t len;

    if (argc > 1)
        golden = argv[1];
    if (argc > 2)
        only = argv[2];
    load_texts();
    snprintf(path, sizeof path, "%s/cases.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "missing goldens in %s\n", golden);
        return 2;
    }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char stem[64], sname[16], key[16];
        int idx, sr, afc, pilot;
        double ctx, crx;
        size_t samples;
        unsigned long long hash;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %15s %15s %d %d %zu %lf %lf %d %d %llx", stem, sname, key, &idx,
                   &sr, &samples, &ctx, &crx, &afc, &pilot, &hash) != 11) {
            CHECK(0, "bad line: %s", line);
            continue;
        }
        check_vector(stem, sname, key, idx, sr, samples, ctx, crx, afc, pilot,
                     (uint64_t)hash);
    }
    free(buf);
    if (!only)
        CHECK(vectors_done == 56 * 2 + 7, "%d vectors", vectors_done);
    if (!REFERENCE_PLATFORM) {
        printf("noisy decodes: text errors port %zu, fldigi %zu; complete texts port %d, "
               "fldigi %d\n", noisy_port, noisy_fldigi, full_port, full_fldigi);
        CHECK(noisy_port <= noisy_fldigi + noisy_fldigi / 5 + 10,
              "noisy decodes: %zu text errors, fldigi %zu", noisy_port, noisy_fldigi);
        CHECK(full_port + 5 >= full_fldigi, "complete texts: port %d, fldigi %d", full_port,
              full_fldigi);
    }
    printf("fldigi-psk-core variants vs fldigi 4.2.13: %d overs, TX identical in %d, RX "
           "identical in %d of %d decodes%s\n", vectors_done, tx_identical, rx_exact, rx_total,
           REFERENCE_PLATFORM ? " (reference platform)" : "");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
