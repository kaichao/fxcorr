#!/usr/bin/env bash
# run_invalid.sh —— VDIF 的 invalid 位：帧在位、数据不可用（reader-model.md 4.12）
#
# 这一条与其它 filler 形态**不是一回事**：全零头与 FILL_PATTERN 是"帧里没有数据"
# （不占时间轴），而 invalid 位说的是"**这一帧在时间轴上存在、但数据不可信**"——帧号
# 照常推进、槽照占。2026-09-19 实测（同一份数据、只把一段的 invalid 位置 1）：
#
#   形态                         上游 mpifxcorr 的基准 SWIN      fxcorr（修之前）
#   原样                         6 条记录、时间戳递增            正常
#   中段 229 帧 invalid 位 = 1   **记录数与时间戳逐条相同**、     **时间轴压缩 229 帧**：
#                                仅 weight 打折（0.4768/0.6389    subint 2/3 读同一 readoff
#                                对 0.9893/1.0）                  2843328，missing/filler 各 229
#
# 上游的处置见 `vdifmux.c:905-940`（无效线程的指针指向 `src`、输出头写 `validitymask`、
# 计数进 `nPartialOutput`）。定案：fxcorr 让 invalid 帧**占时间槽**、并把该帧对应的块
# **标无效**（数据不进积分），见 `reader-model.md` 4.12 与 `fxcorr/v5-plan.md` 的 P3。
#
# 生成器还不支持这一形态：`FXSIM_GAPS` 的 `:f`/`:p`/`:h` 都是"不占时间轴"的占位帧，
# 而 invalid 是"占槽"——语义不同，不该塞进同一个参数里。本脚本用**后处理**造：把一段
# 帧的 word0 最高位置 1，帧号与时间轴一个字节都不动。
#
# 判据：
#   * **位置**：invalid 形态的 `READPOS` 序列与对照**逐行相同**——这是"帧没被吞掉"的
#     指纹（修之前这一条最先崩：readoff 前移、相邻 subint 读同一段）；
#   * **数据**：invalid 形态的无效块**逐 subint 不少于**对照，且至少一个 subint 严格
#     更多（数据被清掉了）；
#   * **真值**：`check_reader.py` 零 finding（真值把 invalid 槽算作"应当无效的槽"）；
#   * **自检**：把 invalid 形态的读位置整体前移 N 帧，绝对判据必须报红。
#
# 用法：./run_invalid.sh [workdir]     （默认 /tmp/gapsi）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/gapsi}
STATION=${STATION:-T1}
START=${START:-125}		# 置 invalid 位的起始帧（文件内帧序，0-based）
COUNT=${COUNT:-229}		# 置位帧数
FPS=${FPS:-250}			# test.vex：8 Ms/s ÷ 每帧 32000 采样
FRAMEBYTES=${FRAMEBYTES:-8032}	# test.vex：payload 8000 + 32 字节帧头
SHIFT=${SHIFT:-$COUNT}		# 自检：读位置整体前移的帧数

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

# 生成 → 跑 f（verbose）→ 无效块分布 + 真值对账
run_one()
{
	local log=$1
	rm -rf "$WORKDIR/fengine"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) > "$log" 2>&1 ||
		fail "fxcorr-f 退出码非零（见 $log）"
	python3 "$SCRIPTDIR/sp_valid.py" \
		"$WORKDIR/fengine/$BATCHID/$STATION/ds_0/band_00.sp" --machine > "$log.invalid"
	ABS_RC=0
	python3 "$CHECKER" --log "$log" --vdif "$VDIF" --batch-json "$BATCHJSON" \
		--fps "$FPS" > "$log.check" 2>&1 || ABS_RC=$?
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好，见本目录 README 的坑）"
[ -f "$CHECKER" ] || fail "找不到 $CHECKER"

echo "--- 无中断对照 ---"
rm -f "$WORKDIR"/raw/*/*.vdif
FXSIM_NOISE=${FXSIM_NOISE:-0} "$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
VDIF="$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif"
BATCHJSON="$WORKDIR/batches/$BATCHID.json"
run_one "$WORKDIR/ctrl.log"
grep "GAPCHECK summary" "$WORKDIR/ctrl.log" | sed 's/^/  /'
[ "$ABS_RC" = 0 ] || fail "对照就有 finding（判据本身有问题，见 ctrl.log.check）"

echo "--- 把帧 $START..$((START + COUNT - 1))（$COUNT 帧）的 invalid 位置 1（帧号与时间轴不动）---"
python3 - "$VDIF" "$START" "$COUNT" <<'PYEOF'
import struct, sys
path, start, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, 'r+b') as f:
    framebytes = (struct.unpack('<I', f.read(32)[8:12])[0] & 0xFFFFFF) * 8
    for i in range(start, start + n):
        f.seek(i * framebytes)
        w0 = struct.unpack('<I', f.read(4))[0]
        f.seek(i * framebytes)
        f.write(struct.pack('<I', w0 | 0x80000000))
print("  %d 帧已置位（framebytes=%d）" % (n, framebytes))
PYEOF
run_one "$WORKDIR/inv.log"
grep "GAPCHECK summary" "$WORKDIR/inv.log" | sed 's/^/  /'

# 判据一：位置序列与对照逐行相同
if ! diff <(grep "^READPOS" "$WORKDIR/ctrl.log") <(grep "^READPOS" "$WORKDIR/inv.log") > "$WORKDIR/readpos.diff"; then
	sed 's/^/  /' "$WORKDIR/readpos.diff" >&2
	fail "invalid 形态的读取位置与对照不同——invalid 帧被当成了 filler（时间轴被压缩，见 reader-model.md 4.12）"
fi
echo "  读取位置：与对照逐行相同 ✓"

# 判据二：无效块逐 subint 不少于对照，且至少一个 subint 更多
paste -d' ' <(grep -v -E '^(bps|total)' "$WORKDIR/ctrl.log.invalid") \
            <(grep -v -E '^(bps|total)' "$WORKDIR/inv.log.invalid") |
awk -F'[: ]+' '{ printf "  subint %s: 对照 %4d  invalid %4d\n", $1, $2, $4
                 if ($4 < $2) bad = 1
                 if ($4 > $2) more = 1 }
               END { exit (bad || !more) }' || {
	fail "invalid 形态的无效块没有严格多于对照（数据没被清掉），或有 subint 反而更少"
}
CTRLTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/ctrl.log.invalid")
INVTOTAL=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/inv.log.invalid")
echo "  无效块总数：对照 $CTRLTOTAL，invalid $INVTOTAL"

# 判据三：真值对账
if [ "$ABS_RC" != 0 ]; then
	grep -E "^(  sub |summary)" "$WORKDIR/inv.log.check" | sed 's/^/  /' >&2
	fail "invalid 形态：check_reader 报了 finding（rc=$ABS_RC，见 inv.log.check）"
fi
grep -E "^(vdif|summary|E4 coverage)" "$WORKDIR/inv.log.check" | sed 's/^/  /'

# 自检：判据能报红
echo "--- 自检：把 invalid 形态的读位置整体前移 $SHIFT 帧 ---"
python3 - "$WORKDIR/inv.log" "$WORKDIR/tamper.log" "$SHIFT" "$FRAMEBYTES" <<'PY'
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
echo "PASS: invalid 位帧占着时间槽（读取位置与对照逐条相同）、数据被清（无效块增加）、"
echo "      真值对账干净，判据自检能报红"
