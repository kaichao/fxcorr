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
			*)  printf '%s\n' "${PWD%/}/$val" ;;	# 相对值按 cwd 绝对化（Q20）；`%/` 防 cwd=/ 时拼出 `//rel`（2026-10-04 修，与 C++ absolutise 同规则）
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

# 记录值的书写（2026-10-04 定案，data-spec 5.2.1）：记录描述的是**布局**、不是某个
# 挂载视图下的路径——容器化下同一物理目录必然有多个路径别名（宿主 /shared/mydata/x
# 与容器内 /cluster_data_root/x 是同一份数据），写死绝对路径会让换个视图跑就误报
# "换了根"。规则：等于 workdir/<规范名> 的根写相对形式 ./<名>（相对 = 相对本 batch
# 的 workdir）；重定向的根写绝对。末位 / 一律归一（拼写稳定，比对是逐字的）。
fxcorr_root_norm()
{
	local val="$1"
	while [ "$val" != "/" ] && [ "${val%/}" != "$val" ]; do
		val="${val%/}"
	done
	printf '%s\n' "$val"
}

fxcorr_root_record()
{
	local val name="$2"
	val=$(fxcorr_root_norm "$1")
	if [ "$val" = "$(fxcorr_root_norm "$FXCORR_ROOT_WORKDIR")/$name" ]; then
		printf '%s\n' "./$name"
	else
		printf '%s\n' "$val"
	fi
}

# 写 meta/roots/<batch_id>.json：记下本 batch 生效的全部根（Q4）。
# 环境变量不进 batch.json，这份记录是事后追溯"这批数据用了哪套布局"的唯一手段。
fxcorr_write_roots()
{
	local bid="$1" dir="$FXCORR_ROOT_WORKDIR/meta/roots"
	mkdir -p "$dir" || return 1
	cat > "$dir/$bid.json" <<EOF
{
  "workdir": "$(fxcorr_root_norm "$FXCORR_ROOT_WORKDIR")",
  "raw": "$(fxcorr_root_record "$FXCORR_ROOT_RAW" raw)",
  "fengine": "$(fxcorr_root_record "$FXCORR_ROOT_FENGINE" fengine)",
  "vis": "$(fxcorr_root_record "$FXCORR_ROOT_VIS" vis)",
  "product": "$(fxcorr_root_record "$FXCORR_ROOT_PRODUCT" product)"
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

# 归一化后再比（data-spec 5.2.1，2026-10-04）：记录写的是"布局"——等于
# <workdir>/<规范名> 的根记作 './<名>'（旧式记录里是同一件事的绝对写法），两种
# 写法折成同一形式，容器挂载别名（宿主 /shared/mydata/x 与容器内
# /cluster_data_root/x 是同一份数据）才不会误报；重定向的根没有别名可言，仍是
# 逐字比。末位 / 归一。与 FxcorrPath::checkRoots 同规则。
def norm(value, wd, name):
    v = value.rstrip('/') or '/'
    wd = (wd or '').rstrip('/')
    if wd and v == wd + '/' + name:
        return './' + name
    return v

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
        if got and norm(got, d.get('workdir'), key) != norm(want, workdir, key):
            sys.exit('roots.sh: %s changed since %s\n  was %s\n  now %s\n'
                     'experiment-level outputs would split across two locations; '
                     'finish that experiment first or restore the root'
                     % (key, os.path.basename(path), got, want))
PYEOF
}
