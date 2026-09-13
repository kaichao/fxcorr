#!/usr/bin/env python3
"""从无相位阵的 .input 生成相位阵测试变体（algo-plan P8 检验资产）.

在 test.input（2 站单 band、SUBINT 524288000ns = 256 FFT 块、numbufferedffts=10）
基础上生成 test-phasearr.input + test-phasearr.pa：
  1. CONFIG 段 PHASED ARRAY FALSE -> TRUE，其后插
     PHASED ARRAY CONFIG FILE 行（指向同目录 test-phasearr.pa）；
  2. SUBINT NANOSECONDS 524288000 -> 81920000（40 FFT 块）：
     上游 Configuration 校验相位阵时 accffts（= ACC TIME/ffttime）必须是
     numbufferedffts 的整数倍（configuration.cpp populateResultLengths），
     而原 SUBINT 的 256 块不是 10 的整数倍——任何 ACC TIME 都会被拒绝。
     40 块 = 4 个 numbufferedffts 批，ACC TIME 默认 20480000ns（10 块）
     得 numaccs = 4 个窗口；
  3. 生成相位阵配置文件（格式照 processPhasedArrayConfig 的解析序）：
     OUTPUT TYPE FILTERBANK / OUTPUT FORMAT DIFX / ACC TIME (NS) /
     COMPLEX OUTPUT FALSE / OUTPUT BITS 32 / 每 freq：NUM FREQ + 各 pol
     的 FREQ 行 + 各 datastream 权重的 FREQ 行。

注意：mpifxcorr 相位阵链路是死代码（core.cpp 波束结果无消费者），本资产
不做 mpifxcorr 对拍；数值验证由 cmp_beam.py 手算加权和逐位核对。

用法: gen_test_phasearr.py <input_file> [w_t1] [w_t2] [acc_ns]
输出: 同目录 <basename>-phasearr.input 与 <basename>-phasearr.pa
"""
import sys
import os


def kv(key, val):
    """Format a key:val line for DiFX's key:val parser (getinputkeyval):
    the value starts at column 20 (0-based), i.e. after the key + ':' padded
    with spaces to 20 chars.  Shorter padding silently corrupts the value
    (substr(20)); longer keys read from just after the colon."""
    prefix = key + ':'
    pad = ' ' * max(0, 20 - len(prefix))
    return prefix + pad + val


def main():
    path = sys.argv[1]
    w1 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    w2 = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    accns = int(sys.argv[4]) if len(sys.argv) > 4 else 20480000

    src = open(path).read()

    assert 'PHASED ARRAY:       FALSE' in src, 'expected PHASED ARRAY: FALSE line'
    base = os.path.splitext(path)[0]
    paname = os.path.basename(base) + '-phasearr.pa'
    src = src.replace(
        'PHASED ARRAY:       FALSE',
        kv('PHASED ARRAY', 'TRUE') + '\n' + kv('PHASED ARRAY CONFIG FILE', 'config/' + paname),
        1)
    assert 'SUBINT NANOSECONDS: 524288000' in src, 'expected the standard SUBINT'
    src = src.replace('SUBINT NANOSECONDS: 524288000',
                      'SUBINT NANOSECONDS: 81920000', 1)

    # phased array config file (parsed by Configuration::processPhasedArrayConfig:
    # per freq table entry: NUM FREQ, then NUM FREQ pol lines, then one weight
    # line per datastream)
    pa = '\n'.join([
        kv('OUTPUT TYPE', 'FILTERBANK'),
        kv('OUTPUT FORMAT', 'DIFX'),
        kv('ACC TIME (NS)', str(accns)),
        kv('COMPLEX OUTPUT', 'FALSE'),
        kv('OUTPUT BITS', '32'),
        kv('NUM FREQ', '1'),
        kv('FREQ', 'R'),
        kv('FREQ', str(w1)),
        kv('FREQ', str(w2)),
    ]) + '\n'
    open(base + '-phasearr.input', 'w').write(src)
    open(base + '-phasearr.pa', 'w').write(pa)
    print(f'wrote {base}-phasearr.input and {base}-phasearr.pa '
          f'(weights {w1}/{w2}, acc {accns}ns)')


if __name__ == '__main__':
    main()
