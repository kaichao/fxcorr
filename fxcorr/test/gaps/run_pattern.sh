#!/usr/bin/env bash
# run_pattern.sh —— 占位帧的字节形态：全零头 vs vdifio 的 FILL_PATTERN（reader-model.md 4.8）
#
# `vdifmux` 认两种"非数据"的字节模式：FILL_PATTERN（0x11223344）落在帧的**最后 4 字节**
# 时跳整帧（vdifmux.c:598），落在**最前 4 字节**时只跳 8 字节（:606）、随后逐字节重新
# 同步。t25362 的 filler 是"帧头全零"那一种（4.8 的形态清单），另两种 fxcorr 此前完全
# 不认——遇到就按数据帧读，header 里的 0x11223344 被读成帧号，占位帧被算成缺口。
#
# 2026-09-19 实测（同一段时间造三次，只换占位帧的字节；上游要 mpifxcorr，见下）：

#   形态              上游 mpifxcorr 基准 SWIN       fxcorr（认 pattern 之前）
#   f 全零头         90e54869…（基准）               正常：missing 28 / filler 229
#   p 整帧 pattern   90e54869…（**逐字节相同**）     missing 223 / filler 0、2 buffers
#   h 帧首 pattern   90e54869…（**逐字节相同**）     missing 277 / filler 0、3 buffers
#
# 上游对三者一视同仁（帧尾判据直接跳整帧；帧首判据跳 8 字节后逐字节重新同步，净效果同样
# 是跳掉整帧），所以 fxcorr 也按整帧认（4.8 的定案）。本脚本把这条对照固化成判据：
#
#   * 计数：三种形态的 `GAPCHECK summary` 逐字段相同——**都报 filler 229 / missing 28**，
#     "认成 filler 而不是缺口"正是这个缺陷的指纹；
#   * 相对：三种形态**逐 subint 无效块一致**（描述同一段时间，占位帧只占字节）；
#   * 绝对：三种形态各跑 `check_reader.py`，E1–E3 无 finding、E4 = 0；
#   * 自检：把 p 形式的读位置整体前移 229 帧（= 把占位帧当时间轴损失的语义），绝对判据
#     必须报红。
#
# 上游那一列需要 mpifxcorr，不在本脚本里跑（判据用不到它）：复现命令是
# `fxcorr/run_bench.sh <workdir>` 三次，比对 `bench/*.difx/DIFX_*.s0000.b0000` 的 md5。
#
# 用法：./run_pattern.sh [workdir]     （默认 /tmp/gapsp）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/gapsp}
STATION=${STATION:-T1}
MISSING=${MISSING:-28}			# 三种形态共同的真实缺失帧数
FILLER=${FILLER:-229}			# 占位帧数（三者相同，只有字节内容不同）
FPS=${FPS:-250}				# test.vex：8 Ms/s ÷ 每帧 32000 采样
FRAMEBYTES=${FRAMEBYTES:-8032}		# test.vex：payload 8000 + 32 字节帧头
SHIFT=${SHIFT:-$FILLER}			# 自检：读位置整体前移的帧数（= 占位帧数）

