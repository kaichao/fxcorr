#!/usr/bin/env bash
# run_mixed.sh —— 缺口与 filler 在同一段时间里并存、且**多组相邻**（reader-model.md 7.5 第 3 项）
#
# 现有资产把缺口与 filler 分开测：run_filler.sh / run_window.sh 各造**一处**中断
# （filler 段 + 紧随的一个缺口），run_boundary.sh 造两处**纯缺口**。t25362 的 BA ds_2
# 是第三种，2026-09-19 用 scan_filler.py 实测（fps 16000，8 组）：
#
#   组   filler 帧   缺口帧   与上一组之间的数据帧
#   1        81       10       —
#   2        82       10       13
#   3       229       28       25
#   4       180       22        2      ← 只隔 2 帧数据
#   5        17        2       (远)
#   6        49        6        2      ← 又一组相邻
#   7        16        2        5
#   8       508       63       64
#
# 两件事此前没有资产覆盖：
#
#   ① **组的形态是「filler 段紧跟一个真实缺口」**（filler 远长于缺口，81:10、508:63），
#      而不是"缺口两端夹 filler"。读取窗口跨过一组时，位置要被 filler 修正**推前**、
#      再被缺口修正**拉回**，两种修正的方向相反；
#   ② **组与组之间只隔几帧数据**——一个 subint 窗口里要连续吸收两次 filler 修正与两次
#      缺口修正。B2 的窗口长度修法按"被 filler 占掉的宽度"补偿，两段叠加时补偿量翻倍，
#      这正是"短数据段夹在 filler 段之间"（reader-model.md 4.6）的确切含义。
#
# 本脚本造两个场景，各配一个"同一段时间的纯缺口等价形式"（硬约束：两种形式描述的帧号
# 范围必须相同，只是 filler 形式多留了占位字节）：
#
#   M1  两组相邻，照组 3/4 的比例（229+28、180+22），组间 2 帧数据，落在同一个 subint 窗口
#   M2  一组长 filler 跨 subint 边界，照组 8 的比例（508+63），filler 段横跨约 4 个 subint
#
# 判据（两套并存，fxcorr/v4-plan.md 的 A3）：
#   * 相对：filler 形式与纯缺口形式**逐 subint 无效块一致**（±2 块）；
#   * 绝对：check_reader.py 的 E1 定位 / E2 数据 / E3 落点无 finding、**E4 无净损失**——
#     本脚本的中断之后仍有数据，E4 = 0 才是 B2 窗口长度修法在两段叠加下仍生效的证据；
#   * 自检：把 filler 形式的读位置整体前移一次（= 把 filler 当成时间轴损失的语义错误），
#     绝对判据必须报红，否则判据退化成恒绿。
#
# 用法：./run_mixed.sh [workdir]     （默认 /tmp/gapsm）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/gapsm}
STATION=${STATION:-T1}
FPS=${FPS:-250}				# test.vex：8 Ms/s ÷ 每帧 32000 采样
SUBINT_SEC=${SUBINT_SEC:-0.524288}	# test.input 的 subintNS
FRAMEBYTES=${FRAMEBYTES:-8032}		# test.vex：payload 8000 + 32 字节帧头

# name | 混合 spec（filler 形式） | 等价 spec（纯缺口形式） | missing | filler
SCENARIOS=(
	"M1 两组相邻|0.50:28:f229,0.62:22:f180|0.50:28,0.62:22|50|409"
	"M2 长 filler 跨边界|0.45:63:f508|0.45:63|63|508"
)

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FXCORR_SRC=${FXCORR_SRC:-$(cd "$SCRIPTDIR/../../.." && pwd)}
CHECKER="$SCRIPTDIR/../reader/check_reader.py"

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/local/difx/lib}
PATH=$PATH:/usr/local/difx/bin
export PATH

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

require_absolute()
{
	local log=$1
	if [ "$ABS_RC" != 0 ]; then
		grep -E "^(  sub |summary)" "$log.check" | sed 's/^/  /' >&2
		fail "$log: check_reader 报了 finding（rc=$ABS_RC，见 $log.check）"
	fi
	grep -E "^(summary|E4 coverage)" "$log.check" | sed 's/^/  /'
}

require_e4_zero()
{
	local log=$1 lost
	lost=$(awk '/^E4 coverage/{print $4}' "$log.check")
	[ "$lost" = 0 ] ||
		fail "$log: E4 报 $lost 帧净损失——中断之后的数据落在读取窗口之外（reader-model.md 4.7）"
}

