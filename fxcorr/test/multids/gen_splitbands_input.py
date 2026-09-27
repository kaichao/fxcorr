#!/usr/bin/env python3
"""gen_splitbands_input.py —— 把"每站一个 datastream 含多个 band"拆成"每个 band 一个 datastream"

用法：gen_splitbands_input.py <src.input> <dst.input>

动机：分片的单位是 **ds 组**（一组 ds 覆盖同一频段组），而 `test2b` 这类配置是
"每站 1 个 ds 含 2 个 band"——它只有一组，验证不了跨分片的时间归并。拆成"每站
每 band 一个 ds"后，baseline 只连同一频段的两个站，于是自然形成**按频段分组的
多个 ds 组**（test2b：2 组，每组 2 个 ds）。

展开规则：

  - **DATASTREAM**：每站的块按 `REC FREQ INDEX k:` 拆开，每块只留一个 band 组、
    并把组内各字段的 band 序号重编为 0（`NUM RECORDED FREQS: 1`、
    `REC BAND 0 INDEX: 0`）；
  - **ds 编号**：原 ds `a` 的第 `j` 个 band → 新 ds `a*N+j`（N = 每站的 band 数，
    各站相同）；
  - **BASELINE**：原条目的第 j 个 freq slot 单独成一条，A/B 换成对应的新 ds；
  - **DATA TABLE**：每个新 ds 一个文件（名字加 `_ds<N>`）。

FREQ 表与 POL 表不动。
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_multids_input import clean, get_field, kv, parse_sections, set_field  # noqa: E402

BAND_KEYS = ('REC FREQ INDEX', 'CLK OFFSET', 'FREQ OFFSET', 'NUM REC POLS',
             'REC BAND')


def split_datastream_block(block):
    """一站块 → 每 band 一个块。返回 [块, ...]，块 = [行, ...]

    块的结构是：ds 级头部（TELESCOPE INDEX ... NUM RECORDED FREQS）→ 每个 band
    一组（REC FREQ INDEX k 开头）→ NUM ZOOM FREQS 收尾
    """
    head = []
    for line in block:
        if line.startswith('REC FREQ INDEX'):
            break
        head.append(line)
    rest = block[len(head):]

    bands, cur = [], None
    for line in rest:
        if line.startswith('REC FREQ INDEX'):
            cur = [line]
            bands.append(cur)
        elif line.startswith('NUM ZOOM FREQS'):
            cur = None		# 收尾行，每份拷贝自己补
        else:
            # REC BAND k POL / REC BAND k INDEX 集中在块尾，不与各自的
            # REC FREQ INDEX 相邻——按行内的 k 归位，不能按位置归
            m = re.match(r'^REC BAND (\d+)', line)
            if m:
                bands[int(m.group(1))].append(line)
            elif cur is not None:
                cur.append(line)

    zoom = [l for l in rest if l.startswith('NUM ZOOM FREQS')] or [kv('NUM ZOOM FREQS:', 0)]
    header = set_field(head, 'NUM RECORDED FREQS:', 1)
    # DATA FRAME SIZE 是整帧的字节数（含该 ds 的全部 band）。**VDIF 头是每帧
    # 一份（32 字节），只均分 payload**：test2b 的 16032 = 32 + 16000 拆成
    # 32 + 8000 = 8032。若连头一起均分（8016 = 32 + 7984，7984 = 16x499）帧
    # 样本数会带上质因子 499，与公共信号块大小（2 的幂 x 5 的幂）永不整除，
    # 生成时报 "block is not a whole number of frames"。
    framesize = int(get_field(head, 'DATA FRAME SIZE:'))
    header = set_field(header, 'DATA FRAME SIZE:',
                       32 + (framesize - 32) // len(bands))

    out = []
    for band in bands:
        copy = list(header)
        for line in band:
            # 组内的 band 序号重编为 0：REC FREQ INDEX k / CLK OFFSET k (us) /
            # FREQ OFFSET k (Hz) / NUM REC POLS k / REC BAND k POL / REC BAND k INDEX
            line = re.sub(r'^((?:%s) )\d+' % '|'.join(BAND_KEYS), r'\g<1>0', line)
            if line.startswith('REC BAND 0 INDEX:'):
                line = kv('REC BAND 0 INDEX:', 0)
            copy.append(line)
        copy += zoom
        out.append(copy)
    return out


def split_datastreams(lines):
    head, blocks = [], []
    for line in lines:
        if line.startswith('TELESCOPE INDEX:'):
            blocks.append([line])
        elif blocks:
            blocks[-1].append(line)
        else:
            head.append(line)
    out = []
    stations = []			# 每站的 band 数（用于 ds 编号映射）
    for block in blocks:
        block = clean(block)
        perband = int(get_field(block, 'NUM RECORDED FREQS:'))
        stations.append(perband)
        for copy in split_datastream_block(block):
            out += copy
    nds = sum(stations)
    head = set_field(head, 'DATASTREAM ENTRIES:', nds)
    return head + out, stations


def split_baselines(lines, stations):
    head, blocks = [], []
    for line in lines:
        if line.startswith('D/STREAM A INDEX'):
            blocks.append([line])
        elif blocks:
            blocks[-1].append(line)
        else:
            head.append(line)

    # 原 ds a 的第 j 个 band 对应新 ds a*N + j（N 与站无关，各站相同）
    ndsper = stations[0] if stations else 1
    out = []
    newidx = 0
    for block in blocks:
        block = clean(block)
        a = int(block[0].rsplit(':', 1)[1])
        b = int(block[1].rsplit(':', 1)[1])
        # 按 TARGET FREQ 分 slot，每个 slot 4 行
        slots, cur = [], None
        for line in block[2:]:
            if line.startswith('TARGET FREQ'):
                cur = [line]
                slots.append(cur)
            elif cur is not None:
                cur.append(line)
        for j, slot in enumerate(slots):
            out.append(kv('D/STREAM A INDEX %d:' % newidx, a * ndsper + j))
            out.append(kv('D/STREAM B INDEX %d:' % newidx, b * ndsper + j))
            out.append(kv('NUM FREQS %d:' % newidx, 1))
            for line in slot:
                # slot 号与 baseline 号都归零：拆完每条 baseline 只有一个 slot
                m = re.match(r'^(TARGET FREQ |POL PRODUCTS )(\d+)(/\d+)', line)
                if m:
                    out.append('%s%d/0%s' % (m.group(1), newidx, line[m.end():]))
                    continue
                # 'D/STREAM [AB] BAND <k>:' 的编号是 polproduct 序号（不是
                # baseline 号，见 gen_multids_input.py 的说明），这里恒为 0；
                # 值（band 索引）也归零：新 ds 只含一个 band，保留原值会指向
                # 不存在的 band
                m = re.match(r'^(D/STREAM [AB] BAND )\d+(:)', line)
                if m:
                    out.append(kv('%s0%s' % (m.group(1), m.group(2)), 0))
                    continue
                out.append(line)
            newidx += 1
    return set_field(head, 'BASELINE ENTRIES:', newidx) + out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    sections = parse_sections(open(src).read())

    idds = next(i for i, (t, _) in enumerate(sections) if 'DATASTREAM TABLE' in t)
    idbl = next(i for i, (t, _) in enumerate(sections) if 'BASELINE TABLE' in t)
    iddt = next(i for i, (t, _) in enumerate(sections) if 'DATA TABLE' in t)
    idc = next(i for i, (t, _) in enumerate(sections) if 'COMMON SETTINGS' in t)
    idcfg = next(i for i, (t, _) in enumerate(sections) if 'CONFIGURATIONS' in t)

    dsbody, stations = split_datastreams(sections[idds][1])
    nds = sum(stations)
    sections[idds] = (sections[idds][0], dsbody)
    baselines_body = split_baselines(sections[idbl][1], stations)
    sections[idbl] = (sections[idbl][0], baselines_body)
    nbl = int(get_field(baselines_body, 'BASELINE ENTRIES:'))

    # DATA TABLE：每个新 ds 一个文件，名字加 _ds<N>（站内序号）
    olddt = [l for l in sections[iddt][1] if l.startswith('FILE ')]
    names = [l.split(':', 1)[1].strip() for l in olddt]
    out = []
    for i, name in enumerate(names):
        base = name[:-len('.vdif')] if name.endswith('.vdif') else name
        for j in range(stations[i]):
            out.append(kv('D/STREAM %d FILES:' % (i * stations[0] + j), 1))
            out.append(kv('FILE %d/0:' % (i * stations[0] + j), '%s_ds%d.vdif' % (base, j)))
    sections[iddt] = (sections[iddt][0], out)

    sections[idc] = (sections[idc][0],
                     set_field(set_field(sections[idc][1], 'ACTIVE DATASTREAMS:', nds),
                               'ACTIVE BASELINES:', nbl))

    kept = [l for l in sections[idcfg][1]
            if not re.match(r'^(DATASTREAM|BASELINE) \d+ INDEX:', l)]
    trail = []
    while kept and kept[-1].strip() == '':
        trail.insert(0, kept.pop())
    idx = [kv('DATASTREAM %d INDEX:' % i, i) for i in range(nds)]
    idx += [kv('BASELINE %d INDEX:' % i, i) for i in range(nbl)]
    sections[idcfg] = (sections[idcfg][0], kept + idx + trail)

    with open(dst, 'w') as f:
        for title, body in sections:
            if title:
                f.write(title + '\n')
            for line in body:
                f.write(line + '\n')
    print('gen_splitbands_input.py: %s -> %s (%d station-band(s) per station, '
          '%d datastreams, %d baselines)' % (src, dst, stations[0] if stations else 0,
                                             nds, nbl))


if __name__ == '__main__':
    main()
