#!/usr/bin/env bash
# check_record.sh —— 根记录的"布局"写法与比对判据（data-spec 5.2.1，2026-10-04 定案）
#
# 规则两条，两处实现都要守（bash 侧 `fxcorr/roots.sh`，C++ 侧 `FxcorrPath`）：
#   * **写侧**：等于 <workdir>/<规范名> 的根写相对形式 `./<名>`——记录描述的是
#     **布局**（"raw 在 workdir 下"），不是某个挂载视图下的路径；重定向的根写
#     绝对；末位 `/` 归一。
#   * **比侧**：两侧都先归一化再逐字比。于是**同一份布局换个路径别名**（宿主
#     `/shared/mydata/x` 与容器内 `/cluster_data_root/x` 是同一份数据）判一致，
#     而"真的换了地方"仍报错；**旧式绝对记录经一次折叠即等价、不用重写**。
#
# 判据（每条都配反例——反例不报红，说明判据本身失效）：
#   1 写侧形式：默认档 `./<名>`；重定向根绝对；末位 `/` 归一后仍判默认档
#   2 bash 比侧（`fxcorr_check_roots`）：别名下新式记录 → 过；别名下旧式绝对记录
#     → 过；`vis` 指到别处 → 报错
#   3 C++ 比侧（`fxcorr-f` 实跑，checkRoots 早于读 batch.json）：
#     别名下新式记录 → 过；别名下旧式绝对记录 → 过；别名 + 末位 `/` 的根值 → 过；
#     记录里 raw 指到别处 → 报错；运行时 raw 指到别处 → 报错
#
# 「过」= 不出现 `disagrees with`（工具随后会因缺 batches/<id>.json 而停下，
# 与本判据无关）；「报错」= 出现 `disagrees with` 且退出非 0。
#
# 用法：./check_record.sh [tmpdir]     （默认 /tmp/fxrootsrec；需要 fxcorr-f 在 PATH）
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOTS_SH="$SCRIPTDIR/../../roots.sh"
TMP=${1:-/tmp/fxrootsrec}
rm -rf "$TMP"
mkdir -p "$TMP"

WD=$TMP/real          # workdir 的真实路径
ALIAS=$TMP/alias      # 同一目录的另一个路径（模拟宿主机 / 容器两套挂载视图）
mkdir -p "$WD"
ln -s "$WD" "$ALIAS"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

# 清掉调用方可能残留的根变量：判据只认调用点显式给的那些
ENVCLEAN=(-u FXCORR_RAW_ROOT -u FXCORR_FENGINE_ROOT -u FXCORR_VIS_ROOT
          -u FXCORR_PRODUCT_ROOT -u FXCORR_WORKDIR)

# 走被验的写侧实现落一份记录
write_record()
{
	local wd=$1; shift
	env "${ENVCLEAN[@]}" "$@" bash -c '. "'"$ROOTS_SH"'"; fxcorr_roots "'"$wd"'"; fxcorr_write_roots 00000001'
}

# 旧式记录（2026-10-04 之前的形式：四个根全写绝对）
write_legacy_record()
{
	local wd=$1
	mkdir -p "$wd/meta/roots"
	cat > "$wd/meta/roots/00000001.json" <<EOF
{
  "workdir": "$wd",
  "raw": "$wd/raw",
  "fengine": "$wd/fengine",
  "vis": "$wd/vis",
  "product": "$wd/product"
}
EOF
}

# 走被验的 bash 比侧实现（$1 = 跑 check 的 workdir，其余为环境变量）
check_bash()
{
	local wd=$1; shift
	env "${ENVCLEAN[@]}" "$@" bash -c '. "'"$ROOTS_SH"'"; fxcorr_roots "'"$wd"'"; fxcorr_check_roots 00000001' 2>&1
}

# 跑 C++ 比侧（$1 = workdir，其余为环境变量）：out 与 RC 带回给调用方。
# 「过」的用例里工具随后会因缺 batches/<id>.json 而停下——那是预期，不比它的退出码。
run_f()
{
	local wd=$1; shift
	RC=0
	out=$(env "${ENVCLEAN[@]}" "$@" fxcorr-f 00000001 BA "$wd" 0 2>&1) || RC=$?
}

