#!/usr/bin/env python3
"""LBA fxcorr 自洽验证（mpifxcorr 基准不可用时的降级验收）。

验证两项：
1. fengine autocorr.bin 峰位：T1 tone 1.5MHz -> ch = 1.5/4*nchan（4096 通道
   时 = 1536），T2 tone 1.0MHz -> ch = 1.0/4*nchan（= 1024）。实正弦有镜像峰
   （nchan-ch），取低频半幅内最大。
2. fxcorr-lba.difx SWIN：逐记录 weight > 0 且有限，可见度无 NaN/Inf。

用法: verify_lba.py <fengine_dir> <swin_file> <nchan>
"""

import struct
import sys
import math

HEADER = struct.Struct('<2I 2i d 3i 2s i d 3d')  # 74 bytes, little-endian


def parse_autocorr(path):
    """返回 (spec 功率和, weight 和)。格式见 fenginewriter.cpp writeAutocorrHeader/"""
    with open(path, 'rb') as f:
        d = f.read()
    assert d[:6] == b'FXCAC\0', f'{path}: bad magic'
    off = 6
    version, nsub, acb, nbands, crosspol = struct.unpack_from('<5I', d, off)
    off += 20
    bands = []
    for j in range(nbands):
        bi, nc = struct.unpack_from('<2I', d, off)
        off += 8
        bands.append((bi, nc))
    offsets = [0]
    for bi, nc in bands:
        offsets.append(offsets[-1] + nc)
    nchan = offsets[-1]
    spec = [0.0] * nchan
    wsum = 0.0
    nseg = 1 + (1 if crosspol else 0)
    for s in range(nsub):
        for a in range(acb):
            for seg in range(nseg):
                for j, (bi, nc) in enumerate(bands):
                    vals = struct.unpack_from(f'<{nc * 2}f', d, off)
                    off += nc * 8
                    base = offsets[j]
                    for k in range(nc):
                        spec[base + k] += vals[2 * k] ** 2 + vals[2 * k + 1] ** 2
                ws = struct.unpack_from(f'<{nbands}f', d, off)
                off += nbands * 4
                wsum += sum(ws) / nbands
    return spec, wsum, nchan


def check_autocorr(station, tone_mhz, nchan_expect, fdir):
    spec, w, nchan = parse_autocorr(f'{fdir}/58948_25200/{station}/autocorr.bin')
    assert nchan == nchan_expect, f'{station}: nchan={nchan} != {nchan_expect}'
    expect_ch = int(round(tone_mhz / 4.0 * nchan))
    # 低频半幅内找最大（镜像峰在 nchan-ch，不参与）
    half = nchan // 2
    peak = max(range(1, half), key=lambda k: spec[k])
    ok = peak == expect_ch
    print(f'{station}: peak ch={peak} expect={expect_ch} pow={spec[peak]:.3e} '
          f'avgweight={w / 1.0:.4f} {"PASS" if ok else "FAIL"}')
    return ok


def check_swin(path, nchan):
    with open(path, 'rb') as f:
        d = f.read()
    off = 0
    n = 0
    bad = 0
    wmin, wmax = float('inf'), 0.0
    while off + 74 <= len(d):
        sync, ver, bl, mjd, sec, cfg, src, frq, pol, pbin, w, u, v, ww = \
            HEADER.unpack_from(d, off)
        off += 74
        if sync != 0xFF00FF00:
            print(f'sync word bad at record {n}')
            return False
        vis = d[off:off + nchan * 8]
        off += nchan * 8
        if len(vis) < nchan * 8:
            print(f'short record {n}')
            return False
        vals = struct.unpack(f'<{nchan * 2}f', vis)
        if any(not math.isfinite(x) for x in vals):
            bad += 1
        wmin = min(wmin, w)
        wmax = max(wmax, w)
        n += 1
    ok = n > 0 and bad == 0 and wmin > 0
    print(f'SWIN: {n} records, weight range [{wmin:.4f}, {wmax:.4f}], '
          f'non-finite vis in {bad} records, {"PASS" if ok else "FAIL"}')
    return ok


def main():
    if len(sys.argv) != 4:
        print('usage: verify_lba.py <fengine_dir> <swin_file> <nchan>')
        sys.exit(1)
    fdir, swin, nchan = sys.argv[1], sys.argv[2], int(sys.argv[3])
    ok = True
    ok &= check_autocorr('T1', 1.5, nchan, fdir)
    ok &= check_autocorr('T2', 1.0, nchan, fdir)
    ok &= check_swin(swin, nchan)
    print('ALL PASS' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
