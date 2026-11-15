#!/usr/bin/env bash
# sync_results.sh —— 把 difx 上的正式结果拉到本地仓库
#
# 详细 run 产物 -> results-detail/（gitignored）
# 跨 run 汇总   -> results/summary.tsv（进 git）
#
# 用法: sync_results.sh [host]
# 环境变量: XP_OPT_ROOT（远端结果根，默认 /data/scalebox/mydata/fxcorr/xp-opt）
set -euo pipefail

HOST=${1:-${XP_OPT_HOST:-difx}}
REMOTE_ROOT=${XP_OPT_ROOT:-/data/scalebox/mydata/fxcorr/xp-opt}
LOCAL_ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOCAL_DETAIL="$LOCAL_ROOT/results-detail"
LOCAL_SUMMARY="$LOCAL_ROOT/results/summary.tsv"

mkdir -p "$LOCAL_DETAIL" "$LOCAL_ROOT/results"
rsync -av "$HOST:$REMOTE_ROOT/results/" "$LOCAL_DETAIL/"
rsync -av "$HOST:$REMOTE_ROOT/results/summary.tsv" "$LOCAL_SUMMARY"
echo "已同步详细结果: $HOST:$REMOTE_ROOT/results/ -> $LOCAL_DETAIL/"
echo "已同步汇总表:   $HOST:$REMOTE_ROOT/results/summary.tsv -> $LOCAL_SUMMARY"
