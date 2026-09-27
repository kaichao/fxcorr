#!/usr/bin/env bash
# run_batch.sh —— fxcorr 流水线（规格见 fxcorr/v1-plan.md 2.4）
#
# 步骤：① 读 batches/<batch_id>.json + .input，前置校验对齐（batch 起点 subint
# 边界、batch 时长 INT TIME 整数倍、INT TIME 为 subint 整数倍，容差同 fxcorr-f/x
# 程序内校验；不通过直接报错退出，不依赖工具兜底）
# → ② DATA TABLE 软链重指本 batch 的 VDIF（make_testdata.sh 多 batch 时软链停在
# 最后 batch，跑其他 batch 前必须重做；raw 数据不存在即报错）
# → ③ 置 status=running（batch.json status 字段）→ ④ 逐站 fxcorr-f（任一失败 →
# status=failed、非 0 退出，不跑后续站）→ ⑤ mkdir .input OUTPUT FILENAME 所在目录
# → ⑥ fxcorr-x（失败同 ④）→ ⑦ 成功 → status=done，追加 meta/batches.index 一行
# <batch_id>,done,<时间戳>。
#
# 用法：./run_batch.sh <batch_id> [workdir]
#   batch_id  批量标识（batches/<batch_id>.json 须已写好，可用 make_testdata.sh 生成）
#   workdir   项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
#   环境变量：FXCORR_X_SHARD=1 走分片路径——逐 ds 组跑 fxcorr-x（各写
#             vis-parts/<bid>/ds<G>.part），再由 fxcorr-x merge 归并写出 SWIN。
#             组数由本脚本从 .input 的 BASELINE TABLE 推导（与程序内
#             deriveDsGroups 同规则，两处必须同改）。不设 = 现行行为。
set -euo pipefail

# 五个可重定向的根（V5 P5）：容器透传与解析共用一份清单
FXCORR_ROOT_VARS=(FXCORR_RAW_ROOT FXCORR_SIM_COMMON_ROOT FXCORR_FENGINE_ROOT FXCORR_VIS_ROOT FXCORR_PRODUCT_ROOT)

# 容器模式开关：FXCORR_RUN_MODE=container 时工具经 docker run 调用（见下方 fxc）
FXCORR_RUN_MODE="${FXCORR_RUN_MODE:-host}"

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# 目录根解析（V5 P5）：与 fxcorr-f/x/sim 内的 FxcorrPath 同规则，一处定义两处用
. "$SCRIPTDIR/roots.sh"
if [ "$FXCORR_RUN_MODE" != "container" ] && ! command -v fxcorr-f >/dev/null 2>&1 && [ -f "$SCRIPTDIR/../setup.bash" ]; then
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
	local img=fxcorr/fxcorr
	local envargs=()
	[ -n "${FXSIM_NOISE+x}" ] && envargs+=(-e FXSIM_NOISE="$FXSIM_NOISE")
	[ -n "${FXSIM_SEED+x}" ] && envargs+=(-e FXSIM_SEED="$FXSIM_SEED")
	# 五个根一并透传（Q12）：容器内程序仍要知道用哪个根。挂载由外部编排平台
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
用法：./run_batch.sh <batch_id> [workdir]
  batch_id  批量标识（batches/<batch_id>.json 须已写好，可用 make_testdata.sh 生成）
  workdir   项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
