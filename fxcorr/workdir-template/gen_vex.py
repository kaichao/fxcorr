#!/usr/bin/env python3
# gen_vex.py —— 从 t25362-base.{vex,v2d} 派生 4 站配置（完整跨度 / mini 跨度）
#
# 用法：./gen_vex.py [outdir]                       # 重新生成模板资产（默认 config/）
#       ./gen_vex.py --install <workdir> <variant>  # 装进一个 workdir、改名 test.*
#
# 产物（与 base 并列）：
#   t25362-4st.vex / .v2d       4 站、完整跨度（7040 MHz）—— S0 第②次跑、S3 基准
#   t25362-4st-mini.vex / .v2d  4 站、跨度压到连续（992 MHz）—— S0 第①次跑
#
# 两个变换：
#
# ① 4 站：复制 base 的 BA / S6 各一份，改名 BX / SX。**坐标照抄**（$SITE 的
#    site_position 逐字复制），但 $SITE/$ANTENNA 用新名字——同名 site 被两站
#    共用时，vex2difx 若按 site 名索引会把两站当成同一个站。其余段（$FREQ /
#    $BBC / $IF / $TRACKS / $EOP / $SOURCE）与站数无关，逐字不动；$MODE 与
#    $SCHED 里逐站列举的行补上新站。
#
# ② mini 跨度：只改 $FREQ 的 chan_def 频率——把 32 个唯一频率重排到
#    MINI_BASE_MHZ 起的连续网格（间隔 MINI_STEP_MHZ），极化对（Ch01/Ch09
#    同频）与 Ch/BBC 编号关系不变；$BBC/$IF 不动（if_freq 是接收链路中频，
#    与 chan_def 的射电频率之间没有 vex2difx/difxcalc 会校验的关系）。
#
#    这一条把公共信号的覆盖跨度从 7072 MHz 压到 1024 MHz（≈6.9×，即
#    data-volume.md §6 的杠杆 1），S0 第①次跑用它先暴露流程问题，不被体量
#    干扰；第②次跑换回完整跨度拿实测数字。
#
# base 与产物的对照（t25362 是 2 站、每站 8 datastream）：
#
#   变体              站数  唯一频率  跨度(MHz)  recorded band  前处理产物
#   t25362-base        2      32      7040          64         T25362_1.input
#   t25362-4st         4      32      7040         128         t25362-4st.input
#   t25362-4st-mini    4      32       992         128         t25362-4st-mini.input
#
#   recorded band = 唯一频率 × 2（极化），也就是 data-volume.md §1.3 的 nband。
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(HERE, 'config')
BASE = 't25362-base'

# 新增站：(新站名, 模板站名, [(模板里的词, 新词)])——第一个替换项由模板名自动加
NEW_STATIONS = [
	('BX', 'BA', [('BOSSCHA', 'BOSSCHA2')]),
	('SX', 'S6', [('SESHAN13', 'SESHAN2')]),
]

MINI_BASE_MHZ = 2936.40		# mini 变体的最低频点（= base 的最低频点）
MINI_STEP_MHZ = 32.0		# = 通道带宽，排满即无间隙

# filelist 的时间范围：t25362 真实观测的 scan（起点 61037.28484954、
# 时长 12 s = vex $SCHED 的 "0 sec : 12 sec"）。vex2difx 只看这里的时间，
# 不看文件是否真的存在——所以路径可以写**相对名**，由 make_testdata.sh 在
# raw 根下建软链（.input 的 DATA TABLE 直接抄 filelist 的路径，见其软链段）。
FILELIST_START_MJD = 61037.28484954
FILELIST_STOP_MJD = 61037.28498843		# = start + 12 s / 86400

# 除 $STATION/$MODE 外，还要逐站克隆的段：段名 -> [(模板 def, 新 def, 额外替换)]
# $SITE 的额外替换把 site_ID 一并改名（BA/BX），$DAS 与 $CLOCK 的 def 名就是站名
CLONE_DEFS = [
	('SITE',    [('BOSSCHA',   'BOSSCHA2',  [('BA', 'BX')]),
	             ('SESHAN13',  'SESHAN2',   [('S6', 'SX')])]),
	('ANTENNA', [('BOSSCHA',   'BOSSCHA2',  []),
	             ('SESHAN13',  'SESHAN2',   [])]),
	('DAS',     [('BA',        'BX',        []),
	             ('S6',        'SX',        [])]),
	('CLOCK',   [('BA',        'BX',        []),
	             ('S6',        'SX',        [])]),
]

VARIANTS = [
	('t25362-4st', False),
	('t25362-4st-mini', True),
]


# ---- 通用文本工具 ----

def split_sections(text):
	"""按行首的 '$NAME;' 切段：返回 [(段名或 None, 行列表)]，拼回去即还原原文。"""
	parts, name, buf = [], None, []
	for ln in text.split('\n'):
		m = re.match(r'^\$([A-Z_]+);\s*$', ln)
		if m:
			parts.append((name, buf))
			name, buf = m.group(1), [ln]
		else:
			buf.append(ln)
	parts.append((name, buf))
	return parts


