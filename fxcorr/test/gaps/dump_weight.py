#!/usr/bin/env python3
"""按积分列出 SWIN 记录的 weight，用于定位 weight 差异落在哪个基线/频点.

用法: dump_weight.py <swin> <nchan> [--diff <other.swin>]

不带 --diff 时打印每个记录的 (积分序号, bl, frq, pol, weight)；
带 --diff 时只打印两个文件 weight 不同的记录。
"""
import struct
import sys

HEADER = struct.Struct('<2I 2i d 3i 2s i d 3d')


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
            print('%s: bad sync at %d' % (path, off - 74))
            break
        off += nchan * 8
        recs.append(dict(bl=bl, sec=sec, frq=frq, pol=pol, w=w))
    return recs


def main():
    path = sys.argv[1]
    nchan = int(sys.argv[2])
    other = None
    if '--diff' in sys.argv:
        other = sys.argv[sys.argv.index('--diff') + 1]

    a = parse(path, nchan)
    if other is None:
        for i, r in enumerate(a):
            print('%4d bl %5d frq %3d pol %s sec %12.6f w %.9f'
                  % (i, r['bl'], r['frq'], r['pol'].decode(), r['sec'], r['w']))
        return

    b = parse(other, nchan)
    print('records: %d vs %d' % (len(a), len(b)))
    t0 = min(r['sec'] for r in a)
    nbad = 0
    for i, (ra, rb) in enumerate(zip(a, b)):
        if abs(ra['w'] - rb['w']) > 1e-9:
            nbad += 1
            print('rec %4d int %2d bl %5d frq %3d pol %s : w=%12.9f vs %12.9f (rel %+.3e)'
                  % (i, int((ra['sec'] - t0) / 1.024), ra['bl'], ra['frq'],
                     ra['pol'].decode(), ra['w'], rb['w'],
                     (ra['w'] - rb['w']) / max(1e-30, rb['w'])))
    print('%d record(s) with weight difference' % nbad)


if __name__ == '__main__':
    main()
