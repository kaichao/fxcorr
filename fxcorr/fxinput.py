#!/usr/bin/env python3
"""fxinput.py —— `.input` 解析与 batch 预处理（fxcorr 编排前端）

2026-09-29 从 `run_batch.sh` 内嵌的 118 行 python 段抽出（V7 P3 前）。抽出来的
理由不是"文件太长"，而是**那段是逻辑、不是编排**：解析 `.input`、前置校验、推导
ds 组划分——其中 **ds 组划分与 fxcorr-x 的 C++ 侧 `deriveDsGroups`
（`applications/fxcorr-x/src/main.cpp`）是同一条规则的两处实现**，中间没有编译器
兜底，改一处漏一处的后果是**静默错数据**（分片边界算错 → 每片少算或多算 ds，
而记录看起来完全正常）。在此之前它藏在 heredoc 里，既不能单测、也不能被别的
脚本复用。

对照判据：`fxcorr/test/input/run_consistency.sh`——同一批 `.input` 喂两侧，
逐组比成员。

2026-10-02：**DATA TABLE 软链重指（`relink_data_table`）退役**——数据定位改为
"任务 headers 的 `real_path` + 命名规则两态"（`v8-plan.md` §2），`fxcorr-f` 不再
读 `.input` 的 `FILE` 行，本脚本也不再建软链。`make_testdata.sh` 的软链保留：
mpifxcorr 基准（`run_bench.sh`）仍按 `FILE` 行读数据。

子命令：

  prepare <workdir> <batch_id> <visroot>
      全套：读 batch.json → 解析 .input → 前置校验 → 推导 ds 组。
      `run_batch.sh` 用它。

  groups <workdir> <batch_id>
      **只**打印 ds 组划分，无任何副作用（不改文件、不建目录）。对照测试用。

输出格式（prepare）：

  CFGIN=<config_file>                  batch.json 的 config_file（相对 workdir）
  NGRP=<组数>
  OUTDIR=<fxcorr-x 的输出目录>
  --
  <station> <dsidx> <group> <datafile> 每个 datastream 一行，按 `.input` 的序

输出格式（groups）：每组一行 `{0,1,8,9}`，行序即组序（= 组内最小 ds 序的顺序，
与 C++ 侧一致）。

退出码：0 成功；1 = 校验或解析失败，消息到 stderr。

**消息前缀刻意保留 `run_batch.sh:`**——抽出前就是它，且日志解析可能已经固化在
别处；改前缀是纯粹的破坏性改名，没有任何收益。
"""

import json
import math
import os
import re
import sys

PROG = 'run_batch.sh:'


class InputError(Exception):
	"""校验或解析失败。CLI 捕获后打印到 stderr 并以 1 退出。"""


# --------------------------------------------------------------------------
# 解析
# --------------------------------------------------------------------------

def read_batch_json(workdir, bid):
	"""读 batches/<bid>.json。"""
	path = os.path.join(workdir, 'batches', bid + '.json')
	if not os.path.exists(path):
		raise InputError('%s %s not found' % (PROG, path))
	with open(path) as f:
		return json.load(f)


def get_input_keyval(text, key, label):
	"""取 `.input` 的单值行 `<key>: <value>`。

	**必须匹配行首**：`.input` 里 `CALC FILENAME` 与 `OUTPUT FILENAME` 之类都
	是顶层键，用非锚定的正则会把 DATA TABLE 里的注释行也吃进来。
	"""
	m = re.search(r'^%s:\s*(.*)$' % re.escape(key), text, re.M)
	if not m:
		raise InputError('%s %s not found in %s' % (PROG, key, label))
	return m.group(1).strip()


