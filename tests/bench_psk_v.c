/*
 * CPU and memory benchmark of the PSK variants (wave-owl R-931.6: below 5 % of one core and
 * below 2 MB per instance). Per submode: 60 s of audio at the submode rate (the core's own
 * pangram over, looped, with light noise so the decoder never idles) through the receiver
 * in 512-sample blocks; the receiver's allocation; the time to render the over at 48 kHz IQ.
 * Prints one line per submode; over-budget values are marked, the exit status stays 0.
 * Usage: bench_psk_v [seconds]
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test_v_util.h"

#include <time.h>

#define BLOCK 512

int main(int argc, char **argv)
{
    static const char *text = "the quick brown fox jumps over the lazy dog 0123456789";
    double seconds = argc > 1 ? atof(argv[1]) : 60.0, worst_cpu = 0;
    size_t worst_mem = 0;
    int i, over = 0;
    uint32_t seed = 12345;

    printf("%-11s %6s %8s %8s %9s %s\n", "submode", "rate", "RX CPU", "RX MB", "TX 48k ms",
           "");
    for (i = 0; i < fpsk_v_submode_count(); i++) {
        const fpsk_v_submode *m = fpsk_v_submode_at(i);
        double carrier = m->bandwidth >= 3000 ? 2250 : 1500;
        size_t n, total = (size_t)(seconds * m->rate), k, nim, got = 0;
        double *over_audio = render_over(m->name, text, carrier, 0, &n), *x, *re, *im = NULL;
        unsigned char *out = malloc(1 << 16);
        fpsk_v_rx *rx = make_rx(m->name, carrier, 1, 0);
        clock_t t0;
        double cpu, mb, tx_ms;
        x = malloc(total * sizeof *x);
        for (k = 0; k < total; k++) {
            seed = seed * 1664525u + 1013904223u;
            x[k] = 0.25 * over_audio[k % n] + 0.01 * ((double)seed / 4294967296.0 - 0.5);
        }
        t0 = clock();
        for (k = 0; k < total; k += BLOCK)
            got += fpsk_v_rx_process(rx, x + k, total - k < BLOCK ? total - k : BLOCK, out,
                                     1 << 16);
        cpu = (double)(clock() - t0) / CLOCKS_PER_SEC / seconds * 100.0;
        mb = fpsk_v_rx_footprint(rx) / 1048576.0;
        t0 = clock();
        re = render_over_iq(m->name, text, carrier, 48000, 0, &im, &nim);
        tx_ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
        printf("%-11s %6d %7.2f%% %8.2f %9.1f %s%s (%zu chars)\n", m->name, m->rate, cpu, mb,
               tx_ms, cpu >= 5.0 ? " CPU>5%" : "", mb >= 2.0 ? " MEM>2MB" : "", got);
        over += cpu >= 5.0 || mb >= 2.0;
        if (cpu > worst_cpu)
            worst_cpu = cpu;
        if (fpsk_v_rx_footprint(rx) > worst_mem)
            worst_mem = fpsk_v_rx_footprint(rx);
        fpsk_v_rx_destroy(rx);
        free(over_audio);
        free(x);
        free(out);
        free(re);
        free(im);
    }
    printf("worst RX CPU %.2f %% of one core, worst RX memory %.2f MB; %d submode(s) over the "
           "R-931.6 budget\n", worst_cpu, worst_mem / 1048576.0, over);
    return 0;
}
