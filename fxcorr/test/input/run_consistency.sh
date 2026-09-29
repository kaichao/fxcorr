#!/usr/bin/env bash
# run_consistency.sh —— ds 组划分一致性判据（V7 P3 前，2026-09-29 加）
#
# 同一套规则有两处实现：python 侧 `fxcorr/fxinput.py` 的 `derive_ds_groups`
# （**编排脚本**用——它决定"分几片、每片是哪些 ds"，也就是 P3 的并行调度粒度与
# tmpfs 容量账）与 C++ 侧 `applications/fxcorr-x/src/main.cpp` 的 `deriveDsGroups`
# （**x 分片真正按它**打 ds 掩码）。规则权威是 `data-spec.md` 第 8 节，但两处
# 之间**没有编译器兜底**——漂移了不会报错，只会静默错数据：脚本按 4 组调度、
# 程序按 5 组算，或者组数对得上而成员对不上，于是每片漏掉或多算几个 ds，
# **而产物看起来完全正常**。
#
# 与 `test/roots/run_consistency.sh` 是同一类判据（那里是 `roots.sh` vs
# `FxcorrPath`），只是这一对更险：根错了程序会报 roots 不一致，组划分错了没有
# 任何症状。
#
# 用法：./run_consistency.sh <workdir> [batch_id ...]
#   workdir   含 batches/*.json 与 config/ 的 workdir（.input 由 batch.json 的
#             config_file 指出）；不给 batch_id 时对该 workdir 下全部 batch 判
#   环境：fxcorr-x 必须在 PATH 里（或已 source setup.bash）。**不需要 raw /
#         fengine 数据**——真值出口在读任何数据之前就退出了，这正是
#         `FXCORR_X_GROUPS_ONLY` 存在的理由
#
# 判据：
#   1. 每个 batch 两侧的组划分**逐组逐成员**相同（组序也相同）
#   自检：把 python 一侧人为改坏（临时副本，不动仓库文件），判据必须报红
#
# 未覆盖：只有单一 `.input` 的 workdir 验证不了变体（多 ds 站、拆带多组等）。
# 那些样本的生成器在 `test/multids/`（gen_multids_input.py / gen_splitbands_input.py），
# 尚未接入本脚本；接入前先按 `test/multids/README.md` 手工确认过一次。
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FXINPUT="$SCRIPTDIR/../../fxinput.py"

[ $# -ge 1 ] || { echo "用法：$0 <workdir> [batch_id ...]" >&2; exit 2; }
WD=$1
shift
[ -d "$WD" ] || { echo "FAIL: workdir $WD 不存在" >&2; exit 2; }
WD=$(cd "$WD" && pwd)

if [ $# -gt 0 ]; then
	BIDS=("$@")
else
	BIDS=()
	for f in "$WD"/batches/*.json; do
		[ -e "$f" ] || { echo "FAIL: $WD/batches/ 下没有 batch" >&2; exit 2; }
		BIDS+=("$(basename "$f" .json)")
	done
fi
[ ${#BIDS[@]} -gt 0 ] || { echo "FAIL: 没有可判的 batch" >&2; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# C++ 侧真值：分片模式打印的组划分。刻意与下面那行运行期日志（`... -> path`）
# 同前缀，于是同一条 sed 也能读懂正常跑批时的输出。
cpp_groups()
{
	local raw
	raw=$(FXCORR_X_GROUPS_ONLY=1 fxcorr-x "$1" "$WD" 0 2>&1 || true)
	printf '%s\n' "$raw" \
		| sed -n 's/^fxcorr-x: shard mode, ds group [0-9]* of [0-9]* = \(.*\)$/\1/p'
}

# python 侧（$1 = fxinput.py 的路径，自检时指向改坏的副本）
py_groups()
{
	python3 "$1" groups "$WD" "$2" 2>/dev/null
}

check_batch()
{
	local bid=$1 pyfile=$2 cpp py
	cpp=$(cpp_groups "$bid")
	if [ -z "$cpp" ]; then
		echo "FAIL: $bid: fxcorr-x 没给出分组（config 资产是否齐全？二进制在 PATH 里吗？）" >&2
		return 1
	fi
	py=$(py_groups "$pyfile" "$bid")
	if [ "$cpp" != "$py" ]; then
		echo "FAIL: $bid: 两处 ds 组划分不一致（左 = fxinput.py，右 = fxcorr-x）" >&2
		diff <(printf '%s\n' "$py") <(printf '%s\n' "$cpp") >&2 || true
		return 1
	fi
	echo "ok: $bid: $(printf '%s\n' "$cpp" | wc -l | tr -d ' ') ds group(s)"
}

# ---- 自检：判据必须能报红，否则"全绿"没有意义 ----
# 改坏的方式是让 python 一侧一条 baseline 都不合并（每组各剩一个 ds）——组数
# 与成员都会变。**前提：被测 .input 至少有一组含多个 ds**；全是单元素组的
# workdir（每个 ds 各自成组）测不出区分力，那种数据上自检会误报，见文件头。
BROKEN="$TMP/fxinput_broken.py"
sed 's/for a, b in dspairs:/for a, b in dspairs[:0]:/' "$FXINPUT" > "$BROKEN"
grep -q 'dspairs\[:0\]' "$BROKEN" || { echo "FAIL: 自检无法改坏 fxinput.py（代码变了吗？）" >&2; exit 1; }
if check_batch "${BIDS[0]}" "$BROKEN" >/dev/null 2>&1; then
	echo "FAIL: 自检未通过——人为改坏 python 一侧后判据仍然报 ok，说明它没有区分力" >&2
	exit 1
fi
echo "ok: 自检（改坏 python 一侧后判据报红）"

# ---- 判据 1：逐 batch 比两侧组划分 ----
rc=0
for bid in "${BIDS[@]}"; do
	check_batch "$bid" "$FXINPUT" || rc=1
done

if [ $rc -eq 0 ]; then
	echo "PASS: ${#BIDS[@]} 个 batch 的 ds 组划分两侧一致"
else
	echo "FAIL: 存在不一致的 batch" >&2
fi
exit $rc
