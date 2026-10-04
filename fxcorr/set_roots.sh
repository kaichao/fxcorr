#!/usr/bin/env bash
# set_roots.sh —— 按当前环境变量重写 meta/roots/*.json（"记录改写"，本地计算模式用）
#
# 背景（data-spec 5.2.1）：根记录描述"这批数据用了哪套布局"，三个程序启动时与它
# 比对、不一致即报错停链。本地计算模式（app-fxcorr 的 FXCORR_ROUTE=1|2）把
# raw/fengine 指到节点本地：共享侧既有的记录若不改写，后续 batch 一跑就报
# `disagrees`（护栏生效，属于预期拦阻）。本脚本把记录重写成"当前环境变量解析出的
# 布局"，workdir 内**全部** batches/*.json 一次处理。
#
# **用法前提（务必）**：在**同一条命令的环境里**先 export 好本地根变量再跑：
#   FXCORR_RAW_ROOT=/tmp/fxcorr/raw FXCORR_FENGINE_ROOT=/tmp/fxcorr/fengine \
#       ./set_roots.sh /cluster_data_root/fxcorr/l4
# 不 export 就跑 = 按共享布局写回（等于什么都没改）。根一律用**直接映射写法**
# （/tmp/... 或 /dev/shm/...，不写 /local_data_root 前缀——容器内两者同指一处，
# 但记录按字符串比对，全链必须统一一种写法；见 data-spec 5.2.1）。
#
# 只应在**确实按该布局准备/搬运了数据**时执行（route=1 的 raw-copy、route=2 的
# 本地 sim 即由此成立）：本脚本不做任何校验，只是"按你给的环境变量写记录"。
#
# 用法：./set_roots.sh [workdir]      （workdir 默认 . 或 $FXCORR_WORKDIR）
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$SCRIPTDIR/roots.sh"

if [ $# -gt 1 ]; then
	echo "用法：./set_roots.sh [workdir]（workdir 默认 . 或 \$FXCORR_WORKDIR）" >&2
	exit 2
fi
WORKDIR="${FXCORR_WORKDIR:-.}"
if [ $# -ge 1 ]; then
	WORKDIR=$1
fi
[ -d "$WORKDIR" ] || { echo "set_roots.sh: workdir ${WORKDIR} not found" >&2; exit 2; }

fxcorr_roots "$WORKDIR"		# 解析（workdir 绝对化 + 四个根，规则见 roots.sh）
WORKDIR=$FXCORR_ROOT_WORKDIR

shopt -s nullglob
batches=("$WORKDIR"/batches/*.json)
if [ ${#batches[@]} -eq 0 ]; then
	echo "set_roots.sh: no batches/*.json under ${WORKDIR}" >&2
	exit 2
fi

for f in "${batches[@]}"; do
	bid=$(basename "$f" .json)
	fxcorr_write_roots "$bid" || {
		echo "set_roots.sh: write meta/roots/${bid}.json failed" >&2
		exit 2
	}
	echo "set_roots.sh: wrote meta/roots/${bid}.json"
done

# 打印写出的值供核对（R8：不 export 时静默写回共享布局——这里是唯一的确认点）
echo "set_roots.sh: workdir=${WORKDIR}"
printf '  %-8s %s\n' raw     "$(fxcorr_root_record "$FXCORR_ROOT_RAW" raw)"
printf '  %-8s %s\n' fengine "$(fxcorr_root_record "$FXCORR_ROOT_FENGINE" fengine)"
printf '  %-8s %s\n' vis     "$(fxcorr_root_record "$FXCORR_ROOT_VIS" vis)"
printf '  %-8s %s\n' product "$(fxcorr_root_record "$FXCORR_ROOT_PRODUCT" product)"
