/*
 * CPU benchmark of fldigi-psk-core: 60 minutes of 8 kHz audio per submode (the clean
 * golden vector looped), processed in 512-sample blocks with AFC on; then the transmitter:
 * a 120 s PSK31 over rendered as 48 kHz analytic IQ (target < 0.5 s CPU).
 * Usage: bench_psk <golden_dir>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fldigi_psk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define RATE 8000
#define MINUTES 60
#define BLOCK 512
#define TX_SECONDS 120

/* CPU seconds to render a TX_SECONDS PSK31 over at 48 kHz IQ; negative on error. */
static double bench_tx(void)
{
    fpsk_tx_config cfg = {48000, 31.25, 1000.0};
    fpsk_tx *tx = fpsk_tx_create(&cfg);
    size_t nbits = (size_t)(TX_SECONDS * 31.25), n, pos, k;
    unsigned char *bits = malloc(nbits);
    double *re, *im, cpu;
    clock_t t0;

    if (!tx || !bits)
        return -1.0;
    for (k = 0; k < nbits; k++)
        bits[k] = (unsigned char)((k * 7 + k / 3) % 3 != 0);
    n = (2 * fpsk_tx_preamble_symbols(tx) + nbits) * fpsk_tx_symbol_len(tx) + 2048 * 6;
    re = malloc(n * sizeof *re);
    im = malloc(n * sizeof *im);
    if (!re || !im)
        return -1.0;
    t0 = clock();
    pos = fpsk_tx_preamble(tx, re, im, n);
    pos += fpsk_tx_bits(tx, bits, nbits, re + pos, im + pos, n - pos);
    pos += fpsk_tx_postamble(tx, re + pos, im + pos, n - pos);
    cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fpsk_tx_destroy(tx);
    free(bits);
    free(re);
    free(im);
    return pos == n ? cpu : -1.0;
}

/* Load a little-endian int16 .s16 file scaled by 1/32768; NULL on error. */
static double *load_s16(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char b[2];
    double *x;
    size_t cap = 1 << 16;

    if (!f)
        return NULL;
    x = malloc(cap * sizeof *x);
    *n = 0;
    while (x && fread(b, 1, 2, f) == 2) {
        if (*n == cap) {
            double *y = realloc(x, 2 * cap * sizeof *x);
            if (!y) {
                free(x);
                x = NULL;
                break;
            }
            x = y;
            cap *= 2;
        }
        x[(*n)++] = (int16_t)(uint16_t)(b[0] | (b[1] << 8)) / 32768.0;
    }
    fclose(f);
    return x;
}

int main(int argc, char **argv)
{
    static const char *const names[] = {"psk31_cq_clean", "psk63_cq_clean",
                                        "psk125_pangram_clean"};
    static const double bauds[] = {31.25, 62.5, 125.0};
    static const char *const submodes[] = {"PSK31", "PSK63", "PSK125"};
    const size_t total = (size_t)RATE * 60 * MINUTES;
    int i, status = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <golden_dir>\n", argv[0]);
        return 2;
    }
    printf("%d min of %d Hz audio per submode, %d-sample blocks, AFC on\n", MINUTES, RATE,
           BLOCK);
    for (i = 0; i < 3; i++) {
        char path[1024];
        double *vec, *audio, cpu;
        size_t nvec, k, nbits = 0;
        uint8_t bits[BLOCK];
        fpsk_config cfg;
        fpsk_rx *rx;
        clock_t t0;

        snprintf(path, sizeof path, "%s/%s.s16", argv[1], names[i]);
        vec = load_s16(path, &nvec);
        audio = malloc(total * sizeof *audio);
        if (!vec || nvec == 0 || !audio) {
            fprintf(stderr, "cannot load %s\n", path);
            free(vec);
            free(audio);
            return 2;
        }
        for (k = 0; k < total; k++)
            audio[k] = vec[k % nvec];
        cfg.sample_rate = RATE;
        cfg.baud = bauds[i];
        cfg.center_hz = 1500.0;
        cfg.afc = 1;
        cfg.afc_range_hz = 0.0;
        rx = fpsk_rx_create(&cfg);
        if (!rx) {
            fprintf(stderr, "create failed\n");
            free(vec);
            free(audio);
            return 2;
        }
        t0 = clock();
        for (k = 0; k < total; k += BLOCK)
            nbits += fpsk_rx_process(rx, audio + k, total - k < BLOCK ? total - k : BLOCK,
                                     bits, BLOCK);
        cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("%-7s %8.3f s CPU  (%.0fx real time, %lu bits)\n", submodes[i], cpu,
               cpu > 0.0 ? 60.0 * MINUTES / cpu : 0.0, (unsigned long)nbits);
        if (nbits == 0)
            status = 1;
        fpsk_rx_destroy(rx);
        free(vec);
        free(audio);
    }
    {
        double cpu = bench_tx();
        printf("TX PSK31 %d s over, 48 kHz IQ: %.3f s CPU (target < 0.5 s)\n", TX_SECONDS, cpu);
        if (cpu < 0.0 || cpu >= 0.5)
            status = 1;
    }
    return status;
}
