#!/usr/bin/env bash
# run_startoffset.sh —— 文件起点晚于 batch 起点（reader-model.md 4.1 的 A 类）
#
# t25362 的 BA 首帧比 batch 起点晚 79.312 ms（1269 帧），S6 恰在秒边界。定位读的每一次
# "时间 → 字节"换算都要用 anchorbytes 把文件起点对上 batch 起点（文件晚于 batch 时为
# **负**），A 类的三条缺陷都在这里：A1 漏算（读取位置整体错位）、A2 取整用 C++ 截断
# 而非 floor（差一帧，奇数序号的 pcal tone 相位翻转）、A3 把负位置**整个 subint** 判
# 无效（该 subint 其实头部无数据、尾部有数据）。三类此前**只在真实观测上验证过**，
# 改坏了没有回归可依（v4-plan.md A2）。
#
# 造法：`FXSIM_STARTOFFSET=<帧数>` 让生成器跳过 batch 开头的这么多帧——它们既不在文件
# 里、也不占文件字节，于是文件第一帧的时间戳就是 batch 起点 + N 帧，而 anchorbytes 为
# 负。帧号仍按时间轴推进，所以文件内容的时间是对的，错的只能是读法。
#
# 判据：`check_reader.py` 的 **E5 锚点**（外加 E1–E4，它们各自仍要绿）
#   * E5 必须**真的启用**——基准不同（真实观测的 VDIF 秒与 batch.json 的 MJD 不同源）、
#     READPOS 不完整、文件含缺口/filler（B/C 类，读取位置本就该偏离名义轴）这三种情况
#     它会跳过 E5。**跳过时退出码同样是 0**，所以本脚本另外检查那一行不是 "skipped"，
#     否则判据形同虚设。
#   * 两侧都应绿：无中断对照（偏移 0）与起点偏移 N 帧。正确数据上实测偏差 3 帧
#     （读取位置相对名义时间轴略早，来自 delay 修正与取整），E5 容差 8。
#   * **自检**：把各 subint 的 readoff 减掉 anchorbytes（模拟 A1 漏算）后必须报红——
#     判据能报红这件事本身也要能被复现，否则它退化成恒绿。
#
# 用法：./run_startoffset.sh [workdir]     （默认 /tmp/soffset，目录须先存在）
# 需要 fxcorr-sim / fxcorr-f 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:-/tmp/soffset}
STATION=${STATION:-T1}
OFFSET=${OFFSET:-18}			# 起点偏移帧数（t25362 的 BA 是 1269）
FPS=${FPS:-250}				# test.vex：8 Ms/s ÷ 每帧 32000 采样
FRAMEBYTES=${FRAMEBYTES:-8032}		# test.vex：payload 8000 + 32 字节帧头

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

# 生成（偏移 off 帧）→ 跑 f（verbose）→ 真值对账；结果落在 $log 与 $log.check，退出码在 CHECKRC
generate_and_run()
{
	local off=$1 log=$2
	# make_testdata.sh 对已存在的 VDIF 幂等跳过，换偏移重生成前必须先删
	rm -f "$WORKDIR"/raw/*/*.vdif
	FXSIM_NOISE=${FXSIM_NOISE:-0} FXSIM_STARTOFFSET="$off" \
		"$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
	BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
	rm -rf "$WORKDIR/fengine"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) > "$log" 2>&1 ||
		fail "fxcorr-f 退出码非零（见 $log）"
	VDIF="$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif"
	CHECKRC=0
	python3 "$CHECKER" --log "$log" --vdif "$VDIF" \
		--batch-json "$WORKDIR/batches/$BATCHID.json" --fps "$FPS" > "$log.check" 2>&1 || CHECKRC=$?
}

report()
{
	grep -E "^(E5 anchor|summary)" "$1.check" | sed 's/^/  /'
}

# E5 跳过时退出码也是 0——判据没跑等于没判，必须单独拦
require_e5_active()
{
	grep -q "^E5 anchor" "$1.check" || fail "$1: 没有 E5 输出，check_reader 版本过旧？"
	grep -q "^E5 anchor .*skipped" "$1.check" &&
		fail "$1: E5 被跳过（见上），本场景的判据没有生效"
	return 0
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好，见本目录 README 的坑）"
[ -f "$CHECKER" ] || fail "找不到 $CHECKER"

echo "--- 无中断对照：FXSIM_STARTOFFSET=0 ---"
generate_and_run 0 "$WORKDIR/zero.log"
report "$WORKDIR/zero.log"
RC_ZERO=$CHECKRC

echo "--- 起点偏移 $OFFSET 帧：FXSIM_STARTOFFSET=$OFFSET ---"
generate_and_run "$OFFSET" "$WORKDIR/offset.log"
report "$WORKDIR/offset.log"
RC_OFF=$CHECKRC

require_e5_active "$WORKDIR/zero.log"
require_e5_active "$WORKDIR/offset.log"
[ "$RC_ZERO" = 0 ] || fail "无中断对照报了 finding（rc=$RC_ZERO，见 $WORKDIR/zero.log.check）"
[ "$RC_OFF" = 0 ] || fail "起点偏移 $OFFSET 帧报了 finding（rc=$RC_OFF，见 $WORKDIR/offset.log.check）"

echo
echo "--- 判据自检：模拟 A1（anchorbytes 漏算进读取位置）---"
# 各 subint 的 readoff 减去 anchorbytes，等价于 locate 没把它加进去；第 1 个 subint 的
# readoff 本来就是 0（负位置被钳到文件首帧），不动它
python3 - "$WORKDIR/offset.log" "$WORKDIR/tamper.log" "$OFFSET" "$FRAMEBYTES" <<'PY'
import re, sys
log, out, frames, framebytes = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
delta = frames * framebytes
src = open(log).read()
def shift(m):
    if int(m.group(1)) == 1:
        return m.group(0)
    return m.group(0).replace(m.group(2), str(int(m.group(2)) - delta), 1)
open(out, 'w').write(re.sub(r'READPOS subint (\d+): readoff (\d+)', shift, src))
PY
BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
if python3 "$CHECKER" --log "$WORKDIR/tamper.log" \
	--vdif "$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif" \
	--batch-json "$WORKDIR/batches/$BATCHID.json" --fps "$FPS" > "$WORKDIR/tamper.log.check" 2>&1; then
	sed 's/^/  /' "$WORKDIR/tamper.log.check" | tail -4
	fail "自检失败：readoff 减掉 anchorbytes 之后判据竟然没报红（E5 是恒绿的吗？）"
fi
grep -E "^(E5 anchor|summary)" "$WORKDIR/tamper.log.check" | sed 's/^/  /'

echo
echo "PASS: 起点偏移下 E5 锚点成立（对照与偏移 $OFFSET 帧都绿），且判据对该类缺陷报红"
