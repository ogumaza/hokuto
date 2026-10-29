#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Makes tests/aac/burst.adts, the AAC stream of the AAC test (tests/aac_test.cpp).

The source is a second of 48 kHz stereo: silence, then from sample 4,800 a burst of 9,600 samples, then silence. The
left channel's burst is noise from a linear congruential generator. The right channel's is a 1 kHz sine. Both peak at
8,000. The Mac's afconvert encodes the source as AAC-LC at 128 kbit/s in ADTS. Run it on a Mac:

    python3 tools/make_aac_burst.py
"""

import math
import pathlib
import struct
import subprocess
import tempfile
import wave

RATE = 48000
LENGTH = 48000
START = 4800
BURST = 9600
PEAK = 8000


def source():
    """The source's frames, as pairs of samples."""
    state = 12345
    frames = []
    for n in range(LENGTH):
        left = right = 0
        if START <= n < START + BURST:
            state = (state * 1103515245 + 12345) % (1 << 31)
            left = (state >> 15) % (2 * PEAK + 1) - PEAK
            right = round(PEAK * math.sin(2 * math.pi * 1000 * (n - START) / RATE))
        frames.append((left, right))
    return frames


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "tests" / "aac" / "burst.adts"
    with tempfile.TemporaryDirectory() as folder:
        wav = pathlib.Path(folder) / "burst.wav"
        with wave.open(str(wav), "wb") as file:
            file.setnchannels(2)
            file.setsampwidth(2)
            file.setframerate(RATE)
            file.writeframes(b"".join(struct.pack("<hh", left, right) for left, right in source()))
        subprocess.run(["afconvert", "-f", "adts", "-d", "aac", "-b", "128000", str(wav), str(out)], check=True)


if __name__ == "__main__":
    main()
