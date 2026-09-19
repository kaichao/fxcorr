#!/usr/bin/env bash
# run_filler.sh —— filler 长度远大于真实缺口的记录中断（reader-model.md 4.6/4.7）
#
# t25362 的 BA ds_2 是这种形态：每次中断写了 81..508 帧全零 filler，而帧号只跳过
# 10..63 帧。此前 FXSIM_GAPS 只能造出"filler 帧数 == 丢失帧数"（`:f`），而这种
# 长 filler 正是把读位置带偏的形态：
#
#   * filler 段让 fillershiftbytes 一次涨掉整段（几百帧），下一次读取位置随之
#     前跳几百帧，中间那段从未被扫过；
#   * 跳过的区段里藏着真正的缺口，它被补扫发现时，当前这次读取的位置早已算好
#     （缺口只对后续 subint 生效）；
#   * 更糟的是 `gapshiftAt` 把 `gapspan` 里的**文件偏移**换算成时间轴槽位时，
#     只补了缺口之前的丢失帧、没扣缺口之前的 filler 帧数，于是缺口的槽位算大了
#     filler_before，要等 t 追上去才生效——这段时间里读位置一直多走。
#
# 表现为 `READPOS ... dst D`（D>0）与 `GAPSHIFT ... holes [0,D)`：缓冲区开头
# D 帧被标成空洞，而这些帧的数据是好的，只是被前移的读位置甩在了别处。
#
# 判据一（相对，两种形式描述同一段时间，这是硬约束）：
#   **filler 形式的无效块总数必须等于缺口形式的**，且都等于
#   `缺失帧数 × 块/帧`（test.vex 下次子带 131.072 帧/subint、2048 块/subint
#   → 15.625 块/帧）。读取位置要是被带偏，filler 形式会多出一批无效块。
#
# 判据二（绝对，`check_reader.py`）：把 VDIF 文件本身当真值，判 E1 定位 / E2 数据 /
#   E3 落点无 finding、E4 无净损失。两套判据并存（`fxcorr/v4-plan.md` 的 A3）——相对
#   判据的两侧错成一样就抓不到，绝对判据用文件真值补上；**两者必须同时绿**。E5 锚点
#   在含缺口/filler 的数据上按设计跳过（B/C 类的读取位置本就该偏离名义时间轴），只
#   打印不判定。末尾的自检把读位置整体前移一次，证明绝对判据对该形态能报红。
#
# 用法：./run_filler.sh [workdir]     （默认 /tmp/gapsf）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/gapsf}
STATION=${STATION:-T1}
GAPSPEC=${GAPSPEC:-"1.0:40"}		# 缺口形式：该处只缺 40 帧
FILLSPEC=${FILLSPEC:-"1.0:40:f400"}	# filler 形式：同一位置缺 40 帧、写 400 帧全零
MISSING=${MISSING:-40}			# 两种形式共同的真实缺失帧数
FPS=${FPS:-250}				# test.vex：8 Ms/s ÷ 每帧 32000 采样
SUBINT_SEC=${SUBINT_SEC:-0.524288}	# test.input 的 subintNS
FRAMEBYTES=${FRAMEBYTES:-8032}		# test.vex：payload 8000 + 32 字节帧头
SHIFT=${SHIFT:-400}			# 自检：读位置整体前移的帧数（= filler 段长度）

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

# .sp 每 subint 的无效块数（valid_flags 里为 0 的块）→ "subint:块数"，另给 bps/total
invalid_per_subint()
{
	python3 "$SCRIPTDIR/sp_valid.py" "$1" --machine
}

