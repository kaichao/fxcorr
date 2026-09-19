#!/usr/bin/env python3
"""扫描一个 VDIF 文件，汇总帧号链上的中断（缺口与全零 filler 段）.

用法: scan_filler.py <file.vdif> [fps]

输出：首帧帧头、总帧数，以及每一段"异常"的摘要：
  类型 zero-run（全零头）/ step（帧号跳跃）/ invalid（invalid 位置位）
  起止 fileidx、帧数、段前段后的帧号（时间轴跳了多少帧）、字节数
"""
import os
import struct
import sys


def parse(b):
    """按 vdifio 的字布局解析 32 字节头（小端 8 个 u32）。"""
    w = struct.unpack('<8I', b)
    return {
        'seconds': w[0] & 0x3FFFFFFF,
        'legacy': (w[0] >> 30) & 1,
        'invalid': (w[0] >> 31) & 1,
        'frame': w[1] & 0xFFFFFF,
        'epoch': (w[1] >> 24) & 0x3F,
        'framelength8': w[2] & 0xFFFFFF,
        'nchan': (w[2] >> 24) & 0x1F,
        'threadid': (w[3] >> 16) & 0x3FF,
        'nbits': (w[3] >> 26) & 0x1F,
        'allzero': b == b'\0' * 32,
    }


def main():
    path = sys.argv[1]
    fps = int(sys.argv[2]) if len(sys.argv) > 2 else 16000
    size = os.path.getsize(path)

    with open(path, 'rb') as f:
        h = parse(f.read(32))
    framebytes = h['framelength8'] * 8
    nframes = size // framebytes

    print('file        : %s (%d bytes)' % (path, size))
    print('first frame : framebytes=%d seconds=%d frame=%d epoch=%d invalid=%d'
          % (framebytes, h['seconds'], h['frame'], h['epoch'], h['invalid']))
    print('              stationid=%d threadid=%d nbits=%d nchan=%d version=%d'
          % (h['stationid'] if False else 0, h['threadid'], h['nbits'],
             h['nchan'], 0))
    print('total frames: %d (stride %d, remainder %d)'
          % (nframes, framebytes, size - nframes * framebytes))

    def fno_of(d):
        return d['epoch'] * (fps * 1000000) + d['seconds'] * fps + d['frame']

    segs = []
    cur = None          # (kind, start_idx)
    prev = None         # (idx, fno, frame, seconds, invalid)

    with open(path, 'rb') as f:
        for i in range(nframes):
            f.seek(i * framebytes)
            d = parse(f.read(32))
            fno = fno_of(d)

            if d['allzero']:
                kind = 'zero-run'
            elif d['invalid']:
                kind = 'invalid'
            elif d['framelength8'] * 8 != framebytes:
                kind = 'framelength=%d' % (d['framelength8'] * 8)
            elif prev is not None and fno != prev[1] + 1:
                kind = 'step=%+d' % (fno - prev[1] - 1)
            else:
                kind = None

            if kind is None:
                if cur is not None:
                    segs.append((cur[0], cur[1], i - 1))
                    cur = None
            elif cur is None:
                cur = (kind, i)
            elif cur[0] != kind:
                segs.append((cur[0], cur[1], i - 1))
                cur = (kind, i)

            prev = (i, fno, d['frame'], d['seconds'], d['invalid'])

        if cur is not None:
            segs.append((cur[0], cur[1], nframes - 1))

    print('abnormal segments: %d' % len(segs))
    print()
    print('  kind          fileidx         nframes  fno_before     fno_after   jump  bytes')
    for (kind, a, b) in segs:
        with open(path, 'rb') as f:
            f.seek((a - 1) * framebytes)
            db = parse(f.read(32))
            f.seek((b + 1) * framebytes)
            da = parse(f.read(32))
        fb, fa = fno_of(db), fno_of(da)
        print('  %-12s  %7d..%-7d %7d  %12d  %12d %6d  %d'
              % (kind, a, b, b - a + 1, fb, fa, fa - fb - 1,
                 (b - a + 1) * framebytes))


if __name__ == '__main__':
    main()