expect_bash_pass()
{
	local tag=$1 wd=$2; shift 2
	if ! out=$(check_bash "$wd" "$@"); then
		echo "$out" >&2
		fail "$tag: 应判一致（bash 比侧）"
	fi
}

expect_bash_mismatch()
{
	local tag=$1 wd=$2; shift 2
	if out=$(check_bash "$wd" "$@"); then
		fail "$tag: 应报错（bash 比侧）"
	fi
	case "$out" in
		*"changed since"*) ;;
		*) echo "$out" >&2; fail "$tag: 报错信息不符（bash 比侧）" ;;
	esac
}

expect_f_pass()
{
	local tag=$1 wd=$2; shift 2
	run_f "$wd" "$@"
	case "$out" in
		*"disagrees with"*) echo "$out" >&2; fail "$tag: 不该报根不一致（C++ 比侧）" ;;
	esac
}

expect_f_mismatch()
{
	local tag=$1 wd=$2; shift 2
	run_f "$wd" "$@"
	case "$out" in
		*"disagrees with"*) ;;
		*) echo "$out" >&2; fail "$tag: 应报根不一致（C++ 比侧）" ;;
	esac
	[ "$RC" -ne 0 ] || fail "$tag: 报不一致时应非零退出（C++ 比侧）"
}

command -v fxcorr-f >/dev/null || fail "PATH 里没有 fxcorr-f（本判据要在装了三工具的容器/测试机上跑）"

echo "根记录判据（tmpdir=$TMP；workdir=$WD，别名=$ALIAS）"

# ---- 1 写侧形式 -----------------------------------------------------------
write_record "$WD"
for k in raw fengine vis product; do
	grep -q "\"$k\": \"./$k\"" "$WD/meta/roots/00000001.json" ||
		fail "1a: 默认档 $k 应写 ./$k（记录的是布局，不是路径）"
done
write_record "$WD" FXCORR_FENGINE_ROOT="$TMP/elsewhere/fengine" FXCORR_VIS_ROOT="$WD/vis/"
grep -q "\"fengine\": \"$TMP/elsewhere/fengine\"" "$WD/meta/roots/00000001.json" ||
	fail "1b: 重定向的根应写绝对"
grep -q '"vis": "./vis"' "$WD/meta/roots/00000001.json" ||
	fail "1b: 末位 / 归一后仍应判默认档（写 ./vis）"
echo "  1 写侧形式：默认档相对、重定向绝对、末位 / 归一 ✓"

# ---- 2 bash 比侧 ----------------------------------------------------------
write_record "$WD"
expect_bash_pass "2a" "$ALIAS"
write_legacy_record "$WD"
expect_bash_pass "2b" "$ALIAS"
mkdir -p "$WD/meta/roots"
cat > "$WD/meta/roots/00000001.json" <<EOF
{
  "workdir": "$WD",
  "vis": "$TMP/elsewhere/vis",
  "product": "$WD/product"
}
EOF
expect_bash_mismatch "2c" "$ALIAS"
echo "  2 bash 比侧：别名 ✓ / 旧式绝对记录 ✓ / vis 换了地方报错 ✓"

# ---- 3 C++ 比侧 -----------------------------------------------------------
write_record "$WD"
expect_f_pass "3a" "$ALIAS"
write_legacy_record "$WD"
expect_f_pass "3b" "$ALIAS"
write_record "$WD"
expect_f_pass "3c" "$ALIAS" FXCORR_RAW_ROOT="$ALIAS/raw/"
cat > "$WD/meta/roots/00000001.json" <<EOF
{
  "workdir": "$WD",
  "raw": "$TMP/elsewhere/raw",
  "fengine": "./fengine",
  "vis": "./vis"
}
EOF
expect_f_mismatch "3d" "$ALIAS"
write_record "$WD"
expect_f_mismatch "3e" "$ALIAS" FXCORR_RAW_ROOT="$TMP/elsewhere/raw"
echo "  3 C++ 比侧：别名 ✓ / 旧式绝对记录 ✓ / 末位 / ✓ / 记录 raw 指别处报错 ✓ / 运行时 raw 指别处报错 ✓"

echo "全部通过"
