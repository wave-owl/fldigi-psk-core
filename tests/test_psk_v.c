/*
 * Unit tests of the fldigi-psk-core PSK variants (wave-owl R-930, AC-930.1, AC-930.2):
 * submode table, version tag, varicode, loopback of every submode, chunk invariance,
 * reset, the TX bound and "max too small" contract, IQ output, invalid configurations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_v_util.h"

/* internal varicode of the core (src/psk_v_varicode.c) */
int pskv_varicode_decode(unsigned int symbol);
int pskv_varicode_encode(unsigned char c, unsigned char *bits);

/* The 53 submodes of R-930.1 by fldigi's short names. */
static const char *VARIANTS[] = {
    "BPSK63F", "BPSK250", "BPSK500", "BPSK1000",
    "QPSK31", "QPSK63", "QPSK125", "QPSK250", "QPSK500",
    "8PSK125", "8PSK125F", "8PSK125FL", "8PSK250", "8PSK250F", "8PSK250FL", "8PSK500",
    "8PSK500F", "8PSK1000", "8PSK1000F", "8PSK1200F",
    "PSK125R", "PSK250R", "PSK500R", "PSK1000R",
    "PSK63RC4", "PSK63RC5", "PSK63RC10", "PSK63RC20", "PSK63RC32",
    "PSK125RC4", "PSK125RC5", "PSK125RC10", "PSK125RC12", "PSK125RC16",
    "PSK250RC2", "PSK250RC3", "PSK250RC5", "PSK250RC6", "PSK250RC7",
    "PSK500RC2", "PSK500RC3", "PSK500RC4", "PSK800RC2", "PSK1000RC2",
    "PSK125C12", "PSK250C6", "PSK500C2", "PSK500C4", "PSK800C2", "PSK1000C2",
    "OFDM500F", "OFDM750F", "OFDM3500",
};
#define NVARIANTS ((int)(sizeof VARIANTS / sizeof VARIANTS[0]))

static double carrier_of(const fpsk_v_submode *m)
{
    return strcmp(m->name, "OFDM3500") == 0 ? 2250.0 : 1500.0;
}

/* Over + 1 s lead + 12 s tail of silence, decoded in calls of chunk samples. */
static unsigned char *loopback(const fpsk_v_submode *m, const char *text, size_t chunk,
                               int reset_first, size_t *got)
{
    size_t n, i, total;
    double *a = render_over(m->name, text, carrier_of(m), 0, &n), *x;
    fpsk_v_rx *rx;
    unsigned char *out;
    if (!a)
        return NULL;
    total = (size_t)m->rate * 13 + n;
    x = (double *)calloc(total, sizeof(double));
    for (i = 0; i < n; i++)
        x[(size_t)m->rate + i] = a[i] * 0.25;
    rx = make_rx(m->name, carrier_of(m), 0, 0);
    if (reset_first) {
        size_t dummy;
        free(decode(rx, x, total / 3, 0, &dummy));
        fpsk_v_rx_reset(rx);
    }
    out = decode(rx, x, total, chunk, got);
    fpsk_v_rx_destroy(rx);
    free(x);
    free(a);
    return out;
}

static void test_table(void)
{
    int i, j;
    const char *v = fpsk_version();
    CHECK(strstr(v, "contract=psk-c1 abi=1 tx=1 variants=1") != NULL, "version %s", v);
    CHECK(fpsk_v_submode_count() == 56, "56 psk modes, got %d", fpsk_v_submode_count());
    CHECK(fpsk_v_submode_at(-1) == NULL && fpsk_v_submode_at(56) == NULL, "index range");
    for (i = 0; i < NVARIANTS; i++)
        CHECK(fpsk_v_submode_find(VARIANTS[i]) != NULL, "submode %s", VARIANTS[i]);
    CHECK(fpsk_v_submode_find("qpsk31") == fpsk_v_submode_find("QPSK-31"), "case / label");
    CHECK(fpsk_v_submode_find("4xPSK63R") == fpsk_v_submode_find("PSK63RC4"), "long name");
    CHECK(fpsk_v_submode_find("OFDM2000") == NULL, "OFDM-2000 is not a fldigi 4.2.13 mode");
    CHECK(fpsk_v_submode_find(NULL) == NULL, "NULL name");
    for (i = 0; i < fpsk_v_submode_count(); i++) {
        const fpsk_v_submode *m = fpsk_v_submode_at(i);
        CHECK(m->rate == 8000 || m->rate == 16000, "%s rate", m->name);
        CHECK(m->sc_bw == (double)(m->rate / m->symbollen), "%s sc_bw", m->name);
        CHECK(m->carriers >= 1 && m->carriers <= 32, "%s carriers", m->name);
        for (j = 0; j < i; j++)
            CHECK(strcmp(m->name, fpsk_v_submode_at(j)->name) != 0
                  && m->fldigi_id != fpsk_v_submode_at(j)->fldigi_id, "%s unique", m->name);
    }
}

