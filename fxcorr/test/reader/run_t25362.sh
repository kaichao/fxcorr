#!/usr/bin/env bash
# run_t25362.sh —— 真实数据回归（v4-plan.md 阶段 D；背景见 reader-model.md 6.5）
#
# t25362 是目前唯一暴露过 A/B/C 三类缺陷的真实观测，也是唯一能验"filler 与缺口并存"
# 的数据：BA 的 ds_2 有 1162 帧 filler + 143 帧缺口，ds_0 是无 filler 的对照（145 帧
# 缺口）。它**不在仓库里**——数据在 `ssh difx` 那台的 /mnt/VGOS/，仓库同机在 ~/fxcorr，
# `make sync` 会把改动推过去——所以本脚本从**本地**驱动：
#
#   1. `make sync`（仓库根）+ difx 上编译安装 fxcorr-f
#   2. 两个 ds 各跑一遍（verbose），与**基线**逐字段比对 GAPCHECK summary
#   3. `check_reader.py` 对账（文件真值，绝对判据），两个 ds 都必须零 finding
#
# 判据是双重的，两层各管一件事：
#   * 基线锁"账没变"——它是修复完成时的实测值（2026-09-19，B2 与 C 的三次重构后
#     逐字节不变），任何 reader 改动都该先解释它为什么不该动；
#   * 真值对账锁"位置对"——不依赖任何基准的绝对判据（E1–E4），基线相同而位置错了
#     的情况它才抓得到。
#
# 用法：./run_t25362.sh [--no-sync] [--no-build] [--no-run] [host]
#   --no-run  不重跑 fxcorr-f，直接对账 host 上现成的 /tmp/t25362_ds<N>.log
#             （判据自身的自检用它：篡改日志后必须报红）
# 前置：本地能 `ssh difx`；difx 上有 t25362 数据与 ~/fxcorr 仓库。
set -uo pipefail

HOST=difx
DO_SYNC=1
DO_BUILD=1
DO_RUN=1
for arg in "$@"; do
	case "$arg" in
	--no-sync)  DO_SYNC=0 ;;
	--no-build) DO_BUILD=0 ;;
	--no-run)   DO_RUN=0 ;;
	-*) echo "用法: $0 [--no-sync] [--no-build] [--no-run] [host]" >&2; exit 2 ;;
	*)  HOST="$arg" ;;
	esac
done

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
REMOTE_WORKDIR=/data/scalebox/t25362work
BATCH=61037_24611
STATION=BA
FPS=16000
DATA=/mnt/VGOS/DiFX_test_data/t25362/test_run/data

# 基线：修复完成时的实测值（B2 之后的每一次重构都必须逐项相同）
summary_baseline() {
	case "$1" in
	2) echo "buffers 2181 frames 180892 discontinuities 8 missing frames 143 filler frames 1162 boundaries 2180 step range -18..0" ;;
	0) echo "buffers 2181 frames 181023 discontinuities 8 missing frames 145 filler frames 0 boundaries 2180 step range -72..0" ;;
	*) echo "" ;;
	esac
}
vdif_file() {
	case "$1" in
	0) echo "$DATA/t25362_ba_362-0650a_1.vdif" ;;
	2) echo "$DATA/t25362_ba_362-0650a_3.vdif" ;;
	*) echo "" ;;
	esac
}

FAIL=0
ok()   { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

echo "=== t25362 真实数据回归（host=$HOST, repo=${REPO_ROOT}）==="

if [ "$DO_SYNC" = 1 ]; then
	echo "--- make sync（仓库根，推送到 fxcorr 与 difx 两台）---"
	(cd "$REPO_ROOT" && make sync) >/dev/null 2>&1 || { echo "make sync 失败" >&2; exit 1; }
fi

if [ "$DO_BUILD" = 1 ]; then
	echo "--- difx 上编译安装 fxcorr-f ---"
	ssh "$HOST" 'cd ~/fxcorr/applications/fxcorr-f && bash -c "source ~/fxcorr/setup.bash && make -j8" >/tmp/t25362_build.log 2>&1 && sudo -n make install >/dev/null 2>&1' \
		|| { echo "difx 上构建失败（见 /tmp/t25362_build.log）" >&2; exit 1; }
fi

for ds in 2 0; do
	role=$([ "$ds" = 2 ] && echo "有 filler" || echo "无 filler 对照")
	echo "--- ds_${ds}（${role}）---"
	log=/tmp/t25362_ds$ds.log
	vdif=$(vdif_file "$ds")

	if [ "$DO_RUN" = 1 ]; then
		ssh "$HOST" "cd $REMOTE_WORKDIR && LD_LIBRARY_PATH=/usr/local/difx/lib FXCORR_LOGLEVEL=verbose /usr/local/difx/bin/fxcorr-f $BATCH $STATION . $ds > $log 2>&1" \
			|| { bad "ds_$ds 运行失败（见 $HOST:${log}）"; continue; }
	else
		ssh "$HOST" "test -s $log" || { bad "ds_$ds 的日志不存在（$HOST:$log），去掉 --no-run"; continue; }
	fi

	got=$(ssh "$HOST" "grep -h 'GAPCHECK summary' $log" | sed 's/^GAPCHECK summary: //' | tr -s ' ')
	want=$(summary_baseline "$ds")
	if [ "$got" = "$want" ]; then
		ok "ds_$ds GAPCHECK summary 与基线逐字段相同"
	else
		bad "ds_$ds GAPCHECK summary 变了"
		echo "        期望: $want"
		echo "        实测: $got"
	fi

	# 真值对账：E1–E4（E5 在真实数据上按设计跳过——VDIF 秒与 batch.json 的 start_mjd 不同源）
	out=$(ssh "$HOST" "python3 ~/fxcorr/fxcorr/test/reader/check_reader.py --log $log --vdif $vdif --fps $FPS --batch-json $REMOTE_WORKDIR/batches/$BATCH.json" 2>&1)
	rc=$?
	line=$(echo "$out" | grep -E '^summary' | tail -1)
	cov=$(echo "$out" | grep -E '^E4 coverage' | tail -1)
	echo "        $line"
	echo "        $cov"
	if [ "$rc" = 0 ] && echo "$line" | grep -q '0 with a finding' && echo "$cov" | grep -q '^E4 coverage : 0 data frames'; then
		ok "ds_$ds 真值对账零 finding、E4 零净损失"
	else
		bad "ds_$ds 真值对账有 finding（退出码 ${rc}）"
		echo "$out" | grep -E 'finding|verdict|E4 coverage|E3 |missing' | head -10 | sed 's/^/        /'
	fi
done

echo "=== $([ "$FAIL" = 0 ] && echo '全部通过' || echo "$FAIL 项失败") ==="
exit $([ "$FAIL" = 0 ] && echo 0 || echo 1)
