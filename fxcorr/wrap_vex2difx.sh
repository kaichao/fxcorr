#!/usr/bin/env bash
# wrap_vex2difx.sh —— vex2difx 的规范适配封装（V5 P5）
#
# 职责只有两件：把 vex2difx 放在它要求的 cwd 里调用，再把它的产物规范化。
#
# ① cwd：vex2difx 的 vex= 路径相对 cwd（不是相对 .v2d 所在目录），.input/.calc
#    也输出到 cwd。所以固定从 config/ 目录内调用——这正是"bash 做规范适配层"
#    的那一层转换（v5-plan.md Q15），以前内联在 make_testdata.sh 里。
# ② 规范化：vex2difx 会把 .input 的 CALC FILENAME 与 OUTPUT FILENAME 按 cwd
#    写成绝对路径，配置目录一搬就断。这里把**绝对**值改回相对 config 目录的
#    形式（相对值原样保留），与 fxcorr 侧"只对相对路径拼根"的规则对称
#    （v5-plan.md Q20）。幂等：重复跑不会二次改写。
#
# 用法：./wrap_vex2difx.sh [workdir] <v2d> [v2d ...]
#   workdir  项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
#   v2d      相对 config/ 的 .v2d 文件名
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
用法：./wrap_vex2difx.sh [workdir] <v2d> [v2d ...]
  workdir  项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
  v2d      相对 config/ 的 .v2d 文件名
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
	echo "wrap_vex2difx.sh: $CFG not found" >&2
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

for v2d in "$@"; do
	if [ ! -f "$CFG/$v2d" ]; then
		echo "wrap_vex2difx.sh: $CFG/$v2d not found" >&2
		exit 2
	fi
	echo "wrap_vex2difx.sh: vex2difx $v2d (cwd=$CFG)" >&2
	(cd "$CFG" && fxc vex2difx "$v2d")
done

# 规范化：绝对路径 -> 相对 config 目录。程序侧对这些字段一律"相对才拼根、
# 绝对原样"，所以不改也不会读错；改是为了让整个 config/ 目录可以整体搬走。
python3 - "$CFG" <<'PYEOF'
import glob, os, re, sys

cfg = sys.argv[1]
keys = ('CALC FILENAME', 'OUTPUT FILENAME')
changed = 0
for path in sorted(glob.glob(os.path.join(cfg, '*.input'))):
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
        print('wrap_vex2difx.sh: normalised %s' % os.path.basename(path), file=sys.stderr)
if not changed:
    print('wrap_vex2difx.sh: nothing to normalise (already relative)', file=sys.stderr)
PYEOF
