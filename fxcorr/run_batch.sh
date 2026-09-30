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
#             组数由 fxinput.py 从 .input 的 BASELINE TABLE 推导（与程序内
#             deriveDsGroups 同规则，两处必须同改）。不设 = 现行行为。
#
#   FXCORR_PARALLEL=N（P3，按 ds 组并行调度）：把调度单元下沉到 **(batch, ds
#             组)**——同时最多 N 个组在跑，每组内部先并行跑完该组的全部 f
#             任务（FXCORR_GROUP_JOBS 个同时），再跑该组的 x 分片，最后按
#             FXCORR_PURGE_FENGINE 决定是否删掉该组 fengine。**组数 N 由配置
#             推导、不是拍脑袋**：N = min(tmpfs ÷ 单组 fengine, 可用核 ÷ 每组
#             任务数, 带宽 ÷ 单任务读速率 ÷ 每组任务数)，见 v7-plan 10.1。
#             **设了它就是分片路径**（自带 FXCORR_X_SHARD 的语义），末了仍做
#             batch 级 merge；与 FXCORR_X_SHARD 同时设时不冲突，只是后者被
#             吸收。不设 = 现行行为（逐站串行 f，x 全量或按 FXCORR_X_SHARD）。
#   FXCORR_GROUP_JOBS=M（P3，默认 1）：组内同时跑几个 f 任务。矩阵 A 用 8
#             （4 站 × 2 ds 一次铺开、OMP_NUM_THREADS=1），矩阵 E 用 1（组内
#             串行、每个 f 任务 OMP_NUM_THREADS=8），两者在 3 组时都占 24 核。
#   FXCORR_PURGE_FENGINE=1（P3）：**该组用完即删，无论成败**（2026-09-30 起
#             ——原先只在 x 分片成功后删，失败路径"保留现场"）。**fengine 落
#             tmpfs 时这是硬需求而不是优化**——62 GB 只装得下 3 组，不删上一
#             波下一波就进不来（v7-plan 10.1）。失败路径不删的代价实测是**连
#             锁**：失败组的 fengine 注定无人消费（x 已跳过），却仍占着 16.8 GB
#             把后续组挤爆；而"留现场"的收益本就低——f 只依赖 raw，重跑即可再
#             生，日志（含 FEngineWriter 的写失败原因）都在 meta/logs/<batch_id>/。
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

# ---- P3 并行度参数：**校验必须在任何副作用之前** ----
# 放在这里而不是调度分支里：那边已经在 mark_status running 之后，参数写错会把
# batch 卡在 running 上（status 改了、活没干），重跑还得先手工改回来。
if [ -n "${FXCORR_PARALLEL:-}" ]; then
	PARALLEL=$FXCORR_PARALLEL
	GROUP_JOBS=${FXCORR_GROUP_JOBS:-1}
	PURGE=${FXCORR_PURGE_FENGINE:-0}
	case "$PARALLEL" in
		''|*[!0-9]*) echo "run_batch.sh: FXCORR_PARALLEL must be a positive integer (got '$PARALLEL')" >&2; exit 2 ;;
	esac
	[ "$PARALLEL" -ge 1 ] || { echo "run_batch.sh: FXCORR_PARALLEL must be >= 1 (got $PARALLEL)" >&2; exit 2; }
	case "$GROUP_JOBS" in
		''|*[!0-9]*) echo "run_batch.sh: FXCORR_GROUP_JOBS must be a positive integer (got '$GROUP_JOBS')" >&2; exit 2 ;;
	esac
	[ "$GROUP_JOBS" -ge 1 ] || { echo "run_batch.sh: FXCORR_GROUP_JOBS must be >= 1 (got $GROUP_JOBS)" >&2; exit 2; }
fi

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
# 按组调度正消费它（DSGROUP 列），FXCORR_PARALLEL 未设时只取前两列。
declare -a DSTATION	# "<station> <站内 ds 序号> <ds 组号>"
while read -r st di g fn; do
	DSTATION+=("$st $di $g")
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
# ---- P3 并行调度的三个辅助（FXCORR_PARALLEL 未设时不参与执行）----

