#!/usr/bin/env python3
"""beam.bin 数值校验（algo-plan P8；mpifxcorr 相位阵是死代码，无对拍）.

读两站的 band_00.sp 与 beam.bin，按与 BeamEngine::processSubint 相同的
累加顺序（每 acc 窗口：块序 -> 站序 -> 通道序）手算波束加权和，逐位核对：
  - 每条记录的 scan/sec/ns（窗口起点 = subint 起点 + acc 序号 x acc_ns）；
  - 波束谱 = sum_ds DWeight[ds] * sum_fft spectrum_ds[fft]（f32 相同顺序）。

用法: cmp_beam.py <sp1> <sp2> <beam.bin> <w1> <w2> [max_subints]
"""
import struct
import sys


def f32(x):
    """Round-trip a value through f32, matching the C++ float arithmetic
    (float weight * float spectrum accumulated into a float accumulator)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def read_sp(path):
    """返回 (header dict, records list)。record = (scan, sec, ns, specs[bps*nchan*2])"""
    # .sp header offsets from data-spec 5.3: magic 0..5, version 6..9,
    # band_index 10..13, pol 14..15, nchan 16..19, bandwidth 20..27,
    # bandedge 28..35, lsb 36..39, complex 40..43, n_subints 44..47,
    # subint_ns 48..51, blocks_per_send 52..55, num_buffered_ffts 56..59,
    # flag_words 60..63
    with open(path, 'rb') as f:
        hdr = f.read(256)
    magic, ver = hdr[0:6], struct.unpack_from('<I', hdr, 6)[0]
    assert magic == b'FXCSP\0' and ver == 1, 'bad .sp header'
    nchan = struct.unpack_from('<I', hdr, 16)[0]
    nsub = struct.unpack_from('<I', hdr, 44)[0]
    subns = struct.unpack_from('<I', hdr, 48)[0]
    bps = struct.unpack_from('<I', hdr, 52)[0]
    flagwords = struct.unpack_from('<I', hdr, 60)[0]

    records = []
    with open(path, 'rb') as f:
        f.seek(256)
        for _ in range(nsub):
            scan, sec, ns = struct.unpack('<3i', f.read(12))
            f.seek(flagwords * 4 + bps * 4, 1)  # flags + weights
            raw = f.read(bps * nchan * 8)
            if len(raw) < bps * nchan * 8:
                break
            specs = struct.unpack('<%df' % (bps * nchan * 2), raw)
            records.append((scan, sec, ns, specs))
    return dict(nchan=nchan, nsub=nsub, subns=subns, bps=bps), records


def main():
    sp1, sp2, beam, w1, w2 = sys.argv[1:6]
    maxsub = int(sys.argv[6]) if len(sys.argv) > 6 else 0
    w1, w2 = float(w1), float(w2)

    hdr1, recs1 = read_sp(sp1)
    hdr2, recs2 = read_sp(sp2)
    assert hdr1['nchan'] == hdr2['nchan'] and hdr1['bps'] == hdr2['bps'] \
        and hdr1['subns'] == hdr2['subns'], 'stations differ in structure'
    nchan, bps, subns = hdr1['nchan'], hdr1['bps'], hdr1['subns']

    with open(beam, 'rb') as f:
        bhdr = f.read(256)
    assert bhdr[0:6] == b'FXCBM\0' and struct.unpack_from('<I', bhdr, 6)[0] == 1, \
        'bad beam.bin header'
    bnsub = struct.unpack_from('<I', bhdr, 10)[0]
    naccs = struct.unpack_from('<I', bhdr, 14)[0]
    accns = struct.unpack_from('<I', bhdr, 18)[0]
    nsegs = struct.unpack_from('<I', bhdr, 22)[0]
    segs = []
    with open(beam, 'rb') as f:
        f.seek(256)
        for i in range(nsegs):
            fi, pol, nc = struct.unpack('<IcI', f.read(9))
            segs.append((fi, pol.decode(), nc))
        accffts = accns * bps // subns  # FFT blocks per acc window
        assert accns * bps % subns == 0 and accffts % 10 == 0, \
            'acc window not an integer number of buffered-FFT batches'
        assert all(nc == nchan for _, _, nc in segs), 'seg nchan != .sp nchan'

        ncheck = min(bnsub, len(recs1), len(recs2))
        if maxsub:
            ncheck = min(ncheck, maxsub)
        bad = 0
        for s in range(ncheck):
            for a in range(naccs):
                bscan, bsec, bns = struct.unpack('<3i', f.read(12))
                # expected window timestamp
                wns = recs1[s][2] + a * accns
                esec = recs1[s][1] + wns // 1000000000
                ens = wns % 1000000000
                if (bscan, bsec, bns) != (recs1[s][0], esec, ens):
                    print(f'MISMATCH ts subint {s} acc {a}: '
                          f'{bscan}/{bsec}/{bns} vs {recs1[s][0]}/{esec}/{ens}')
                    bad += 1
                # reference beam, same accumulation order and precision as
                # BeamEngine (fft order -> station order -> channel order,
                # f32 weight x f32 spectrum into a f32 accumulator)
                w1f, w2f = f32(w1), f32(w2)
                ref = [0.0] * (nchan * 2)
                fbase = a * accffts
                for ft in range(accffts):
                    o = (fbase + ft) * nchan * 2
                    for ch in range(nchan * 2):
                        ref[ch] = f32(ref[ch] + f32(w1f * recs1[s][3][o + ch]))
                        ref[ch] = f32(ref[ch] + f32(w2f * recs2[s][3][o + ch]))
                raw = f.read(nsegs * nchan * 8)
                got = struct.unpack('<%df' % (nsegs * nchan * 2), raw)
                for i in range(nchan * 2):
                    if got[i] != ref[i]:
                        # f32 comparison; report the first few mismatches
                        if bad < 5:
                            print(f'MISMATCH subint {s} acc {a} ch {i//2} '
                                  f'{"re" if i % 2 == 0 else "im"}: '
                                  f'{got[i]} vs {ref[i]}')
                        bad += 1
                        break
            print(f'subint {s}: {naccs} accs checked')
    if bad:
        print(f'{bad} mismatches - FAIL')
        sys.exit(1)
    print(f'{ncheck} subints x {naccs} accs all identical - PASS')


if __name__ == '__main__':
    main()
