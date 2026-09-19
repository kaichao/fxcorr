#!/usr/bin/env bash
# run_boundary.sh —— 缺口跨 subint 边界的检验（reader-model.md 4.5 / 7.5 第 1 项）
#
# 复现 B5，两个场景各查一条：
#
#   场景 A（GAPSPEC=0.52:30）：缺口 [130,160) 跨 subint 1/2 的边界 131.072，
#     但不覆盖 subint 2 的读取位置（locate 给的 128）。
#     **判据：有缺口时每个 subint 的 firstfno 不得早于无缺口时的对应值。**
#     缺口若按"已检测到"累计，跨界那个 subint 会被多减 30 帧、读到属于前一个
#     subint 时间的数据（现状 98，对照 128）。
#
#   场景 B（OVERSPEC=0.48:30）：缺口 [120,150) 把 subint 2 的读取位置 128
#     盖住了，读取位置只能落到缺口之后（帧 150）。
#     **判据：该 subint 的权重数组从第 0 块起连续为 0，块数 = 帧差 × 块/帧。**
#     buffer 的第一帧不再等于 subint 起点，shiftFrameGaps 必须按帧号把它放到
#     (150-128)=22 帧处；不这么做时帧号连续、它根本不会被调用，洞会被当成数据。
#
# 两次运行都在同一站、同一 batch 上重新生成数据（无缺口作对照），只差一个
# 环境变量。子带帧率 fps=250、payloadbytes/blockbytes = blocks_per_send/subint帧数
# 都由 test.vex 的配置决定，换配置时要跟着改（判据依赖 fps，见本目录 README 的坑）。
#
# **绝对判据（判据二，`check_reader.py`）**：上面两条都是相对判据（与无缺口对照比），
# 对照本身错掉就抓不到；判据二拿 VDIF 文件当真值，判 E1 定位 / E2 数据 / E3 落点无
# finding、E4 无净损失，两者必须同时绿（`fxcorr/v4-plan.md` 的 A3）。E5 锚点在含
# 缺口的数据上按设计跳过，只打印不判定。末尾两条自检各对应上面一条相对判据——把
# 该判据抓的缺陷形态造进日志，绝对判据必须同样报红。
#
# 用法：./run_boundary.sh [workdir]        （默认 /tmp/gapsb）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/gapsb}
STATION=${STATION:-T1}
GAPSPEC=${GAPSPEC:-"0.52:30"}		# 场景 A：跨界但不覆盖读取位置
OVERSPEC=${OVERSPEC:-"0.48:30"}		# 场景 B：覆盖读取位置
FPS=${FPS:-250}				# test.vex：8 Ms/s ÷ 每帧 32000 采样
SUBINT_SEC=${SUBINT_SEC:-0.524288}	# test.input 的 subintNS
TAMPER_SUBINT=${TAMPER_SUBINT:-2}	# 自检改动的 subint：A 的跨界者、B 的被盖住者
TAMPER_DELTA=${TAMPER_DELTA:-30}	# 自检 1 少报的帧数（= 场景 A 的缺口帧数）

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

# READPOS 行（verbose 级）→ 每行的 "subint:firstfno"
readpos_series()
{
	awk '/^READPOS subint / { for(i=1;i<=NF;i++){ if($i=="subint"){s=$(i+1)} if($i=="firstfno"){f=$(i+1)} } sub(/:$/,"",s); print s":"f }' "$1"
}

# .sp 的每 subint 零权重块区间 → "subint:块数:首块-末块,..."（见 data-spec 5.3）
zero_weight_runs()
{
	python3 - "$1" <<-'PY'
	import struct, sys
	d = open(sys.argv[1], "rb").read()
	nchan, = struct.unpack_from("<I", d, 16)
	nsub, subns, bps, nbf, fw = struct.unpack_from("<IIIII", d, 44)
	rec = 12 + 4*fw + 4*bps + 8*bps*nchan
	off = 256
	for s in range(nsub):
	    w = struct.unpack_from("<%df" % bps, d, off + 12 + 4*fw)
	    runs = []
	    for i in [i for i, x in enumerate(w) if x == 0.0]:
	        if runs and i == runs[-1][1] + 1:
	            runs[-1][1] = i
	        else:
	            runs.append([i, i])
	    print("%d:%d:%s" % (s+1, bps, ";".join("%d-%d" % (a, b) for a, b in runs) or "-"))
	    off += rec
	PY
}