EOF
	exit "${1:-2}"
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage
BID=$1
shift
WORKDIR="${FXCORR_WORKDIR:-.}"
if [ $# -gt 0 ] && [ -d "$1" ]; then
	WORKDIR=$1	# 位置参数优先于环境变量
	shift
fi
[ $# -eq 0 ] || usage
WORKDIR=$(cd "$WORKDIR" && pwd)
fxcorr_roots "$WORKDIR"
WORKDIR=$FXCORR_ROOT_WORKDIR
if [ ! -f "$WORKDIR/batches/$BID.json" ]; then
	echo "run_batch.sh: $WORKDIR/batches/$BID.json not found" >&2
	exit 2
fi
if [ "$FXCORR_RUN_MODE" = "container" ]; then
	command -v docker >/dev/null 2>&1 || { echo "run_batch.sh: docker not found (FXCORR_RUN_MODE=container)" >&2; exit 2; }
else
	command -v fxcorr-f >/dev/null 2>&1 || { echo "run_batch.sh: fxcorr-f not found (source setup.bash or install)" >&2; exit 2; }
	command -v fxcorr-x >/dev/null 2>&1 || { echo "run_batch.sh: fxcorr-x not found (source setup.bash or install)" >&2; exit 2; }
fi
mkdir -p "$WORKDIR/meta"
fxcorr_mkroots	# Q19：五个根由编排层建齐，程序遇根不存在只报错

# ---- ①② 读 batch.json + .input、前置校验、DATA TABLE 软链重做 ----
OUT=$(mktemp)
trap 'rm -f "${OUT:-}"' EXIT
python3 - "$WORKDIR" "$BID" "$FXCORR_ROOT_RAW" "$FXCORR_ROOT_VIS" <<'PYEOF' > "$OUT"
import json, math, os, re, sys

workdir, bid, rawroot, visroot = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
bjpath = os.path.join(workdir, 'batches', bid + '.json')
bj = json.load(open(bjpath))
cfgrel = bj['config_file']
cfg = os.path.join(workdir, cfgrel)
if not os.path.exists(cfg):
    sys.exit('run_batch.sh: config_file %s not found' % cfgrel)
text = open(cfg).read()

def one(key):
    m = re.search(r'^%s:\s*(.*)$' % re.escape(key), text, re.M)
    if not m:
        sys.exit('run_batch.sh: %s not found in %s' % (key, cfgrel))
    return m.group(1).strip()

# ① 前置校验（与 fxcorr-f/x 程序内校验同语义，提前挡）：
#   - batch 起点在 subint 边界（1µs 容差，同 fxcorr-f main.cpp:175-182，
#     吸收 start_mjd 的 f64 表示误差）
#   - batch 时长为 INT TIME 整数倍（跨 batch 追加不碎片化，data-spec 12）
#   - INT TIME 为 subint 整数倍（fxcorr-x 的 offsetnsperintegration==0 硬校验）
mjd0 = int(one('START MJD'))
sec0 = float(one('START SECONDS'))
subint = int(bj['subint_ns'])
inttime = float(bj['integration_sec'])
initsec = (bj['start_mjd'] - mjd0 - sec0 / 86400.0) * 86400.0
batchstartns = int(math.floor(initsec * 1.0e9 + 0.5))
rem = batchstartns % subint
if rem > 1000 and (subint - rem) > 1000:
    sys.exit('run_batch.sh: batch start is not on a subint boundary (%d ns into a %d ns subint)' % (rem, subint))
batchdur = int(bj['n_subints']) * subint / 1.0e9
nint = round(batchdur / inttime)
if abs(nint * inttime - batchdur) > 1e-6:
    sys.exit('run_batch.sh: batch duration %g is not an integer multiple of INT TIME %g' % (batchdur, inttime))
trem = inttime * 1.0e9 % subint
if min(trem, subint - trem) > 1.0:
    sys.exit('run_batch.sh: INT TIME %g is not a multiple of SUBINT %d ns' % (inttime, subint))

# f 任务展开按 .input datastream 序（TELESCOPE INDEX 逐个；多 datastream 站
# 重复出现，每 datastream 一个 f 任务带站内序号 dsidx）；batch.json 的
# stations 是去重元数据（data-spec 5.3），仅作交叉校验
telnames = re.findall(r'^TELESCOPE NAME \d+:\s*(\S+)\s*$', text, re.M)
ds_tel = re.findall(r'^TELESCOPE INDEX:\s*(\d+)\s*$', text, re.M)
stations = [telnames[int(t)] for t in ds_tel]
if bj.get('stations') is not None and set(bj['stations']) != set(stations):
    sys.exit('run_batch.sh: batch.json stations do not match .input datastreams')
dsidx = []
seen = {}
for st in stations:
    dsidx.append(seen.get(st, 0))
    seen[st] = dsidx[-1] + 1
datafiles = re.findall(r'^FILE \d+/\d+:\s*(\S+)\s*$', text, re.M)
if len(stations) != len(datafiles):
    sys.exit('run_batch.sh: station/file count mismatch in %s (%d stations, %d files)' % (cfgrel, len(stations), len(datafiles)))

# ② DATA TABLE 软链重指本 batch 的 VDIF（fxcorr-f 按 DATA TABLE 文件名读数据）。
# make_testdata.sh 布局（raw/<st>/<st>_<bid>.vdif）下软链重指本 batch，落在
# RAW 根下（Q2）；真实观测场景 FILE 行已是数据文件路径（绝对路径或直接可见），
# 不软链。
for st, fn in zip(stations, datafiles):
    src = os.path.join(rawroot, st, '%s_%s.vdif' % (st, bid))
    if os.path.isfile(src):
        tgt = os.path.join(rawroot, fn)
        if os.path.islink(tgt) or os.path.exists(tgt):
            os.unlink(tgt)
        # absolute target: a relative one breaks across filesystems, which is
        # the whole point of pointing the raw root at a different disk (Q2)
        os.symlink(src, tgt)

# OUTPUT FILENAME resolves against the vis root (Q20); an absolute value wins,
# os.path.join drops the prefix for it - same rule as the programs
# ds 组推导（分片模式用）：与 fxcorr-x 的 deriveDsGroups **同规则**——每条
# baseline 绑定一对 ds，把全部条目并查集合并，连通分量就是一组（覆盖同一频段
# 组的那些 ds）。两处实现没有编译器兜底，改一处要改两处（data-spec 5.9）。
blka = re.findall(r'^D/STREAM A INDEX \d+:\s*(\d+)\s*$', text, re.M)
blkb = re.findall(r'^D/STREAM B INDEX \d+:\s*(\d+)\s*$', text, re.M)
parent = list(range(len(stations)))
def _find(x):
    while parent[x] != x:
        parent[x] = parent[parent[x]]
        x = parent[x]
    return x
for xa, xb in zip(blka, blkb):
    ra, rb = _find(int(xa)), _find(int(xb))
    if ra < rb:
        parent[rb] = ra
    elif rb < ra:
        parent[ra] = rb
ngroups = len({_find(i) for i in range(len(stations))})

outdir = os.path.join(visroot, one('OUTPUT FILENAME').rstrip('/'))
print('CFGIN=%s' % cfgrel)
print('NGRP=%d' % ngroups)
print('OUTDIR=%s' % outdir)
print('--')
for st, di, fn in zip(stations, dsidx, datafiles):
    print('%s %s %s' % (st, di, fn))
PYEOF

CFGIN= OUTDIR= NGRP=1
while IFS= read -r line && [ "$line" != "--" ]; do
	[ -n "$line" ] || continue
	k=${line%%=*}
	v=${line#*=}
	case "$k" in
		CFGIN) CFGIN=$v ;;
		OUTDIR) OUTDIR=$v ;;
		NGRP) NGRP=$v ;;
	esac
