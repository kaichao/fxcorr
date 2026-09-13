#!/usr/bin/env python3
"""Generate single-band 2-bit VDIF test data for the fxcorr V1 small test.

Replaces generateVDIF (used by tests/Synthetic) which is not available on the
test machine.  Frames are 8032 bytes (32 header + 8000 payload) to match the
track_frame_format VDIF/8032/2 in test.vex.

Usage: gen_test_vdif.py <output.vdif> <duration_sec> <tone_mhz> <samplerate_mhz>
"""

import math
import struct
import sys


def main():
    if len(sys.argv) != 5:
        print("usage: gen_test_vdif.py <output.vdif> <duration_sec> <tone_mhz> <samplerate_mhz>")
        sys.exit(1)
    outname = sys.argv[1]
    duration = float(sys.argv[2])
    tone_mhz = float(sys.argv[3])
    rate_mhz = float(sys.argv[4])

    # 2020y100d07h00m00s (start of scan No0001 in test.vex), epoch 32 = 2000-01-01
    import datetime
    start = datetime.datetime(2020, 4, 9, 7, 0, 0)
    epoch = datetime.datetime(2000, 1, 1)
    start_sec = int((start - epoch).total_seconds())

    payload_bytes = 8000           # 2-bit samples: 4 samples/byte
    frame_bytes = 32 + payload_bytes
    framelength8 = frame_bytes // 8

    nsamp_per_frame = payload_bytes * 4
    nframes = int(duration * rate_mhz * 1e6 / nsamp_per_frame)
    nsamp_total = nframes * nsamp_per_frame
    fps = int(rate_mhz * 1e6) // nsamp_per_frame   # frames per second

    # 2-bit quantised single tone, samples packed low bits first
    # (matches mark5access format_vdif.c lut2bit: sample i of a byte at bits 2*i..2*i+1)
    data = bytearray(nsamp_total // 4)
    for i in range(nsamp_total):
        phase = 2.0 * math.pi * tone_mhz * i / rate_mhz
        v = 0.7 * math.sin(phase)                      # peak within +/-1.5 of 2-bit range
        q = int(round(v * 2.0)) + 2                    # 0..4 -> clamp to 0..3
        if q < 0:
            q = 0
        if q > 3:
            q = 3
        data[i // 4] |= q << (2 * (i % 4))             # low bits first

    with open(outname, "wb") as f:
        for n in range(nframes):
            sec = start_sec + n * nsamp_per_frame // int(rate_mhz * 1e6)
            frame = n % fps   # frame number within the second (VDIF word2)
            header = struct.pack(
                "<IIIIIIII",
                (sec & 0x3FFFFFFF),                    # word0: [29:0] seconds; bit30 legacymode=0
                                                       # (legacy would mean 16-byte header), bit31 invalid=0
                (0 << 24) | (frame & 0xFFFFFF),        # word1: [29:24] ref epoch (0 = 2000.0, matches the
                                                       # word0 seconds counted from 2000.0; mark5access epoch
                                                       # index 0 -> mjdepochs[0] = 51544), [23:0] frame number
                (1 << 29) | framelength8,              # word2: [31:29] VDIF version 1, [28:24] log2 nchan = 0,
                                                       # [23:0] frame length in 8-byte units
                (0 << 16) | (0 << 6) | (2 << 1) | 0,   # word3: station=0, thread=0, nbits=2, iscomplex=0
                0,
                0,
                0,
                0,
            )
            f.write(header)
            f.write(data[n * payload_bytes:(n + 1) * payload_bytes])

    print(f"wrote {outname}: {nframes} frames, {nsamp_total} samples, {nframes * frame_bytes} bytes")


if __name__ == "__main__":
    main()