def join_sections(parts):
	return '\n'.join('\n'.join(buf) for _, buf in parts)


def def_block(lines, name):
	"""返回 [start, end) —— 'def <name>;' 到配对的 'enddef;'。"""
	start = None
	for i, ln in enumerate(lines):
		if start is None:
			if re.match(r'^\s*def\s+%s\s*;\s*$' % re.escape(name), ln):
				start = i
		elif re.match(r'^\s*enddef;\s*$', ln):
			return start, i + 1
	raise SystemExit('gen_vex.py: def %s not found' % name)


def rename(line, subs):
	"""整词替换（前后不是字母/数字/下划线），subs 是 [(旧, 新)]。"""
	for old, new in subs:
		line = re.sub(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % re.escape(old), new, line)
	return line


def clone_def(lines, tmpl, new, extra=()):
	"""复制 tmpl 的 def 块、改名 new，插回原块之后；返回新行列表。"""
	subs = [(tmpl, new)] + list(extra)
	s, e = def_block(lines, tmpl)
	block = [rename(ln, subs) for ln in lines[s:e]]
	lines[e:e] = block
	return block


# ---- vex 变换 ----

def expand_station(body):
	out = list(body)
	for new, tmpl, extra in NEW_STATIONS:
		clone_def(out, tmpl, new, extra)
	return out


def expand_mode(body):
	out = []
	for ln in body:
		# 三条"一次列举全部站"的行：在收尾分号前补上新站
		if re.match(r'^\s*ref \$(PASS_ORDER|ROLL|PHASE_CAL_DETECT)\s*=', ln):
			names = ' : '.join(n[0] for n in NEW_STATIONS)
			out.append(ln.replace(';', ' : ' + names + ' ;', 1))
			continue
		out.append(ln)
		# 逐站的行：模板站后面补上对应的新站行
		m = re.match(r'^(\s*ref \$\w+\s*=[^:]*:\s*)([A-Za-z][A-Za-z0-9]*)(\s*;)\s*$', ln)
		if m:
			for new, tmpl, _ in NEW_STATIONS:
				if tmpl == m.group(2):
					out.append(m.group(1) + new + m.group(3))
	return out


def expand_sched(body):
	out = []
	for ln in body:
		out.append(ln)
		m = re.match(r'^(\s*station\s*=\s*)([A-Za-z][A-Za-z0-9]*)(\s*:.*)$', ln)
		if m:
			for new, tmpl, _ in NEW_STATIONS:
				if tmpl == m.group(2):
					out.append(m.group(1) + new + m.group(3))
	return out


def chan_freqs(body):
	"""$FREQ 段里出现的全部 chan_def 频率（升序去重）。"""
	return sorted(set(float(m) for m in re.findall(
		r'chan_def\s*=\s*&\w+\s*:\s*([0-9.]+)\s*MHz', '\n'.join(body))))


def compress_span(body, uniq):
	"""把 chan_def 频率重排到连续网格；极化对（同频的两个 chan_def）保持同频。"""
	mapping = dict((f, MINI_BASE_MHZ + i * MINI_STEP_MHZ) for i, f in enumerate(uniq))
	out = []
	for ln in body:
		m = re.search(r'(chan_def\s*=\s*&\w+\s*:\s*)([0-9.]+)(\s*MHz)', ln)
		if m:
			ln = (ln[:m.start(2)] + '%.2f' % mapping[float(m.group(2))]
			      + ln[m.end(2):])
		out.append(ln)
	return out


def make_vex(mini):
	text = open(os.path.join(CFG, BASE + '.vex')).read()
	uniq = []
	parts = []
	for name, body in split_sections(text):
		if name == 'STATION':
			body = expand_station(body)
		elif name == 'MODE':
			body = expand_mode(body)
		elif name == 'SCHED':
			body = expand_sched(body)
		elif name == 'FREQ':
			uniq = chan_freqs(body)
			if mini:
				body = compress_span(body, uniq)
				uniq = chan_freqs(body)
		else:
			for secname, clones in CLONE_DEFS:
				if name == secname:
					for tmpl, new, extra in clones:
						clone_def(body, tmpl, new, extra)
		parts.append((name, body))
	return join_sections(parts), uniq


# ---- v2d 变换 ----

def station_v2d_lines(lines, tmpl):
	"""抽出模板站在 v2d 里的全部行：ANTENNA 块 + 空行 + 它的 DATASTREAM 行。"""
	blk, inblk = [], False
	for ln in lines:
		if re.match(r'^ANTENNA\s+%s\s*$' % re.escape(tmpl), ln):
			inblk = True
		if inblk:
			blk.append(ln)
			if ln.strip() == '}':
				inblk = False
	dss = [ln for ln in lines if re.match(r'^DATASTREAM\s+%s' % re.escape(tmpl), ln)]
	return blk + [''] + dss


def rename_v2d(line, tmpl, new):
	"""v2d 的改名：站名/datastream 前缀（BA、BA1..BA8）+ filelist 里的小写站名。

	不能用整词替换——`BA1` 的 BA 后面跟着数字，词边界会挡住它。前导的
	`(?<![A-Za-z0-9_])` 足以排除 `nBand` 里的 `Band`（B 前是字母 n）。
	"""
	line = re.sub(r'(?<![A-Za-z0-9_])%s' % re.escape(tmpl), new, line)
	return re.sub(r'(?<=_)%s(?=_)' % re.escape(tmpl.lower()), new.lower(), line)