# form | FXSIM_GAPS spec
FORMS=(
	"f|0.50:28:f229"
	"p|0.50:28:p229"
	"h|0.50:28:h229"
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

run_one()
{
	local spec=$1 log=$2
	# make_testdata.sh 对已存在的 VDIF 幂等跳过，换形态重生成前必须先删
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

REFSYNC=
for row in "${FORMS[@]}"; do
	IFS='|' read -r form spec <<< "$row"
	echo "--- 形态 $form：FXSIM_GAPS=$spec ---"

	run_one "$spec" "$WORKDIR/$form.log"
	# 自检只对 p 形式做，用的是**它自己**那次的 VDIF（run_one 的全局变量到最后
	# 一次迭代已换成别的形态，拿错文件就会在对不上的地方报红）
	if [ "$form" = p ]; then
		PVDIF=$VDIF
		PBATCHJSON=$BATCHJSON
	fi

	# 判据一：占位帧被认成 filler（的计数指纹）
	grep -q "missing frames $MISSING filler frames $FILLER" "$WORKDIR/$form.log" || {
		grep "GAPCHECK summary" "$WORKDIR/$form.log" >&2
		fail "形态 $form 应报 missing frames $MISSING / filler frames $FILLER —— 占位帧没被当成 filler"
	}
	grep "GAPCHECK summary" "$WORKDIR/$form.log" | sed 's/^/  /'

	# 判据二：绝对判据（E1–E3 无 finding、E4 = 0）
	if [ "$ABS_RC" != 0 ]; then
		grep -E "^(  sub |summary)" "$WORKDIR/$form.log.check" | sed 's/^/  /' >&2
		fail "形态 $form：check_reader 报了 finding（rc=$ABS_RC，见 $form.log.check）"
	fi
	grep -E "^(summary|E4 coverage)" "$WORKDIR/$form.log.check" | sed 's/^/  /'
	lost=$(awk '/^E4 coverage/{print $4}' "$WORKDIR/$form.log.check")
	[ "$lost" = 0 ] || fail "形态 $form：E4 报 $lost 帧净损失"

	# 判据三：三种形态逐 subint 无效块一致（f 为基准）
	if [ -z "$REFSYNC" ]; then
		REFSYNC="$WORKDIR/$form.log.invalid"
	else
		awk -F: 'NR==FNR { if ($1!="total" && $1!="bps") g[$1]=$2; next }
		         $1=="total" || $1=="bps" { next }
		         { d=$2-g[$1]; if(d<0) d=-d
		           if(d>2) { printf "  subint %s: %s 块 vs 基准 %d 块（差 %d）\n", $1, $2, g[$1], d; bad=1 } }
		         END { exit bad }' "$REFSYNC" "$WORKDIR/$form.log.invalid" || {
			fail "形态 $form 的无效块与全零头形式不一致——占位帧的字节形态不该改变落点"
		}
	fi
done

echo
echo "逐 subint 无效块（三种形态描述同一段时间，必须逐项相同）："
paste -d' ' <(grep -v -E '^(bps|total)' "$WORKDIR/f.log.invalid") \
            <(grep -v -E '^(bps|total)' "$WORKDIR/p.log.invalid") \
            <(grep -v -E '^(bps|total)' "$WORKDIR/h.log.invalid") |
awk -F'[: ]+' '{ printf "  subint %s:  零头 %4d   整帧 pattern %4d   帧首 pattern %4d\n", $1, $2, $4, $6 }'

echo
echo "--- 自检：把 p 形式的读位置整体前移 $SHIFT 帧（= 把占位帧当时间轴损失的语义）---"
python3 - "$WORKDIR/p.log" "$WORKDIR/tamper.log" "$SHIFT" "$FRAMEBYTES" <<'PY'
import re, sys
log, out, frames, framebytes = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
delta = frames * framebytes
def shift(m):
    return m.group(0).replace(m.group(2), str(int(m.group(2)) + delta), 1)
open(out, 'w').write(re.sub(r'READPOS subint (\d+): readoff (-?\d+)', shift, open(log).read()))
PY
if python3 "$CHECKER" --log "$WORKDIR/tamper.log" --vdif "$PVDIF" \
	--batch-json "$PBATCHJSON" --fps "$FPS" > "$WORKDIR/tamper.log.check" 2>&1; then
	sed 's/^/  /' "$WORKDIR/tamper.log.check" | tail -4
	fail "自检失败：读位置前移 $SHIFT 帧之后判据竟然没报红（判据是恒绿的吗？）"
fi
grep -E "^(summary|E4 coverage)" "$WORKDIR/tamper.log.check" | sed 's/^/  /'

echo
echo "PASS: 三种占位帧形态（全零头 / 整帧 FILL_PATTERN / 帧首 FILL_PATTERN）"
echo "      被同样认成 filler——计数、落点、真值对账三者一致，判据自检能报红"
