#!/usr/bin/env python3
"""gen_multids_input.py —— 把单 datastream 的 .input 展开成"每天线多 datastream"

用法：gen_multids_input.py <src.input> <dst.input> [ds_per_station]

动机（v5-plan.md P6「多 datastream 生成」）：真实观测里每天线有多个 datastream
（t25362 每站 8 个 = 4 频段组 x 2 极化，见 data-volume.md §7.3），而 .input 由
vex2difx 从单 ds 的 .v2d 生成时只有一个。vex2v2d 的 -2/-4/-8 走的是 thread 拆分
（另一套语义，且需要 freqId 配合），本脚本直接按 .input 的表格结构展开，得到的是
"同一个频段、不同极化"的多个 ds——正是 fxcorr-sim 多 ds 生成与 fxcorr-f/x 多 ds
任务所需要的最小配置。

展开规则（对每个原有的 datastream 复制 ds_per_station 份）：
  - TELESCOPE INDEX 不变（同一个站的多个 ds）；
  - REC BAND k POL 交替 R/L（成对极化，与 t25362 的 ds0=ds1 同频段不同极化同构）；
  - 文件名的 DATA TABLE 行加 _ds<N> 后缀（make_testdata.sh 会把它软链到
    raw/<station>/<station>_<batch_id>_ds<N>.vdif）；
  - BASELINE TABLE 取原条目的 ds 展开集的笛卡尔积（每条 baseline 的其余字段照抄）。

不改 FREQ 表——同一频段的两个极化共用一个 freq 条目是合法的（极化记在
DATASTREAM 的 REC BAND POL 里，不在 FREQ 表里）。
"""

import re
import sys


def parse_sections(text):
    """按 '# XXX ###!' 段标题切分 → [(标题行, [内容行])]"""
    sections = []
    for line in text.split('\n'):
        if line.startswith('#') and line.rstrip().endswith('!'):
            sections.append((line, []))
        else:
            if not sections:
                sections.append(('', []))
            sections[-1][1].append(line)
    return sections


def find_section(sections, keyword):
    for i, (title, _) in enumerate(sections):
        if keyword in title:
            return i
    return -1


# .input 的列约定（configuration.cpp:3585 的 getinputkeyval）：值一律从第
# DEFAULT_KEY_LENGTH = 20 列开始，它按 substr(20) 取值——行短于 20 字符会抛
# std::out_of_range（不是解析错误，是进程崩掉），所以每个 kv 行都必须补足列宽
KEY_LEN = 20


def clean(block):
    """去掉块首尾的空行 —— 空行是段与段之间的排版，把它复制进块内会让 .input
    的解析器读到空行（getinputkeyval 不跳空行，substr(20) 直接越界）"""
    i, j = 0, len(block)
    while i < j and block[i].strip() == '':
        i += 1
    while j > i and block[j - 1].strip() == '':
        j -= 1
    return block[i:j]


def kv(key, value):
    """'KEY:  value' —— 值对齐到第 20 列（key 要带末尾的冒号）"""
    return '%-*s%s' % (KEY_LEN, key, value)


def set_field(lines, key, value):
    """把 'KEY:  old' 改成 'KEY:  value'（保持值的起始列不变）"""
    for i, line in enumerate(lines):
        if line.startswith(key):
            colon = line.index(':')
            vpos = colon + 1
            while vpos < len(line) and line[vpos] == ' ':
                vpos += 1
            if vpos < KEY_LEN:
                vpos = KEY_LEN
            return lines[:i] + [line[:vpos] + str(value)] + lines[i + 1:]
    raise SystemExit('gen_multids_input.py: field %r not found' % key)


def get_field(lines, key):
    for line in lines:
        if line.startswith(key):
            return line[len(key):].strip()
    raise SystemExit('gen_multids_input.py: field %r not found' % key)


def get_index_field(lines, key):
    """'D/STREAM A INDEX 0: 0' 这类带序号的行 —— 取行尾的值"""
    for line in lines:
        if line.startswith(key):
            return int(line.rsplit(':', 1)[1].strip())
    raise SystemExit('gen_multids_input.py: field %r not found' % key)


def expand_datastreams(lines, nds):
    """DATASTREAM TABLE 段：每个 ds 块复制 nds 份，极化交替"""
    head, blocks = [], []
    for line in lines:
        if line.startswith('TELESCOPE INDEX:'):
            blocks.append([line])
        elif blocks:
            blocks[-1].append(line)
        else:
            head.append(line)
    if not blocks:
        raise SystemExit('gen_multids_input.py: no DATASTREAM blocks found')

    out = []
    for block in blocks:
        block = clean(block)
        for copy in range(nds):
            for line in block:
                m = re.match(r'^(REC BAND \d+ POL:\s*)(\S+)\s*$', line)
                if m:
                    # 同一频段的两份记录用不同极化（同 t25362 的 ds 对）
                    line = '%s%s' % (m.group(1), 'R' if copy % 2 == 0 else 'L')
                out.append(line)
    return set_field(head, 'DATASTREAM ENTRIES:', len(blocks) * nds) + out


def expand_data_table(lines, nds):
    """DATA TABLE 段：每个 ds 的文件行复制 nds 份，名字加 _ds<N>"""
    head, blocks = [], []
    for line in lines:
        m = re.match(r'^D/STREAM (\d+) FILES:', line)
        if m:
            blocks.append((int(m.group(1)), [line]))
        elif blocks:
            blocks[-1][1].append(line)
        else:
            head.append(line)

    out = []
    for oldds, block in blocks:
        block = clean(block)
        for copy in range(nds):
            newds = oldds * nds + copy
            for line in block:
                m = re.match(r'^D/STREAM \d+ FILES:\s*(\d+)\s*$', line)
                if m:
                    out.append(kv('D/STREAM %d FILES:' % newds, m.group(1)))
                    continue
                m = re.match(r'^FILE \d+/(\d+):\s*(\S+)\s*$', line)
                if m:
                    name = m.group(2)
                    if name.endswith('.vdif'):
                        name = name[:-len('.vdif')]
                    out.append(kv('FILE %d/%s:' % (newds, m.group(1)),
                                  '%s_ds%d.vdif' % (name, copy)))
                    continue
                out.append(line)
    return head + out