static void test_varicode(void)
{
    int c, n, i, ok = 1;
    unsigned char bits[16];
    for (c = 0; c < 256; c++) {
        unsigned int code = 0;
        n = pskv_varicode_encode((unsigned char)c, bits);
        for (i = 0; i < n; i++)
            code = (code << 1) | bits[i];
        /* every code starts and ends with 1 and has no "00" (pskvaricode.cxx) */
        ok &= n >= 1 && n <= 12 && bits[0] == 1 && bits[n - 1] == 1;
        for (i = 1; i < n; i++)
            ok &= !(bits[i] == 0 && bits[i - 1] == 0);
        ok &= pskv_varicode_decode(code) == c;
    }
    CHECK(ok, "PSK varicode round trip of all 256 bytes");
    CHECK(pskv_varicode_decode(0x2AA) == -1, "unknown code");
}

/* AC-930.1: TX -> RX for every submode returns the text. fldigi 4.2.13 itself does not
 * decode its own OFDM-500F: its vestigial AFC (psk.cxx:1470-1473) runs on carrier 0 for the
 * OFDM modes whether or not AFC is switched on, finds the data carrier instead of a pilot
 * and detunes the receiver (fldigi has the OFDM modes disabled, fl_digi.cxx:1971-1982).
 * The port reproduces fldigi's decode byte for byte (test_psk_v_fldigi); here OFDM-500F is
 * expected to lose the text exactly as fldigi does. */
static void test_loopback(void)
{
    static const char *text = "CQ CQ DE DG6FL DG6FL K";
    int i;
    for (i = 0; i < fpsk_v_submode_count(); i++) {
        const fpsk_v_submode *m = fpsk_v_submode_at(i);
        size_t got;
        unsigned char *out = loopback(m, text, 0, 0, &got);
        int ok = out && contains(out, got, "CQ CQ DE DG6FL DG6FL");
        if (strcmp(m->name, "OFDM500F") == 0)
            ok = out != NULL && !contains(out, got, "CQ CQ DE DG6FL DG6FL");
        CHECK(ok, "loopback %s: %.*s", m->name, out ? (int)got : 0, out ? (char *)out : "");
        free(out);
    }
}

/* The text does not depend on the call sizes; reset gives the fresh state. */
static void test_chunks_and_reset(void)
{
    static const char *names[] = {"QPSK63", "8PSK125F", "PSK250RC3", "PSK500C2", "OFDM750F",
                                  "BPSK63F"};
    static const size_t chunks[] = {1, 7, 511, 513, 4096};
    size_t k, c;
    for (k = 0; k < sizeof names / sizeof names[0]; k++) {
        const fpsk_v_submode *m = fpsk_v_submode_find(names[k]);
        size_t got0, got;
        unsigned char *ref = loopback(m, "the quick brown fox 0123", 0, 0, &got0);
        for (c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
            unsigned char *out = loopback(m, "the quick brown fox 0123", chunks[c], 0, &got);
            CHECK(got == got0 && memcmp(out, ref, got) == 0, "%s chunk %zu", m->name,
                  chunks[c]);
            free(out);
        }
        {
            unsigned char *out = loopback(m, "the quick brown fox 0123", 0, 1, &got);
            CHECK(got == got0 && memcmp(out, ref, got) == 0, "%s reset", m->name);
            free(out);
        }
        free(ref);
    }
}

/* Every call needs max >= bound; a refused call leaves the state unchanged. */
static void test_tx_contract(void)
{
    int i;
    for (i = 0; i < fpsk_v_submode_count(); i += 5) {
        const fpsk_v_submode *m = fpsk_v_submode_at(i);
        fpsk_v_tx_config c;
        fpsk_v_tx *tx;
        size_t b, n0, n = 0, k;
        double *ref = render_over(m->name, "ab 7", carrier_of(m), 0, &n0), *buf;
        memset(&c, 0, sizeof c);
        c.submode = m->name;
        c.carrier_hz = carrier_of(m);
        tx = fpsk_v_tx_create(&c);
        b = fpsk_v_tx_bound(tx);
        buf = (double *)malloc((b * 8) * sizeof(double));
        CHECK(fpsk_v_tx_begin(tx, buf, NULL, b - 1) == 0, "%s small max", m->name);
        CHECK(fpsk_v_tx_begin(tx, NULL, NULL, b) == 0, "%s NULL out", m->name);
        n += fpsk_v_tx_begin(tx, buf + n, NULL, b * 8 - n);
        for (k = 0; k < 4; k++) {
            CHECK(fpsk_v_tx_put(tx, (unsigned char)"ab 7"[k], buf + n, NULL, b - 1) == 0,
                  "%s small max put", m->name);
            n += fpsk_v_tx_put(tx, (unsigned char)"ab 7"[k], buf + n, NULL, b * 8 - n);
        }
        n += fpsk_v_tx_end(tx, buf + n, NULL, b * 8 - n);
        CHECK(n == n0 && memcmp(buf, ref, n * sizeof(double)) == 0, "%s refused calls",
              m->name);
        CHECK(fpsk_v_tx_rate(tx) == m->rate, "%s rate", m->name);
        free(buf);
        free(ref);
        fpsk_v_tx_destroy(tx);
    }
}

