#!/usr/bin/env python3
"""SWIN 逐记录比较（impl-plan 验收标准 2）.

解析两个 SWIN 文件（74 字节二进制头 + nchan*8 数据/记录，见
visibility.cpp appendSWINHeaderBuffered），逐记录比较头字段与可见度复数。

用法: cmp_swin.py <file_a> <file_b> <nchan> [maxrecords]
"""
import struct
import sys

HEADER = struct.Struct('<2I 2i d 3i 2s i d 3d')  # 74 bytes, little-endian


def parse(path, nchan):
    recs = []
    with open(path, 'rb') as f:
        data = f.read()
    off = 0
    while off + 74 <= len(data):
        sync, ver, bl, mjd, sec, cfg, src, frq, pol, pbin, w, u, v, ww = \
            HEADER.unpack_from(data, off)
        off += 74
        if sync != 0xFF00FF00:
            print(f"{path}: bad sync word at offset {off - 74}")
            break
        vis = data[off:off + nchan*8]
        if len(vis) < nchan*8:
            print(f"{path}: short record at offset {off - 74}")
            break
        off += nchan*8
        recs.append(dict(bl=bl, mjd=mjd, sec=sec, cfg=cfg, src=src, frq=frq,
                         pol=pol, pbin=pbin, w=w, uvw=(u, v, ww), vis=vis))
    return recs


def cmp_record(ra, rb, tol=1e-6):
    diffs = []
    for key in ('bl', 'mjd', 'sec', 'cfg', 'src', 'frq', 'pbin', 'w'):
        if key == 'sec' or key == 'w':
            if abs(ra[key] - rb[key]) > tol * max(1.0, abs(rb[key])):
                diffs.append(f"{key}: {ra[key]} vs {rb[key]}")
        elif ra[key] != rb[key]:
            diffs.append(f"{key}: {ra[key]} vs {rb[key]}")
    if ra['pol'] != rb['pol']:
        diffs.append(f"pol: {ra['pol']} vs {rb['pol']}")
    if ra['uvw'] != rb['uvw']:
        diffs.append(f"uvw: {ra['uvw']} vs {rb['uvw']}")
    # visibility: relative error vs |b|, tolerance like impl-plan 4.2
    va = struct.unpack(f'<{nchan*2}f', ra['vis'])
    vb = struct.unpack(f'<{nchan*2}f', rb['vis'])
    maxrel = 0.0
    maxabs = 0.0
    for i in range(len(vb)):
        maxabs = max(maxabs, abs(vb[i]))
        if abs(va[i] - vb[i]) > maxrel * max(1.0, abs(vb[i])):
            maxrel = abs(va[i] - vb[i]) / max(1.0, abs(vb[i]))
            if maxrel > tol:
                diffs.append(f"vis[{i}]: {va[i]} vs {vb[i]} (rel {maxrel:.2e})")
                break
    return diffs


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    patha, pathb, nchan = sys.argv[1], sys.argv[2], int(sys.argv[3])
    maxrec = int(sys.argv[4]) if len(sys.argv) > 4 else None

    ra = parse(patha, nchan)
    rb = parse(pathb, nchan)
    print(f"{patha}: {len(ra)} records")
    print(f"{pathb}: {len(rb)} records")
    if maxrec is not None:
        ra, rb = ra[:maxrec], rb[:maxrec]

    nbad = 0
    for i, (a, b) in enumerate(zip(ra, rb)):
        diffs = cmp_record(a, b)
        if diffs:
            nbad += 1
            print(f"record {i} (bl {b['bl']} frq {b['frq']} pol {b['pol'].decode()} sec {b['sec']:.6f}):")
            for d in diffs:
                print(f"  {d}")
    if len(ra) != len(rb):
        print(f"record count differs: {len(ra)} vs {len(rb)}")
        nbad += 1
    if nbad == 0:
        print("OK: all compared records match")
    else:
        print(f"{nbad} record(s) differ")
        sys.exit(1)
