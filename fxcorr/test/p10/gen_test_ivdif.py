#!/usr/bin/env python3
"""Generate fanout-mode 2-thread VDIF test data (INTERLACEDVDIF).

DiFX fanout semantics (vdiffile.cpp:549-563, nrecordedbands < nthreads):
each of the nThreadPerBand threads carries every nThreadPerBand-th sample
of the band (time fanout); the muxed stream is the interleaved
reconstruction.  The corner-turners (vdifio cornerturners.c and mpifxcorr
datamuxer.cpp cornerturn_2thread_2bit, identical algorithms) output
a_s0 b_s0 a_s1 b_s1 ... so thread 0 must hold even samples and thread 1
odd samples.

fakemultiVDIF does NOT produce fanout data (it duplicates every frame to
both threads), which is why it cannot serve as the P10 multi-thread test
data -- see README.md.

Frame layout follows gen_test_vdif.py (test.vex 8032-byte frames, 2-bit,
low bits first).  Per-thread frame rate = 62.5 fps (8e6 samples/s band
split across 2 threads = 4e6 samples/s = 2e6 bytes/s per thread).

Usage: gen_test_ivdif.py <output.ivdif> <duration_sec> <tone_mhz> <samplerate_mhz>
"""

import math
import struct
import sys


def main():
    if len(sys.argv) != 5:
        print("usage: gen_test_ivdif.py <output.ivdif> <duration_sec> <tone_mhz> <samplerate_mhz>")
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

    nthreads = 2
    payload_bytes = 8000           # per-thread frame payload: 32000 samples @ 2-bit
    frame_bytes = 32 + payload_bytes
    framelength8 = frame_bytes // 8

    nsamp_per_frame = payload_bytes * 4
    nsamp_total = int(duration * rate_mhz * 1e6)
    nframes = nsamp_total // (nsamp_per_frame * nthreads)
    fps = rate_mhz * 1e6 / (nsamp_per_frame * nthreads)   # 62.5 per thread

    # 2-bit quantised single tone over the global sample index; each thread
    # carries every other sample (fanout): thread t gets samples where
    # (sample index % nthreads) == t
    data = [[] for _ in range(nthreads)]
    for n in range(nframes):
        for t in range(nthreads):
            buf = bytearray(payload_bytes)
            for j in range(nsamp_per_frame):
                # global sample index: frame group n covers samples
                # [n*nthreads*nsamp_per_frame, ...); thread t carries every
                # nthreads-th sample starting at offset t
                g = n * nthreads * nsamp_per_frame + t + j * nthreads
                phase = 2.0 * math.pi * tone_mhz * g / rate_mhz
                v = 0.7 * math.sin(phase)
                q = int(round(v * 2.0)) + 2
                if q < 0:
                    q = 0
                if q > 3:
                    q = 3
                buf[j // 4] |= q << (2 * (j % 4))
            data[t].append(buf)

    with open(outname, "wb") as f:
        for n in range(nframes):
            sec = start_sec + int(n / fps)
            frame = n % int(fps)             # frame number within the second
            for t in range(nthreads):
                header = struct.pack(
                    "<IIIIIIII",
                    (sec & 0x3FFFFFFF),
                    (0 << 24) | (frame & 0xFFFFFF),
                    (1 << 29) | framelength8,
                    # word3 in the vdifio/mark5access bit layout (NOT the old
                    # spec): stationid [15:0], threadid [25:16], nbits-1 [30:26],
                    # iscomplex [31]; see fxcorr/test/p10/README.md
                    (0 << 0) | (t << 16) | ((2 - 1) << 26) | 0,
                    0, 0, 0, 0,
                )
                f.write(header)
                f.write(data[t][n])

    print(f"wrote {outname}: {nframes} frame pairs, {nframes * nthreads} frames, "
          f"{nframes * nthreads * frame_bytes} bytes, {fps} fps/thread")


if __name__ == "__main__":
    main()