# 生成数据（gapspec 为空 = 无缺口）→ 跑一个站 → READPOS 序列写 $2、零权重写 $3
generate_and_run()
{
	local gapspec=$1 readposout=$2 zeroout=$3
	# make_testdata.sh 对已存在的 VDIF 会跳过（幂等），换缺口重生成前必须先删
	rm -f "$WORKDIR"/raw/*/*.vdif
	FXSIM_NOISE=${FXSIM_NOISE:-0} FXSIM_GAPS="$gapspec" \
		"$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
	# batch_id 只在生成之后才知道（workdir 可以是空的）
	BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
	rm -rf "$WORKDIR/fengine"
	# DATA TABLE 的路径相对进程 cwd 解析（fxcorr-f/CLAUDE.md），必须在 workdir 里跑
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) > "$readposout" 2>&1 ||
		fail "fxcorr-f 退出码非零（见 $readposout）"
	zero_weight_runs "$WORKDIR/fengine/$BATCHID/$STATION/ds_0/band_00.sp" > "$zeroout"

	# 判据二：check_reader 有 finding 时退出码为 1，另存供 require_absolute 判定
	VDIF="$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif"
	BATCHJSON="$WORKDIR/batches/$BATCHID.json"
	ABS_RC=0
	python3 "$CHECKER" --log "$readposout" --vdif "$VDIF" --batch-json "$BATCHJSON" \
		--fps "$FPS" > "$readposout.check" 2>&1 || ABS_RC=$?
}

# 判据二（绝对）：$1 那次运行的 check_reader 结果。E1–E3 有 finding 时退出码非零；
# E4 的净损失不参与 nbad 计数（不看退出码），所以两条分开查。
require_absolute()
{
	local log=$1 lost
	if [ "$ABS_RC" != 0 ]; then
		grep -E "^(  sub |summary)" "$log.check" | sed 's/^/  /' >&2
		fail "$log: check_reader 报了 finding（rc=$ABS_RC，见 $log.check）"
	fi
	lost=$(awk '/^E4 coverage/{print $4}' "$log.check")
	[ "$lost" = 0 ] ||
		fail "$log: E4 报 $lost 帧净损失（两个场景都是缺口形式，没有占字节的 filler）"
	grep -E "^(E5 anchor|summary|E4 coverage)" "$log.check" | sed 's/^/  /'
}

check_firstfno()
{
	echo "=== 判据 1（场景 A，$GAPSPEC）：firstfno 不得早于对照 ==="
	local status=0 sub nofno gfno
	while IFS=: read -r sub nofno; do
		gfno=$(echo "$GAPSERIES" | awk -F: -v s="$sub" '$1==s {print $2}')
		[ -n "$gfno" ] || fail "有缺口那次运行缺 subint $sub 的 READPOS"
		if [ "$gfno" -lt "$nofno" ]; then
			echo "  subint $sub: firstfno $gfno < 对照 $nofno  ✗（读到了早于本 subint 起点的数据）"
			status=1
		else
			echo "  subint $sub: firstfno $gfno >= 对照 $nofno  ✓"
		fi
	done <<< "$NOGAPSERIES"
	[ $status -eq 0 ] || fail "跨界的 subint 被多减（B5 第 1 条）——见 reader-model.md 4.5"
}

check_overstart()
{
	echo "=== 判据 2（场景 B，$OVERSPEC）：被缺口盖住起点的 subint 从第 0 块起无效 ==="
	local nofr gapfr bps frames expect
	nofr=$(echo "$NOGAPSERIES" | awk -F: '$1==2 {print $2}')
	gapfr=$(echo "$OVERSERIES" | awk -F: '$1==2 {print $2}')
	[ -n "$gapfr" ] || fail "场景 B 缺 subint 2 的 READPOS"
	if [ "$gapfr" -le "$nofr" ]; then
		fail "场景 B 的 subint 2 读取位置没有落到缺口之后（firstfno $gapfr 对 $nofr）"
	fi
	# 读取位置跳过的那 22 帧正是这个 subint 开头没有数据的部分，块数按
	# payloadbytes/blockbytes = blocks_per_send / (subint 帧数) 换算
	bps=$(echo "$OVERZERO" | awk -F: '$1==2 {print $2}')
	frames=$(echo "$OVERZERO" | awk -F: '$1==2 {print $3}' | awk -F'[;-]' '{print $2+1}')
	expect=$(awk -v d="$((gapfr - nofr))" -v bps="$bps" -v fps="$FPS" -v ss="$SUBINT_SEC" \
	          'BEGIN { printf "%d", d*bps/(ss*fps) + 0.5 }')
	[ -n "$frames" ] || fail "场景 B 的 subint 2 没有零权重块——buffer 起点的帧号偏移没有被用上"
	first=$(echo "$OVERZERO" | awk -F: '$1==2 {print $3}' | cut -d';' -f1)
	case "$first" in
	0-*) ;;
	*) fail "场景 B 的 subint 2 零权重块不从第 0 块开始（$first）——洞被标到了错位置" ;;
	esac
	if [ "$frames" -lt $((expect - 2)) ] || [ "$frames" -gt $((expect + 2)) ]; then
		fail "场景 B 的 subint 2 零权重块数 $frames，应为 $expect（$((gapfr - nofr)) 帧 × $bps 块/帧 ÷ 子带帧数）"
	fi
	echo "  subint 2: firstfno $gapfr（对照 $nofr，跳过 $((gapfr - nofr)) 帧），零权重块 0-$((frames - 1)) = $frames 块，期望 $expect  ✓"
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好，见本目录 README 的坑）"
[ -f "$CHECKER" ] || fail "找不到 $CHECKER"

echo "--- 无缺口（对照）---"
generate_and_run "" "$WORKDIR/nogap.log" "$WORKDIR/nogap.zero"
NOGAPSERIES=$(readpos_series "$WORKDIR/nogap.log")
echo "$NOGAPSERIES"
require_absolute "$WORKDIR/nogap.log"

echo "--- 场景 A：FXSIM_GAPS=$GAPSPEC ---"
generate_and_run "$GAPSPEC" "$WORKDIR/gap.log" "$WORKDIR/gap.zero"
GAPSERIES=$(readpos_series "$WORKDIR/gap.log")
echo "$GAPSERIES"
grep -q "missing frames 30 filler frames 0" "$WORKDIR/gap.log" ||
	{ grep "GAPCHECK summary" "$WORKDIR/gap.log" >&2; fail "场景 A 的 missing frames 应为 30、filler 应为 0"; }
grep "GAPCHECK summary" "$WORKDIR/gap.log"
require_absolute "$WORKDIR/gap.log"
check_firstfno

echo
echo "--- 判据二自检 1（对应判据 1）：把 subint $TAMPER_SUBINT 的 firstfno 减 $TAMPER_DELTA（模拟 B5 未修的多减）---"
python3 - "$WORKDIR/gap.log" "$WORKDIR/tamper_a.log" "$TAMPER_SUBINT" "$TAMPER_DELTA" <<'PY'
import re, sys
log, out, sub, delta = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
def fix(m):
    if int(m.group(1)) != sub:
        return m.group(0)
    return re.sub(r'firstfno (-?\d+)', lambda x: 'firstfno %d' % (int(x.group(1)) - delta),
                  m.group(0), count=1)
open(out, 'w').write(re.sub(r'READPOS subint (\d+):.*', fix, open(log).read()))
PY
if python3 "$CHECKER" --log "$WORKDIR/tamper_a.log" --vdif "$VDIF" \
	--batch-json "$BATCHJSON" --fps "$FPS" > "$WORKDIR/tamper_a.log.check" 2>&1; then
	sed 's/^/  /' "$WORKDIR/tamper_a.log.check" | tail -4
	fail "自检 1 失败：firstfno 少报 $TAMPER_DELTA 帧之后判据竟然没报红（判据是恒绿的吗？）"
fi
grep -q "firstfno .* vs file" "$WORKDIR/tamper_a.log.check" ||
	fail "自检 1 报红了，但不是 E2（报的 firstfno 与文件里的帧号不符）——见 $WORKDIR/tamper_a.log.check"
grep -E "^(summary)" "$WORKDIR/tamper_a.log.check" | sed 's/^/  /'

echo "--- 场景 B：FXSIM_GAPS=$OVERSPEC ---"
generate_and_run "$OVERSPEC" "$WORKDIR/over.log" "$WORKDIR/over.zero"
OVERSERIES=$(readpos_series "$WORKDIR/over.log")
echo "$OVERSERIES"
grep -q "missing frames 30 filler frames 0" "$WORKDIR/over.log" ||
	{ grep "GAPCHECK summary" "$WORKDIR/over.log" >&2; fail "场景 B 的 missing frames 应为 30、filler 应为 0"; }
grep "GAPCHECK summary" "$WORKDIR/over.log"
require_absolute "$WORKDIR/over.log"
OVERZERO=$(cat "$WORKDIR/over.zero")
check_overstart

echo
echo "--- 判据二自检 2（对应判据 2）：清空 subint $TAMPER_SUBINT 的 GAPCHECK holes（模拟洞被当数据积分）---"
python3 - "$WORKDIR/over.log" "$WORKDIR/tamper_b.log" "$TAMPER_SUBINT" <<'PY'
import re, sys
log, out, buf = sys.argv[1], sys.argv[2], int(sys.argv[3])
def clear(m):
    return m.group(0) if int(m.group(1)) != buf else 'GAPCHECK holes buf %s:' % m.group(1)
open(out, 'w').write(re.sub(r'GAPCHECK holes buf (\d+):.*', clear, open(log).read()))
PY
if python3 "$CHECKER" --log "$WORKDIR/tamper_b.log" --vdif "$VDIF" \
	--batch-json "$BATCHJSON" --fps "$FPS" > "$WORKDIR/tamper_b.log.check" 2>&1; then
	sed 's/^/  /' "$WORKDIR/tamper_b.log.check" | tail -4
	fail "自检 2 失败：把洞清掉之后判据竟然没报红（判据是恒绿的吗？）"
fi
grep -q "missing [1-9]" "$WORKDIR/tamper_b.log.check" ||
	fail "自检 2 报红了，但不是漏标（missing slots）——见 $WORKDIR/tamper_b.log.check"
grep -E "^(summary)" "$WORKDIR/tamper_b.log.check" | sed 's/^/  /'

echo
echo "PASS: 缺口跨 subint 边界时读取位置与无效块都落在正确的位置（相对判据），文件真值对账也干净（绝对判据）"
