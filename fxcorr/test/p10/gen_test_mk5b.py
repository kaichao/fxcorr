#!/usr/bin/env python3
"""Generate single-band 2-bit Mark5B test data (10016-byte frames).

Frame layout follows the mark5access reader (demorest/mark5access m5bfile.c,
mark5_format_mark5b.c), NOT the official Mark5B spec -- same lesson as the
VDIF header in fxcorr/test/tcal/README.md:

  16-byte header (all little-endian byte order):
    bytes 0-3   sync word 0xABADDEED (stored ED DE AD AB)
    bytes 4-5   frame number within the second, 15 bits (byte5 bit7 = invalid
                flag, 0 here); frame number = n % frames_per_second
    bytes 6-7   unused, 0
    bytes 8-11  BCD: seconds (5 BCD digits: byte8=tens/ones, byte9=thousands/
                hundreds, byte10 low nibble=ten-thousands) + day of century
                (3 BCD digits: byte10 high nibble=ones, byte11 low=tens,
                byte11 high=hundreds); day of century = MJD % 1000
    bytes 12-15 unused, 0 (mark5access derives ns from frame number when the
                format has a frame rate)

  payload: 10000 bytes of raw 2-bit samples, low bits first per byte,
  consecutive-sample stream (channel blocking is implicit in the unpacker,
  same as gen_test_vdif.py)

Usage: gen_test_mk5b.py <output.m5b> <duration_sec> <tone_mhz> <samplerate_mhz>
"""

import math
import struct
import sys


def bcd(value):
    return ((value // 10) << 4) | (value % 10)


def main():
    if len(sys.argv) != 5:
        print("usage: gen_test_mk5b.py <output.m5b> <duration_sec> <tone_mhz> <samplerate_mhz>")
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
    start_mjd = 58948          # MJD of 2020y100d07h00m00s (fixmjd ref aligns by 1000)
    day_of_century = start_mjd % 1000

    header_bytes = 16
    payload_bytes = 10000
    frame_bytes = header_bytes + payload_bytes

    nsamp_per_frame = payload_bytes * 4
    nframes = int(duration * rate_mhz * 1e6 / nsamp_per_frame)
    nsamp_total = nframes * nsamp_per_frame
    fps = int(rate_mhz * 1e6) // nsamp_per_frame

    # 2-bit quantised single tone, samples packed low bits first
    # (same as gen_test_vdif.py / mark5access lut2bit)
    data = bytearray(nsamp_total // 4)
    for i in range(nsamp_total):
        phase = 2.0 * math.pi * tone_mhz * i / rate_mhz
        v = 0.7 * math.sin(phase)
        q = int(round(v * 2.0)) + 2
        if q < 0:
            q = 0
        if q > 3:
            q = 3
        data[i // 4] |= q << (2 * (i % 4))

    with open(outname, "wb") as f:
        for n in range(nframes):
            frame = n % fps
            sec = start_sec + n // fps
            day = day_of_century + (sec - start_sec) // 86400   # day-of-century rolls over midnight
            s = sec % 86400
            header = struct.pack(
                "<IIII",
                0xABADDEED,                            # sync word
                (frame & 0x7FFF) | (0 << 7),          # frame number, valid
                (bcd(s % 100) |
                 (bcd((s // 100) % 100) << 8) |
                 (((s // 10000) % 10) << 16) |
                 ((day % 10) << 20) |
                 (((day // 10) % 10) << 24) |
                 ((day // 100) << 28)),               # BCD seconds + day of century
                0,                                    # unused
            )
            f.write(header)
            f.write(data[n * payload_bytes:(n + 1) * payload_bytes])

    print(f"wrote {outname}: {nframes} frames, {nsamp_total} samples, {nframes * frame_bytes} bytes")


if __name__ == "__main__":
    main()
