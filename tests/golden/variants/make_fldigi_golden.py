#!/usr/bin/env python3
"""Golden data from fldigi 4.2.13 itself for the PSK variants of fldigi-psk-core (stdlib only).

Drives the headless fldigi bench build (configure --enable-benchmark plus the TX extension
of wave-owl's archived change fix-psk-rtty-vs-fldigi, bench/BUILD.md) for every psk mode of
fldigi 4.2.13. fldigi 4.2.13 does not construct the OFDM modes (init_modem, fl_digi.cxx
1971-1982, "Commented out pending development work"), so the bench binary used here has
that case enabled (bench-only change, the modem DSP is untouched; see README).

* TX: fldigi renders the text (bench-tx, txlevel -12 dB, PCM16, modem rate 8000 or 16000 Hz).
  The FNV-1a 64 hash of every vector goes to cases.txt.
* RX: fldigi decodes 1 s silence + the TX vector + 12 s silence, clean and buried in
  deterministic noise at -5, -10 and -15 dB (2500 Hz reference bandwidth, see noisy()).
  The raw bytes fldigi hands to put_rx_char() go to rx/<stem>_<snr>.txt.
* AFC / pilot cases: fldigi transmits at the TX carrier and decodes at the RX carrier with
  --benchmark-afc 1; the pilot cases set fldigi's PSKPILOT (vestigial carrier) for TX and RX.
* Carrier 1500 Hz; OFDM-500F/750F are fixed at 1500 Hz by fldigi (psk.cxx:306-309);
  OFDM-3500 at 2250 Hz (psk.cxx:306-307) with the 4000 Hz waterfall (HIGHFREQCUTOFF), the
  only setting at which fldigi does not clamp its carrier away.

    FLDIGI_BENCH_BIN=/path/to/src-bench-ofdm/src/fldigi \\
        python3 tests/golden/variants/make_fldigi_golden.py [stem ...]

The C tests (tests/test_psk_v_fldigi.c) rebuild the same noisy inputs bit for bit.
SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLDIGI = os.environ.get("FLDIGI_BENCH_BIN", "fldigi")
TEXTS = {"cq": "CQ CQ DE DG6FL DG6FL K",
         "pangram": "the quick brown fox jumps over the lazy dog 0123456789 ,.'/()?@-=#\"+;:! "
                    "<x> 73"}
TAIL_S = 12
SNRS = ("clean", "m05", "m10", "m15")

# (fldigi trx_mode id = index into mode_info[], sname), globals/globals.cxx:127-164, 227-256
MODES = [
    (61, "BPSK63F"), (63, "BPSK250"), (64, "BPSK500"), (65, "BPSK1000"),
    (66, "PSK125C12"), (67, "PSK250C6"), (68, "PSK500C2"), (69, "PSK500C4"),
    (70, "PSK800C2"), (71, "PSK1000C2"),
    (72, "QPSK31"), (73, "QPSK63"), (74, "QPSK125"), (75, "QPSK250"), (76, "QPSK500"),
    (77, "8PSK125"), (78, "8PSK125FL"), (79, "8PSK125F"), (80, "8PSK250"), (81, "8PSK250FL"),
    (82, "8PSK250F"), (83, "8PSK500"), (84, "8PSK500F"), (85, "8PSK1000"), (86, "8PSK1000F"),
    (87, "8PSK1200F"),
    (88, "OFDM500F"), (89, "OFDM750F"), (90, "OFDM3500"),
    (138, "PSK125R"), (139, "PSK250R"), (140, "PSK500R"), (141, "PSK1000R"),
    (142, "PSK63RC4"), (143, "PSK63RC5"), (144, "PSK63RC10"), (145, "PSK63RC20"),
    (146, "PSK63RC32"), (147, "PSK125RC4"), (148, "PSK125RC5"), (149, "PSK125RC10"),
    (150, "PSK125RC12"), (151, "PSK125RC16"), (152, "PSK250RC2"), (153, "PSK250RC3"),
    (154, "PSK250RC5"), (155, "PSK250RC6"), (156, "PSK250RC7"), (157, "PSK500RC2"),
    (158, "PSK500RC3"), (159, "PSK500RC4"), (160, "PSK800RC2"), (161, "PSK1000RC2"),
    # BPSK31/63/125 through the variants engine as well
    (59, "BPSK31"), (60, "BPSK63"), (62, "BPSK125"),
]
CARRIER = {"OFDM3500": 2250}
DEFS = {"OFDM3500": "<HIGHFREQCUTOFF>4000</HIGHFREQCUTOFF>\n"}
# (stem, sname, tx carrier, rx carrier, afc, pilot)
EXTRA = [
    ("bpsk250_afc", "BPSK250", 1507, 1500, 1, 0),
    ("qpsk63_afc", "QPSK63", 1495, 1500, 1, 0),
    ("psk125r_afc", "PSK125R", 1504, 1500, 1, 0),
    ("bpsk63f_afc", "BPSK63F", 1497, 1500, 1, 0),
    ("8psk125_pilot", "8PSK125", 1500, 1500, 1, 1),
    ("8psk250f_pilot", "8PSK250F", 1500, 1500, 1, 1),
    ("8psk125fl_pilot_afc", "8PSK125FL", 1503, 1500, 1, 1),
]


def run_fldigi(mid, sname, args, env_extra=None, freq=1500, defs=""):
    cfg = tempfile.mkdtemp(prefix="fldigi-pskv-golden-")
    Path(cfg, "fldigi_def.xml").write_text("<FLDIGI_DEFS>\n" + defs + "</FLDIGI_DEFS>\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLDIGI_BENCH_TX")}
    env.update(env_extra or {})
    try:
        p = subprocess.run([FLDIGI, "--config-dir", cfg, "--benchmark-modem", str(mid),
                            "--benchmark-frequency", str(freq), *args],
                           env=env, capture_output=True, text=True, timeout=3600)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if f"mode={sname} " not in p.stderr:
        raise RuntimeError(f"fldigi did not run {sname}: {p.stderr[-400:]}")
    return p.stderr


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        sr = w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    return sr, a


def write_wav(path, sr, pcm):
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(a.tobytes())


def fnv1a64(pcm):
    h = 0xCBF29CE484222325
    for v in pcm:
        u = v & 0xFFFF
        for byte in (u & 0xFF, u >> 8):
            h ^= byte
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Lcg:
    """32-bit LCG (Numerical Recipes); the same in tests/test_psk_v_fldigi.c."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def uniform(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0


