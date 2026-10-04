#!/usr/bin/env bash
# make_testdata.sh —— 构建 data-spec 布局的标准测试数据（规格见 fxcorr/v1-plan.md 2.4）
#
# 步骤：① config/ 前处理（vex2difx + difxcalc，幂等）→ ② 从 .input 推导 batch 参数、
# 分配 batch_id（8 位顺序号，已有同参数的 batch 则复用，见 data-spec 第 6 节）
# → ③ 写 batches/<batch_id>.json（全字段一次写全）→ ④ fxcorr-sim 生成 raw VDIF
# （无 tone：新路径，(batch, station, ds) 任务并行分发，公共信号各任务就地合成；带 tone：legacy 逐站）
# → ⑤ 软链 <DATA TABLE 文件名> 到最后 batch 的 VDIF → ⑥ stdout 打印 batch_id。
#
# 用法：./make_testdata.sh [-n N] [-p P] [--nodes "host:st1,st2 ..."] [workdir] [tone_mhz ...]
#
#   -n N          连续 N 个 batch（时间连续切分，验证 SWIN 跨 batch 追加）
#   -p P          station 任务本地并行度（默认 1 = 串行，P1：
#                 (batch,station,ds) 任务 xargs 并行分发）
#   --nodes MAP   ssh 节点映射（P1）：把站分发到远程节点跑 station（共享存储假设，
#                 workdir 全节点同路径可见），未列出站回落本地；每 entry
#                 host:st1,st2，可多个 entry 空格分隔
#   workdir       项目根目录（默认 .）
#   tone_mhz ...  fxcorr-sim 基带 tone（MHz）：0 个 = 无 tone（新频域路径）；1 个 = 全 band
#                 同频（legacy 时域路径）；nbands 个 = 逐 band
#   环境变量：FXSIM_NOISE/SEED/ADAPTIVE/SPECRES/LINE/FLUX/SEFD 透传 fxcorr-sim
#   （FXSIM_DELAY 由程序默认开）；BATCH_NSUBINTS 覆盖每 batch subint 数；
#   FXCORR_WORKDIR 定义项目根目录（位置参数优先）
set -euo pipefail

# 四个可重定向的根（V5 P5）：容器/ssh 透传与解析共用一份清单
FXCORR_ROOT_VARS=(FXCORR_RAW_ROOT FXCORR_FENGINE_ROOT FXCORR_VIS_ROOT FXCORR_PRODUCT_ROOT)

# 容器模式开关：FXCORR_RUN_MODE=container 时工具经 docker run 调用（见下方 fxc）
FXCORR_RUN_MODE="${FXCORR_RUN_MODE:-host}"
# 单镜像：串行链路上所有命令同在 fxcorr/fxcorr（V5 P4）；容器模式下 fxc 与 stationcmd 共用
CONTAINER_IMG=fxcorr/fxcorr

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# 目录根解析（V5 P5）：与 fxcorr-f/x/sim 内的 FxcorrPath 同规则，一处定义两处用
. "$SCRIPTDIR/roots.sh"
# 判据用 fxcorr 自己的工具，不用 vex2difx：有的机器已经把另一份 difx 的 bin 放进
# 了 PATH（测试机即如此——BASH_ENV=/etc/profile.d/difx.sh 注入 /opt/difx/2.9.0），
# 那里有 vex2difx 却没有 fxcorr 三工具，用 vex2difx 判据会跳过 source：后续
# fxcorr-sim 找不到，前处理还会静默落到那份 difx 的 vex2difx/difxcalc 上。
if [ "$FXCORR_RUN_MODE" != "container" ] && ! command -v fxcorr-sim >/dev/null 2>&1 && [ -f "$SCRIPTDIR/../setup.bash" ]; then
	# setup.bash 的 PurgePath 引用可能未设置的变量（PERL5LIB 等），
	# 与 set -u 冲突，source 时临时放开
	set +u
	. "$SCRIPTDIR/../setup.bash"
	set -u
