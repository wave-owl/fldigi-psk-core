#!/usr/bin/env python3
"""Regenerate the golden test data in this directory from a wave-owl checkout.

Usage (from anywhere, with the wave-owl virtualenv active):

    python tests/golden/make_golden.py /path/to/wave-owl [out_dir]

Inputs come from ``src.modes.conformance.psk_golden.golden_cases()`` and the frozen
reference bits, carrier frequencies, SNRs and filter taps from
``tests/fixtures/psk_golden/golden.json`` of wave-owl. Outputs:

* ``<name>.s16`` -- little-endian int16 raw audio (value = sample * 32768), only for cases
  whose float audio is exactly int16 / 32768 (verified round trip).
* ``<name>.f64`` -- little-endian float64 raw audio for all other cases; cases with identical
  audio share one file.
* ``expected.txt`` -- one line per case:
  ``name submode center_hz afc chunk(0=none) freq_hz snr_db file nbits bits_hex``;
  ``bits_hex`` packs the bits MSB first (``np.packbits``), zero-padded to whole bytes
  (``-`` = no bits).
* ``taps_<submode>_fir1.txt`` / ``taps_<submode>_fir2.txt`` -- the filter taps, one
  ``%.17g`` per line.
"""

import hashlib
import json
import os
import sys

import numpy as np


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    wave_owl = os.path.abspath(sys.argv[1])
    out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(__file__))
    sys.path.insert(0, wave_owl)
    os.chdir(wave_owl)
    from src.modes.conformance.psk_golden import golden_cases

    with open("tests/fixtures/psk_golden/golden.json") as f:
        ref = json.load(f)
    cases = golden_cases()
    files_by_hash = {}
    lines = []
    for name, ref_case in ref["cases"].items():
        case = cases[name]
        assert case.submode == ref_case["submode"], name
        assert case.center_hz == ref_case["center_hz"], name
        assert bool(case.afc) == bool(ref_case["afc"]), name
        assert case.chunk == ref_case["chunk"], name
        audio = np.ascontiguousarray(case.audio, dtype=np.float64)
        assert len(audio) == ref_case["samples"], name
        digest = hashlib.sha256(audio.tobytes()).hexdigest()
        if digest != ref_case["audio_sha256"]:
            sys.exit("audio hash mismatch for %s" % name)
        fname = files_by_hash.get(digest)
        if fname is None:
            scaled = audio * 32768.0
            pcm = np.round(scaled)
            if (np.all(pcm == scaled) and pcm.min() >= -32768 and pcm.max() <= 32767
                    and np.array_equal(pcm.astype("<i2").astype(np.float64) / 32768.0, audio)):
                fname = name + ".s16"
                data = pcm.astype("<i2").tobytes()
            else:
                fname = name + ".f64"
                data = audio.astype("<f8").tobytes()
            with open(os.path.join(out, fname), "wb") as f:
                f.write(data)
            files_by_hash[digest] = fname
        nbits = ref_case["nbits"]
        assert len(ref_case["bits"]) == 2 * ((nbits + 7) // 8), name
        lines.append("%s %s %.17g %d %d %.17g %.17g %s %d %s" % (
            name, ref_case["submode"], ref_case["center_hz"], int(ref_case["afc"]),
            ref_case["chunk"] or 0, ref_case["freq_hz"], ref_case["snr_db"], fname,
            nbits, ref_case["bits"] or "-"))
    with open(os.path.join(out, "expected.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    for submode, taps in ref["taps"].items():
        for which in ("fir1", "fir2"):
            with open(os.path.join(out, "taps_%s_%s.txt" % (submode, which)), "w") as f:
                f.write("".join("%.17g\n" % t for t in taps[which]))
    print("wrote %d cases (%d audio files) to %s (source commit %s)"
          % (len(lines), len(files_by_hash), out, ref.get("source_commit")))


if __name__ == "__main__":
    main()
