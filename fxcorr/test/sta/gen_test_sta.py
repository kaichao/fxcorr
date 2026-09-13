#!/usr/bin/env python3
"""生成 STA 对拍 .input 变体（algo-plan P9 检验资产）.

在 test.input 基础上生成：
  <basename>-sta.input     CHANS TO AVG 0: 1 -> 4（激活 STA 频域平均分支的真实考验：
                           minpostavfreqchannels 4096 -> 1024，仍 >= STADumpChannels 32；
                           CHANS TO AVG=1 时平均分支等价于非平均，测不出差异）
  <basename>-stampi.input  上者 + EXECUTE TIME 截断到 2（mpifxcorr 基准，照 test-mpi2 先例）

CHANS TO AVG>1 会让 autocorr.bin 通道数变小，SWIN 对拍（fxcorr-x）不用这些变体，
仅用于 STA 抓包对拍；kurtosis 折叠用 recordedbandchannels 与 CHANS TO AVG 无关。

用法: gen_test_sta.py <input_file>
"""
import os
import sys


def main():
    path = sys.argv[1]
    src = open(path).read()

    if 'CHANS TO AVG 0:     4' in src:
        print('already averaged, nothing to do')
        return

    src4 = src.replace('CHANS TO AVG 0:     1', 'CHANS TO AVG 0:     4')
    if src4 == src:
        print('CHANS TO AVG 0: 1 line not found, no variant written')
        return

    base = os.path.splitext(path)[0]
    open(base + '-sta.input', 'w').write(src4)
    open(base + '-stampi.input', 'w').write(
        src4.replace('EXECUTE TIME (SEC): 1200', 'EXECUTE TIME (SEC): 2'))
    print(f'wrote {base}-sta.input and {base}-stampi.input')


if __name__ == '__main__':
    main()