fi

# ---- 容器模式执行前缀：单镜像（链路上所有命令同镜像），workdir 整体挂载、cwd 与宿主直跑一致 ----
run_in_container()
{
	local tool="$1"; shift
	local img=$CONTAINER_IMG
	local envargs=()
	[ -n "${FXSIM_NOISE+x}" ] && envargs+=(-e FXSIM_NOISE="$FXSIM_NOISE")
	[ -n "${FXSIM_SEED+x}" ] && envargs+=(-e FXSIM_SEED="$FXSIM_SEED")
	[ -n "${FXSIM_SPECRES+x}" ] && envargs+=(-e FXSIM_SPECRES="$FXSIM_SPECRES")
	[ -n "${FXSIM_LINE+x}" ] && envargs+=(-e FXSIM_LINE="$FXSIM_LINE")
	[ -n "${FXSIM_FLUX+x}" ] && envargs+=(-e FXSIM_FLUX="$FXSIM_FLUX")
	[ -n "${FXSIM_SEFD+x}" ] && envargs+=(-e FXSIM_SEFD="$FXSIM_SEFD")
	[ -n "${FXSIM_PCAL+x}" ] && envargs+=(-e FXSIM_PCAL="$FXSIM_PCAL")
	# 四个根一并透传（Q12）：容器内程序仍要知道用哪个根。挂载由外部编排平台
	# 按"各根按宿主同路径可见"的约定负责，本脚本不实现多根挂载。
	local r
	for r in "${FXCORR_ROOT_VARS[@]}"; do
		[ -n "${!r+x}" ] && envargs+=(-e "$r=${!r}")
	done
	docker run --rm "${envargs[@]}" -v "$WORKDIR:$WORKDIR" -w "$(pwd)" "$img:latest" "$tool" "$@"
}
fxc()
{
	if [ "$FXCORR_RUN_MODE" = "container" ]; then
		run_in_container "$@"
	else
		"$@"
	fi
}

usage()
{
	cat >&2 <<'EOF'
用法：./make_testdata.sh [-n N] [-p P] [--nodes "host:st1,st2 ..."] [workdir] [tone_mhz ...]
  -n N          连续 N 个 batch（时间连续切分，验证 SWIN 跨 batch 追加）
  -p P          station 任务本地并行度（默认 1 = 串行）
  --nodes MAP   ssh 节点映射 host:st1,st2（多 entry 空格分隔；未列出站本地跑）
  workdir       项目根目录（默认 .）
  tone_mhz ...  fxcorr-sim 基带 tone（MHz）：0 个 = 无 tone；1 个 = 全 band 同频；nbands 个 = 逐 band
  环境变量：FXSIM_NOISE/SEED/ADAPTIVE/SPECRES/LINE/FLUX/SEFD 透传 fxcorr-sim；BATCH_NSUBINTS 覆盖每 batch subint 数；FXCORR_WORKDIR 定义项目根目录（位置参数优先）
EOF
	exit "${1:-2}"
}

# --nodes 长选项先从参数序列摘出（任意位置；getopts 不支持长选项）
NODEMAP=""
ARGS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--nodes) shift; [ $# -gt 0 ] || usage; NODEMAP=$1; shift ;;
		--) shift; ARGS+=("$@"); break ;;
		*) ARGS+=("$1"); shift ;;
	esac
done
set -- "${ARGS[@]}"

NBATCH=1
PAR=1
while getopts "n:p:h" opt; do
	case "$opt" in
		n) NBATCH=$OPTARG ;;
		p) PAR=$OPTARG ;;
		h) usage 0 ;;
		*) usage ;;
	esac
done
shift $((OPTIND - 1))
if ! [[ $NBATCH =~ ^[1-9][0-9]*$ ]]; then
	echo "make_testdata.sh: -n must be a positive integer" >&2
	exit 2
