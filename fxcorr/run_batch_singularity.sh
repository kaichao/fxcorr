#!/bin/bash
# run_batch_singularity.sh —— 用 Singularity 镜像跑单个 batch（V7 P6）
#
# 用法：./run_batch_singularity.sh <batch_id> <workdir>
#
# 要点（见 v7-plan.md §17）：
#  1. FXCORR_RUN_MODE=host —— 我们**已经在容器里**，run_batch.sh 的 fxc() 直调才是对的
#     （设成 container 会让它再套一层 docker run，那台机器没有 docker）
#  2. 源码目录要 bind 进去：**编排脚本以集群这份为准**。镜像里其实也有一份 run_batch.sh /
#     fxinput.py / make_testdata.sh（P3 起），但那是冻在构建时刻的快照——bind 之后改了脚本
#     不用重建 .sif。二进制则相反：取**镜像内**的，那才是"容器化"要验的对象（v7-plan §17.3）
#  3. 四个根**显式透传并 bind** —— 这是 V7 踩过的坑：缺省时 fengine 会落到共享存储
#  4. **不是路径的开关只透传、不 bind**（P3 起）：并行度三件套与 OMP_NUM_THREADS
#     是 run_batch.sh 在容器里要读的环境变量，漏掉白名单里的任何一个，它都会在
#     容器内**静默退回串行**——外面看着 yield 了参数，里面当没看见
set -euo pipefail

SIF=${FXCORR_SIF:-/public/home/cstu0036/fxcorr/singularity/fxcorr.sif}
SRC=${FXCORR_SRC:-/public/home/cstu0036/fxcorr/src}
BID=${1:?用法: $0 <batch_id> <workdir>}
WORKDIR=${2:?用法: $0 <batch_id> <workdir>}

MINIFORGE=${FXCORR_MINIFORGE:-/public/software/apps/miniforge3-25.3.1-0}
BINDS=(--bind "$WORKDIR:$WORKDIR" --bind "$SRC:$SRC" --bind "$MINIFORGE:$MINIFORGE")
# 镜像里没有 python3，而 run_batch.sh 的校验段要它（2026-09-29 实测）
ENVS=(--env "FXCORR_RUN_MODE=host" --env "PATH=$MINIFORGE/bin:/usr/local/difx/bin:/usr/local/bin:/usr/bin:/bin")
# 四个根是路径：既透传又 bind（--bind 的源路径必须先存在，否则 singularity 拒绝创建容器）
for v in FXCORR_RAW_ROOT FXCORR_FENGINE_ROOT FXCORR_VIS_ROOT FXCORR_PRODUCT_ROOT; do
	val=${!v:-}
	[ -n "$val" ] || continue
	mkdir -p "$val"
	ENVS+=(--env "$v=$val")
	BINDS+=(--bind "$val:$val")
done

# 其余开关不是路径，只透传。P3 的并行度必须在这里，否则容器内的 run_batch.sh
# 看不到 FXCORR_PARALLEL、照旧走串行路径（外面 yield 了参数也白搭）
for v in FXCORR_X_SHARD FXCORR_X_ALLOW_EMPTY FXCORR_LOGLEVEL \
         FXCORR_PARALLEL FXCORR_GROUP_JOBS FXCORR_PURGE_FENGINE OMP_NUM_THREADS; do
	val=${!v:-}
	[ -n "$val" ] || continue
	ENVS+=(--env "$v=$val")
done

echo "=== singularity: batch $BID ==="
echo "    镜像    : $SIF"
echo "    workdir : $WORKDIR"
for v in FXCORR_RAW_ROOT FXCORR_FENGINE_ROOT FXCORR_VIS_ROOT FXCORR_PRODUCT_ROOT; do
	printf "    %-22s = %s\n" "$v" "${!v:-（未设置 → 落 workdir）}"
done

exec singularity exec "${BINDS[@]}" "${ENVS[@]}" "$SIF" \
	bash "$SRC/fxcorr/run_batch.sh" "$BID" "$WORKDIR"
