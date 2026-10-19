# Changelog

## 0.3.0

- PSK variants after fldigi 4.2.13 (`psk.cxx`), receiver and transmitter, text API
  `fpsk_v_*`, capability tag `variants=1`: QPSK31..500, 8PSK125..1200F (with and without
  FEC), BPSK63F/250/500/1000, PSK125R..1000R, the 20 multi-carrier PSK-R and 6 multi-carrier
  BPSK modes, OFDM-500F/750F/3500, and BPSK31/63/125 through the same engine. The core owns
  the PSK and MFSK varicode, the FEC and the interleaver of these submodes.
- `dsp/`: fldigi-dsp-core 0.9.2 as pinned submodule (`dsp.lock`, `tools/check_dsp.sh`).
- Conformance with fldigi 4.2.13 itself: 119 overs TX sample-identical, 476 decodes RX
  byte-identical (clean, noisy, AFC, pilot), `tests/test_psk_v_fldigi`.
- Version string `fldigi-psk-core 0.3.0 contract=psk-c1 abi=1 tx=1 variants=1 dsp=0.9.2`.
  The BPSK31/63/125 bit API and its results are unchanged (all 0.2.1 goldens pass); the
  goldens now also freeze AFC off (`*_afc0` in `tests/golden/expected.txt`, from 0.2.1).

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
