#!/usr/bin/env python3
"""VDIF 真值台账：文件自己的"哪些时间槽有数据"，与任何读取实现无关.

用法:
  file_truth.py <file.vdif> [--fps N] [--json out.json] [--quiet]

为什么要有它（背景见 fxcorr/test/reader/README.md）：VDIF 的帧号就是时间槽，所以

  * 文件里没有某个帧号  → 那个槽没有数据（缺口）；
  * 全零头 / invalid 帧 → 占文件字节、不占槽（filler），那个槽同样没有数据。

两类在台账里都只是"槽未被占用"，与 datareader.cpp 的 gapinvalid 想表达的是同一件事；
区别在于台账从**文件本身**算出它，不经过任何读取过程，因此可以用来对账 fxcorr 的
READPOS（定位）与 GAPCHECK holes（落点），而不是让 reader 自证。

输出（--json）分两半：文件属性（framebytes / nframes / fps / 首帧）与段表。段表里
data 段内部帧号连续（帧号 = sec*fps + frame，段首记 sec/frame），段与段之间的帧号差
就是缺口；filler / other（帧长异常）段只占文件字节。
"""

import argparse
import json
import os
import struct
import sys

HDRBYTES = 32
CHUNK_FRAMES = 4096		# 顺序读的批大小，避免逐帧 seek（1.5 GB 的文件也要扫得动）


def parse_header(b, off=0):
    """按 vdifio 的字布局解析 32 字节头（同 datareader.cpp 的 vdifFrameNumber/vdifIsFiller）."""
    w = struct.unpack_from('<8I', b, off)
    return {
        'seconds': w[0] & 0x3FFFFFFF,
        'invalid': (w[0] >> 31) & 1,
        'frame': w[1] & 0xFFFFFF,
        'framelength8': w[2] & 0xFFFFFF,
    }


def is_filler(d):
    """与 datareader.cpp 的 vdifIsFiller 同一判据：invalid 位置起，或全零头."""
    return d['invalid'] == 1 or (d['seconds'] == 0 and d['frame'] == 0)


def fno(seg, fps):
    """data 段首帧的绝对帧号."""
    return seg['sec'] * fps + seg['frame']