done < "$OUT"
[ -n "$CFGIN" ] || { echo "run_batch.sh: failed to parse batch $BID" >&2; exit 2; }
declare -a DSTATION
while read -r st di fn; do
	DSTATION+=("$st $di")
done < <(awk 'f{print} /^--$/{f=1}' "$OUT")

# ---- ③④⑤⑥⑦ status 流转与逐站/基线执行 ----
# status 写回 batch.json（json.dump 保字段序与 make_testdata.sh 一致）；
# done 时顺带追加 meta/batches.index（规格⑥⑦）
mark_status()
{
	python3 -c '
import json, os, sys
from datetime import datetime
workdir, bid, st = sys.argv[1], sys.argv[2], sys.argv[3]
p = os.path.join(workdir, "batches", bid + ".json")
b = json.load(open(p))
b["status"] = st
with open(p, "w") as f:
	json.dump(b, f, indent=2)
	f.write("\n")
if st == "done":
	with open(os.path.join(workdir, "meta", "batches.index"), "a") as f:
		f.write("%s,done,%s\n" % (bid, datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")))
' "$WORKDIR" "$BID" "$1"
}
# ---- 根记录与实验级一致性检查（Q18、Q4）：开跑前先挡不一致，再记下本 batch 的根 ----
# 检查先于写入：两者的 roots.json 都在 meta/roots/ 下，写过的不能再当"已有记录"
fxcorr_check_roots "$BID"
fxcorr_write_roots "$BID"

mark_status running

# 逐 datastream fxcorr-f（多 datastream 站每流一个 f 任务，带站内序号）：
# 任一失败 → status=failed、非 0 退出，不跑后续站（规格③④）
for entry in "${DSTATION[@]}"; do
	st=${entry%% *}
	di=${entry#* }
	echo "run_batch.sh: fxcorr-f $BID $st ds$di" >&2
	if ! fxc fxcorr-f "$BID" "$st" "$WORKDIR" "$di"; then
		echo "run_batch.sh: fxcorr-f failed for station $st ds$di" >&2
		mark_status failed
		exit 1
	fi
done

mkdir -p "$OUTDIR"    # 规格⑤（fxcorr-x 自身也会建，先建无害）
# FXCORR_X_SHARD=1：按 ds 组分片（每片一个 x 任务），再由 merge 归并写出 SWIN。
# 分片任务不写 SWIN（D16 落 vis-parts/），**merge 是 SWIN 的唯一写入者**——写出
# 顺序必须时间单调，而分片各自追加必然时间回退（data-spec 5.9）。组数由上面
# 的 python 段从 .input 推导（与程序内同一规则）。
if [ "${FXCORR_X_SHARD:-0}" = "1" ]; then
	echo "run_batch.sh: shard mode, $NGRP ds group(s)" >&2
	g=0
	while [ "$g" -lt "$NGRP" ]; do
		if ! fxc fxcorr-x "$BID" "$WORKDIR" "$g"; then
			echo "run_batch.sh: fxcorr-x shard $g failed for batch $BID" >&2
			mark_status failed
			exit 1
		fi
		g=$((g + 1))
	done
	if ! fxc fxcorr-x merge "$BID" "$WORKDIR"; then
		echo "run_batch.sh: fxcorr-x merge failed for batch $BID" >&2
		mark_status failed
		exit 1
	fi
elif ! fxc fxcorr-x "$BID" "$WORKDIR"; then
	echo "run_batch.sh: fxcorr-x failed for batch $BID" >&2
	mark_status failed
	exit 1
fi

mark_status done
echo "run_batch.sh: batch $BID done, SWIN in $(basename "$OUTDIR")"