fi
if ! [[ $PAR =~ ^[1-9][0-9]*$ ]]; then
	echo "make_testdata.sh: -p must be a positive integer" >&2
	exit 2
fi
if [ "$FXCORR_RUN_MODE" = "container" ] && { [ "$PAR" -gt 1 ] || [ -n "$NODEMAP" ]; }; then
	echo "make_testdata.sh: -p/--nodes need FXCORR_RUN_MODE=host (container orchestration is V2 scalebox)" >&2
	exit 2
fi

WORKDIR="${FXCORR_WORKDIR:-.}"
if [ $# -gt 0 ] && [ -d "$1" ]; then
	WORKDIR=$1	# 位置参数优先于环境变量
	shift
fi
TONES=("$@")
WORKDIR=$(cd "$WORKDIR" && pwd)
fxcorr_roots "$WORKDIR"
WORKDIR=$FXCORR_ROOT_WORKDIR
CFG="$WORKDIR/config"
mkdir -p "$CFG" "$WORKDIR/batches"
fxcorr_mkroots	# Q19：四个根由编排层建齐，程序遇根不存在只报错

# ---- ① 前处理（幂等） ----
# config 资产缺才复制。
# 单 batch（对拍场景）直接用 difxcalc 原产物（SUBINT 0.524288s、
# INT TIME 1.048576，6/6 对拍同配置）；帧对齐的 128ms SUBINT 变体只用于
# -n 多 batch（连续切分起点须帧边界，0.524288s 与帧网格公倍数 65.5s 不可用）。
# 128ms 变体会触发 mpifxcorr vdifmux 帧号 bit7 错读（~每 256 帧坏 0.5s，
# 见 v1-plan 2.4），多 batch 对拍不做，仅 fxcorr 侧自测。
[ -f "$CFG/test.vex" ] || cp "$SCRIPTDIR/test/test.vex" "$CFG/test.vex"
[ -f "$CFG/test.v2d" ] || cp "$SCRIPTDIR/test/test.v2d" "$CFG/test.v2d"
# 前处理走各自的规范适配封装（V5 P5 Q15）：cwd 的处理与"绝对路径改回相对"的
# 规范化都在那两个脚本里，本脚本只负责幂等与输出约定。
# 前处理工具的进度/警告输出重定向 stderr，脚本 stdout 只留 batch_id（规格⑥）
#
# 产物名不写死：vex2difx 按 .v2d 的 startSeries 命名（0 → test.input、
# 1 → test_1.input），真实观测的 v2d 多为 1，而两个 wrap 脚本都按 glob 处理
# ——此处同口径。*-sim.input 是本脚本自己派生出来的，不参与判定。
pre_input()
{
	local p
	for p in "$CFG"/*.input; do
		[ -e "$p" ] || continue
		case "$p" in *-sim.input) continue ;; esac
		printf '%s\n' "$p"
		return 0
	done
	return 1
}

INPUTFILE=$(pre_input) || INPUTFILE=""
if [ -z "$INPUTFILE" ]; then
	"$SCRIPTDIR/wrap_vex2difx.sh" "$WORKDIR" test.v2d >&2
	INPUTFILE=$(pre_input)
fi
if ! ls "$CFG"/*.im >/dev/null 2>&1; then
	# .calc 名同样由 startSeries 决定，glob 取（wrap_difxcalc.sh 接受多个）
	CALCS=$(cd "$CFG" && ls *.calc 2>/dev/null) || CALCS=""
	if [ -n "$CALCS" ]; then
		# shellcheck disable=SC2086  # 有意分词：可能是多个 .calc
		"$SCRIPTDIR/wrap_difxcalc.sh" "$WORKDIR" $CALCS >&2
	fi
fi
if [ "$NBATCH" -gt 1 ]; then
	if [ ! -f "$CFG/test-sim.input" ]; then
		sed -e 's/^INT TIME (SEC):[[:space:]]*[0-9.]*$/INT TIME (SEC):     0.256/' \
		    -e 's/^SUBINT NANOSECONDS:[[:space:]]*[0-9]*$/SUBINT NANOSECONDS: 128000000/' \
		    "$INPUTFILE" > "$CFG/test-sim.input"
	fi
	INPUT="$CFG/test-sim.input"
else
	INPUT="$INPUTFILE"
fi

# ---- ②③ 解析 .input、推导 batch 参数、写 batches/<batch_id>.json ----
# python3：f64 精确 repr（start_mjd）、MJD 转历表（start_time）、数组字段拼装
OUT=$(mktemp)
trap 'rm -f "${OUT:-}" "${TASKS:-}"' EXIT
export NBATCH INPUT
python3 - "$WORKDIR" <<'PYEOF' > "$OUT"
import json, os, re, sys
from datetime import datetime, timedelta

workdir = sys.argv[1]
text = open(os.environ['INPUT']).read()

def one(key):
    m = re.search(r'^%s:\s*(.*)$' % re.escape(key), text, re.M)
    if not m:
        sys.exit('make_testdata.sh: %s not found in %s' % (key, os.environ['INPUT']))
    return m.group(1).strip()

nbatch = int(os.environ['NBATCH'])
mjd = int(one('START MJD'))
startsec = float(one('START SECONDS'))
subintns = int(one('SUBINT NANOSECONDS'))
inttime = float(one('INT TIME (SEC)'))
nchan = int(one('NUM CHANNELS 0'))
pol = one('REC BAND 0 POL')          # 兜底值：极化产品解析失败时用第一个 datastream 的

# datastream 序 → 站名：DATASTREAM 段逐块 TELESCOPE INDEX + TELESCOPE TABLE
telnames = re.findall(r'^TELESCOPE NAME \d+:\s*(\S+)\s*$', text, re.M)
ds_tel = re.findall(r'^TELESCOPE INDEX:\s*(\d+)\s*$', text, re.M)
stations = [telnames[int(t)] for t in ds_tel]
datafiles = re.findall(r'^FILE \d+/\d+:\s*(\S+)\s*$', text, re.M)
if len(stations) != len(datafiles):
    sys.exit('make_testdata.sh: station/file count mismatch in %s' % os.environ['INPUT'])

# 站内 datastream 序号（0-based）与该站的 ds 总数：多 datastream 站每 ds 一个
# 文件、一个 station 任务，序号口径与 fxcorr-f 的 ds_index 一致（data-spec 5.2）
dsidx = []
nds_of = {}
for st in stations:
    dsidx.append(nds_of.get(st, 0))
    nds_of[st] = dsidx[-1] + 1

# BASELINE TABLE 的 D/STREAM A/B INDEX → 站名对
a = re.findall(r'^D/STREAM A INDEX \d+:\s*(\d+)\s*$', text, re.M)
b = re.findall(r'^D/STREAM B INDEX \d+:\s*(\d+)\s*$', text, re.M)
baselines = ['%s-%s' % (stations[int(x)], stations[int(y)]) for x, y in zip(a, b)]

# 极化产品：**逐条 baseline 推导**（data-spec 第 8 节）——每条 baseline 只出 1 个
# product，其极化 = A 侧 band 的极化 × B 侧 band 的极化；RR/RL/LR/LL 四种组合就是
# 四条独立条目（极化维度展开进了 baseline 编号）。.input 文本里没有现成的组合
# 字符串，只能按 recordedbandpols[band] 自己拼——与 configuration.cpp:1029-1042
# 的 polpairs 同规则。此前这里硬写 [polx+polx]（单元素），与规范不符。
dsblocks = re.split(r'^TELESCOPE INDEX:', text, flags=re.M)[1:]
dspols = [dict((int(k), v) for k, v in
               re.findall(r'^REC BAND (\d+) POL:\s*(\S+)\s*$', blk, re.M))
          for blk in dsblocks]
polarizations = []
ca = cb = ba = None
for ln in text.split('\n'):
    m = re.match(r'D/STREAM A INDEX \d+:\s*(\d+)\s*$', ln)
    if m:
        ca, cb, ba = int(m.group(1)), None, None
        continue
    m = re.match(r'D/STREAM B INDEX \d+:\s*(\d+)\s*$', ln)
    if m:
        cb = int(m.group(1))
        continue
    m = re.match(r'D/STREAM A BAND \d+:\s*(\d+)\s*$', ln)
    if m:
        ba = int(m.group(1))
        continue
    m = re.match(r'D/STREAM B BAND \d+:\s*(\d+)\s*$', ln)
    if not m:
        continue
    if None in (ca, cb, ba):
        continue
    pa = dspols[ca].get(ba, '') if ca < len(dspols) else ''
    pb = dspols[cb].get(int(m.group(1)), '') if cb < len(dspols) else ''
    if pa and pb and (pa + pb) not in polarizations:
        polarizations.append(pa + pb)
if not polarizations:
    sys.stderr.write('make_testdata.sh: WARNING: no pol product derived from '
                     'BASELINE TABLE, falling back to the first datastream pol\n')
    polarizations = [pol + pol]

try:
    nsub = int(os.environ.get('BATCH_NSUBINTS', '4'))
except ValueError:
    sys.exit('make_testdata.sh: BATCH_NSUBINTS must be an integer')
if nsub < 1:
    sys.exit('make_testdata.sh: BATCH_NSUBINTS must be positive')

# batch_id 分配（data-spec 第 6 节）：8 位零填充顺序号，取 batches/ 下已有编号
# 的最大值 +1。编号不承载时间信息，所以重跑要另外保证幂等——已有 batch 的
# (start_mjd, n_subints, subint_ns) 与本次规划一致时复用它的编号，否则整批
# 数据会被重新生成一遍（测试机上每 batch 几十 GB）。三个分量都要比：单 batch
# 用 difxcalc 原产物（0.524288s subint）、-n 多 batch 用 test-sim.input（128ms），
# 起止时刻可能相同而粒度和时长不同。
batchdir = os.path.join(workdir, 'batches')
existing = []          # [(start_mjd, n_subints, subint_ns, bid)]
maxnum = 0
for name in (os.listdir(batchdir) if os.path.isdir(batchdir) else []):
    if not name.endswith('.json'):
        continue
    stem = name[:-len('.json')]
    if stem.isdigit():
        maxnum = max(maxnum, int(stem))
    try:
        bj = json.load(open(os.path.join(batchdir, name)))
    except (ValueError, IOError):
        continue
    if bj.get('start_mjd') is not None and bj.get('n_subints') and bj.get('subint_ns'):
        existing.append((bj['start_mjd'], int(bj['n_subints']), int(bj['subint_ns']), stem))
nextnum = maxnum + 1

def allocate_bid(startmjd):
    for smjd, sns, ssub, sbid in existing:
        # 1 µs 容差，与 fxcorr-f/x 的 batch 起点校验同口径（f64 表示误差）
        if sns == nsub and ssub == subintns and abs(smjd - startmjd) * 86400.0 < 1e-6:
            print('make_testdata.sh: reusing batch_id %s for start_mjd %r' % (sbid, startmjd), file=sys.stderr)
            return sbid
    return None

# calc/im 路径取自 .input 的 CALC FILENAME；wrap_vex2difx.sh 规范化后通常是
# 相对 config/ 的裸名，绝对路径也接受（Q20：只对相对路径拼根）
calcfull = one('CALC FILENAME')
if not os.path.isabs(calcfull):
    calcfull = os.path.join(os.path.dirname(os.path.abspath(os.environ['INPUT'])), calcfull)
calcrel = os.path.relpath(calcfull, workdir)
imrel = calcrel[:-len('.calc')] + '.im' if calcrel.endswith('.calc') else calcrel + '.im'

# 公共信号种子（V6 S2.5）：各站的 station 任务在本地各自合成公共信号，种子
# 必须一致，否则跨站相干就没了——所以它随 batch.json 走（公共信号不落盘，
# 没有别的交接点）。默认值与 fxcorr-sim 的默认值一致。
seed = int(os.environ.get('FXSIM_SEED') or '20260912')

epo = datetime(1858, 11, 17)
created = datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ')
for i in range(nbatch):
    startsec_i = startsec + i * nsub * subintns / 1.0e9
    startmjd_i = mjd + startsec_i / 86400.0   # json.dump 用 repr()，f64 往返精确
    bid = allocate_bid(startmjd_i)
    if bid is None:
        bid = '%08d' % nextnum
        nextnum += 1
    batch = {
        'batch_id': bid,
        'start_mjd': startmjd_i,
        'start_time': (epo + timedelta(days=mjd, seconds=startsec_i)).strftime('%Y-%m-%dT%H:%M:%S'),
        'duration_sec': nsub * subintns / 1.0e9,
        'stations': stations,
        'baselines': baselines,
        'config_file': os.path.relpath(os.environ['INPUT'], workdir),
        'calc_file': calcrel,
        'im_file': imrel,
        'n_subints': nsub,
        'subint_ns': subintns,
        'integration_sec': inttime,
        'n_channels': nchan,
        'polarizations': polarizations,
        # 照抄 .input 的 OUTPUT FILENAME 原样（Q10）：实际落点由运行时根决定，
        # 本字段只作记录——没有任何程序把它当路径读
        'difx_dir': one('OUTPUT FILENAME'),
        'created_at': created,
        # 公共信号的 PRNG 种子：本 batch 的全部 station 任务必须读到同一个值
        'seed': seed,
        'status': 'running',
        'fxcorr_f_version': '0.1.0',
        'fxcorr_x_version': '0.1.0',
    }
    with open(os.path.join(workdir, 'batches', bid + '.json'), 'w') as f:
        json.dump(batch, f, indent=2)
        f.write('\n')
    print(bid)
print('--')
for st, di, fn in zip(stations, dsidx, datafiles):
    print('%s %s %d %s' % (st, di, nds_of[st], fn))
PYEOF

# ---- ④ fxcorr-sim：逐 (batch, station, ds) 任务生成（新路径），并行分发 ----
# 带 tone 参数 = legacy 时域合成路径（对拍回归）；无 tone = 新频域路径
# （公共信号在任务内就地合成 + 站噪声）。VDIF 已存在则跳过，幂等。
# 分发（P1）：(batch, station) 任务列表 → xargs -P（-p P，默认 1 = 串行）；
# --nodes 映射的站改 ssh 远程执行（共享存储假设：workdir 全节点同路径可见）。
# $OUT：前段每行一个 batch_id，"--" 之后每行 "站名 文件名"
BATCHES=()
while IFS= read -r line && [ "$line" != "--" ]; do
	BATCHES+=("$line")
done < "$OUT"
declare -a DSTATION DSDI DSNDS DSFILE
while read -r st di nds fn; do
	DSTATION+=("$st")
	DSDI+=("$di")
	DSNDS+=("$nds")
	DSFILE+=("$fn")
done < <(awk 'f{print} /^--$/{f=1}' "$OUT")

# raw/<station>/<station>_<batch_id>[_ds<N>].vdif（相对 raw 根）——后缀只在多
# datastream 站出现，与 fxcorr-sim 的 stationOutPath 同规则；单 ds 站的文件名
# 与加多 ds 支持之前逐字相同
vdifrel()
{
	local st=$1 di=$2 nds=$3 bid=$4
	local base="${st}_${bid}"
	[ "$nds" -gt 1 ] && base="${base}_ds${di}"
	printf '%s/%s.vdif' "$st" "$base"
}

# ---- ③b 根记录与实验级一致性检查（Q18、Q4）：造数前先挡不一致，再记下本 batch 的根 ----
# 检查先于写入：两者的 roots.json 都在 meta/roots/ 下，写过的不能再当"已有记录"
for bid in "${BATCHES[@]}"; do
	fxcorr_check_roots "$bid"
	fxcorr_write_roots "$bid"
done

# station → 节点映射（--nodes）；未列出站留空 = 本地
declare -A NODE_OF
for entry in $NODEMAP; do
	host=${entry%%:*}
	sts=${entry#*:}
	if [ -z "$host" ] || [ -z "$sts" ]; then
		echo "make_testdata.sh: bad --nodes entry '$entry' (want host:st1,st2)" >&2
		exit 2
	fi
	for s in ${sts//,/ }; do
		NODE_OF[$s]=$host
	done
done

# 远程站命令：ssh 非交互 shell 无 setup.bash，用全路径 fxcorr-sim +
# LD_LIBRARY_PATH（$DIFXROOT/lib，默认 /usr/local/difx）；FXSIM_* 透传（存在才传）；
# BatchMode 防交互卡死，accept-new 首次 host key 自动接受。
FXCSIM=$(command -v fxcorr-sim || echo fxcorr-sim)
# 决定"生成出什么数据"的变量必须整组透传：漏掉一个，远程站就与本地站生成
# 不同的数据，而两边的日志都不会说（--nodes 的回归判据正是逐位比较两边产物）
ENVS=()
[ -n "${FXSIM_NOISE+x}" ] && ENVS+=("FXSIM_NOISE=$FXSIM_NOISE")
[ -n "${FXSIM_SEED+x}" ] && ENVS+=("FXSIM_SEED=$FXSIM_SEED")
[ -n "${FXSIM_LIGHT+x}" ] && ENVS+=("FXSIM_LIGHT=$FXSIM_LIGHT")
[ -n "${FXSIM_ADAPTIVE+x}" ] && ENVS+=("FXSIM_ADAPTIVE=$FXSIM_ADAPTIVE")
[ -n "${FXSIM_SPECRES+x}" ] && ENVS+=("FXSIM_SPECRES=$FXSIM_SPECRES")
[ -n "${FXSIM_BLOCK_US+x}" ] && ENVS+=("FXSIM_BLOCK_US=$FXSIM_BLOCK_US")
[ -n "${FXSIM_LINE+x}" ] && ENVS+=("FXSIM_LINE=$FXSIM_LINE")
[ -n "${FXSIM_FLUX+x}" ] && ENVS+=("FXSIM_FLUX=$FXSIM_FLUX")
[ -n "${FXSIM_SEFD+x}" ] && ENVS+=("FXSIM_SEFD=$FXSIM_SEFD")
[ -n "${FXSIM_PCAL+x}" ] && ENVS+=("FXSIM_PCAL=$FXSIM_PCAL")
[ -n "${FXSIM_DELAY+x}" ] && ENVS+=("FXSIM_DELAY=$FXSIM_DELAY")
[ -n "${FXSIM_GAPS+x}" ] && ENVS+=("FXSIM_GAPS=$FXSIM_GAPS")
[ -n "${FXSIM_STARTOFFSET+x}" ] && ENVS+=("FXSIM_STARTOFFSET=$FXSIM_STARTOFFSET")
# 四个根同样透传给远程站（分片生成的产物必须落在与本地同一套根上）
for r in "${FXCORR_ROOT_VARS[@]}"; do
	[ -n "${!r+x}" ] && ENVS+=("$r=${!r}")
done
# 输出一条 station 任务命令行（本地直跑或 ssh 远程），xargs 按行执行
# 本地在容器模式下经 docker run（与 fxc 同前缀；--nodes 与容器模式互斥，前面已挡）
stationcmd()
{
	local bid=$1 st=$2 ds=$3
	local host=${NODE_OF[$st]:-}
	# `${TONES[*]:-}` 的 `:-` 不是多余的：bash < 4.4（RHEL 7 是 4.2）在 `set -u` 下
	# 展开**空数组**会报 unbound variable，而"无 tone"正是空数组。见 build.md
	local args="station '$bid' '$st' '$WORKDIR' '$ds' ${TONES[*]:-}"
	if [ -n "$host" ]; then
		printf 'ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new %q %q\n' \
			"$host" "cd '$WORKDIR' && env ${ENVS[*]:-} LD_LIBRARY_PATH=${DIFXROOT:-/usr/local/difx}/lib:\"\$LD_LIBRARY_PATH\" '$FXCSIM' $args"
	elif [ "$FXCORR_RUN_MODE" = "container" ]; then
		local -a cprefix=(docker run --rm)
		local e
		for e in "${ENVS[@]}"; do cprefix+=(-e "$e"); done
		cprefix+=(-v "$WORKDIR:$WORKDIR" -w "$WORKDIR" "$CONTAINER_IMG" fxcorr-sim)
		printf '%q ' "${cprefix[@]}"
		printf '%s\n' "$args"
	else
		printf '%q %s\n' "$FXCSIM" "$args"
	fi
}

TASKS=$(mktemp)
for bid in "${BATCHES[@]}"; do
	for i in "${!DSTATION[@]}"; do
		st=${DSTATION[$i]}
		rel=$(vdifrel "$st" "${DSDI[$i]}" "${DSNDS[$i]}" "$bid")
		if [ -s "$FXCORR_ROOT_RAW/$rel" ]; then
			echo "make_testdata.sh: skip existing raw/$rel" >&2
		else
			stationcmd "$bid" "$st" "${DSDI[$i]}" >> "$TASKS"
		fi
	done
done
# 任务并行执行；任一失败 xargs 退出 123 → set -e 终止脚本。
# 空列表时跳过（GNU xargs 空输入仍会执行一次命令，BSD 不会，跨平台显式挡）
if [ -s "$TASKS" ]; then
	xargs -0 -P "$PAR" -n 1 bash -c < <(tr '\n' '\0' < "$TASKS")
fi

# ---- ⑤ 软链 <DATA TABLE 文件名> → 最后 batch 的 VDIF ----
# .input 的 DATA TABLE 每个 datastream 只有一个文件名，软链只能指向一个 batch。
# **2026-10-02 起 fxcorr-f 不再读 FILE 行**（数据定位改 real_path/命名规则两态，
# v8-plan.md §2）、run_batch.sh 也不再重指软链——本软链现在只为 **mpifxcorr 基准
# （run_bench.sh：按 FILE 行读数据、并靠软链定位 batch）** 保留；difx2fits 已实测
# **不依赖** FILE 行（v8-plan.md §2.5-1）。
last=${BATCHES[-1]}
for i in "${!DSTATION[@]}"; do
	# 软链落在 raw 根下（Q2）：workdir 里不再散落软链，raw 区整体可在大盘上；
	# 目标用绝对路径——相对目标跨文件系统会断。多 datastream 站的 DATA TABLE
	# 每 ds 一行，各链到本 ds 的文件（data-spec 5.2）
	rel=$(vdifrel "${DSTATION[$i]}" "${DSDI[$i]}" "${DSNDS[$i]}" "$last")
	ln -sf "$FXCORR_ROOT_RAW/$rel" "$FXCORR_ROOT_RAW/${DSFILE[$i]}"
done
if [ "${#BATCHES[@]}" -gt 1 ]; then
	echo "make_testdata.sh: DATA TABLE links point at batch $last (last of ${#BATCHES[@]})" >&2
fi

# ---- ⑥ stdout 打印 batch_id ----
printf '%s\n' "${BATCHES[@]}"
