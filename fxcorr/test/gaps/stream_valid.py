#!/usr/bin/env python3
"""统计 stream.bin（fxcorr-fp 的中间数据流）里无效 FFT 块（valid flags = 0）的分布.

与 sp_valid.py --machine 输出同一格式（bps 行 + 每 subint 一行 + total），供
run_v9_compare.sh 把 fxcorr-fp 的 stream.bin 与 fxcorr-f 的 band_XX.sp 逐行比对——
两边是同一份 fillValidFlags 的位图，位=0 表示该 FFT 块无效。

格式见 fxcorr/v9-plan.md 4：header 92 B；每 subint 一条定长 record =
28 B（status/validbytes/scan/sec/ns/offsetsec/offsetns）+ flagwords×4 B flags
+ sendbytes 数据区；flags 每块 1 位、s32 字打包（FLAGS_PER_INT = 30）。
"""
import struct
import sys

HDR = 92
FLAGS_PER_INT = 30


def main():
    path = sys.argv[1]

    with open(path, 'rb') as f:
        h = f.read(HDR)
        assert h[:8] == b'FXCORR9S', 'bad magic'
        (dsindex, subintns, bps, nbuf, nsub,
         sendbytes, framebytes, fps) = struct.unpack_from('<8I', h, 60)
        fw = (bps + FLAGS_PER_INT - 1) // FLAGS_PER_INT
        rec = 28 + 4 * fw + sendbytes

        print('bps:%d' % bps)
        total = 0
        for i in range(nsub):
            f.seek(HDR + i * rec + 28)
            flags = struct.unpack('<%dI' % fw, f.read(4 * fw))
            nbad = 0
            for k in range(bps):
                if (flags[k // FLAGS_PER_INT] >> (k % FLAGS_PER_INT)) & 1 == 0:
                    nbad += 1
            total += nbad
            print('%d:%d' % (i + 1, nbad))
        print('total:%d' % total)


if __name__ == '__main__':
    main()