def scan(path, fps=None):
    """扫一遍文件，返回台账 dict（不含路径相关的运行时字段）."""
    filesize = os.path.getsize(path)
    with open(path, 'rb') as f:
        first = parse_header(f.read(HDRBYTES))
    framebytes = first['framelength8'] * 8
    if framebytes <= 0:
        raise SystemExit('file_truth: %s: first frame has framelength 0' % path)
    nframes = filesize // framebytes
    remainder = filesize - nframes * framebytes

    segments = []
    rollovers = []		# 跨秒的相邻 data 帧对 (prev_frame, cur_frame)
    cur = None
    prev = None			# (kind, sec, frame)

    with open(path, 'rb') as f:
        for base in range(0, nframes, CHUNK_FRAMES):
            f.seek(base * framebytes)
            blob = f.read(min(CHUNK_FRAMES, nframes - base) * framebytes)
            for j in range(len(blob) // framebytes):
                d = parse_header(blob, j * framebytes)
                i = base + j
                # filler 先判：全零头同时也没有帧长，按帧长判会把它归到 other
                if is_filler(d):
                    kind = 'filler'
                elif d['framelength8'] * 8 != framebytes:
                    kind = 'other'
                else:
                    kind = 'data'

                cont = False
                if cur is not None and cur['kind'] == kind:
                    if kind != 'data':
                        cont = True
                    else:
                        # 段内帧号连续：同一秒内 +1，或跨秒回绕到 0
                        cont = ((d['seconds'] == cur['lastsec'] and d['frame'] == cur['lastframe'] + 1) or
                                (d['seconds'] == cur['lastsec'] + 1 and d['frame'] == 0))
                if cont:
                    cur['count'] += 1
                    cur['lastsec'] = d['seconds']
                    cur['lastframe'] = d['frame']
                else:
                    if cur is not None:
                        segments.append(cur)
                    cur = {'kind': kind, 'start': i, 'count': 1}
                    if kind == 'data':
                        cur['sec'] = d['seconds']
                        cur['frame'] = d['frame']
                        cur['lastsec'] = d['seconds']
                        cur['lastframe'] = d['frame']

                if (prev is not None and prev[0] == 'data' and kind == 'data'
                        and d['seconds'] == prev[1] + 1):
                    rollovers.append((prev[2], d['frame']))
                prev = (kind, d['seconds'], d['frame'])

    if cur is not None:
        segments.append(cur)

    # fps: 秒边界上帧号回绕到 0 的那一帧，其前一帧的帧号就是 fps-1。取最大值以躲开
    # 恰好落在秒边界上的缺口（缺口只会让这个候选偏小）。
    inferred = False
    if fps is None:
        cand = [pf + 1 for (pf, cf) in rollovers if cf == 0]
        if cand:
            fps = max(cand)
            inferred = True

    truth = {
        'file': os.path.abspath(path),
        'filesize': filesize,
        'remainder': remainder,
        'framebytes': framebytes,
        'nframes': nframes,
        'fps': fps,
        'fpsinferred': inferred,
        'first': {'sec': first['seconds'], 'frame': first['frame']},
        'segments': segments,
    }

    if fps:
        data = [s for s in segments if s['kind'] == 'data']
        gaps = []
        for a, b in zip(data, data[1:]):
            lost = fno(b, fps) - (fno(a, fps) + a['count'])
            gaps.append({'after': a['start'] + a['count'], 'frames': lost})
        truth['gaps'] = gaps
        truth['dataframes'] = sum(s['count'] for s in data)
        truth['fillerframes'] = sum(s['count'] for s in segments if s['kind'] == 'filler')
        truth['otherframes'] = sum(s['count'] for s in segments if s['kind'] == 'other')

    return truth


def load(path):
    with open(path) as f:
        return json.load(f)


def present_ranges(truth, fps=None):
    """文件里**存在**的帧号区间 [(f0, f1), ...]，升序、互不相交."""
    fps = fps or truth['fps']
    out = []
    for s in truth['segments']:
        if s['kind'] == 'data':
            f0 = fno(s, fps)
            out.append((f0, f0 + s['count']))
    return out


def holes_in_window(truth, f_lo, f_hi, fps=None):
    """[f_lo, f_hi) 里文件没有数据的帧号区间，作为相对 f_lo 的槽区间返回."""
    out = []
    pos = f_lo
    for (a, b) in present_ranges(truth, fps):
        if b <= pos:
            continue
        if a > pos:
            out.append((pos - f_lo, min(a, f_hi) - f_lo))
        pos = max(pos, b)
        if pos >= f_hi:
            break
    if pos < f_hi:
        out.append((pos - f_lo, f_hi - f_lo))
    return out


def summary_line(truth):
    fps = truth['fps']
    nseg = len(truth['segments'])
    return ('framebytes %d nframes %d%s fps %s data %s filler %s gaps %s'
            % (truth['framebytes'], truth['nframes'],
               (' (%d bytes left over)' % truth['remainder']) if truth['remainder'] else '',
               ('%d%s' % (fps, '(inferred)' if truth['fpsinferred'] else '')) if fps else '?',
               truth.get('dataframes', '-'), truth.get('fillerframes', '-'),
               len(truth.get('gaps', [])) if nseg else '-'))


def main():
    ap = argparse.ArgumentParser(description='VDIF truth ledger for reader checks')
    ap.add_argument('vdif')
    ap.add_argument('--fps', type=int, help='frames per second (default: infer from a second rollover)')
    ap.add_argument('--json', dest='jsonpath', help='write the ledger here')
    ap.add_argument('--quiet', action='store_true', help='no table, only the one-line summary')
    args = ap.parse_args()

    truth = scan(args.vdif, args.fps)
    if truth['fps'] is None:
        raise SystemExit('file_truth: cannot infer fps from %s (no second rollover); pass --fps'
                         % args.vdif)

    if args.jsonpath:
        with open(args.jsonpath, 'w') as f:
            json.dump(truth, f, indent=1)

    print('file        : %s (%d bytes)' % (truth['file'], truth['filesize']))
    if not args.quiet:
        print('first frame : sec %d frame %d' % (truth['first']['sec'], truth['first']['frame']))
        print('segments    : %d' % len(truth['segments']))
        print('  %-7s %-11s %8s  %-12s %s' % ('kind', 'fileidx', 'frames', 'sec', 'frame'))
        for s in truth['segments']:
            print('  %-7s %5d..%-5d %8d  %-12s %s'
                  % (s['kind'], s['start'], s['start'] + s['count'] - 1, s['count'],
                     s.get('sec', '-'), s.get('frame', '-')))
        for g in truth.get('gaps', []):
            print('  gap after fileidx %d: %d frames missing' % (g['after'], g['frames']))
    print(summary_line(truth))


if __name__ == '__main__':
    main()
