#!/usr/bin/env python3
"""逐记录比较两个 DifxMessageSTARecord 原始流（P9 STA 对拍工具）.

两个输入都是原始 record 追加流（sta_ctrl recv 的抓包 / fxcorr-f container
模式 meta/difxmsg/<...>.sta，布局相同）：每条 = 72 字节头（10 个 int +
32 字节 identifier）+ nChan 个 f32。

按 (messageType, dsindex, bandindex, scan, sec, ns, nswidth) 分组匹配，
组内 nChan 与 data 逐位（f32 位模式）比对——两边同为该机 f32 运算与
libm，逐位相等是预期。组播丢包会表现为 missing/extra，重跑即可。

用法: cmp_sta.py <bench.sta> <fxcorr.sta> [min_absns]

min_absns（可选）：只比对 subint 绝对起点 (sec*1e9+ns) >= min_absns 的记录。
mpifxcorr 的 dump 由运行时控制消息开启，消息在线程 spawn 前发出的会丢，
首 subint 常无基准记录；两边用同一 min_absns（如 524288000 从第 2 个
subint 起）过滤后对齐。identifier（jobname）不比对：基准与 fxcorr 的
.input 文件名可能不同（test-stampi vs test-sta）。
"""
import struct
import sys

HEADER = '<10i32s'
HEADER_BYTES = struct.calcsize(HEADER)  # 72


def parse(path, min_absns):
    with open(path, 'rb') as f:
        data = f.read()

    recs = {}
    off = 0
    while off + HEADER_BYTES <= len(data):
        hdr = struct.unpack_from(HEADER, data, off)
        off += HEADER_BYTES
        nchan = hdr[9]
        if nchan <= 0 or nchan > 65536:
            print(f'{path}: bad nChan {nchan} at offset {off - HEADER_BYTES}')
            break
        if off + 4 * nchan > len(data):
            print(f'{path}: truncated record at offset {off - HEADER_BYTES}')
            break
        vals = struct.unpack_from(f'<{nchan}f', data, off)
        off += 4 * nchan

        # messageType, dsindex, bandindex, scan, sec, ns, nswidth
        key = (hdr[0], hdr[5], hdr[8], hdr[1], hdr[2], hdr[3], hdr[4])
        if min_absns is not None and hdr[2] * 1000000000 + hdr[3] < min_absns:
            continue
        if key in recs:
            print(f'{path}: duplicate record {key}')
        recs[key] = (hdr, vals)
    return recs


def main():
    bench, fx = sys.argv[1], sys.argv[2]
    min_absns = int(sys.argv[3]) if len(sys.argv) > 3 else None
    benchrecs = parse(bench, min_absns)
    fxrecs = parse(fx, min_absns)

    missing = sorted(set(benchrecs) - set(fxrecs))
    extra = sorted(set(fxrecs) - set(benchrecs))
    if missing:
        print(f'missing {len(missing)} records (in benchmark, not in fxcorr):')
        for k in missing[:20]:
            print(f'  {k}')
    if extra:
        print(f'extra {len(extra)} records (in fxcorr, not in benchmark):')
        for k in extra[:20]:
            print(f'  {k}')

    matched = 0
    mismatched = 0
    for key in sorted(set(benchrecs) & set(fxrecs)):
        bhdr, bvals = benchrecs[key]
        fhdr, fvals = fxrecs[key]
        # nChan must agree; coreindex/threadindex are 0 on both sides and
        # identifier is the jobname (.input basename, may legitimately
        # differ: test-stampi vs test-sta), so skip those
        if bhdr[9] != fhdr[9]:
            print(f'nChan differs {key}: bench {bhdr[9]} vs fx {fhdr[9]}')
            mismatched += 1
            continue
        bad = [i for i in range(bhdr[9]) if bvals[i] != fvals[i]]
        if bad:
            print(f'data differs {key}: {len(bad)}/{bhdr[9]} channels, e.g. chan {bad[0]}: '
                  f'{bvals[bad[0]]} vs {fvals[bad[0]]}')
            mismatched += 1
        else:
            matched += 1

    total = len(benchrecs)
    print(f'benchmark {total} records, fxcorr {len(fxrecs)} records; '
          f'matched {matched}, mismatched {mismatched}, missing {len(missing)}, extra {len(extra)}')
    if matched > 0 and mismatched == 0 and not missing and not extra:
        print('PASS: all records bitwise identical')
        return 0
    print('FAIL')
    return 1


if __name__ == '__main__':
    main()
