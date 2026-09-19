#!/usr/bin/env bash
# run_window.sh —— 读取窗口被 filler 挤占，段后数据落在窗口之外（reader-model.md 4.7）
#
# t25362 的 BA ds_2 里，filler 段的**后面**还跟着数据，而定位读按字节数读一段
# **连续**区域（sendbytes）：窗口跨进 filler 段时，被 filler 占掉的宽度不会补回来，
# 段后那段数据就落在窗口之外——读不到，也不报错。`shiftFrameGaps` 只是诚实地把没
# 填满的尾部槽标成无效（表现为 E3 的 extra：洞的尾巴一直延伸到缓冲区末尾），而
# 那些帧本身从未进入任何窗口（E4 的净损失）。前三次修复（B5、D-a、D-b）都只处理
# 读位置的修正量，**没有处理窗口长度**，这是定位读相对顺序读多出来的语义缺口：
# 上游 `vdifmux` 顺序读会滑过 filler 继续填满输出缓冲，所以 mpifxcorr 没有这个形态。
#
# 复现条件（test.vex：subint 131.072 帧、fps 250、framebytes 8032）：
#
#     净损失 = span - (A - t_k) - G      （> 0 才复现）
#
# A = filler 段的时间轴帧号（FXSIM_GAPS 的秒数 × fps），t_k = 它所在 subint 的窗口
# 起点，G = 同一次中断的真实缺口帧数。现有 `run_filler.sh` 用的 `1.0:40:f400`
# （A=250，落在 subint 1 窗口的后部）算出来是 **-27.9**，所以那个场景 E4 = 0；
# 把中断前移到窗口前部（`0.6:40:f400`，A=150，t_k=131）给出 131-19-40 = **69**，
# 与实测的 E4 = 69 帧逐项吻合。
#
# 判据（绝对，不依赖基准）：`check_reader.py` 的 **E4 = 0**
#   * 复现形态：修复前报 69 帧净损失 → 红；修复后应转绿
#   * 无中断对照：E4 = 0（判据不误报的检验）
#
# 用法：./run_window.sh [workdir]     （默认 /tmp/gapsw，目录须先存在）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/gapsw}
STATION=${STATION:-T1}
FILLSPEC=${FILLSPEC:-"0.6:40:f400"}	# filler 段（400 帧）落在 subint 1 窗口的前部
FPS=${FPS:-250}				# test.vex：8 Ms/s ÷ 每帧 32000 采样

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

# 生成 → 跑 f（verbose）→ 真值对账，输出对账全文
generate_and_run()
{
	local gapspec=$1 log=$2
	# make_testdata.sh 对已存在的 VDIF 幂等跳过，换中断重生成前必须先删
	rm -f "$WORKDIR"/raw/*/*.vdif
	FXSIM_NOISE=${FXSIM_NOISE:-0} FXSIM_GAPS="$gapspec" \
		"$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
	BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
	rm -rf "$WORKDIR/fengine"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) > "$log" 2>&1 ||
		fail "fxcorr-f 退出码非零（见 $log）"
	# check_reader 有 finding 时退出码为 1（E3 的 extra 就是本形态的一部分），
	# 判据取 E4 的数字，不看退出码
	python3 "$CHECKER" --log "$log" \
		--vdif "$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif" \
		--batch-json "$WORKDIR/batches/$BATCHID.json" --fps "$FPS" > "$log.check" 2>&1 || true
}

# E4 行：`E4 coverage : N data frames inside the batch never fell inside any read window`
lost_frames()
{
	awk '/^E4 coverage/{print $4}' "$1"
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好，见本目录 README 的坑）"
[ -f "$CHECKER" ] || fail "找不到 $CHECKER"

echo "--- 无中断（对照）---"
generate_and_run "" "$WORKDIR/none.log"
grep -E "^(summary|E4 coverage)" "$WORKDIR/none.log.check" | sed 's/^/  /'
LOST_CTRL=$(lost_frames "$WORKDIR/none.log.check")

echo "--- 复现形态：FXSIM_GAPS=$FILLSPEC ---"
generate_and_run "$FILLSPEC" "$WORKDIR/fill.log"
grep "GAPCHECK summary" "$WORKDIR/fill.log" | sed 's/^/  /'
sed -n '/^  sub /,$p' "$WORKDIR/fill.log.check" | sed 's/^/  /'
LOST_FILL=$(lost_frames "$WORKDIR/fill.log.check")

echo
echo "E4 净损失：无中断 $LOST_CTRL 帧，复现形态 $LOST_FILL 帧"

[ "$LOST_CTRL" = "0" ] || fail "无中断对照报了 $LOST_CTRL 帧净损失，判据本身有问题（先查 check_reader）"
if [ "$LOST_FILL" != "0" ]; then
	echo "FAIL: filler 段之后的数据落在读取窗口之外（净损失 $LOST_FILL 帧）" >&2
	echo "      E3 的 extra 是同一个缺陷的标记面（洞的尾巴延伸到缓冲区末尾），" >&2
	echo "      净损失才是数据面——参见 reader-model.md 4.7" >&2
	exit 1
fi

echo "PASS: filler 段之后的数据没有落在窗口之外（E4 = 0）"
