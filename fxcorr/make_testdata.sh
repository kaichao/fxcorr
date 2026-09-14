#!/usr/bin/env bash
# make_testdata.sh —— 构建 data-spec 布局的标准测试数据（规格见 fxcorr/impl-plan.md 2.4）
#
# 步骤：① config/ 前处理（vex2difx + difxcalc，幂等）→ ② 从 .input 推导 batch 参数
# → ③ 写 batches/<batch_id>.json（全字段一次写全）→ ④ fxcorr-sim 两段式生成 raw VDIF
# （无 tone：每 batch 先 common 一次、station 任务并行分发；带 tone：legacy 逐站）
# → ⑤ 软链 <DATA TABLE 文件名> 到最后 batch 的 VDIF → ⑥ stdout 打印 batch_id。
#
# 用法：./make_testdata.sh [-n N] [-p P] [--nodes "host:st1,st2 ..."] [workdir] [tone_mhz ...]
#
#   -n N          连续 N 个 batch（时间连续切分，验证 SWIN 跨 batch 追加）；
#                 多 batch 时 n_subints 自动提升到每 batch 时长 ≥ 1s（batch_id 秒唯一）
#   -p P          station 任务本地并行度（默认 1 = 串行，P1：common 一次后
#                 (batch,station) 任务 xargs 并行分发）
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

# 容器模式开关：FXCORR_RUN_MODE=container 时工具经 docker run 调用（见下方 fxc）
FXCORR_RUN_MODE="${FXCORR_RUN_MODE:-host}"

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ "$FXCORR_RUN_MODE" != "container" ] && ! command -v vex2difx >/dev/null 2>&1 && [ -f "$SCRIPTDIR/../setup.bash" ]; then
	# setup.bash 的 PurgePath 引用可能未设置的变量（PERL5LIB 等），
	# 与 set -u 冲突，source 时临时放开
	set +u
	. "$SCRIPTDIR/../setup.bash"
	set -u
fi

# ---- 容器模式执行前缀：工具→镜像映射，workdir 整体挂载、cwd 与宿主直跑一致 ----
run_in_container()
{
	local tool="$1"; shift
	local img
	case "$tool" in
		fxcorr-f)   img=fxcorr-f ;;
		fxcorr-x)   img=fxcorr-x ;;
		fxcorr-sim) img=fxcorr-sim ;;
		vex2difx|difxcalc|difx2fits) img=difx-tools ;;
		*) echo "fxc: no image for tool $tool" >&2; exit 2 ;;
	esac
	local envargs=()
	[ -n "${FXSIM_NOISE+x}" ] && envargs+=(-e FXSIM_NOISE="$FXSIM_NOISE")
	[ -n "${FXSIM_SEED+x}" ] && envargs+=(-e FXSIM_SEED="$FXSIM_SEED")
	[ -n "${FXSIM_SPECRES+x}" ] && envargs+=(-e FXSIM_SPECRES="$FXSIM_SPECRES")
	[ -n "${FXSIM_LINE+x}" ] && envargs+=(-e FXSIM_LINE="$FXSIM_LINE")
	[ -n "${FXSIM_FLUX+x}" ] && envargs+=(-e FXSIM_FLUX="$FXSIM_FLUX")
	[ -n "${FXSIM_SEFD+x}" ] && envargs+=(-e FXSIM_SEFD="$FXSIM_SEFD")
	[ -n "${FXSIM_PCAL+x}" ] && envargs+=(-e FXSIM_PCAL="$FXSIM_PCAL")
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
CFG="$WORKDIR/config"
mkdir -p "$CFG" "$WORKDIR/batches"