def noisy(pcm, sr, snr, seed):
    """1 s + pcm + 12 s, plus Irwin-Hall Gaussian noise at -5 / -10 / -15 dB SNR in 2500 Hz.

    Only +, -, *, / and sqrt (all correctly rounded), so C reproduces it bit for bit.
    SNR = mean square of the burst / (noise variance * 2500 / (sr / 2)).
    """
    padded = [0.0] * sr + [float(v) for v in pcm] + [0.0] * (TAIL_S * sr)
    if snr == "clean":
        return [int(v) for v in padded]
    ps = 0.0
    for v in pcm:
        ps += float(v) * float(v)
    ps = ps / len(pcm)
    sq = math.sqrt(10.0)
    inv = {"m05": 1.0 / sq, "m10": 1.0 / 10.0, "m15": 1.0 / (10.0 * sq)}[snr]
    s2 = ps / inv * (sr / 2.0) / 2500.0
    sd = math.sqrt(s2)
    gain = 3276.7 / math.sqrt(ps * len(pcm) / len(padded) + s2)
    rng = Lcg(seed)
    out = []
    for v in padded:
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        y = (v + sd * z) * gain
        q = math.floor(y + 0.5)
        out.append(max(-32768, min(32767, int(q))))
    return out


def cases():
    out = []
    for mid, sname in MODES:
        c = CARRIER.get(sname, 1500)
        for key in TEXTS:
            out.append((f"{sname.lower()}_{key}", mid, sname, key, c, c, 0, 0))
    ids = dict((s, m) for m, s in MODES)
    for stem, sname, ctx, crx, afc, pilot in EXTRA:
        out.append((stem, ids[sname], sname, "cq", ctx, crx, afc, pilot))
    return out


def one(idx, case):
    stem, mid, sname, key, ctx, crx, afc, pilot = case
    defs = DEFS.get(sname, "") + ("<PSKPILOT>1</PSKPILOT>\n" if pilot else "")
    with tempfile.TemporaryDirectory() as td:
        wav = Path(td, stem + ".wav")
        run_fldigi(mid, sname, ["--benchmark-input", "1"],
                   {"FLDIGI_BENCH_TX_TEXT": TEXTS[key], "FLDIGI_BENCH_TX_WAV": str(wav)},
                   freq=ctx, defs=defs)
        sr, pcm = read_wav(wav)
        line = (f"{stem} {sname} {key} {idx} {sr} {len(pcm)} {ctx} {crx} {afc} {pilot} "
                f"{fnv1a64(pcm):016x}")
        for i, snr in enumerate(SNRS):
            inp = Path(td, f"{stem}_{snr}.wav")
            write_wav(inp, sr, noisy(pcm, sr, snr, 1000 * idx + i))
            out = Path(td, f"rx_{snr}.txt")
            run_fldigi(mid, sname, ["--benchmark-input", str(inp), "--benchmark-afc", str(afc),
                                    "--benchmark-output", str(out)], freq=crx, defs=defs)
            text = out.read_bytes() if out.exists() else b""
            Path(HERE, "rx", f"{stem}_{snr}.txt").write_bytes(text)
    print(line, flush=True)
    return line


def main():
    (HERE / "rx").mkdir(exist_ok=True)
    only = set(sys.argv[1:])
    todo = [(i, c) for i, c in enumerate(cases()) if not only or c[0] in only]
    with ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "10"))) as ex:
        lines = list(ex.map(lambda a: one(*a), todo))
    if only:
        return 0
    header = ("# stem fldigi-sname text index samplerate samples tx-carrier rx-carrier afc pilot"
              " fnv1a64(int16 LE) -- fldigi 4.2.13 bench TX, txlevel -12 dB; noise seed ="
              " 1000 * index + snr step (clean, -5, -10, -15 dB in 2500 Hz)\n")
    (HERE / "cases.txt").write_text(header + "\n".join(lines) + "\n")
    (HERE / "texts.txt").write_text("".join(f"{k} {v}\n" for k, v in TEXTS.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
