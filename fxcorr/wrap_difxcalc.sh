#!/usr/bin/env bash
# wrap_difxcalc.sh —— difxcalc 的规范适配封装（V5 P5）
#
# 与 wrap_vex2difx.sh 同构：① 固定从 config/ 目录内调用（difxcalc 按 cwd 解析
# .calc 的位置、也把产物 .im 的路径按 cwd 写回 .calc）；② 把 .calc 里被写成
# 绝对路径的 IM FILENAME / FLAG FILENAME 改回相对 config 目录（v5-plan.md Q20），
# 让整个配置目录可整体搬走。幂等。
#
# 用法：./wrap_difxcalc.sh [workdir] <calc> [calc ...]
#   workdir  项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
#   calc     相对 config/ 的 .calc 文件名
#
# 环境变量：FXCORR_RUN_MODE=container 时经 docker run 调用同一镜像
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$SCRIPTDIR/roots.sh"

FXCORR_RUN_MODE="${FXCORR_RUN_MODE:-host}"
CONTAINER_IMG=fxcorr/fxcorr

usage()
{
	cat >&2 <<'EOF'
用法：./wrap_difxcalc.sh [workdir] <calc> [calc ...]
  workdir  项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
  calc     相对 config/ 的 .calc 文件名
EOF
	exit "${1:-2}"
}

[ $# -ge 1 ] || usage

WORKDIR="${FXCORR_WORKDIR:-.}"
if [ -d "$1" ]; then
	WORKDIR=$1	# 位置参数优先于环境变量
	shift
fi
[ $# -ge 1 ] || usage
fxcorr_roots "$WORKDIR"
WORKDIR=$FXCORR_ROOT_WORKDIR

CFG="$WORKDIR/config"
if [ ! -d "$CFG" ]; then
	echo "wrap_difxcalc.sh: $CFG not found" >&2
	exit 2
fi

run_in_container()
{
	docker run --rm -v "$WORKDIR:$WORKDIR" -w "$(pwd)" "$CONTAINER_IMG:latest" "$@"
}
fxc()
{
	if [ "$FXCORR_RUN_MODE" = "container" ]; then
		run_in_container "$@"
	else
		"$@"
	fi
}

for calc in "$@"; do
	if [ ! -f "$CFG/$calc" ]; then
		echo "wrap_difxcalc.sh: $CFG/$calc not found" >&2
		exit 2
	fi
	echo "wrap_difxcalc.sh: difxcalc $calc (cwd=$CFG)" >&2
	(cd "$CFG" && fxc difxcalc "$calc")
done

# 规范化：绝对路径 -> 相对 config 目录（与 wrap_vex2difx.sh 同一规则、同一理由）
python3 - "$CFG" <<'PYEOF'
import glob, os, re, sys

cfg = sys.argv[1]
keys = ('IM FILENAME', 'FLAG FILENAME')
changed = 0
for path in sorted(glob.glob(os.path.join(cfg, '*.calc'))):
    with open(path) as f:
        lines = f.readlines()
    out = []
    touched = False
    for line in lines:
        m = re.match(r'^(%s):(\s*)(\S+)\s*$' % '|'.join(re.escape(k) for k in keys), line)
        if m and m.group(3).startswith('/'):
            rel = os.path.relpath(m.group(3), cfg)
            line = '%s:%s%s\n' % (m.group(1), m.group(2), rel)
            touched = True
        out.append(line)
    if touched:
        with open(path, 'w') as f:
            f.writelines(out)
        changed += 1
        print('wrap_difxcalc.sh: normalised %s' % os.path.basename(path), file=sys.stderr)
if not changed:
    print('wrap_difxcalc.sh: nothing to normalise (already relative)', file=sys.stderr)
PYEOF
