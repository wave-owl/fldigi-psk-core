/*
 * CPU benchmark of fldigi-psk-core: 60 minutes of 8 kHz audio per submode (the clean
 * golden vector looped), processed in 512-sample blocks with AFC on.
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
    return status;
}
