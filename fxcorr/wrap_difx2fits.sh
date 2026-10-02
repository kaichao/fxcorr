#!/usr/bin/env bash
# wrap_difx2fits.sh —— difx2fits 的规范适配封装（V5 P5 Q16）
#
# difx2fits 是原 difx 程序：不认识 fxcorr 的根变量，且要求
#   <base>.difx/   SWIN 数据目录
#   <base>.input   DiFX 输入文件
#   <base>.calc    模型文件
# 三者同名同目录，相对路径按进程 cwd 解析；它**没有 -o 选项**，产物只能落在
# 运行目录，所以本脚本把三件套组装到 $FXCORR_PRODUCT_ROOT 下、cd 过去单参数
# 调用，跑完清掉临时件（只留 .FITS）。这是 Q15 那层"规范 → 原程序"的转换。
#
# **.calc 的源文件由 .input 的 `CALC FILENAME` 行指出**（相对 config 目录、绝对
# 原样），不假定 `<base>.calc` 同名：多 batch 的仿真变体只给 `.input` 换名
# （test-sim.input，128ms subint），`.calc` 仍是 test.calc——"三件套同名"是
# difx2fits 对**组装产物**的要求，由本脚本在 PRODUCT 目录里满足（2026-10-02
# 全链联调实测：按同名假定拼路径，test-sim 实验在 fits 阶段直接报 not found）。
#
# 与 run_bench.sh 同一套手法：复制一份 .input/.calc 并把里面的相对路径绝对化
# ——fxcorr 的 config 现在是"相对 .input 所在目录"写的，而这个程序按 cwd 找。
#
# 这是**实验级**操作：SWIN 跨 batch 追加、difx2fits 一次读整个 .difx 目录，
# 不能放进 run_batch.sh 逐 batch 调用——由编排层在该实验全部 batch 完成后调一次。
# 产物落 $FXCORR_PRODUCT_ROOT（可指向本地盘；本地 → 全局的迁移由编排层负责）。
#
# 用法：./wrap_difx2fits.sh [workdir] <base>
#   workdir  项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
#   base     config/ 下的 .input/.calc 基名（不带扩展名）；产物名由 difx2fits 按
#            它自己规则生成（<base> 或 <base>.0.bin*.source*.FITS），落在 PRODUCT 根
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
用法：./wrap_difx2fits.sh [workdir] <base>
  workdir  项目根目录（默认 .；环境变量 FXCORR_WORKDIR 亦可定义，位置参数优先）
  base     config/ 下的 .input/.calc 基名（不带扩展名）
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
BASE=$1
[ $# -le 1 ] || usage
fxcorr_roots "$WORKDIR"
WORKDIR=$FXCORR_ROOT_WORKDIR

CFG="$WORKDIR/config"
[ -f "$CFG/$BASE.input" ] || { echo "wrap_difx2fits.sh: $CFG/$BASE.input not found" >&2; exit 2; }

# .calc 的源文件：`CALC FILENAME` 行（相对 config 目录、绝对原样）——见头注释
CALC_SRC=$(python3 - "$CFG/$BASE.input" "$CFG" <<'PYEOF'
import re, sys
src, cfg = sys.argv[1], sys.argv[2]
for line in open(src):
    m = re.match(r'^CALC FILENAME:\s*(\S+)\s*$', line)
    if m:
        v = m.group(1)
        print(v if v.startswith('/') else cfg.rstrip('/') + '/' + v)
        break
else:
    sys.exit('wrap_difx2fits.sh: no CALC FILENAME in %s' % src)
PYEOF
)
[ -f "$CALC_SRC" ] || { echo "wrap_difx2fits.sh: $CALC_SRC not found (CALC FILENAME of $CFG/$BASE.input)" >&2; exit 2; }
PROD="$FXCORR_ROOT_PRODUCT"
mkdir -p "$PROD" || exit 2

# SWIN 目录：.input 的 OUTPUT FILENAME 相对 VIS 根解析（Q20），绝对则原样
SWINDIR=$(python3 - "$CFG/$BASE.input" "$FXCORR_ROOT_VIS" <<'PYEOF'
import re, sys
src, vis = sys.argv[1], sys.argv[2]
for line in open(src):
    m = re.match(r'^OUTPUT FILENAME:\s*(\S+)\s*$', line)
    if m:
        v = m.group(1)
        print(v if v.startswith('/') else vis.rstrip('/') + '/' + v)
        break
else:
    sys.exit('wrap_difx2fits.sh: no OUTPUT FILENAME in %s' % src)
PYEOF
)
[ -d "$SWINDIR" ] || { echo "wrap_difx2fits.sh: SWIN directory $SWINDIR not found" >&2; exit 2; }

# 组装三件套（临时件跑完即删）。相对路径绝对化：.input 的 CALC → config 目录、
# OUTPUT → SWIN 目录；.calc 的 IM/FLAG → config 目录
cleanup()
{
	rm -f "$PROD/$BASE.input" "$PROD/$BASE.calc" "$PROD/$BASE.difx"
}
trap cleanup EXIT
# CALC 必须指向**本目录这份**（$PROD/$BASE.calc），不能指向 config 下的原件：
# 原件里的 `IM FILENAME` 是相对名，difx2fits 拿它按 cwd 解析就找不到 .im，
# 而 .im 缺失时它的 fitsMC.c 不检查 NULL 直接索引 scan->im[antId] —— **段错误**
# （fitsML.c 有 `if(scan->im)` 保护，只在 MC 这一遍崩）。绝对化的 .calc 就在
# 本目录，指过来即可，不必动 config 里的原件。
sed -e "s|^CALC FILENAME:[[:space:]]*\([^/].*\)\$|CALC FILENAME:      $PROD/$BASE.calc|" \
    -e "s|^OUTPUT FILENAME:[[:space:]]*\([^/].*\)\$|OUTPUT FILENAME:    $SWINDIR|" \
    "$CFG/$BASE.input" > "$PROD/$BASE.input"
# IM/FLAG 相对 .calc 所在目录（configuration 的规则），拼它而不是 config 目录
CALCDIR=$(dirname "$CALC_SRC")
sed -e "s|^IM FILENAME:[[:space:]]*\([^/].*\)\$|IM FILENAME:        $CALCDIR/\1|" \
    -e "s|^FLAG FILENAME:[[:space:]]*\([^/].*\)\$|FLAG FILENAME:      $CALCDIR/\1|" \
    "$CALC_SRC" > "$PROD/$BASE.calc"
if [ -e "$PROD/$BASE.difx" ] && [ ! -L "$PROD/$BASE.difx" ]; then
	echo "wrap_difx2fits.sh: $PROD/$BASE.difx exists and is not a symlink; refusing to replace" >&2
	exit 2
fi
ln -sfn "$SWINDIR" "$PROD/$BASE.difx"

echo "wrap_difx2fits.sh: difx2fits $BASE (cwd=$PROD, output under $PROD)" >&2
if [ "$FXCORR_RUN_MODE" = "container" ]; then
	(cd "$PROD" && docker run --rm -v "$WORKDIR:$WORKDIR" -w "$(pwd)" "$CONTAINER_IMG:latest" difx2fits "$BASE")
else
	(cd "$PROD" && difx2fits "$BASE")
fi