class InputConfig(object):
	"""一份 `.input` 里本模块关心的全部内容（不做任何 I/O）。"""

	def __init__(self, label, text):
		self.label = label
		self.text = text
		self.mjd = int(get_input_keyval(text, 'START MJD', label))
		self.seconds = float(get_input_keyval(text, 'START SECONDS', label))
		self.outfile = get_input_keyval(text, 'OUTPUT FILENAME', label)

		# f 任务展开按 .input 的 datastream 序（TELESCOPE INDEX 逐个；多
		# datastream 站重复出现，每 datastream 一个 f 任务，带站内序号 dsidx）
		telnames = re.findall(r'^TELESCOPE NAME \d+:\s*(\S+)\s*$', text, re.M)
		self.stations = [telnames[int(t)] for t in
		                 re.findall(r'^TELESCOPE INDEX:\s*(\d+)\s*$', text, re.M)]
		self.dsidx = []
		seen = {}
		for st in self.stations:
			self.dsidx.append(seen.get(st, 0))
			seen[st] = self.dsidx[-1] + 1
		self.datafiles = re.findall(r'^FILE \d+/\d+:\s*(\S+)\s*$', text, re.M)

		# 每条 baseline 绑定一对 ds（全局序号），ds 组划分的输入
		self.dspairs = list(zip(
			[int(x) for x in re.findall(r'^D/STREAM A INDEX \d+:\s*(\d+)\s*$', text, re.M)],
			[int(x) for x in re.findall(r'^D/STREAM B INDEX \d+:\s*(\d+)\s*$', text, re.M)]))


def load_input(workdir, bid, bj):
	"""按 batch.json 的 config_file 读 `.input`。"""
	cfgrel = bj['config_file']
	path = os.path.join(workdir, cfgrel)
	if not os.path.exists(path):
		raise InputError('%s config_file %s not found' % (PROG, cfgrel))
	with open(path) as f:
		return cfgrel, InputConfig(cfgrel, f.read())


# --------------------------------------------------------------------------
# ds 组划分（**与 C++ 侧 deriveDsGroups 同规则，两处必须同改**）
# --------------------------------------------------------------------------

def derive_ds_groups(nds, dspairs):
	"""把 nds 个 datastream 按 baseline 连接关系分组，返回组列表。

	**为什么是连通分量而不是按 ds 序号或 freq 配对**：同频段的 X/Y 是两条不同
	的 freq 条目（按 freq 聚类会把一对极化拆开），而 t25362 的第 2 条 baseline
	是 (ds1, ds8) 而非 (ds1, ds9)——按序号配对同样会配错。机理与实测表见
	`data-spec.md` 第 8 节。

	合并时**小的根胜出**，于是代表元 = 组内最小 ds 序；收集时按 root 升序扫，
	组的顺序就是"最小 ds 序"的顺序（即频段升序）。

	逐行对应 `applications/fxcorr-x/src/main.cpp` 的 deriveDsGroups——那一段的
	注释解释了同样的取舍，改这里必须同改那里。
	"""
	parent = list(range(nds))

	def find(x):
		# 路径压缩；C++ 侧写在同一行里，语义相同
		while parent[x] != x:
			parent[x] = parent[parent[x]]
			x = parent[x]
		return x

	for a, b in dspairs:
		ra, rb = find(a), find(b)
		if ra < rb:
			parent[rb] = ra
		elif rb < ra:
			parent[ra] = rb

	groups = []
	used = [False] * nds
	for root in range(nds):
		if used[root]:
			continue
		grp = []
		for i in range(root, nds):
			if find(i) == root:
				grp.append(i)
				used[i] = True
		groups.append(grp)
	return groups


# --------------------------------------------------------------------------
# 前置校验（与 fxcorr-f/x 的程序内校验同语义，提前挡）
# --------------------------------------------------------------------------

def validate(cfg, bj):
	"""返回 (batchstartns, inttime, batchdur)。

	三条约束，都是"不挡就会静默出错"的那类：

	  - **batch 起点在 subint 边界**（1µs 容差，同 `fxcorr-f` main.cpp:175-182，
	    吸收 start_mjd 的 f64 表示误差）；
	  - **batch 时长为 INT TIME 整数倍**（跨 batch 追加不碎片化，data-spec 12）；
	  - **INT TIME 为 subint 整数倍**（`fxcorr-x` 的
	    offsetnsperintegration==0 硬校验）。
	"""
	subint = int(bj['subint_ns'])
	inttime = float(bj['integration_sec'])
	initsec = (bj['start_mjd'] - cfg.mjd - cfg.seconds / 86400.0) * 86400.0
	batchstartns = int(math.floor(initsec * 1.0e9 + 0.5))

	rem = batchstartns % subint
	if rem > 1000 and (subint - rem) > 1000:
		raise InputError('%s batch start is not on a subint boundary (%d ns into a %d ns subint)'
		                 % (PROG, rem, subint))

	batchdur = int(bj['n_subints']) * subint / 1.0e9
	nint = round(batchdur / inttime)
	if abs(nint * inttime - batchdur) > 1e-6:
		raise InputError('%s batch duration %g is not an integer multiple of INT TIME %g'
		                 % (PROG, batchdur, inttime))

	trem = inttime * 1.0e9 % subint
	if min(trem, subint - trem) > 1.0:
		raise InputError('%s INT TIME %g is not a multiple of SUBINT %d ns'
		                 % (PROG, inttime, subint))
	return batchstartns, inttime, batchdur