generate_and_run()
{
	local gapspec=$1 log=$2
	# make_testdata.sh 对已存在的 VDIF 幂等跳过，换缺口重生成前必须先删
	rm -f "$WORKDIR"/raw/*/*.vdif
	FXSIM_NOISE=${FXSIM_NOISE:-0} FXSIM_GAPS="$gapspec" \
		"$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
	BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
	rm -rf "$WORKDIR/fengine"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) > "$log" 2>&1 ||
		fail "fxcorr-f 退出码非零（见 $log）"
	invalid_per_subint "$WORKDIR/fengine/$BATCHID/$STATION/ds_0/band_00.sp" > "$log.invalid"

	# 判据二：check_reader 有 finding 时退出码为 1，另存供 require_absolute 判定
	VDIF="$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif"
	BATCHJSON="$WORKDIR/batches/$BATCHID.json"
	ABS_RC=0
	python3 "$CHECKER" --log "$log" --vdif "$VDIF" --batch-json "$BATCHJSON" \
		--fps "$FPS" > "$log.check" 2>&1 || ABS_RC=$?
}

# 缺失帧数换算成块数：块/帧 = bps ÷ (subint 秒数 × fps)，bps 由 .sp 头给出
expect_blocks()
{
	awk -v m="$MISSING" -v ss="$SUBINT_SEC" -v f="$FPS" -v bps="$1" \
		'BEGIN { printf "%d", m*bps/(ss*f) + 0.5 }'
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
		fail "$log: E4 报 $lost 帧净损失（本场景被跳过的区段全是 filler，应为 0）"
	grep -E "^(E5 anchor|summary|E4 coverage)" "$log.check" | sed 's/^/  /'
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好，见本目录 README 的坑）"
[ -f "$CHECKER" ] || fail "找不到 $CHECKER"

# 无中断对照：给出与中断无关的常数无效块（subint 1 的头块等），两种形式都含它
echo "--- 无中断（对照）---"
generate_and_run "" "$WORKDIR/none.log"
grep "GAPCHECK summary" "$WORKDIR/none.log" | sed 's/^/  /'
require_absolute "$WORKDIR/none.log"
CTRLTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/none.log.invalid")

echo "--- 缺口形式：FXSIM_GAPS=$GAPSPEC ---"
generate_and_run "$GAPSPEC" "$WORKDIR/gap.log"
grep -q "missing frames $MISSING filler frames 0" "$WORKDIR/gap.log" ||
	{ grep "GAPCHECK summary" "$WORKDIR/gap.log" >&2; fail "缺口形式应报 missing frames $MISSING / filler 0"; }
grep "GAPCHECK summary" "$WORKDIR/gap.log" | sed 's/^/  /'
require_absolute "$WORKDIR/gap.log"

echo "--- filler 形式：FXSIM_GAPS=$FILLSPEC ---"
generate_and_run "$FILLSPEC" "$WORKDIR/fill.log"
grep -q "missing frames $MISSING filler frames 400" "$WORKDIR/fill.log" ||
	{ grep "GAPCHECK summary" "$WORKDIR/fill.log" >&2; fail "filler 形式应报 missing frames $MISSING / filler 400"; }
grep "GAPCHECK summary" "$WORKDIR/fill.log" | sed 's/^/  /'
require_absolute "$WORKDIR/fill.log"

GAPTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/gap.log.invalid")
FILLTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/fill.log.invalid")
BPS=$(awk -F: '$1=="bps"{print $2}' "$WORKDIR/gap.log.invalid")
EXPECT=$(expect_blocks "$BPS")

echo
echo "逐 subint 无效块（缺口形式是同一缺失时间的诚实形态，filler 形式必须与它一致）："
paste -d' ' <(grep -v -E '^(bps|total)' "$WORKDIR/gap.log.invalid") \
            <(grep -v -E '^(bps|total)' "$WORKDIR/fill.log.invalid") |
awk -F'[: ]+' '{ printf "  subint %s: 缺口 %4d  filler %4d\n", $1, $2, $4 }'
echo "总数：无中断 $CTRLTOTAL，缺口 $GAPTOTAL，filler $FILLTOTAL（$MISSING 帧 ≈ $EXPECT 块）"

# 参照：缺口形式比无中断形式多出的部分，应当就是缺口本身（留 ±4 块余量吸收取整）
GAPEXTRA=$((GAPTOTAL - CTRLTOTAL))
[ "$GAPEXTRA" -ge $((EXPECT - 4)) ] && [ "$GAPEXTRA" -le $((EXPECT + 4)) ] ||
	fail "缺口形式比无中断形式多 $GAPEXTRA 块，应为 $EXPECT（参照本身就不对，先查缺口形式）"

# 主判据：逐 subint 一致（±2 块）
awk -F: 'NR==FNR { if ($1!="total" && $1!="bps") g[$1]=$2; next }
         $1=="total" || $1=="bps" { next }
         { d=$2-g[$1]; if(d<0) d=-d
           if(d>2) { printf "  subint %s: filler %d 块 vs 缺口 %d 块（差 %d）\n", $1, $2, g[$1], d; bad=1 } }
         END { exit bad }' "$WORKDIR/gap.log.invalid" "$WORKDIR/fill.log.invalid" || {
	fail "filler 形式的无效块与缺口形式不一致（上面列出的 subint）：读位置被 filler 带偏，好数据被标成了空洞"
}

echo
echo "--- 判据二自检：读位置整体前移 $SHIFT 帧（模拟被长 filler 带偏）---"
# 判据能报红这件事本身也要能复现，否则它退化成恒绿（同 run_startoffset.sh）
python3 - "$WORKDIR/fill.log" "$WORKDIR/tamper.log" "$SHIFT" "$FRAMEBYTES" <<'PY'
import re, sys
log, out, frames, framebytes = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
delta = frames * framebytes
def shift(m):
    return m.group(0).replace(m.group(2), str(int(m.group(2)) + delta), 1)
open(out, 'w').write(re.sub(r'READPOS subint (\d+): readoff (-?\d+)', shift, open(log).read()))
PY
if python3 "$CHECKER" --log "$WORKDIR/tamper.log" --vdif "$VDIF" \
	--batch-json "$BATCHJSON" --fps "$FPS" > "$WORKDIR/tamper.log.check" 2>&1; then
	sed 's/^/  /' "$WORKDIR/tamper.log.check" | tail -4
	fail "自检失败：读位置前移 $SHIFT 帧之后判据竟然没报红（判据是恒绿的吗？）"
fi
grep -E "^(summary|E4 coverage)" "$WORKDIR/tamper.log.check" | sed 's/^/  /'

echo
echo "PASS: 长 filler 中断下，两种形式标出的无效数据量一致（相对判据），文件真值对账也干净（绝对判据）"
