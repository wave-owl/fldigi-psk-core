/*
 * Tests of fldigi-psk-core against the golden data in tests/golden (see make_golden.py).
 * Usage: test_psk <golden_dir>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fldigi_psk.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CASES 32
/* Longest golden bit sequence that can be stored. */
#define MAX_BITS 8192
#define RATE 8000.0
#define LCG_SEED 12345u

typedef struct {
    char name[64];
    char submode[16];
    double center_hz, freq_hz, snr_db;
    int afc;
    size_t chunk;
    char file[128];
    uint8_t bits[MAX_BITS];
    size_t nbits;
} golden_case;

static const char *g_dir;
static int g_failures, g_passes;
/* Calls whose bit count exceeded the documented bound n / (rate / baud / 16) + 2. */
static unsigned long g_bound_violations;

static void report(const char *test, int ok, const char *detail)
{
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", test, detail[0] ? ": " : "", detail);
    if (ok)
        g_passes++;
    else
        g_failures++;
}

static void path_join(char *out, size_t size, const char *file)
{
    snprintf(out, size, "%s/%s", g_dir, file);
}

/* Read a whole file; returns NULL on error. */
static unsigned char *read_file(const char *file, size_t *len)
{
    char path[1024];
    FILE *f;
    unsigned char *buf;
    long size;

    path_join(path, sizeof path, file);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

/* Load .s16 (little-endian int16, scaled by 1/32768) or .f64 (little-endian float64). */
static double *load_audio(const char *file, size_t *n)
{
    size_t len, i, dot = strlen(file);
    unsigned char *raw = read_file(file, &len);
    double *x = NULL;

    if (!raw)
        return NULL;
    if (dot > 4 && strcmp(file + dot - 4, ".s16") == 0) {
        *n = len / 2;
        x = malloc(*n * sizeof *x + 1);
        for (i = 0; x && i < *n; i++) {
            int16_t v = (int16_t)(uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
            x[i] = v / 32768.0;
        }
    } else if (dot > 4 && strcmp(file + dot - 4, ".f64") == 0) {
        *n = len / 8;
        x = malloc(*n * sizeof *x + 1);
        for (i = 0; x && i < *n; i++) {
            uint64_t bits = 0;
            int b;
            for (b = 7; b >= 0; b--)
                bits = (bits << 8) | raw[8 * i + (size_t)b];
            memcpy(&x[i], &bits, sizeof bits);
        }
    }
    free(raw);
    return x;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Unpack nbits bits from hex (MSB first, as np.packbits); 0 on malformed input. */
static int unpack_hex(const char *hex, size_t nbits, uint8_t *bits)
{
    size_t i;
    if (nbits == 0)
        return strcmp(hex, "-") == 0;
    if (strlen(hex) != 2 * ((nbits + 7) / 8))
        return 0;
    for (i = 0; i < nbits; i++) {
        int d = hex_digit(hex[i / 4]);
        if (d < 0)
            return 0;
        bits[i] = (uint8_t)((d >> (3 - i % 4)) & 1);
    }
    return 1;
}

static int load_expected(golden_case *cases, size_t *ncases)
{
    static char hex[2 * MAX_BITS];
    char path[1024];
    FILE *f;

    path_join(path, sizeof path, "expected.txt");
    f = fopen(path, "r");
    if (!f)
        return 0;
    *ncases = 0;
    while (*ncases < MAX_CASES) {
        golden_case *c = &cases[*ncases];
        unsigned long chunk, nbits;
        if (fscanf(f, "%63s %15s %lf %d %lu %lf %lf %127s %lu %16383s", c->name, c->submode,
                   &c->center_hz, &c->afc, &chunk, &c->freq_hz, &c->snr_db, c->file, &nbits,
                   hex) != 10)
            break;
        if (nbits > MAX_BITS || !unpack_hex(hex, (size_t)nbits, c->bits)) {
            fclose(f);
            return 0;
        }
        c->chunk = (size_t)chunk;
        c->nbits = (size_t)nbits;
        (*ncases)++;
    }
    fclose(f);
    return *ncases > 0;
}

static double submode_baud(const char *submode)
{
    if (strcmp(submode, "PSK31") == 0)
        return 31.25;
    if (strcmp(submode, "PSK63") == 0)
        return 62.5;
    if (strcmp(submode, "PSK125") == 0)
        return 125.0;
    return 0.0;
}

static fpsk_config make_config(const char *submode, double center_hz, int afc)
{
    fpsk_config cfg;
    cfg.sample_rate = RATE;
    cfg.baud = submode_baud(submode);
    cfg.center_hz = center_hz;
    cfg.afc = afc;
    cfg.afc_range_hz = 0.0;
    return cfg;
}

/* Input samples per decimated sample: rate / baud / 16 (16, 8 or 4 at 8 kHz). */
static size_t decim_of(double baud)
{
    return (size_t)(RATE / baud / 16.0 + 0.5);
}

/* Documented upper bound of the bits returned by one call of n samples. */
static size_t call_bound(size_t n, size_t decim)
{
    return n / decim + 2;
}

/* Bit buffer large enough for n samples split into any number of calls (<= n). */
static uint8_t *alloc_bits(size_t n, size_t decim)
{
    return malloc(call_bound(n, decim) + 2 * n);
}

/* One process() call; counts violations of the documented per-call bound. */
static size_t process(fpsk_rx *rx, const double *x, size_t len, size_t decim, uint8_t *bits,
                      size_t room)
{
    size_t bound = call_bound(len, decim), got;
    if (room < bound)
        g_bound_violations++;
    got = fpsk_rx_process(rx, x, len, bits, room < bound ? room : bound);
    if (got > bound)
        g_bound_violations++;
    return got;
}

/*
 * Feed x in calls of `chunk` samples (0 = one call) into bits (sized by alloc_bits);
 * returns the number of bits.
 */
static size_t run(fpsk_rx *rx, const double *x, size_t n, size_t decim, size_t chunk,
                  uint8_t *bits)
{
    size_t got = 0, i, cap = call_bound(n, decim) + 2 * n;

    if (chunk == 0)
        return process(rx, x, n, decim, bits, cap);
    for (i = 0; i < n; i += chunk) {
        size_t len = n - i < chunk ? n - i : chunk;
        got += process(rx, x + i, len, decim, bits + got, cap - got);
    }
    return got;
}

/* Feed x in pseudo-random calls of 1..4000 samples (fixed-seed LCG). */
static size_t run_random(fpsk_rx *rx, const double *x, size_t n, size_t decim, uint8_t *bits)
{
    uint32_t state = LCG_SEED;
    size_t got = 0, i = 0, cap = call_bound(n, decim) + 2 * n;

    while (i < n) {
        size_t len;
        state = state * 1664525u + 1013904223u;
        len = 1 + (size_t)((state >> 8) % 4000u);
        if (len > n - i)
            len = n - i;
        got += process(rx, x + i, len, decim, bits + got, cap - got);
        i += len;
    }
    return got;
}

static int same_bits(const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    return na == nb && (na == 0 || memcmp(a, b, na) == 0);
}

/* Index of the first differing bit, or the shorter length. */
static size_t first_diff(const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    size_t i, m = na < nb ? na : nb;
    for (i = 0; i < m; i++)
        if (a[i] != b[i])
            return i;
    return m;
}

static void test_golden(const golden_case *c)
{
    char test[128], detail[256];
    size_t n = 0, got;
    double *x = load_audio(c->file, &n);
    fpsk_config cfg = make_config(c->submode, c->center_hz, c->afc);
    size_t decim = decim_of(cfg.baud);
    uint8_t *bits = alloc_bits(n, decim);
    fpsk_rx *rx;
    double snr, freq;
    int ok;

    snprintf(test, sizeof test, "golden %s", c->name);
    if (!x || !bits) {
        report(test, 0, "cannot load audio");
        free(x);
        free(bits);
        return;
    }
    rx = fpsk_rx_create(&cfg);
    if (!rx) {
        report(test, 0, "create failed");
        free(x);
        free(bits);
        return;
    }
    got = run(rx, x, n, decim, c->chunk, bits);
    snr = fpsk_rx_snr_db(rx);
    freq = fpsk_rx_freq_hz(rx);
    ok = same_bits(bits, got, c->bits, c->nbits) && fabs(snr - c->snr_db) <= 0.01
         && fabs(freq - c->freq_hz) <= 0.01;
    snprintf(detail, sizeof detail,
             "%lu bits (expected %lu, first diff %lu), freq %.4f Hz (expected %.4f), "
             "snr %.4f dB (expected %.4f)",
             (unsigned long)got, (unsigned long)c->nbits,
             (unsigned long)first_diff(bits, got, c->bits, c->nbits), freq, c->freq_hz, snr,
             c->snr_db);
    report(test, ok, detail);
    fpsk_rx_destroy(rx);
    free(x);
    free(bits);
}

/* The documented per-call bound holds for every golden case fed in random chunks. */
static void test_bound(const golden_case *c)
{
    char test[128], detail[128];
    size_t n = 0, got;
    double *x = load_audio(c->file, &n);
    fpsk_config cfg = make_config(c->submode, c->center_hz, c->afc);
    size_t decim = decim_of(cfg.baud);
    uint8_t *bits = alloc_bits(n, decim);
    fpsk_rx *rx = fpsk_rx_create(&cfg);
    unsigned long before = g_bound_violations;

    snprintf(test, sizeof test, "bit bound %s", c->name);
    if (!x || !bits || !rx) {
        report(test, 0, "setup failed");
    } else {
        got = run_random(rx, x, n, decim, bits);
        snprintf(detail, sizeof detail, "%lu bits from %lu samples, %lu violation(s)",
                 (unsigned long)got, (unsigned long)n, g_bound_violations - before);
        report(test, got > 0 && g_bound_violations == before, detail);
    }
    fpsk_rx_destroy(rx);
    free(x);
    free(bits);
}

static void test_taps(const char *submode)
{
    static const int which[2] = {FPSK_FIR1, FPSK_FIR2};
    fpsk_config cfg = make_config(submode, 1500.0, 0);
    fpsk_rx *rx = fpsk_rx_create(&cfg);
    int w;

    for (w = 0; w < 2; w++) {
        char file[64], path[1024], test[64], detail[128];
        double taps[128], ref, maxerr = 0.0;
        size_t len, nref = 0;
        int ok = 1;
        FILE *f;

        snprintf(file, sizeof file, "taps_%s_fir%d.txt", submode, w + 1);
        snprintf(test, sizeof test, "taps %s fir%d", submode, w + 1);
        path_join(path, sizeof path, file);
        f = fopen(path, "r");
        if (!rx || !f) {
            report(test, 0, "setup failed");
            if (f)
                fclose(f);
            continue;
        }
        len = fpsk_rx_taps(rx, which[w], taps, sizeof taps / sizeof taps[0]);
        while (fscanf(f, "%lf", &ref) == 1) {
            if (nref >= len) {
                ok = 0;
                break;
            }
            if (fabs(taps[nref] - ref) > maxerr)
                maxerr = fabs(taps[nref] - ref);
            nref++;
        }
        fclose(f);
        ok = ok && len > 0 && nref == len && maxerr <= 1e-12;
        snprintf(detail, sizeof detail, "%lu taps (golden %lu), max error %.3g",
                 (unsigned long)len, (unsigned long)nref, maxerr);
        report(test, ok, detail);
    }
    report("taps invalid selector", rx && fpsk_rx_taps(rx, 3, NULL, 0) == 0, "");
    fpsk_rx_destroy(rx);
}

static void test_version(void)
{
    const char *v = fpsk_version();
    report("version", v && strstr(v, "contract=psk-c1") && strstr(v, "abi=1"), v ? v : "NULL");
}

/* One call vs random chunks on a clean vector; returns 1 if identical. */
static int chunk_compare(const golden_case *clean, int afc, char *detail, size_t size)
{
    size_t n = 0, nref, got;
    double *x = load_audio(clean->file, &n);
    fpsk_config cfg = make_config(clean->submode, clean->center_hz, afc);
    size_t decim = decim_of(cfg.baud);
    uint8_t *ref = alloc_bits(n, decim), *bits = alloc_bits(n, decim);
    fpsk_rx *rx = fpsk_rx_create(&cfg);
    double f1, f2;
    int same;

    if (!x || !rx || !ref || !bits) {
        snprintf(detail, size, "setup failed");
        free(x);
        free(ref);
        free(bits);
        fpsk_rx_destroy(rx);
        return 0;
    }
    nref = run(rx, x, n, decim, 0, ref);
    f1 = fpsk_rx_freq_hz(rx);
    fpsk_rx_reset(rx);
    got = run_random(rx, x, n, decim, bits);
    f2 = fpsk_rx_freq_hz(rx);
    same = nref > 0 && same_bits(bits, got, ref, nref);
    snprintf(detail, size, "%lu bits (one call: %lu, first diff %lu), freq %.6f / %.6f Hz",
             (unsigned long)got, (unsigned long)nref,
             (unsigned long)first_diff(bits, got, ref, nref), f2, f1);
    fpsk_rx_destroy(rx);
    free(x);
    free(ref);
    free(bits);
    return same;
}

static void test_chunk_invariance(const golden_case *clean)
{
    char test[128], detail[256];
    int same;

    snprintf(test, sizeof test, "chunk invariance afc=0 %s", clean->name);
    same = chunk_compare(clean, 0, detail, sizeof detail);
    report(test, same, detail);

    /* With AFC the correction takes effect per call-aligned symbol block (see header), so
     * call splitting may change the bits in general; on the clean vectors they are
     * identical. */
    snprintf(test, sizeof test, "chunk invariance afc=1 %s", clean->name);
    same = chunk_compare(clean, 1, detail, sizeof detail);
    report(test, same, detail);
}

static void test_truncation(const golden_case *clean)
{
    uint8_t buf[12], guard[8];
    size_t n, got;
    double *x = load_audio(clean->file, &n);
    fpsk_config cfg = make_config(clean->submode, clean->center_hz, clean->afc);
    fpsk_rx *rx = fpsk_rx_create(&cfg);
    char detail[96];

    if (!x || !rx) {
        report("max_bits truncation", 0, "setup failed");
        free(x);
        fpsk_rx_destroy(rx);
        return;
    }
    /* Only the first 4 bits may be written; the guard bytes behind them stay intact. */
    memset(guard, 0xAA, sizeof guard);
    memset(buf, 0xAA, sizeof buf);
    got = fpsk_rx_process(rx, x, n, buf, 4);
    snprintf(detail, sizeof detail, "returned %lu (expected %lu)",
             (unsigned long)got, (unsigned long)clean->nbits);
    report("max_bits truncation",
           got == clean->nbits && memcmp(buf, clean->bits, 4) == 0
           && memcmp(buf + 4, guard, sizeof guard) == 0, detail);
    fpsk_rx_reset(rx);
    got = fpsk_rx_process(rx, x, n, NULL, 0);
    report("max_bits zero", got == clean->nbits, "");
    fpsk_rx_destroy(rx);
    free(x);
}

static void test_invalid_config(void)
{
    fpsk_config cfg = make_config("PSK31", 1500.0, 0);
    fpsk_rx *rx;

    cfg.baud = 50.0;
    rx = fpsk_rx_create(&cfg);
    report("invalid config baud 50", rx == NULL, "");
    fpsk_rx_destroy(rx);

    cfg = make_config("PSK31", 1500.0, 0);
    cfg.sample_rate = 12000.0;
    rx = fpsk_rx_create(&cfg);
    report("invalid config sample_rate 12000", rx == NULL, "");
    fpsk_rx_destroy(rx);

    report("create NULL config", fpsk_rx_create(NULL) == NULL, "");
}

static void test_reset(const golden_case *c)
{
    size_t n = 0, n1, n2;
    double *x = load_audio(c->file, &n);
    fpsk_config cfg = make_config(c->submode, c->center_hz, c->afc);
    size_t decim = decim_of(cfg.baud);
    uint8_t *first = alloc_bits(n, decim), *second = alloc_bits(n, decim);
    fpsk_rx *rx = fpsk_rx_create(&cfg);
    double snr1, snr2, f1, f2;
    char test[96];

    snprintf(test, sizeof test, "reset %s", c->name);
    if (!x || !rx || !first || !second) {
        report(test, 0, "setup failed");
        free(x);
        free(first);
        free(second);
        fpsk_rx_destroy(rx);
        return;
    }
    n1 = run(rx, x, n, decim, c->chunk, first);
    snr1 = fpsk_rx_snr_db(rx);
    f1 = fpsk_rx_freq_hz(rx);
    fpsk_rx_reset(rx);
    n2 = run(rx, x, n, decim, c->chunk, second);
    snr2 = fpsk_rx_snr_db(rx);
    f2 = fpsk_rx_freq_hz(rx);
    report(test, same_bits(first, n1, second, n2) && snr1 == snr2 && f1 == f2, "");
    fpsk_rx_destroy(rx);
    free(x);
    free(first);
    free(second);
}

static void test_set_freq(void)
{
    fpsk_config cfg = make_config("PSK31", 1500.0, 0);
    fpsk_rx *rx = fpsk_rx_create(&cfg);
    char detail[96];
    double before, after;

    if (!rx) {
        report("set_freq", 0, "create failed");
        return;
    }
    before = fpsk_rx_freq_hz(rx);
    fpsk_rx_set_freq(rx, 1234.5);
    after = fpsk_rx_freq_hz(rx);
    snprintf(detail, sizeof detail, "%.3f Hz -> %.3f Hz", before, after);
    report("set_freq", before == 1500.0 && after == 1234.5, detail);
    fpsk_rx_destroy(rx);
}

static const golden_case *find_case(const golden_case *cases, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(cases[i].name, name) == 0)
            return &cases[i];
    return NULL;
}

int main(int argc, char **argv)
{
    static golden_case cases[MAX_CASES];
    static const char *const submodes[] = {"PSK31", "PSK63", "PSK125"};
    static const char *const clean_names[] = {
        "psk31_cq_clean", "psk63_cq_clean", "psk125_pangram_clean"};
    const golden_case *clean[3], *station;
    size_t ncases = 0, i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <golden_dir>\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    if (!load_expected(cases, &ncases)) {
        fprintf(stderr, "cannot read %s/expected.txt\n", g_dir);
        return 2;
    }
    for (i = 0; i < 3; i++) {
        clean[i] = find_case(cases, ncases, clean_names[i]);
        if (!clean[i]) {
            fprintf(stderr, "golden case %s missing\n", clean_names[i]);
            return 2;
        }
    }
    station = find_case(cases, ncases, "psk31_station_m5");
    if (!station) {
        fprintf(stderr, "golden case psk31_station_m5 missing\n");
        return 2;
    }

    test_version();
    for (i = 0; i < 3; i++)
        test_taps(submodes[i]);
    for (i = 0; i < ncases; i++)
        test_golden(&cases[i]);
    for (i = 0; i < ncases; i++)
        test_bound(&cases[i]);
    for (i = 0; i < 3; i++)
        test_chunk_invariance(clean[i]);
    test_truncation(clean[0]);
    test_invalid_config();
    test_reset(clean[0]);
    test_reset(station);
    test_set_freq();

    report("bit bound over all runs", g_bound_violations == 0, "");
    printf("%s: %d passed, %d failure(s)\n", g_failures ? "FAILED" : "OK", g_passes,
           g_failures);
    return g_failures ? 1 : 0;
}
