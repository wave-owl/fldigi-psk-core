# Changelog

## 0.2.1

- Initialise the windowed-sinc scratch buffer so the `-Werror` sanitizer build passes with
  GCC on Linux (`-Wmaybe-uninitialized` at `-O1`). No change in output.

## 0.2.0

- Transmitter for PSK31/63/125 (BPSK) after fldigi 4.2.13 `psk::tx_process`: additive
  `fpsk_tx_*` API, real audio (fldigi-exact) or analytic IQ output, rates 8000 and 48000 Hz.
- Version string carries the capability tag `tx=1`; contract `psk-c1` and ABI 1 unchanged,
  receiver unchanged.
- Tests against fldigi's own TX output (sample-identical), `make sanitize` (ASan + UBSan),
  TX timing in `make bench`.

## 0.1.0

- Receiver for PSK31/63/125, C99 port of wave-owl's Python port of the fldigi 4.2.13
  receiver; bits identical to the frozen Python goldens. Contract `psk-c1`, ABI 1.