# 等到池里有空位：**bash 4.2 没有 wait -n**（目标集群实测 4.2.46），而
# `${#arr[@]}` / `"${arr[@]}"` 对空数组在 set -u 下会报 unbound。作业表绕开两者——
# `jobs -rp` 只列**正在运行**的后台作业（已完成但未 wait 的不计入），正好是"当前
# 有几个任务在跑"；组函数在后台子 shell 里执行，所以组间、组内两层池各看各自的
# 作业表，互不干扰。
pool_wait()
{
	while [ "$(jobs -rp | wc -l)" -ge "$1" ]; do
		sleep 1
	done
}

# 删掉一个 ds 组的 fengine（只在它的 x 分片成功之后调用）。fengine 落 tmpfs 时
# 容量是硬墙：不删上一波，下一波就进不来（v7-plan 10.1）。失败路径刻意不调用。
purge_group()
{
	local t st di
	local root=${FXCORR_ROOT_FENGINE:?}
	for t in ${GROUP_TASKS[$1]:-}; do
		st=${t%%:*}
		di=${t#*:}
		rm -rf "$root/$BID/$st/ds_$di"
	done
}

# 跑一个 ds 组：组内 f 并行 → 该组 x 分片 → 按开关 purge。在后台子 shell 里调用
# （run_group N &），退出码 0 = 该组完整成功。组内顺序固定（.input 的站序），
# 并行只改变"谁先跑"，不改变任何任务的输入。
run_group()
{
	local g=$1 t st di
	local failed=""
	local rc=0
	local -a pids tasks
	local n=0 i

	for t in ${GROUP_TASKS[$g]:-}; do
		st=${t%%:*}
		di=${t#*:}
		pool_wait "$GROUP_JOBS"
		fxc fxcorr-f "$BID" "$st" "$WORKDIR" "$di" > "$LOGDIR/f-$st-ds$di.log" 2>&1 &
		pids[$n]=$!
		tasks[$n]="$st:$di"
		n=$((n + 1))
	done

	# **失败语义（P3 起）**：不再是"任一失败即停、不跑后续"，而是**跑完该组
	# 全部任务再判**——并行下"第一个失败"的位置本身不携带信息，要点名才有用。
	# `wait <pid>` 在作业结束后仍返回它的退出码，所以可以逐个回收。
	for ((i = 0; i < n; i++)); do
		if wait "${pids[$i]}"; then
			continue
		fi
		st=${tasks[$i]%%:*}
		di=${tasks[$i]#*:}
		echo "run_batch.sh: fxcorr-f failed for station $st ds$di (batch $BID, ds group $g); log: meta/logs/$BID/f-$st-ds$di.log" >&2
		failed="$failed $st:ds$di"
	done

	if [ -n "$failed" ]; then
		# 本组数据不全，x 分片没有意义：不跑
		echo "run_batch.sh: ds group $g has failed f task(s):$failed; its x shard is skipped" >&2
		rc=1
	elif ! fxc fxcorr-x "$BID" "$WORKDIR" "$g" > "$LOGDIR/x-g$g.log" 2>&1; then
		echo "run_batch.sh: fxcorr-x shard $g failed for batch $BID; log: meta/logs/$BID/x-g$g.log" >&2
		rc=1
	fi

	# **purge 不看成败（2026-09-30 定）**：`FXCORR_PURGE_FENGINE=1` 的语义是"该组
	# 用完即删"，不是"成功才删"。失败路径原先刻意保留现场，实际代价是**连锁**——
	# 失败组的 fengine 注定无人消费（x 已跳过），却继续占着 tmpfs；一组 16.8 GB，
	# 62 GB 的 tmpfs 少一组就少跑一组，实测把整批拖垮（x 全失败 → 残留 48 GB →
	# 后续组写爆）。而"留现场"的收益本来就很低：f 只依赖 raw、不依赖别的中间产物，
	# **重跑 f 就能再生**，且日志（含 FEngineWriter 的写失败原因）都在
	# meta/logs/$BID/ 里，不随 purge 消失。
	if [ "$PURGE" = "1" ]; then
		purge_group "$g"
	fi
	return $rc
}

# ---- 根记录与实验级一致性检查（Q18、Q4）：开跑前先挡不一致，再记下本 batch 的根 ----
# 检查先于写入：两者的 roots.json 都在 meta/roots/ 下，写过的不能再当"已有记录"
fxcorr_check_roots "$BID"
fxcorr_write_roots "$BID"

mark_status running

# 逐任务日志目录（P3）：并行后各任务的 stdout/stderr 交错，不分离就没法定位
LOGDIR="$WORKDIR/meta/logs/$BID"

if [ -n "${FXCORR_PARALLEL:-}" ]; then
	# ========= P3：按 ds 组并行调度（计算单元 = (batch, ds 组)）=========
	# PARALLEL / GROUP_JOBS / PURGE 已在前面校验过（那段必须早于任何副作用）
	mkdir -p "$LOGDIR"

	# 组号 → 该组的 f 任务（"<station>:<站内 ds 序号>"），组内保持 .input 的站序。
	# 组号来自 fxinput.py，与 fxcorr-x 分片模式的组号同一条规则。
	declare -A GROUP_TASKS
	for entry in "${DSTATION[@]}"; do
		st=${entry%% *}
		rest=${entry#* }
		di=${rest%% *}
		grp=${rest#* }
		GROUP_TASKS[$grp]="${GROUP_TASKS[$grp]:-} $st:$di"
	done

	echo "run_batch.sh: parallel mode, $NGRP ds group(s), FXCORR_PARALLEL=$PARALLEL FXCORR_GROUP_JOBS=$GROUP_JOBS FXCORR_PURGE_FENGINE=$PURGE" >&2

	# 组间由池限制同时在跑的个数；组内部再由 run_group 的池限制任务数。
	# 组的启动顺序 = 组号顺序，运行顺序取决于谁先空出来（这正是并行的意义）。
	declare -a gpids
	grp=0
	while [ "$grp" -lt "$NGRP" ]; do
		pool_wait "$PARALLEL"
		run_group "$grp" &
		gpids[$grp]=$!
		grp=$((grp + 1))
	done

	# 全部组跑完再判——与组内同一套失败语义
	gfail=""
	for ((grp = 0; grp < NGRP; grp++)); do
		if ! wait "${gpids[$grp]}"; then
			gfail="$gfail $grp"
		fi
	done
	if [ -n "$gfail" ]; then
		echo "run_batch.sh: batch $BID failed in ds group(s):$gfail (logs in meta/logs/$BID)" >&2
		mark_status failed
		exit 1
	fi

	# batch 级 merge：本 batch 各组的 ds<G>.part → merged.part。**不写 SWIN**：SWIN 的
	# 唯一写入者是实验级的 `fxcorr-x merge --experiment`（data-spec 5.9 末条）。
	if ! fxc fxcorr-x merge "$BID" "$WORKDIR" > "$LOGDIR/merge.log" 2>&1; then
		echo "run_batch.sh: fxcorr-x merge failed for batch $BID; log: meta/logs/$BID/merge.log" >&2
		mark_status failed
		exit 1
	fi
else
	# 逐 datastream fxcorr-f（多 datastream 站每流一个 f 任务，带站内序号）：
	# 任一失败 → status=failed、非 0 退出，不跑后续站（规格③④）
	for entry in "${DSTATION[@]}"; do
		st=${entry%% *}
		rest=${entry#* }
		di=${rest%% *}
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
	# 组数由 fxinput.py 从 .input 推导（与程序内同一规则）。此路径不并发，失败语义
	# 维持原样（任一失败即停），P3 起只在 FXCORR_PARALLEL 那条路径上改成"跑完再判"。
	if [ "${FXCORR_X_SHARD:-0}" = "1" ]; then
		echo "run_batch.sh: shard mode, $NGRP ds group(s)" >&2
		grp=0
		while [ "$grp" -lt "$NGRP" ]; do
			if ! fxc fxcorr-x "$BID" "$WORKDIR" "$grp"; then
				echo "run_batch.sh: fxcorr-x shard $grp failed for batch $BID" >&2
				mark_status failed
				exit 1
			fi
			grp=$((grp + 1))
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
fi

mark_status done
if [ -n "${FXCORR_PARALLEL:-}" ]; then
	echo "run_batch.sh: batch $BID done, $NGRP ds group(s) merged into vis-parts/$BID/merged.part"
elif [ "${FXCORR_X_SHARD:-0}" = "1" ]; then
	echo "run_batch.sh: batch $BID done, shards merged into vis-parts/$BID/merged.part"
else
	echo "run_batch.sh: batch $BID done, SWIN in $(basename "$OUTDIR")"
fi