def expand_v2d(text, vexname, nds):
	lines = text.split('\n')
	out = []
	for ln in lines:
		m = re.match(r'^antennas\s*=\s*(.*?)\s*$', ln)
		if m:
			ln = 'antennas = %s, %s' % (m.group(1), ', '.join(n[0] for n in NEW_STATIONS))
		m = re.match(r'^vex\s*=\s*.*$', ln)
		if m:
			ln = 'vex = %s' % vexname
		m = re.match(r'^machines\s*=\s*.*$', ln)
		if m:
			ln = 'machines = ' + ','.join(['difx'] * nds)
		out.append(ln)

	while out and out[-1].strip() == '':
		out.pop()		# 末尾空行稍后由块自身补，避免堆积
	for new, tmpl, extra in NEW_STATIONS:
		block = [rename_v2d(ln, tmpl, new) for ln in station_v2d_lines(out, tmpl)]
		out.append('')
		out.extend(block)
	return '\n'.join(out) + '\n'


# ---- filelist 与主流程 ----

def write_filelists(outdir, stations, nds_per_station):
	"""每站每 datastream 一个 filelist：'<ST>_ds<N>.vdif <start> <stop>'。

	v2d 里的名字是 `data/filelist_<小写站名>_t<N+1>`（base 的既有命名），文件
	里的路径是 .input DATA TABLE 会照抄的相对名——见模块头的说明。
	"""
	datadir = os.path.join(outdir, 'data')
	if not os.path.isdir(datadir):
		os.mkdir(datadir)
	for st in stations:
		for i in range(nds_per_station):
			path = os.path.join(datadir, 'filelist_%s_t%d' % (st.lower(), i + 1))
			open(path, 'w').write('%s_ds%d.vdif %.8f %.8f\n'
			                      % (st, i, FILELIST_START_MJD, FILELIST_STOP_MJD))


def install(variant, workdir):
	"""把变体装进 <workdir>/config/，改名成 test.vex / test.v2d。

	make_testdata.sh 与 wrap_*.sh 认死 `test.` 这个前缀（四处），所以变体名
	只活在模板里；v2d 里的 `vex =` 行要跟着改，否则 vex2difx 找不到 vex
	（它按 cwd 解析这个路径，见 wrap_vex2difx.sh 的说明）。
	"""
	cfg = os.path.join(workdir, 'config')
	datadir = os.path.join(cfg, 'data')
	for d in (cfg, datadir):
		if not os.path.isdir(d):
			os.makedirs(d)
	v2d = open(os.path.join(CFG, variant + '.v2d')).read()
	v2d = re.sub(r'^vex\s*=.*$', 'vex = test.vex', v2d, flags=re.M)
	open(os.path.join(cfg, 'test.vex'), 'w').write(
		open(os.path.join(CFG, variant + '.vex')).read())
	open(os.path.join(cfg, 'test.v2d'), 'w').write(v2d)
	for name in os.listdir(os.path.join(CFG, 'data')):
		open(os.path.join(datadir, name), 'w').write(
			open(os.path.join(CFG, 'data', name)).read())
	print('gen_vex.py: installed %s as %s/config/test.{vex,v2d} (+ %d filelists)'
	      % (variant, workdir, len(os.listdir(datadir))))


def main():
	args = sys.argv[1:]
	if args and args[0] == '--install':
		if len(args) != 3:
			sys.exit('usage: gen_vex.py --install <workdir> <variant>')
		install(args[2], args[1])
		return
	outdir = args[0] if args else CFG
	if not os.path.isfile(os.path.join(CFG, BASE + '.vex')):
		sys.exit('gen_vex.py: %s/%s.vex not found' % (CFG, BASE))

	# v2d 的 machines 行长度 = datastream 总数 = 站数 × 每站 ds 数
	base_v2d = open(os.path.join(CFG, BASE + '.v2d')).read()
	nds = len(re.findall(r'^DATASTREAM\s', base_v2d, re.M))
	nstation = 2 + len(NEW_STATIONS)
	nds4 = nds // 2 * nstation
	stations = ['BA', 'S6'] + [n[0] for n in NEW_STATIONS]

	for stem, mini in VARIANTS:
		vex, uniq = make_vex(mini)
		v2d = expand_v2d(base_v2d, stem + '.vex', nds4)
		open(os.path.join(outdir, stem + '.vex'), 'w').write(vex)
		open(os.path.join(outdir, stem + '.v2d'), 'w').write(v2d)
		lo, hi = uniq[0], uniq[-1]
		print('gen_vex.py: %s  %d stations, %d datastreams, %d unique freqs, '
		      'span %.1f-%.1f MHz (%.0f MHz)'
		      % (stem, nstation, nds4, len(uniq), lo, hi, hi - lo))
	write_filelists(outdir, stations, nds // 2)


if __name__ == '__main__':
	main()