def check_stations(cfg, bj):
	"""batch.json 的 stations 是去重元数据（data-spec 5.3），仅作交叉校验。"""
	if bj.get('stations') is not None and set(bj['stations']) != set(cfg.stations):
		raise InputError('%s batch.json stations do not match .input datastreams' % PROG)
	if len(cfg.stations) != len(cfg.datafiles):
		raise InputError('%s station/file count mismatch in %s (%d stations, %d files)'
		                 % (PROG, cfg.label, len(cfg.stations), len(cfg.datafiles)))


# --------------------------------------------------------------------------
# 组装
# --------------------------------------------------------------------------

def parse_batch(workdir, bid):
	"""读 batch.json 与 .input：返回 (bj, cfgrel, cfg)。无副作用。"""
	bj = read_batch_json(workdir, bid)
	cfgrel, cfg = load_input(workdir, bid, bj)
	check_stations(cfg, bj)
	return bj, cfgrel, cfg


def resolve_outdir(visroot, cfg):
	"""OUTPUT FILENAME 相对 vis 根解析（Q20）；绝对值优先——os.path.join 对绝对
	路径会丢掉前缀，与程序内同一规则。"""
	return os.path.join(visroot, cfg.outfile.rstrip('/'))


def groups_of(cfg):
	bj_nds = len(cfg.stations)
	return derive_ds_groups(bj_nds, cfg.dspairs)


def prepare(workdir, bid, visroot):
	"""全套预处理。返回可以直接喂给 bash 的文本行。"""
	bj, cfgrel, cfg = parse_batch(workdir, bid)
	validate(cfg, bj)
	groups = groups_of(cfg)

	# ds 全局序号 → 组号：C++ 侧按 ds 序号打掩码，这里反向查表，两侧一致
	groupof = [0] * len(cfg.stations)
	for g, grp in enumerate(groups):
		for i in grp:
			groupof[i] = g

	out = ['CFGIN=%s' % cfgrel,
	       'NGRP=%d' % len(groups),
	       'OUTDIR=%s' % resolve_outdir(visroot, cfg),
	       '--']
	for i in range(len(cfg.stations)):
		out.append('%s %d %d %s' % (cfg.stations[i], cfg.dsidx[i], groupof[i], cfg.datafiles[i]))
	return out


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def usage():
	sys.stderr.write(
		'用法：fxinput.py prepare <workdir> <batch_id> <visroot>\n'
		'      fxinput.py groups  <workdir> <batch_id>\n')
	return 2


def main(argv):
	if len(argv) < 2:
		return usage()
	cmd = argv[1]

	if cmd == 'prepare':
		if len(argv) != 5:
			return usage()
		workdir, bid, visroot = argv[2:5]
		lines = prepare(workdir, bid, visroot)
	elif cmd == 'groups':
		if len(argv) != 4:
			return usage()
		workdir, bid = argv[2:4]
		_, _, cfg = parse_batch(workdir, bid)
		lines = ['{%s}' % ','.join(str(i) for i in grp) for grp in groups_of(cfg)]
	else:
		return usage()

	sys.stdout.write('\n'.join(lines) + '\n')
	return 0


if __name__ == '__main__':
	try:
		sys.exit(main(sys.argv))
	except InputError as e:
		sys.stderr.write(str(e) + '\n')
		sys.exit(1)
