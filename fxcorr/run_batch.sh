#!/usr/bin/env bash
# run_batch.sh —— fxcorr 流水线（规格见 fxcorr/v1-plan.md 2.4）
#
# 步骤：① 读 batches/<batch_id>.json + .input，前置校验对齐（batch 起点 subint
# 边界、batch 时长 INT TIME 整数倍、INT TIME 为 subint 整数倍，容差同 fxcorr-f/x
# 程序内校验；不通过直接报错退出，不依赖工具兜底）
# → ② DATA TABLE 软链重指本 batch 的 VDIF（make_testdata.sh 多 batch 时软链停在
# 最后 batch，跑其他 batch 前必须重做；raw 数据不存在即报错）
#
# ①② 的实现都在 fxinput.py（2026-09-29 抽出，V7 P3 前）——解析 .input、前置校验、
# 软链、ds 组划分都在那里，本脚本只剩编排。
# → ③ 置 status=running（batch.json status 字段）→ ④ 逐站 fxcorr-f（任一失败 →
# status=failed、非 0 退出，不跑后续站）→ ⑤ mkdir .input OUTPUT FILENAME 所在目录
# → ⑥ fxcorr-x（失败同 ④）→ ⑦ 成功 → status=done，追加 meta/batches.index 一行
# <batch_id>,done,<时间戳>。
#
# 用法：./run_batch.sh <batch_id> [workdir]
#   batch_id  批量标识（batches/<batch_id>.json 须已写好，可用 make_testdata.sh 生成）
#   workdir   项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
#   环境变量：FXCORR_X_SHARD=1 走分片路径——逐 ds 组跑 fxcorr-x（各写
#             vis-parts/<bid>/ds<G>.part），再由 **batch 级** merge 归并成
#             vis-parts/<bid>/merged.part（**不写 SWIN**：SWIN 由实验级的
#             `fxcorr-x merge --experiment` 单点写出，那是实验级操作、不在本
#             脚本里——见 data-spec 5.9 末条）。
#             组数由本脚本从 .input 的 BASELINE TABLE 推导（与程序内
#             deriveDsGroups 同规则，两处必须同改）。不设 = 现行行为。
set -euo pipefail

# 四个可重定向的根（V5 P5）：容器透传与解析共用一份清单
FXCORR_ROOT_VARS=(FXCORR_RAW_ROOT FXCORR_FENGINE_ROOT FXCORR_VIS_ROOT FXCORR_PRODUCT_ROOT)

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
fxcorr_mkroots	# Q19：四个根由编排层建齐，程序遇根不存在只报错

# ---- ①② 读 batch.json + .input、前置校验、DATA TABLE 软链重做 ----
OUT=$(mktemp)
trap 'rm -f "${OUT:-}"' EXIT
# 解析 .input、前置校验、DATA TABLE 软链、ds 组推导**都在 fxinput.py 里**：
# 那是"逻辑"而不是"编排"，且其中的 ds 组划分与 fxcorr-x 的 C++ 侧
# deriveDsGroups 是同一条规则的两处实现（对照判据 test/input/run_consistency.sh），
# 藏在 heredoc 里既不能单测、也不能被别的脚本复用。失败时它自己打印
# `run_batch.sh:` 前缀的消息并以非 0 退出——与抽出前逐字一致，所以这里只让它
# 的退出码经 set -e 传出去，不再包一层。
python3 "$SCRIPTDIR/fxinput.py" prepare "$WORKDIR" "$BID" \
	"$FXCORR_ROOT_RAW" "$FXCORR_ROOT_VIS" > "$OUT"

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
# 站表：<station> <站内 ds 序号> <ds 组号> <数据文件>。组号由 fxinput.py 推出，与
# fxcorr-x 分片模式的组号一一对应（C++ 侧按 ds 序号打掩码，同一条规则）——P3 的
# 按组调度消费它，这里暂时只取前两列。
declare -a DSTATION
while read -r st di g fn; do
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
# FXCORR_X_SHARD=1：按 ds 组分片（每片一个 x 任务），再由 **batch 级** merge 归并成
# vis-parts/<bid>/merged.part。分片与 batch 级 merge 都**不写 SWIN**（D16 落
# vis-parts/）——分片各自追加 SWIN 必然时间回退、被 difx2fits 静默丢弃，所以 SWIN 的
# 写出收敛到实验级的 `fxcorr-x merge --experiment` 一次（形态 A，data-spec 5.9 末条）。
# 组数由上面的 python 段从 .input 推导（与程序内同一规则）。
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
if [ "${FXCORR_X_SHARD:-0}" = "1" ]; then
	echo "run_batch.sh: batch $BID done, shards merged into vis-parts/$BID/merged.part"
else
	echo "run_batch.sh: batch $BID done, SWIN in $(basename "$OUTDIR")"
fi
