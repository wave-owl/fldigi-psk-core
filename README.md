# fldigi-psk-core

A small C99 BPSK receiver library for PSK31, PSK63 and PSK125: audio samples in, decided
bits out.

It is a C port of [wave-owl](https://github.com/wave-owl/wave-owl)'s Python port of the
fldigi 4.2.13 PSK receiver (`psk::rx_process`):

| Part | fldigi 4.2.13 source |
|------|----------------------|
| Complex NCO, fir1 low-pass + decimation to 16 samples per symbol | `src/psk/psk.cxx:2020-2044` |
| Filters per submode (raised cosine + PSKCore matched filter, Blackman sinc for PSK125) | `src/psk/psk.cxx:940-958`, `src/psk/pskcoeff.cxx:203-312` |
| Bit clock from the envelope (syncbuf early/late balance) | `src/psk/psk.cxx:2061-2158` |
| Differential phase decision, signal quality | `src/psk/psk.cxx:1490-1580` |
| AFC from the residual phase error (`phaseafc`) | `src/psk/psk.cxx:1442-1468` |

The library returns exactly the bit sequences, carrier frequencies and SNRs of the Python
reference on the golden data in `tests/golden/`. It is used by wave-owl as its native PSK
core.

## Provenance

- Upstream: fldigi, <https://git.code.sf.net/p/fldigi/fldigi>, tag `v4.2.13`,
  commit `e7c3ab3709ad62a7d91829364eb48a05fa325c0d`
- Mirror: <https://github.com/wave-owl/fldigi> (same tag and commit)
- fldigi files: `src/psk/psk.cxx`, `src/psk/pskcoeff.cxx`
- Python reference: wave-owl `src/modes/psk/demod.py`

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Derived from fldigi, Copyright (C) Dave Freese
W1HKJ and the fldigi developers.

## Contract

Contract `psk-c1`, ABI 1 (`FPSK_CONTRACT`, `FPSK_ABI` in `include/fldigi_psk.h`).
`fpsk_version()` returns `"fldigi-psk-core <x.y.z> contract=psk-c1 abi=1"`; callers check
this string when loading the shared library.

## API

```c
#include "fldigi_psk.h"

fpsk_config cfg = {
    .sample_rate = 8000, .baud = 31.25, .center_hz = 1000,
    .afc = 1, .afc_range_hz = 0,   /* <= 0: one symbol rate */
};
fpsk_rx *rx = fpsk_rx_create(&cfg);           /* NULL on invalid config / OOM */
uint8_t bits[64];
size_t n = fpsk_rx_process(rx, audio, nsamples, bits, 64);
fpsk_rx_destroy(rx);
```

| Function | Purpose |
|----------|---------|
| `fpsk_version()` | Version and contract string |
| `fpsk_rx_create(cfg)` / `fpsk_rx_destroy(rx)` | Create / free a receiver; baud 31.25, 62.5 or 125 with `sample_rate / baud` = 256, 128 or 64 (8 kHz) |
| `fpsk_rx_reset(rx)` | Back to the initial state |
| `fpsk_rx_process(rx, x, n, bits, max_bits)` | Process `n` samples (doubles); writes at most `max_bits` bits (1 = no phase reversal, 0 = reversal) and returns the number decided (excess bits are counted but dropped); at most `n / (rate / baud / 16) + 2` bits per call |
| `fpsk_rx_set_freq(rx, hz)` | Retune the carrier and the AFC centre, keeping the receiver state |
| `fpsk_rx_freq_hz(rx)` | Current carrier frequency (moves with AFC) |
| `fpsk_rx_snr_db(rx)` | Phase-scatter SNR estimate (0 before 10 symbols) |
| `fpsk_rx_quality(rx)` | fldigi's `norm(quality)`: 1 for clean BPSK, near 0 for noise |
| `fpsk_rx_taps(rx, FPSK_FIR1 / FPSK_FIR2, out, max)` | The filter taps (65 for PSK31/63, 64 for PSK125) |

The input is processed in blocks of one symbol starting at `x[0]`, so an AFC correction
takes effect within a symbol. Without AFC the result does not depend on how the audio is
split into calls; with AFC it may (as in the reference implementation). Varicode decoding is
left to the caller.

## Build

Plain `make`, C99, no dependencies besides `libm`:

```sh
make                          # static + shared library into build/
make test                     # build and run tests/test_psk against tests/golden
make bench                    # CPU benchmark: 60 min of 8 kHz audio per submode
make BUILD=/abs/dir shared    # out-of-tree shared library (used by wave-owl)
```

## Tests

`tests/test_psk <golden_dir>` checks the golden cases (bits exact, carrier frequency within
0.01 Hz, SNR within 0.01 dB), the filter taps per submode (within 1e-12), the version
string, chunk invariance on the clean vectors (one call vs. random call sizes of 1..4000
samples, AFC off and on), the per-call bound `n / (rate / baud / 16) + 2` on every golden
case fed in random call sizes, `max_bits` truncation, invalid configurations, reset and
`set_freq`.

The golden data (`tests/golden/`) is generated from a wave-owl checkout with
`tests/golden/make_golden.py /path/to/wave-owl` (wave-owl virtualenv): raw little-endian
`int16` (`.s16`, scaled by 1/32768) or `float64` (`.f64`) audio at 8 kHz, `expected.txt`
(`name submode center_hz afc chunk(0=none) freq_hz snr_db file nbits bits_hex`, bits packed
MSB first as `np.packbits`) and `taps_<submode>_fir1.txt` / `taps_<submode>_fir2.txt`.

## Notes

- Compiled with `-ffp-contract=off` (no implicit fused multiply-add) so that the arithmetic
  follows the reference order; sums and complex magnitudes reproduce numpy's pairwise sum
  and `abs()` bit for bit.
- Performance (`make bench`, Apple Silicon, `-O3`, 512-sample blocks, AFC on): about 0.6 s
  (PSK31), 0.9 s (PSK63) and 1.5 s (PSK125) CPU per hour of audio.