def expand_baselines(lines, nds, ndatastreams):
    """BASELINE TABLE 段：原条目的 ds 展开集取笛卡尔积"""
    head, blocks = [], []
    for line in lines:
        if line.startswith('D/STREAM A INDEX'):
            blocks.append([line])
        elif blocks:
            blocks[-1].append(line)
        else:
            head.append(line)

    out = []
    newidx = 0
    for block in blocks:
        block = clean(block)
        a = get_index_field(block, 'D/STREAM A INDEX')
        b = get_index_field(block, 'D/STREAM B INDEX')
        # 其余字段照抄（NUM FREQS / TARGET FREQ / POL PRODUCTS / ... BAND）
        rest = [l for l in block if not l.startswith('D/STREAM A INDEX')
                and not l.startswith('D/STREAM B INDEX')]
        for da in range(a * nds, a * nds + nds):
            for db in range(b * nds, b * nds + nds):
                if da >= ndatastreams or db >= ndatastreams:
                    continue
                out.append(kv('D/STREAM A INDEX %d:' % newidx, da))
                out.append(kv('D/STREAM B INDEX %d:' % newidx, db))
                for line in rest:
                    # 'D/STREAM [AB] BAND <k>:' 的编号是 **polproduct 序号**（每条
                    # baseline 内从 0 开始），不是 baseline 号——mpifxcorr 与
                    # fxcorrcommon 都按 getinputline(..., "D/STREAM A BAND ", k)
                    # 定位（mpifxcorr/src/configuration.cpp:1089、
                    # libraries/fxcorrcommon/src/configuration.cpp:1016），编号
                    # 写错会让 mpifxcorr 直接报错退出。本生成器只造每条 baseline
                    # 一个 polproduct 的配置，所以编号恒为 0。
                    if re.match(r'^D/STREAM [AB] BAND \d+:', line):
                        out.append(re.sub(r'^(D/STREAM [AB] BAND )\d+(:)', r'\g<1>0\g<2>', line))
                        continue
                    # '<KEY> <baseline>/<slot>:' —— baseline 号是斜杠前那个
                    m = re.match(r'^([A-Z/ ]+ )(\d+)(/\d+:.*)$', line)
                    if m:
                        out.append('%s%d%s' % (m.group(1), newidx, m.group(3)))
                        continue
                    # '<KEY> <baseline>:'
                    m = re.match(r'^([A-Z/ ]+ )(\d+)(:.*)$', line)
                    if m:
                        out.append('%s%d%s' % (m.group(1), newidx, m.group(3)))
                        continue
                    out.append(line)
                newidx += 1
    return set_field(head, 'BASELINE ENTRIES:', newidx) + out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    nds = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    if nds < 1:
        sys.exit('gen_multids_input.py: ds_per_station must be >= 1')

    text = open(src).read()
    sections = parse_sections(text)

    idds = find_section(sections, 'DATASTREAM TABLE')
    idbl = find_section(sections, 'BASELINE TABLE')
    iddt = find_section(sections, 'DATA TABLE')
    if idds < 0 or idbl < 0 or iddt < 0:
        sys.exit('gen_multids_input.py: %s is missing one of the DATASTREAM / '
                 'BASELINE / DATA tables' % src)

    # 原 ds 数：从 DATASTREAM TABLE 的块数数出来
    oldds = sum(1 for l in sections[idds][1] if l.startswith('TELESCOPE INDEX:'))
    newds = oldds * nds

    sections[idds] = (sections[idds][0], expand_datastreams(sections[idds][1], nds))
    sections[idbl] = (sections[idbl][0],
                      expand_baselines(sections[idbl][1], nds, newds))
    sections[iddt] = (sections[iddt][0], expand_data_table(sections[iddt][1], nds))

    # COMMON SETTINGS：ACTIVE DATASTREAMS / ACTIVE BASELINES
    idc = find_section(sections, 'COMMON SETTINGS')
    baselines = int(get_field(sections[idbl][1], 'BASELINE ENTRIES:'))
    sections[idc] = (sections[idc][0],
                     set_field(set_field(sections[idc][1], 'ACTIVE DATASTREAMS:',
                                         newds),
                               'ACTIVE BASELINES:', baselines))

    # CONFIGURATIONS：DATASTREAM k INDEX / BASELINE k INDEX 列表（在段尾重建）
    idcfg = find_section(sections, 'CONFIGURATIONS')
    kept = [l for l in sections[idcfg][1]
            if not re.match(r'^(DATASTREAM|BASELINE) \d+ INDEX:', l)]
    trail = []
    while kept and kept[-1].strip() == '':
        trail.insert(0, kept.pop())
    idx = [kv('DATASTREAM %d INDEX:' % i, i) for i in range(newds)]
    idx += [kv('BASELINE %d INDEX:' % i, i) for i in range(baselines)]
    sections[idcfg] = (sections[idcfg][0], kept + idx + trail)

    with open(dst, 'w') as f:
        for title, body in sections:
            if title:
                f.write(title + '\n')
            for line in body:
                f.write(line + '\n')

    print('gen_multids_input.py: %s -> %s (%d datastream(s)/station, %d '
          'datastreams, %d baselines)' % (src, dst, nds, newds, baselines))


if __name__ == '__main__':
    main()
