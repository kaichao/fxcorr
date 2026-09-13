#!/usr/bin/env python3
"""从无 zoom 的 .input 生成 zoom 测试变体（algo-plan P4a 检验资产）.

在 test.input（1 父 band）基础上生成 test-zoom.input：
  1. FREQ TABLE 追加一个 zoom 窄带项（默认 201.5MHz 起、1MHz、1024ch——
     与父 4MHz/4096ch 的通道间隔 976.5625Hz 对齐：nchan = BW/父通道间隔）；
  2. 每个 DATASTREAM 段的 NUM ZOOM FREQS 0 -> 1 + ZOOM 定义行；
  3. BASELINE 段加第二 freq（TARGET FREQ = zoom freq index）。
注意：D/STREAM A/B BAND 行的 key 序号是 pol product 序号（configuration.cpp
按 k 取行），不是 freq 序号——写错会静默解析为 0。

用法: gen_test_zoom.py <input_file> [zoom_lowedge_mhz] [zoom_bw_mhz] [zoom_nchan]
输出: 同目录 <basename>-zoom.input（与 EXECUTE TIME 截断变体 <basename>-zoom-mpi2.input，
EXECUTE TIME 改为 2 截断到完整积分段供 mpifxcorr 基准）
"""
import sys
import os


def main():
    path = sys.argv[1]
    lowedge = float(sys.argv[2]) if len(sys.argv) > 2 else 201.5
    bw = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    nchan = int(sys.argv[4]) if len(sys.argv) > 4 else 1024

    src = open(path).read()

    # 1) FREQ TABLE: append zoom freq 1 after the first freq block
    src = src.replace('FREQ ENTRIES:', 'FREQ ENTRIES:', 1)  # placeholder, real edit below
    zoomfreq = f'''FREQ (MHZ) 1:       {lowedge:.6f}
BW (MHZ) 1:         {bw:.6f}
SIDEBAND 1:         U
NUM CHANNELS 1:     {nchan}
CHANS TO AVG 1:     1
OVERSAMPLE FAC. 1:  1
DECIMATION FAC. 1:  1
PHASE CALS 1 OUT:   0
'''
    src = src.replace('PHASE CALS 0 OUT:   0\n', 'PHASE CALS 0 OUT:   0\n' + zoomfreq, 1)
    src = src.replace('FREQ ENTRIES:       1', 'FREQ ENTRIES:       2', 1)

    # 2) DATASTREAM: NUM ZOOM FREQS 0 -> 1 in every datastream section
    zoomds = '''NUM ZOOM FREQS:     1
ZOOM FREQ INDEX 0:  1
NUM ZOOM POLS 0:    1
ZOOM BAND 0 POL:    R
ZOOM BAND 0 INDEX:  0
'''
    src = src.replace('NUM ZOOM FREQS:     0\n', zoomds)

    # 3) BASELINE: second freq = zoom freq index 1, one pol product
    src = src.replace('''NUM FREQS 0:        1
TARGET FREQ 0/0:    0
POL PRODUCTS 0/0:   1
D/STREAM A BAND 0:  0
D/STREAM B BAND 0:  0
''', '''NUM FREQS 0:        2
TARGET FREQ 0/0:    0
POL PRODUCTS 0/0:   1
D/STREAM A BAND 0:  0
D/STREAM B BAND 0:  0
TARGET FREQ 0/1:    1
POL PRODUCTS 0/1:   1
D/STREAM A BAND 0:  1
D/STREAM B BAND 0:  1
''', 1)

    base = os.path.splitext(path)[0]
    out = base + '-zoom.input'
    open(out, 'w').write(src)
    open(base + '-zoom-mpi2.input', 'w').write(
        src.replace('EXECUTE TIME (SEC): 1200', 'EXECUTE TIME (SEC): 2'))
    print(f'wrote {out} and {base}-zoom-mpi2.input')


if __name__ == '__main__':
    main()
