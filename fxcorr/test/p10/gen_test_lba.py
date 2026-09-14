#!/usr/bin/env python3
"""Generate LBASTD (2-bit LBA) test data: ASCII header + raw sample stream.

The LBA file format as read by the upstream base DataStream
(datastream.cpp:1825-1875): an old-style first line "YYYYMMDD:HHMMSS" (15
chars + newline), immediately followed by the raw payload -- no framing, pure
consecutive sample bytes.

2-bit packing is low bits first per byte (4 samples/byte, sample i of a byte
at bits (2i)..(2i+1)) -- the same order as VDIF/Mark5B.  This matches
LBAMode's lookup table (mode.cpp LBAMode constructor: "(i >> shift) & 0x03"
with shift starting at 0, i.e. the first sample is the byte's two LEAST
significant bits, because the lookup walks a little-endian u16) and the
unpackvalues {1,-1,3,-3} mapping.

2026-09-14: the generator originally packed MSB-first (q << (6-2i)) and the
fengine autocorrelation peak of a 1.5 MHz tone landed on channel 1024 (the
1 MHz position): in-byte time reversal modulates the tone by fs/8.  Fixing
the packing order to LSB-first puts the peak back on ch 1536.

Usage: gen_test_lba.py <output.lba> <duration_sec> <tone_mhz> <samplerate_mhz>
"""

import math
import sys


def main():
    if len(sys.argv) != 5:
        print("usage: gen_test_lba.py <output.lba> <duration_sec> <tone_mhz> <samplerate_mhz>")
        sys.exit(1)
    outname = sys.argv[1]
    duration = float(sys.argv[2])
    tone_mhz = float(sys.argv[3])
    rate_mhz = float(sys.argv[4])

    nsamp_total = int(duration * rate_mhz * 1e6)
    data = bytearray(nsamp_total // 4)

    # 2-bit quantised single tone, samples packed low bits first
    # (LBAMode::stdunpackvalues = {1, -1, 3, -3} maps code 0..3)
    for i in range(nsamp_total):
        phase = 2.0 * math.pi * tone_mhz * i / rate_mhz
        v = 0.7 * math.sin(phase)
        q = int(round(v * 2.0)) + 2
        if q < 0:
            q = 0
        if q > 3:
            q = 3
        data[i // 4] |= q << (2 * (i % 4))

    # 2020y100d07h00m00s (start of scan No0001 in test.vex)
    header = b"20200409:070000\n"    # 15 chars + newline, old-style header
                                      # (YYYYMMDD:HHMMSS -- the time part has no colons)

    with open(outname, "wb") as f:
        f.write(header)
        f.write(data)

    print(f"wrote {outname}: {len(header)} header bytes + {len(data)} payload bytes")


if __name__ == "__main__":
    main()
