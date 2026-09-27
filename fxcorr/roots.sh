# fxcorr/roots.sh —— 目录根解析（V5 P5），与 fxcorrcommon 的 FxcorrPath 同规则
#
# 规则唯一权威是 fxcorr/data-spec.md 5.2.1；这里的实现必须与
# libraries/fxcorrcommon/src/fxcorrpath.cpp 保持一致（两处实现、一套规则）。
#
# 用法（在 set -u 的脚本里）：
#   . "$SCRIPTDIR/roots.sh"
#   fxcorr_roots "$WORKDIR"        # 解析（必须在使用任何 FXCORR_ROOT_* 之前调用）
#
# 解析后可用：
#   FXCORR_ROOT_WORKDIR      绝对化的项目根
#   FXCORR_ROOT_RAW / _FENGINE / _VIS / _PRODUCT
#
# 三档回退（v5-plan.md Q9）：FXCORR_<X>_ROOT 已设置 → 用它的值（相对则按 cwd
# 绝对化）；否则 <workdir>/<目录名>。config/ batches/ meta/ beam/ 没有独立根，
# 恒在 workdir 下（Q17），脚本里直接 "$FXCORR_ROOT_WORKDIR/<name>" 拼即可。
#
# 注意本文件只定义函数、不执行任何动作，也不得依赖调用方的局部变量。

# 单个根的解析：$1 = 环境变量名，$2 = 规范目录名
fxcorr_root()
{
	local varname="$1" dirname="$2" val
	val="${!varname-}"
	if [ -n "$val" ]; then
		case "$val" in
			/*) printf '%s\n' "$val" ;;
			*)  printf '%s\n' "$PWD/$val" ;;	# 相对值按 cwd 绝对化（Q20）
		esac
	else
		printf '%s\n' "$FXCORR_ROOT_WORKDIR/$dirname"
	fi
}

# 解析全部根：$1 = workdir（相对则按 cwd 绝对化）
fxcorr_roots()
{
	FXCORR_ROOT_WORKDIR=$(cd "$1" && pwd)
	FXCORR_ROOT_RAW=$(fxcorr_root FXCORR_RAW_ROOT raw)
	FXCORR_ROOT_FENGINE=$(fxcorr_root FXCORR_FENGINE_ROOT fengine)
	FXCORR_ROOT_VIS=$(fxcorr_root FXCORR_VIS_ROOT vis)
	FXCORR_ROOT_PRODUCT=$(fxcorr_root FXCORR_PRODUCT_ROOT product)
}

# 建齐四个根（Q19：根由编排层创建，程序遇根不存在只报错、不自动建）
fxcorr_mkroots()
{
	local r
	for r in "$FXCORR_ROOT_RAW" "$FXCORR_ROOT_FENGINE" \
	         "$FXCORR_ROOT_VIS" "$FXCORR_ROOT_PRODUCT"; do
		mkdir -p "$r" || {
			echo "roots.sh: cannot create root $r" >&2
			return 1
		}
	done
}

# 写 meta/roots/<batch_id>.json：记下本 batch 生效的全部根（Q4）。
# 环境变量不进 batch.json，这份记录是事后追溯"这批数据用了哪套布局"的唯一手段。
fxcorr_write_roots()
{
	local bid="$1" dir="$FXCORR_ROOT_WORKDIR/meta/roots"
	mkdir -p "$dir" || return 1
	cat > "$dir/$bid.json" <<EOF
{
  "workdir": "$FXCORR_ROOT_WORKDIR",
  "raw": "$FXCORR_ROOT_RAW",
  "fengine": "$FXCORR_ROOT_FENGINE",
  "vis": "$FXCORR_ROOT_VIS",
  "product": "$FXCORR_ROOT_PRODUCT"
}
EOF
}

# 实验级根的一致性检查（Q18）。
# vis/ 与 product/ 是跨 batch 追加的：同一实验中途换了根，产物会分裂在两处而
# 没有任何报错。这里与已有 batch 的记录比对，不一致即失败退出（不改任何文件）。
fxcorr_check_roots()
{
	local bid="$1"
	python3 - "$FXCORR_ROOT_WORKDIR" "$bid" "$FXCORR_ROOT_VIS" "$FXCORR_ROOT_PRODUCT" <<'PYEOF'
import glob, json, os, sys

workdir, bid, vis, product = sys.argv[1:5]
# 本 batch 自己的记录也参与比对：重跑同一个 batch 换了根，新旧产物同样会分裂
# 在两处。调用方保证 check 发生在 write 之前，所以首次运行时还没有自己的记录。
for path in sorted(glob.glob(os.path.join(workdir, 'meta', 'roots', '*.json'))):
    try:
        with open(path) as f:
            d = json.load(f)
    except (IOError, ValueError):
        continue
    for key, want in (('vis', vis), ('product', product)):
        got = d.get(key)
        if got and got != want:
            sys.exit('roots.sh: %s changed since %s\n  was %s\n  now %s\n'
                     'experiment-level outputs would split across two locations; '
                     'finish that experiment first or restore the root'
                     % (key, os.path.basename(path), got, want))
PYEOF
}