/* IQ at 48 kHz: re divided by each symbol's peak is the real mode; envelope <= 1. */
static void test_iq(void)
{
    static const char *names[] = {"QPSK31", "8PSK250F", "PSK125RC4", "PSK500C4"};
    size_t k;
    for (k = 0; k < sizeof names / sizeof names[0]; k++) {
        const fpsk_v_submode *m = fpsk_v_submode_find(names[k]);
        size_t nr, ni, i, s, sl = (size_t)m->symbollen * (size_t)(48000 / m->rate);
        double *im = NULL, maxerr = 0, peak = 0;
        double *real = render_over_iq(m->name, "73 de DG6FL", 1500, 48000, 0, NULL, &nr);
        double *re = render_over_iq(m->name, "73 de DG6FL", 1500, 48000, 0, &im, &ni);
        CHECK(nr == ni && nr > 0, "%s IQ length", m->name);
        for (s = 0; s * sl + sl <= ni; s++) {
            double mx = 0;
            for (i = s * sl; i < s * sl + sl; i++)
                if (fabs(re[i]) > mx)
                    mx = fabs(re[i]);
            for (i = s * sl; i < s * sl + sl; i++) {
                double want = mx != 0 ? re[i] / mx : re[i], e = fabs(want - real[i]);
                double env = sqrt(re[i] * re[i] + im[i] * im[i]);
                if (e > maxerr)
                    maxerr = e;
                if (env > peak)
                    peak = env;
            }
        }
        CHECK(maxerr < 1e-12, "%s IQ re vs real: %g", m->name, maxerr);
        CHECK(peak <= 1.0 + 1e-9 && peak > 0.5, "%s envelope peak %g", m->name, peak);
        free(real);
        free(re);
        free(im);
    }
}

static void test_invalid(void)
{
    fpsk_v_rx_config rc;
    fpsk_v_tx_config tc;
    fpsk_v_rx *rx;
    memset(&rc, 0, sizeof rc);
    memset(&tc, 0, sizeof tc);
    CHECK(fpsk_v_rx_create(NULL) == NULL && fpsk_v_tx_create(NULL) == NULL, "NULL config");
    rc.submode = "PSK31X";
    rc.carrier_hz = 1500;
    CHECK(fpsk_v_rx_create(&rc) == NULL, "unknown submode");
    rc.submode = "PSK63RC32";               /* 2753 Hz wide: 1376 .. 1624 Hz */
    rc.carrier_hz = 1000;
    CHECK(fpsk_v_rx_create(&rc) == NULL, "carrier outside fldigi's range");
    tc.submode = "QPSK63";
    tc.carrier_hz = 1500;
    tc.rate = 44100;
    CHECK(fpsk_v_tx_create(&tc) == NULL, "rate 44100");
    tc.rate = 16000;
    CHECK(fpsk_v_tx_create(&tc) == NULL, "QPSK63 at 16000");
    rc.submode = "QPSK63";
    rc.carrier_hz = 1500;
    rx = fpsk_v_rx_create(&rc);
    fpsk_v_rx_set_carrier(rx, 5000);
    CHECK(fpsk_v_rx_carrier_hz(rx) == 3000 - 31, "set_carrier clamps (%g)",
          fpsk_v_rx_carrier_hz(rx));
    CHECK(fpsk_v_rx_process(rx, NULL, 10, NULL, 0) == 0, "NULL audio");
    CHECK(fpsk_v_rx_footprint(rx) > 0, "footprint");
    fpsk_v_rx_destroy(rx);
    fpsk_v_rx_destroy(NULL);
    fpsk_v_tx_destroy(NULL);
}

int main(void)
{
    test_table();
    test_varicode();
    test_loopback();
    test_chunks_and_reset();
    test_tx_contract();
    test_iq();
    test_invalid();
    printf("%s: %d passed, %d failure(s)\n", failures ? "FAILED" : "OK", passes, failures);
    return failures ? 1 : 0;
}
