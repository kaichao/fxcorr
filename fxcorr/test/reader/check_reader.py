#!/usr/bin/env python3
"""reader 对账：把 fxcorr-f 的逐 subint 诊断与 VDIF 文件真值对上.

用法:
  check_reader.py --log <f 侧 verbose 日志> --vdif <file.vdif>
                  [--truth truth.json] [--batch-json batches/<id>.json]
                  [--fps N] [--json out.json] [--quiet]

背景见 fxcorr/test/reader/README.md。三条断言各管一段，互不代偿：

  E1 定位  读取窗口起点应落在名义时间轴上（batch 起点 + k×subint），相邻 subint 差
           恒定。位置错（A/B/C 类）在这里现形。
  E2 数据  readoff 处文件里的第一个数据帧，其帧号应等于该 subint 报的 firstfno。
  E3 落点  文件真值算出的空洞槽区间 == GAPCHECK holes 报的槽区间。多标 = fxcorr 把
           有数据的槽判成洞（丢好数据），漏标 = 洞被当数据积分。

E3 的窗口起点由**帧号**反推（第一个数据帧的帧号与 framens 定出 f_lo = sec*fps + framens），
不采信读到的缓冲区自身——否则 reader 自证（这正是 GAPCHECK holes 单独看时做不到的）。
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import file_truth

READPOS_RE = re.compile(
    r'READPOS subint (\d+): readoff (-?\d+) firstfno (-?\d+) lastfno (-?\d+) '
    r'nframes (\d+) missing (\d+) filler (\d+) gapshift (-?\d+) fillershift (-?\d+) '
    r'gapframes (-?\d+) dst (-?\d+) framens (-?\d+)'
    r'(?: uncorr (-?\d+) passes (\d+))?')	# uncorr/passes 是 2026-09-18 加的
HOLES_RE = re.compile(r'GAPCHECK holes buf (\d+):(.*)')
HOLE_RANGE_RE = re.compile(r'\[(-?\d+),(-?\d+)\)')


def parse_log(path):
    """抽 READPOS 与 GAPCHECK holes 两类逐 subint 诊断."""
    subints = []
    holes = {}
    with open(path, errors='replace') as f:
        for line in f:
            m = READPOS_RE.search(line)
            if m:
                v = [int(x) if x is not None else None for x in m.groups()]
                subints.append({
                    'n': v[0], 'readoff': v[1], 'firstfno': v[2], 'lastfno': v[3],
                    'nframes': v[4], 'missing': v[5], 'filler': v[6], 'gapshift': v[7],
                    'fillershift': v[8], 'gapframes': v[9], 'dst': v[10], 'framens': v[11],
                    'uncorr': v[12], 'passes': v[13],
                })
                continue
            m = HOLES_RE.search(line)
            if m:
                holes[int(m.group(1))] = [(int(a), int(b)) for (a, b) in HOLE_RANGE_RE.findall(m.group(2))]
    return subints, holes


def norm(ranges):
    """排序并合并相邻/重叠区间."""
    out = []
    for (a, b) in sorted(ranges):
        if b <= a:
            continue
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return [(a, b) for (a, b) in out]


def compare_ranges(truth, got):
    """返回 (多标, 漏标)：两侧都是已归一化的区间列表."""
    extra = []			# fxcorr 标了、真值说不是洞
    missing = []		# 真值说是洞、fxcorr 没标
    for (a, b) in truth:
        cur = a
        for (c, d) in got:
            if d <= cur:
                continue
            if c >= b:
                break
            if c > cur:
                missing.append((cur, min(c, b)))
            cur = max(cur, d)
            if cur >= b:
                break
        if cur < b:
            missing.append((cur, b))
    for (a, b) in got:
        cur = a
        for (c, d) in truth:
            if d <= cur:
                continue
            if c >= b:
                break
            cur = max(cur, d)
            if cur >= b:
                break
        if cur < b:
            extra.append((cur, b))
    return extra, missing


def infer_span(rows):
    """subint 跨度的帧数：没有 subint_ns 时用相邻窗口起点之差推."""
    prev = None
    for row in rows:
        if 'f_lo' in row:
            if prev is not None:
                return row['f_lo'] - prev
            prev = row['f_lo']
    return None


def first_data_frame(fh, firstidx, nframes, framebytes, total):
    """文件帧 firstidx 起、窗口内的第一个数据帧: (sec, frame, 相对帧序号)."""
    for k in range(nframes):
        i = firstidx + k
        if i >= total:
            return None, None, None
        fh.seek(i * framebytes)
        d = file_truth.parse_header(fh.read(file_truth.HDRBYTES))
        if d['framelength8'] * 8 != framebytes or file_truth.is_filler(d):
            continue
        return d['seconds'], d['frame'], k
    return None, None, None


def main():
    ap = argparse.ArgumentParser(description='check fxcorr-f diagnostics against the VDIF truth')
    ap.add_argument('--log', required=True)
    ap.add_argument('--vdif', required=True)
    ap.add_argument('--truth', help='ledger JSON (default: scan the VDIF now)')
    ap.add_argument('--batch-json', help='batch metadata, for the E1 absolute anchor')
    ap.add_argument('--fps', type=int)
    ap.add_argument('--json', dest='jsonpath')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--only-bad', action='store_true', help='print only the subints with a finding')
    args = ap.parse_args()

    subints, holes = parse_log(args.log)
    if not subints:
        raise SystemExit('check_reader: no READPOS lines in %s (need FXCORR_LOGLEVEL=verbose)' % args.log)

    truth = file_truth.load(args.truth) if args.truth else file_truth.scan(args.vdif, args.fps)
    fps = args.fps or truth['fps']
    if not fps:
        raise SystemExit('check_reader: no fps (pass --fps)')
    framebytes = truth['framebytes']
    total = truth['nframes']

    # subint 跨度（E1 的判据与 f_lo 递推都要用）
    subint_ns = None
    if args.batch_json:
        with open(args.batch_json) as f:
            subint_ns = json.load(f).get('subint_ns')

    span = int(round(subint_ns / 1e9 * fps)) if subint_ns else None
    rows = []
    nbad = 0
    extra_slots = 0
    missing_slots = 0
    fh = open(args.vdif, 'rb')
    try:
        for k, s in enumerate(subints):
            row = dict(s)
            readoff = s['readoff']
            idx = readoff // framebytes
            row['e2'] = None
            if 0 <= idx < total and readoff % framebytes == 0:
                sec, frame, k2 = first_data_frame(fh, idx, s['nframes'], framebytes, total)
                if sec is None:
                    # 整个窗口都是 filler：读不出帧号来定锚，但有洞是确定的（见下面的递推）
                    row['e2'] = 'no data frame in window'
                else:
                    row['e2'] = 'ok' if frame == s['firstfno'] else 'firstfno %d vs file %d' % (s['firstfno'], frame)
                    # E3：窗口起点由帧号反推（f_lo ≡ framens mod fps，取 ≤ 该帧的那个）
                    d = (frame - s['framens']) % fps
                    row['f_lo'] = sec * fps + frame - d
            else:
                row['e2'] = 'readoff past EOF'
            rows.append(row)
    finally:
        fh.close()

    # 锚只要有一条成立就能推其余：相邻 subint 的窗口起点差恒为跨度。窗口里没有数据帧
    # 的那些（整段 filler）因此也能对账——它们的洞是 [0, nframes)。
    hs = span if span else infer_span(rows)
    if hs:
        for k, row in enumerate(rows):
            if 'f_lo' in row:
                continue
            prev = next((rows[j]['f_lo'] for j in range(k - 1, -1, -1) if 'f_lo' in rows[j]), None)
            if prev is not None:
                row['f_lo'] = prev + hs

    for row in rows:
        if 'f_lo' not in row:
            continue
        row['truth_holes'] = norm(file_truth.holes_in_window(
            truth, row['f_lo'], row['f_lo'] + row['nframes'], fps))
        got = list(holes.get(row['n'], []))
        # A 类起点偏移（文件起点晚于 batch 起点）不走 gapinvalid：locate 算出负位置后由
        # settleReadPosition 钳到文件首帧、把跳过的块记进 lastcount，fillValidFlags 再把
        # 它们置为无效。那不体现在 GAPCHECK holes 里，所以用 uncorr 反推补上——同一段
        # 代码的同一公式（skippedframes = ceil(-uncorr/framebytes)）。
        if row.get('uncorr') is not None and row['uncorr'] < 0:
            skf = (-row['uncorr'] + framebytes - 1) // framebytes
            if skf > 0:
                got.append((0, min(skf, row['nframes'])))
        got = norm(got)
        row['fx_holes'] = got
        extra, missing = compare_ranges(row['truth_holes'], got)
        row['extra'] = extra
        row['missing'] = missing
        extra_slots += sum(b - a for (a, b) in extra)
        missing_slots += sum(b - a for (a, b) in missing)
        row['e3'] = 'ok' if not (extra or missing) else 'extra %s missing %s' % (fmt(extra), fmt(missing))
        if row['e2'] != 'ok' or row['e3'] != 'ok':
            nbad += 1

    # E4：读取窗口对文件的覆盖。定位读按字节数读**一段连续**区域，被 filler 隔开的本
    # subint 数据落在窗口之外时既不会报错、也不会出现在任何 .sp 里。窗口之间若有数据
    # 帧从未被任何 subint 读到，那就是净损失——与 E3 的"标记口径"是两回事。
    wins = []
    for row in rows:
        i0 = row['readoff'] // framebytes
        if 0 <= i0 < total and row['readoff'] % framebytes == 0:
            wins.append((i0, min(i0 + row['nframes'], total)))
    merged = norm(wins)
    uncovered = []
    pos = 0
    for (a, b) in merged:
        if a > pos:
            uncovered.append((pos, a))
        pos = max(pos, b)
    if pos < total:
        uncovered.append((pos, total))
    # 只算 batch 窗口内的损失：文件常比 batch 长（t25362 的 BA 文件 12 s、batch 11.264 s），
    # 尾部那截数据本就不该被读，报出来会淹掉真正的损失。
    flo = [row['f_lo'] for row in rows if 'f_lo' in row]
    lo_f = flo[0] if flo else None
    hi_f = flo[-1] + rows[-1]['nframes'] if flo else None

    lost = 0
    lost_spans = []
    outside = 0
    for (a, b) in uncovered:
        n_in = n_out = 0
        for seg in truth['segments']:
            if seg['kind'] != 'data':
                continue
            c0 = max(a, seg['start'])
            c1 = min(b, seg['start'] + seg['count'])
            if c1 <= c0:
                continue
            f0 = seg['sec'] * fps + seg['frame'] + (c0 - seg['start'])
            if lo_f is not None:
                inside = max(0, min(f0 + (c1 - c0), hi_f) - max(f0, lo_f))
                n_in += inside
                n_out += (c1 - c0) - inside
            else:
                n_in += c1 - c0
        if n_in:
            lost += n_in
            lost_spans.append((a, n_in))
        outside += n_out

    # E1：窗口起点的帧号序列。相邻差应为 subint 跨度（subint_sec×fps 取整，mod fps）。
    e1 = []
    prev = None
    prevreadoff = None
    for row in rows:
        if 'f_lo' not in row:
            e1.append('-')
        elif span is None:
            e1.append('?')
        elif prev is None or (prevreadoff is not None and prevreadoff < framebytes):
            # 第一条，以及上一条的读取位置被钳在文件首帧的那一条：batch 起点之前的
            # delay 修正无处可读（locate 的跳块把它吸收成 lastcount），窗口起点因此
            # 与时间轴对不齐，这是语义而非缺陷。
            e1.append('anchor')
        else:
            delta = row['f_lo'] - prev
            e1.append('ok' if abs(delta - span) <= 1
                      else 'delta %d vs span %d (off by %d)' % (delta, span, delta - span))
        if 'f_lo' in row:
            prev = row['f_lo']
            prevreadoff = row['readoff']

    if not args.quiet:
        print('log         : %s' % args.log)
        print('vdif        : %s (%s)' % (args.vdif, file_truth.summary_line(truth)))
        print('  %-5s %-11s %-12s %-6s %-6s %-8s %-18s %-18s %s'
              % ('sub', 'readoff', 'f_lo', 'framens', 'first', 'E1', 'truth holes', 'fxcorr holes', 'verdict'))
        for row, e in zip(rows, e1):
            ok = row['e2'] == 'ok' and row.get('e3', 'ok') == 'ok'
            if args.only_bad and ok:
                continue
            print('  %-5d %-11d %-12s %-6d %-6d %-8s %-18s %-18s %s'
                  % (row['n'], row['readoff'], row.get('f_lo', '-'), row['framens'], row['firstfno'],
                     e, fmt(row.get('truth_holes', [])), fmt(row.get('fx_holes', [])),
                     'ok' if ok else '<<< %s | %s' % (row['e2'], row.get('e3', ''))))
        if span:
            # READPOS 的编号不等于 subint 索引（无效 subint 不打印），所以不假定两者对齐。
            # 绝对锚也不假定：VDIF 的秒字段与 batch.json 的 MJD 之间隔着记录器的秒零点
            # （t25362 差 180 天，当日秒一致），强行相减只会给出误导的常数。这里报的是
            # 窗口起点在**当日**的位置，供人工对照。
            flo = [row['f_lo'] for row in rows if 'f_lo' in row]
            print('E1 day      : span %d, window starts at day-second %d → %d (%d subints)'
                  % (span, (flo[0] // fps) % 86400, (flo[-1] // fps) % 86400, len(flo)))

    print('summary     : %d subints, %d with a finding; E3 extra %d slots, missing %d slots'
          % (len(rows), nbad, extra_slots, missing_slots))
    print('E4 coverage : %d data frames inside the batch never fell inside any read window%s'
          % (lost, '' if not lost else ' -> ' + ' '.join('[%d,+%d)' % (a, n) for (a, n) in lost_spans[:8])))
    if outside:
        print('              (%d further frames outside the batch window, expected for a file longer than the batch)'
              % outside)

    if args.jsonpath:
        with open(args.jsonpath, 'w') as f:
            json.dump({'fps': fps, 'framebytes': framebytes, 'rows': rows,
                       'extra_slots': extra_slots, 'missing_slots': missing_slots}, f, indent=1)

    return 1 if nbad else 0


def fmt(ranges):
    return ' '.join('[%d,%d)' % (a, b) for (a, b) in ranges) if ranges else '-'


if __name__ == '__main__':
    sys.exit(main())