# 生成 → 跑 f（verbose）→ 无效块分布 + 真值对账
run_one()
{
	local spec=$1 log=$2
	# make_testdata.sh 对已存在的 VDIF 幂等跳过，换中断重生成前必须先删
	rm -f "$WORKDIR"/raw/*/*.vdif
	FXSIM_NOISE=${FXSIM_NOISE:-0} FXSIM_GAPS="$spec" \
		"$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
	BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
	rm -rf "$WORKDIR/fengine"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) > "$log" 2>&1 ||
		fail "fxcorr-f 退出码非零（见 $log）"
	python3 "$SCRIPTDIR/sp_valid.py" \
		"$WORKDIR/fengine/$BATCHID/$STATION/ds_0/band_00.sp" --machine > "$log.invalid"

	VDIF="$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif"
	BATCHJSON="$WORKDIR/batches/$BATCHID.json"
	ABS_RC=0
	python3 "$CHECKER" --log "$log" --vdif "$VDIF" --batch-json "$BATCHJSON" \
		--fps "$FPS" > "$log.check" 2>&1 || ABS_RC=$?
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好，见本目录 README 的坑）"
[ -f "$CHECKER" ] || fail "找不到 $CHECKER"

echo "--- 无中断（对照）---"
run_one "" "$WORKDIR/none.log"
grep "GAPCHECK summary" "$WORKDIR/none.log" | sed 's/^/  /'
require_absolute "$WORKDIR/none.log"
require_e4_zero "$WORKDIR/none.log"
CTRLTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/none.log.invalid")
BPS=$(awk -F: '$1=="bps"{print $2}' "$WORKDIR/none.log.invalid")

for row in "${SCENARIOS[@]}"; do
	IFS='|' read -r name mixspec gapspec missing filler <<< "$row"
	echo
	echo "=== $name：混合 $mixspec / 等价缺口 $gapspec ==="

	echo "--- 纯缺口形式（同一段时间的诚实形态）---"
	run_one "$gapspec" "$WORKDIR/gap.log"
	grep -q "missing frames $missing filler frames 0" "$WORKDIR/gap.log" ||
		{ grep "GAPCHECK summary" "$WORKDIR/gap.log" >&2; fail "缺口形式应报 missing $missing / filler 0"; }
	grep "GAPCHECK summary" "$WORKDIR/gap.log" | sed 's/^/  /'
	require_absolute "$WORKDIR/gap.log"
	require_e4_zero "$WORKDIR/gap.log"

	echo "--- filler 形式 ---"
	run_one "$mixspec" "$WORKDIR/fill.log"
	grep -q "missing frames $missing filler frames $filler" "$WORKDIR/fill.log" ||
		{ grep "GAPCHECK summary" "$WORKDIR/fill.log" >&2
		  fail "filler 形式应报 missing $missing / filler $filler"; }
	grep "GAPCHECK summary" "$WORKDIR/fill.log" | sed 's/^/  /'
	require_absolute "$WORKDIR/fill.log"
	require_e4_zero "$WORKDIR/fill.log"

	GAPTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/gap.log.invalid")
	FILLTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/fill.log.invalid")

	echo "  逐 subint 无效块（缺口形式 / filler 形式）："
	paste -d' ' <(grep -v -E '^(bps|total)' "$WORKDIR/gap.log.invalid") \
	            <(grep -v -E '^(bps|total)' "$WORKDIR/fill.log.invalid") |
	awk -F'[: ]+' '{ d=$2-$4; if(d<0) d=-d; mark=(d>2) ? "  <-- 不一致" : ""
	                 printf "    subint %s: 缺口 %4d  filler %4d%s\n", $1, $2, $4, mark }'
	echo "  总数：无中断 $CTRLTOTAL，缺口 $GAPTOTAL，filler $FILLTOTAL"

	# 主判据一：逐 subint 一致（±2 块）
	awk -F: 'NR==FNR { if ($1!="total" && $1!="bps") g[$1]=$2; next }
	         $1=="total" || $1=="bps" { next }
	         { d=$2-g[$1]; if(d<0) d=-d
	           if(d>2) { printf "  subint %s: filler %d 块 vs 缺口 %d 块（差 %d）\n", $1, $2, g[$1], d; bad=1 } }
	         END { exit bad }' "$WORKDIR/gap.log.invalid" "$WORKDIR/fill.log.invalid" || {
		fail "$name: filler 形式的无效块与纯缺口形式不一致——多组叠加时读位置被 filler 带偏"
	}

	# 主判据二：无效块总量对上"缺失帧数 × 块/帧"（±4 块吸收取整），
	# 即缺口形式比无中断对照多出的部分应当就是缺口本身
	EXPECT=$(awk -v m="$missing" -v ss="$SUBINT_SEC" -v f="$FPS" -v bps="$BPS" \
		'BEGIN { printf "%d", m*bps/(ss*f) + 0.5 }')
	GAPEXTRA=$((GAPTOTAL - CTRLTOTAL))
	[ "$GAPEXTRA" -ge $((EXPECT - 4)) ] && [ "$GAPEXTRA" -le $((EXPECT + 4)) ] ||
		fail "$name: 缺口形式比无中断多 $GAPEXTRA 块，应为 $EXPECT"

	echo "  --- 自检：filler 形式读位置整体前移 $filler 帧（= 把 filler 当时间轴损失的语义）---"
	python3 - "$WORKDIR/fill.log" "$WORKDIR/tamper.log" "$filler" "$FRAMEBYTES" <<'PY'
import re, sys
log, out, frames, framebytes = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
delta = frames * framebytes
def shift(m):
    return m.group(0).replace(m.group(2), str(int(m.group(2)) + delta), 1)
open(out, 'w').write(re.sub(r'READPOS subint (\d+): readoff (-?\d+)', shift, open(log).read()))
PY
	if python3 "$CHECKER" --log "$WORKDIR/tamper.log" --vdif "$VDIF" \
		--batch-json "$BATCHJSON" --fps "$FPS" > "$WORKDIR/tamper.log.check" 2>&1; then
		sed 's/^/    /' "$WORKDIR/tamper.log.check" | tail -4
		fail "$name: 自检失败——读位置前移 $filler 帧之后判据竟然没报红（判据是恒绿的吗？）"
	fi
	grep -E "^(summary|E4 coverage)" "$WORKDIR/tamper.log.check" | sed 's/^/    /'
done

echo
echo "PASS: 多组相邻的 filler+缺口下，两种形式标出的无效数据量一致（相对判据），"
echo "      文件真值对账干净且无净损失（绝对判据），判据自检能报红"