# ---- ① 前处理（幂等） ----
# config 资产缺才复制。
# 单 batch（对拍场景）直接用 difxcalc 原产物 test.input（SUBINT 0.524288s、
# INT TIME 1.048576，6/6 对拍同配置）；帧对齐的 128ms SUBINT 变体只用于
# -n 多 batch（连续切分起点须帧边界，0.524288s 与帧网格公倍数 65.5s 不可用）。
# 128ms 变体会触发 mpifxcorr vdifmux 帧号 bit7 错读（~每 256 帧坏 0.5s，
# 见 impl-plan 2.4），多 batch 对拍不做，仅 fxcorr 侧自测。
[ -f "$CFG/test.vex" ] || cp "$SCRIPTDIR/test/test.vex" "$CFG/test.vex"
[ -f "$CFG/test.v2d" ] || cp "$SCRIPTDIR/test/test.v2d" "$CFG/test.v2d"
# vex2difx 的 vex= 路径相对 cwd（非 v2d 目录），.input 也输出到 cwd；
# 故在 config/ 内跑，与 .v2d/.vex 同目录（.input 内 CALC FILENAME 会绝对化）
# 前处理工具的进度/警告输出重定向 stderr，脚本 stdout 只留 batch_id（规格⑥）
[ -f "$CFG/test.input" ] || (cd "$CFG" && fxc vex2difx test.v2d >&2)
[ -f "$CFG/test.im" ] || (cd "$CFG" && fxc difxcalc test.calc >&2)
if [ "$NBATCH" -gt 1 ]; then
	if [ ! -f "$CFG/test-sim.input" ]; then
		sed -e 's/^INT TIME (SEC):[[:space:]]*[0-9.]*$/INT TIME (SEC):     0.256/' \
		    -e 's/^SUBINT NANOSECONDS:[[:space:]]*[0-9]*$/SUBINT NANOSECONDS: 128000000/' \
		    "$CFG/test.input" > "$CFG/test-sim.input"
	fi
	INPUT="$CFG/test-sim.input"
else
	INPUT="$CFG/test.input"
fi

# ---- ②③ 解析 .input、推导 batch 参数、写 batches/<batch_id>.json ----
# python3：f64 精确 repr（start_mjd）、MJD 转历表（start_time）、数组字段拼装
OUT=$(mktemp)
trap 'rm -f "${OUT:-}" "${TASKS:-}"' EXIT
export NBATCH INPUT
python3 - "$WORKDIR" <<'PYEOF' > "$OUT"
import json, math, os, re, sys
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
pol = one('REC BAND 0 POL')          # V1 单 pol：取第一个 datastream 的记录偏振
polx = 'X' if pol == 'R' else ('Y' if pol == 'L' else pol)

# datastream 序 → 站名：DATASTREAM 段逐块 TELESCOPE INDEX + TELESCOPE TABLE
telnames = re.findall(r'^TELESCOPE NAME \d+:\s*(\S+)\s*$', text, re.M)
ds_tel = re.findall(r'^TELESCOPE INDEX:\s*(\d+)\s*$', text, re.M)
stations = [telnames[int(t)] for t in ds_tel]
datafiles = re.findall(r'^FILE \d+/\d+:\s*(\S+)\s*$', text, re.M)
if len(stations) != len(datafiles):
    sys.exit('make_testdata.sh: station/file count mismatch in %s' % os.environ['INPUT'])

# BASELINE TABLE 的 D/STREAM A/B INDEX → 站名对
a = re.findall(r'^D/STREAM A INDEX \d+:\s*(\d+)\s*$', text, re.M)
b = re.findall(r'^D/STREAM B INDEX \d+:\s*(\d+)\s*$', text, re.M)
baselines = ['%s-%s' % (stations[int(x)], stations[int(y)]) for x, y in zip(a, b)]

try:
    nsub = int(os.environ.get('BATCH_NSUBINTS', '4'))
except ValueError:
    sys.exit('make_testdata.sh: BATCH_NSUBINTS must be an integer')
if nsub < 1:
    sys.exit('make_testdata.sh: BATCH_NSUBINTS must be positive')
if nbatch > 1:
    # batch_id 秒 = floor(batch 起点秒)，每 batch 时长 ≥ 1s 才保证 batch_id 唯一
    minn = (10**9 + subintns - 1) // subintns
    if nsub < minn:
        print('make_testdata.sh: -n %d raises n_subints to %d (batch duration >= 1s for unique batch_id)' % (nbatch, minn), file=sys.stderr)
        nsub = minn

# calc/im 路径取自 .input 的 CALC FILENAME（difxcalc 写的绝对路径 → 转相对）
calcfull = one('CALC FILENAME')
calcrel = os.path.relpath(calcfull, workdir)
imrel = calcrel[:-len('.calc')] + '.im' if calcrel.endswith('.calc') else calcrel + '.im'

epo = datetime(1858, 11, 17)
created = datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ')
for i in range(nbatch):
    startsec_i = startsec + i * nsub * subintns / 1.0e9
    bid = '%d_%d' % (mjd, math.floor(startsec_i))
    batch = {
        'batch_id': bid,
        'start_mjd': mjd + startsec_i / 86400.0,   # json.dump 用 repr()，f64 往返精确
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
        'polarizations': [polx + polx],
        'difx_dir': 'vis/%s.difx' % bid,
        'created_at': created,
        'status': 'running',
        'fxcorr_f_version': '0.1.0',
        'fxcorr_x_version': '0.1.0',
    }
    with open(os.path.join(workdir, 'batches', bid + '.json'), 'w') as f:
        json.dump(batch, f, indent=2)
        f.write('\n')
    print(bid)
