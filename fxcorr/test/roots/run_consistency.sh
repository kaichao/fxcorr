#!/usr/bin/env bash
# run_consistency.sh —— 根解析一致性判据（V5 P5 遗留第 4 条）
#
# 同一套规则有两处实现：bash 侧 `fxcorr/roots.sh`（编排脚本用）与 C++ 侧
# `libraries/fxcorrcommon/src/fxcorrpath.cpp`（三个工具用）。规则权威是
# `data-spec.md` 5.2.1，但两处实现之间**没有编译器兜底**——漂移了会表现为
# "脚本把根记进 roots.json、程序却按另一个值解析"，然后程序报 roots 不一致
# 或者更糟：静默读到别处。这条判据就是把两处摆在同一组环境变量下比一遍。
#
# 判据（三档回退 + cwd=/ 各覆盖一次，外加自检）：
#   1. 四个根都不设           → 两侧都应给出 <workdir>/<目录名>
#   2. 只设 FXCORR_WORKDIR    → 同上（第二档）
#   3. 四个根都设成自定义值   → 两侧都应给出那些值
#   4. 从 cwd=/ 跑（含相对值）→ 相对值绝对化后两侧必须同一拼写（`%/` 归一，2026-10-04 加）
#   自检：人为改坏脚本一侧，判据必须报红
#
# 用法：./run_consistency.sh [workdir]     （默认 /tmp/rootsconsist）
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOTS_SH="$SCRIPTDIR/../../roots.sh"
WD=${1:-/tmp/rootsconsist}
mkdir -p "$WD"
WD=$(cd "$WD" && pwd)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

# 以给定环境变量跑两侧，把四个根各打一行（顺序固定：raw/fengine/vis/product）
run_both()
{
	local tag=$1; shift
	env "$@" bash -c '. "'"$ROOTS_SH"'"; fxcorr_roots "'"$WD"'";
		printf "%s\n" "$FXCORR_ROOT_RAW" "$FXCORR_ROOT_FENGINE" "$FXCORR_ROOT_VIS" "$FXCORR_ROOT_PRODUCT"' \
		> "$TMP/$tag.script"
	env "$@" FXCORR_PRINT_ROOTS=1 fxcorr-sim nosuchbatch "$WD" 2>&1 |
		sed -n 's/^fxcorr-sim: FXCORR_[A-Z_]*_ROOT = \(.*\)  \[.*\]$/\1/p' \
		> "$TMP/$tag.prog" || true   # fxcorr-sim 找不到 batch.json 会非零退出，与判据无关
	[ "$(wc -l < "$TMP/$tag.prog")" -eq 4 ] ||
		fail "$tag: fxcorr-sim 只打出 $(wc -l < "$TMP/$tag.prog") 个根（应为 4；工具装了吗、PATH 里有 fxcorr-sim 吗？）"
	if ! diff -u "$TMP/$tag.script" "$TMP/$tag.prog" > "$TMP/$tag.diff"; then
		echo "--- $tag ---" >&2
		cat "$TMP/$tag.diff" >&2
		fail "$tag: 脚本侧与程序侧的根解析不一致（上：脚本，下：程序）"
	fi
	echo "  $tag: 两侧一致（$(head -1 "$TMP/$tag.script") …）"
}

echo "roots 一致性判据（workdir=$WD）"
# 1 档：什么都不设（第三档回退：workdir 位置参数 / 默认）
run_both "default" -u FXCORR_RAW_ROOT -u FXCORR_FENGINE_ROOT \
	-u FXCORR_VIS_ROOT -u FXCORR_PRODUCT_ROOT -u FXCORR_WORKDIR
# 2 档：只设 FXCORR_WORKDIR
run_both "workdir" -u FXCORR_RAW_ROOT -u FXCORR_FENGINE_ROOT \
	-u FXCORR_VIS_ROOT -u FXCORR_PRODUCT_ROOT FXCORR_WORKDIR="$WD"
# 3 档：四个根全设（含相对值——两侧都要按 cwd 绝对化，这是最易漂移的一处）
ALT=${WD}_alt
mkdir -p "$ALT"
run_both "custom" FXCORR_RAW_ROOT="$ALT/raw" \
	FXCORR_FENGINE_ROOT="$ALT/fe" FXCORR_VIS_ROOT="rel-vis" FXCORR_PRODUCT_ROOT="$ALT/prod"
# 4 档：cwd=/（2026-10-04 加）——相对值绝对化时 bash 侧曾是 `$PWD/$val`、在 `/` 下拼出
# `//rel-vis`，而 C++ 侧给 `/rel-vis`；这一档把 `${PWD%/}` 的归一锁住
( cd / && run_both "cwdslash" -u FXCORR_RAW_ROOT -u FXCORR_FENGINE_ROOT \
	-u FXCORR_PRODUCT_ROOT -u FXCORR_WORKDIR FXCORR_VIS_ROOT="rel-vis" )

# 自检：把脚本侧第一个值改坏，判据必须报红
sed '1s|^|X|' "$TMP/default.script" > "$TMP/default.bad"
if diff -q "$TMP/default.bad" "$TMP/default.prog" > /dev/null; then
	fail "自检未报红：改坏一侧后仍判为一致，判据无效"
fi
echo "  自检: 改坏一侧能报红 ✅"
echo "PASS: 四个根在两处实现下逐项一致（三档回退 + cwd=/ 各验一次）"
