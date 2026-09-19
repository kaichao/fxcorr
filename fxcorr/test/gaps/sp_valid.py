#!/usr/bin/env python3
"""统计 band_XX.sp 里无效的 FFT 块（valid_flags=0）在时间上的分布.

用法: sp_valid.py <band_00.sp> [--int N]

打印每个积分的无效块数与该积分的平均权重；--int 指定时逐 subint 打印。
布局见 fxcorr/data-spec.md 5.3：每 subint = i32 scan + i32 sec + i32 ns +
u32[flag_words] valid_flags + f32[bps] weights + spectra
"""
import struct
import sys

HDR = 256


def main():
    path = sys.argv[1]
    only_int = None
    if '--int' in sys.argv:
        only_int = int(sys.argv[sys.argv.index('--int') + 1])
    machine = '--machine' in sys.argv

    with open(path, 'rb') as f:
        h = f.read(HDR)
        assert h[:6] == b'FXCSP\0', 'bad magic'
        version, band_index = struct.unpack_from('<II', h, 6)
        numch = struct.unpack_from('<I', h, 16)[0]
        nsub, subint_ns, bps, nbuf, fw = struct.unpack_from('<IIIII', h, 44)

    rec = 12 + fw * 4 + bps * 4 + bps * numch * 8
    if not machine:
        print('band_index %d nchan %d n_subints %d subint_ns %d bps %d flagwords %d rec %d'
              % (band_index, numch, nsub, subint_ns, bps, fw, rec))

    subperint = int(round(1.024 / (subint_ns / 1e9)))
    nint = nsub // subperint

    with open(path, 'rb') as f:
        rows = []
        for i in range(nsub):
            f.seek(HDR + i * rec)
            b = f.read(12 + fw * 4 + bps * 4)
            scan, sec, ns = struct.unpack_from('<iii', b, 0)
            flags = struct.unpack_from('<%dI' % fw, b, 12)
            wts = struct.unpack_from('<%df' % bps, b, 12 + fw * 4)
            nbad = 0
            for k in range(bps):
                if (flags[k // 30] >> (k % 30)) & 1 == 0:
                    nbad += 1
            rows.append((scan, sec, ns, nbad, sum(wts) / bps))
            del wts

    if machine:
        # one line per subint for scripted comparisons (run_filler.sh)
        print('bps:%d' % bps)
        for j, r in enumerate(rows):
            print('%d:%d' % (j + 1, r[3]))
        print('total:%d' % sum(r[3] for r in rows))
        return

    print('integration  invalid_blocks  mean_weight  (blocks/integration %d)'
          % (subperint * bps))
    for it in range(nint):
        chunk = rows[it * subperint:(it + 1) * subperint]
        nb = sum(r[3] for r in chunk)
        mw = sum(r[4] for r in chunk) / len(chunk)
        if only_int is not None and it != only_int:
            continue
        print('%11d  %14d  %.6f' % (it, nb, mw))
        if only_int is not None:
            for j, r in enumerate(chunk):
                if r[3] > 0 or r[4] < 0.999:
                    print('    subint %4d (idx %4d, sec %d ns %9d): invalid %5d  w %.6f'
                          % (it * subperint + j, it * subperint + j, r[1], r[2], r[3], r[4]))


if __name__ == '__main__':
    main()
