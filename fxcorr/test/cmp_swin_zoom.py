#!/usr/bin/env python3
"""SWIN 逐记录比较——支持同一文件内多 freq（各 freq 不同 nchan）.

zoom 配置的 SWIN 把主带与 zoom band 写在同一个 .s0000.b0000 文件里，cmp_swin.py
的固定 nchan 不适用。本脚本按 SWIN 头的 freqindex 分拆记录，对每个 freq 分别
比较头字段（bl/mjd/sec/src/frq/pol/pbin/weight/uvw）与可见度复数。

用法: cmp_swin_zoom.py <file_a> <file_b> <freq0=nchan0,freq1=nchan1,...> [maxrecords]
例:   cmp_swin_zoom.py /tmp/zoom_fxcorr.s0000.b0000 config/test.difx/DIFX_58948_025200.s0000.b0000 0=4096,1=1024
"""
import struct
import sys

HEADER = struct.Struct('<2I 2i d 3i 2s i d 3d')  # 74 bytes, little-endian


def parse_nchan_map(spec):
    m = {}
    for part in spec.split(','):
        f, n = part.split('=')
        m[int(f)] = int(n)
    return m


def parse(path, nchanmap):
    recs = {f: [] for f in nchanmap}
    data = open(path, 'rb').read()
    off = 0
    while off + 74 <= len(data):
        h = HEADER.unpack_from(data, off)
        off += 74
        frq = h[7]
        if frq not in nchanmap:
            print(f"{path}: unknown freqindex {frq} at offset {off-74} (map: {nchanmap})")
            sys.exit(1)
        n = nchanmap[frq]
        recs[frq].append((h, data[off:off + n*8]))
        off += n*8
    return recs


def main():
    patha, pathb, spec = sys.argv[1], sys.argv[2], sys.argv[3]
    maxrecords = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    nchanmap = parse_nchan_map(spec)

    a = parse(patha, nchanmap)
    b = parse(pathb, nchanmap)
    for f in nchanmap:
        print(f"{patha}: freq {f}: {len(a[f])} records")
        print(f"{pathb}: freq {f}: {len(b[f])} records")

    allok = True
    for frq in sorted(nchanmap):
        ra, rb = a[frq], b[frq]
        n = min(len(ra), len(rb))
        if maxrecords > 0:
            n = min(n, maxrecords)
        print(f"freq {frq}: comparing {n} records")
        for i in range(n):
            ha, va = ra[-n+i]
            hb, vb = rb[-n+i]
            errs = []
            for k in (0, 1, 2, 3, 5, 6, 7, 9, 10, 11, 12, 13):
                if k == 3:  # sec (double)
                    if abs(ha[k] - hb[k]) > 1e-9:
                        errs.append(f"sec {ha[k]} vs {hb[k]}")
                elif k == 10:  # weight (float)
                    if abs(ha[k] - hb[k]) > 1e-6:
                        errs.append(f"w {ha[k]} vs {hb[k]}")
                elif ha[k] != hb[k]:
                    errs.append(f"h[{k}] {ha[k]} vs {hb[k]}")
            fa = struct.unpack(f'<{nchanmap[frq]*2}f', va)
            fb = struct.unpack(f'<{nchanmap[frq]*2}f', vb)
            maxrel = max((abs(x-y)/max(1.0, abs(y)) for x, y in zip(fa, fb)))
            ok = (not errs) and maxrel <= 1e-6
            if not ok:
                allok = False
            print(f"  rec {i}: {'OK' if ok else 'BAD ' + str(errs)} (vis max rel {maxrel:.2e})")

    print('ALL OK' if allok else 'MISMATCH')
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