print('--')
for st, fn in zip(stations, datafiles):
    print('%s %s' % (st, fn))
PYEOF

# ---- ④ fxcorr-sim 两段式（新路径）：每 batch 先 common（一次），station 任务并行分发 ----
# 带 tone 参数 = legacy 时域合成路径（对拍回归），无 common 阶段；
# 无 tone = 新频域路径（公共信号 + 站噪声）。VDIF 已存在则跳过，幂等。
# 分发（P1）：(batch, station) 任务列表 → xargs -P（-p P，默认 1 = 串行）；
# --nodes 映射的站改 ssh 远程执行（共享存储假设：workdir 全节点同路径可见）。
# $OUT：前段每行一个 batch_id，"--" 之后每行 "站名 文件名"
BATCHES=()
while IFS= read -r line && [ "$line" != "--" ]; do
	BATCHES+=("$line")
done < "$OUT"
declare -a DSTATION DSFILE
while read -r st fn; do
	DSTATION+=("$st")
	DSFILE+=("$fn")
done < <(awk 'f{print} /^--$/{f=1}' "$OUT")

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
ENVS=()
[ -n "${FXSIM_NOISE+x}" ] && ENVS+=("FXSIM_NOISE=$FXSIM_NOISE")
[ -n "${FXSIM_SEED+x}" ] && ENVS+=("FXSIM_SEED=$FXSIM_SEED")
[ -n "${FXSIM_ADAPTIVE+x}" ] && ENVS+=("FXSIM_ADAPTIVE=$FXSIM_ADAPTIVE")
[ -n "${FXSIM_SPECRES+x}" ] && ENVS+=("FXSIM_SPECRES=$FXSIM_SPECRES")
[ -n "${FXSIM_LINE+x}" ] && ENVS+=("FXSIM_LINE=$FXSIM_LINE")
[ -n "${FXSIM_FLUX+x}" ] && ENVS+=("FXSIM_FLUX=$FXSIM_FLUX")
[ -n "${FXSIM_SEFD+x}" ] && ENVS+=("FXSIM_SEFD=$FXSIM_SEFD")
# 输出一条 station 任务命令行（本地直跑或 ssh 远程），xargs 按行执行
stationcmd()
{
	local bid=$1 st=$2
	local host=${NODE_OF[$st]:-}
	local args="station '$bid' '$st' '$WORKDIR' ${TONES[*]}"
	if [ -n "$host" ]; then
		printf 'ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new %q %q\n' \
			"$host" "cd '$WORKDIR' && env ${ENVS[*]} LD_LIBRARY_PATH=${DIFXROOT:-/usr/local/difx}/lib:\"\$LD_LIBRARY_PATH\" '$FXCSIM' $args"
	else
		printf '%q %s\n' "$FXCSIM" "$args"
	fi
}

TASKS=$(mktemp)
for bid in "${BATCHES[@]}"; do
	if [ "${#TONES[@]}" -eq 0 ]; then
		fxc fxcorr-sim common "$bid" "$WORKDIR"
	fi
	for i in "${!DSTATION[@]}"; do
		st=${DSTATION[$i]}
		out="raw/$st/${st}_${bid}.vdif"
		if [ -s "$WORKDIR/$out" ]; then
			echo "make_testdata.sh: skip existing $out" >&2
		else
			stationcmd "$bid" "$st" >> "$TASKS"
		fi
	done
done
# 任务并行执行；任一失败 xargs 退出 123 → set -e 终止脚本。
# 空列表时跳过（GNU xargs 空输入仍会执行一次命令，BSD 不会，跨平台显式挡）
if [ -s "$TASKS" ]; then
	xargs -0 -P "$PAR" -n 1 bash -c < <(tr '\n' '\0' < "$TASKS")
fi

# ---- ⑤ 软链 <DATA TABLE 文件名> → 最后 batch 的 VDIF ----
# .input 的 DATA TABLE 每个 datastream 只有一个文件名，软链只能指向一个 batch；
# run_batch.sh 跑每 batch 前会重做软链
last=${BATCHES[-1]}
for i in "${!DSTATION[@]}"; do
	ln -sf "raw/${DSTATION[$i]}/${DSTATION[$i]}_${last}.vdif" "$WORKDIR/${DSFILE[$i]}"
done
if [ "${#BATCHES[@]}" -gt 1 ]; then
	echo "make_testdata.sh: DATA TABLE links point at batch $last (last of ${#BATCHES[@]})" >&2
fi

# ---- ⑥ stdout 打印 batch_id ----
printf '%s\n' "${BATCHES[@]}"
