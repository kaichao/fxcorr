#!/usr/bin/env bash
# bench_pair.sh —— v9 两链工具级标定 runner（fxcorr/v9-plan.md 阶段 3，判据 8）
#
# 在同一个 workdir / batch 上先后跑 v8 链（fxcorr-f 逐站逐 ds + fxcorr-x 逐组）
# 与 v9 链（fxcorr-fp + fxcorr-xp），采集每个任务的 WALL / 峰值 RSS 与
# FXCORR_TIMING=1 的分段计时，落 <outdir>/ 下。**破坏性**：每链开跑前清掉
# fengine / vis-parts / vis —— 只在专用于标定的 workdir 上跑。
#
# 用法：
#   bench_pair.sh <workdir> <batch_id> [outdir]
#   env: OMP_NUM_THREADS   两链共用的线程数（默认 1；xp 的站级 ds 并行 + XMAC
#                          共用这个域——线程数扫描就扫它）
#        留空环境时 fp/f/x 串行、xp 按 OMP 默认
#
# 站/ds 列表从 raw/<station>/<station>_<batch>[_ds<N>].vdif 推导；
# 组数从 v8 fxcorr-x 的 FXCORR_X_GROUPS_ONLY=1 真值出口推导（两工具同实现）。
#
# 产出：times-v8.txt / timing-v8.txt / times-v9.txt / timing-v9.txt
#       （每次运行的追加行；重复跑同一 outdir 会累积）
set -u

WD=${1:?usage: bench_pair.sh <workdir> <batch_id> [outdir]}
BATCH=${2:?usage: bench_pair.sh <workdir> <batch_id> [outdir]}
OUT=${3:-$WD/bench-$BATCH}
OMP=${OMP_NUM_THREADS:-1}

mkdir -p "$OUT"
cd "$WD" || exit 1

# ---- 站/ds 列表（raw 目录推导） ----
STATIONS=()
declare -a JOBS=()   # "station ds" 对
for st in $(ls raw 2>/dev/null); do
	[ -d "raw/$st" ] || continue
	found=0
	for f in raw/$st/${st}_${BATCH}*.vdif; do
		[ -e "$f" ] || continue
		base=$(basename "$f" .vdif)
		if [ "$base" = "${st}_${BATCH}" ]; then   # 无 _ds 后缀 = 单 ds
			JOBS+=("$st 0")
			found=1
		else
			case "$base" in
				*_ds*) JOBS+=("$st ${base##*_ds}"); found=1 ;;
			esac
		fi
	done
	[ "$found" = 1 ] && STATIONS+=("$st")
done
if [ ${#JOBS[@]} -eq 0 ]; then
	echo "bench_pair: no raw files for batch $BATCH under $WD/raw" >&2
	exit 1
fi

# ---- 组数（v8 fxcorr-x 的无副作用真值出口；FXLOG 走 stderr，须 2>&1 合并） ----
NGROUPS=$(FXCORR_X_GROUPS_ONLY=1 fxcorr-x "$BATCH" "$WD" 0 2>&1 | grep -c "ds group")
if [ "$NGROUPS" -lt 1 ]; then
	echo "bench_pair: could not derive the ds group count (is fxcorr-x on PATH?)" >&2
	exit 1
fi

echo "bench_pair: batch $BATCH, ${#JOBS[@]} ds job(s) over ${#STATIONS[@]} station(s), $NGROUPS group(s), OMP=$OMP"
echo "bench_pair: output -> $OUT"

reset_products()
{
	rm -rf fengine vis-parts vis
	mkdir -p fengine vis-parts vis
}

# run_timed <tag> <label> <logfile> <cmd...>：跑一条命令，追加 WALL/RSS 与 TIMING 行
run_timed()
{
	tag=$1; label=$2; log=$3; shift 3
	/usr/bin/time -f "$label wall=%e rss=%MkB" -o "$OUT/times-$tag.txt" -a \
		env OMP_NUM_THREADS=$OMP FXCORR_TIMING=1 "$@" >"$OUT/$log" 2>&1
	rc=$?
	grep "^TIMING" "$OUT/$log" >>"$OUT/timing-$tag.txt" 2>/dev/null
	if [ $rc -ne 0 ]; then
		echo "bench_pair: FAILED ($rc): $label -- see $OUT/$log" >&2
		tail -3 "$OUT/$log" >&2
		exit 1
	fi
}

# ---- v8 链 ----
echo "bench_pair: v8 chain (fxcorr-f x${#JOBS[@]}, fxcorr-x x$NGROUPS)"
reset_products
for job in "${JOBS[@]}"; do
	set -- $job
	run_timed v8 "f $1 ds$2" "f-$1-$2.log" fxcorr-f "$BATCH" "$1" "$WD" "$2"
done
for g in $(seq 0 $((NGROUPS-1))); do
	run_timed v8 "x grp$g" "x-grp$g.log" fxcorr-x "$BATCH" "$WD" "$g"
done
du -sb --apparent-size fengine 2>/dev/null | cut -f1 >"$OUT/fengine-v8.txt"

# ---- v9 链 ----
echo "bench_pair: v9 chain (fxcorr-fp x${#JOBS[@]}, fxcorr-xp x$NGROUPS)"
reset_products
for job in "${JOBS[@]}"; do
	set -- $job
	run_timed v9 "fp $1 ds$2" "fp-$1-$2.log" fxcorr-fp "$BATCH" "$1" "$WD" "$2"
done
for g in $(seq 0 $((NGROUPS-1))); do
	run_timed v9 "xp grp$g" "xp-grp$g.log" fxcorr-xp "$BATCH" "$WD" "$g"
done
du -sb --apparent-size fengine 2>/dev/null | cut -f1 >"$OUT/fengine-v9.txt"

echo "bench_pair: done. walls:"
echo "--- v8"; tail -n $(( ${#JOBS[@]} + NGROUPS )) "$OUT/times-v8.txt"
echo "--- v9"; tail -n $(( ${#JOBS[@]} + NGROUPS )) "$OUT/times-v9.txt"
echo "fengine bytes: v8=$(cat "$OUT/fengine-v8.txt") v9=$(cat "$OUT/fengine-v9.txt")"
