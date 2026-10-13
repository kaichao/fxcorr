#!/usr/bin/env bash
# run_v9_compare.sh —— v9 双链对照（fxcorr/v9-plan.md 判据 7 的 fp 侧执行）
#
# 七个 gaps 用例的默认场景（与 run_boundary / run_filler / run_window /
# run_startoffset / run_mixed / run_pattern / run_invalid 的 spec 一致，见下表）
# 各造一次数据，然后 fxcorr-f 与 fxcorr-fp 在同一份数据上各跑一次，判据：
#
#   1. 诊断逐行相同 —— GAPCHECK / READPOS 家族的行（两个工具共用同一份
#      datareader 源码，逐 subint 的读取位置、缺口/filler 计数与洞的落点应逐字一致）；
#   2. 无效块逐 subint 相同 —— fp 的 stream.bin 的 valid flags 与 f 的
#      band_00.sp 的 valid_flags 同源（同一份 fillValidFlags 位图），
#      sp_valid.py 与 stream_valid.py 输出同格式，diff 为空。
#
# 两条都过 = 该场景在 fp 上重跑与 f 等价；用例自身的正确性由各自的
# run_*.sh 判据负责（它们比对文件真值 check_reader.py，与工具无关）。
#
# 用法：./run_v9_compare.sh <workdir>    （workdir 必须已存在，config 齐备）
# 需要 fxcorr-sim / fxcorr-f / fxcorr-fp 在 PATH、LD_LIBRARY_PATH 含 DIFXROOT/lib。
set -euo pipefail

WORKDIR=${1:?usage: run_v9_compare.sh <workdir>}
STATION=${STATION:-T1}

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FXCORR_SRC=${FXCORR_SRC:-$(cd "$SCRIPTDIR/../../.." && pwd)}

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/local/difx/lib}
PATH=$PATH:/usr/local/difx/bin
export PATH

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

[ -d "$WORKDIR" ] || fail "workdir $WORKDIR 不存在（make_testdata.sh 要求它先建好）"

# 造数 + 取 batch_id（spec 为 FXSIM_* 赋值，可为空 = 无中断）
make_data()
{
	rm -f "$WORKDIR"/raw/*/*.vdif
	env "$@" FXSIM_NOISE=${FXSIM_NOISE:-0} \
		"$FXCORR_SRC/fxcorr/make_testdata.sh" "$WORKDIR" > /dev/null
	BATCHID=$(basename "$(ls "$WORKDIR"/batches/*.json)" .json)
}

# 同一份数据上跑 f 与 fp，两条判据
run_pair()
{
	local label=$1
	local logf="$WORKDIR/v9-$label-f.log" logp="$WORKDIR/v9-$label-p.log"

	rm -rf "$WORKDIR/fengine"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-f "$BATCHID" "$STATION" .) \
		> "$logf" 2>&1 || fail "$label: fxcorr-f 退出码非零（见 $logf）"
	(cd "$WORKDIR" && FXCORR_LOGLEVEL=verbose fxcorr-fp "$BATCHID" "$STATION" . 0) \
		> "$logp" 2>&1 || fail "$label: fxcorr-fp 退出码非零（见 $logp）"

	# 判据 1：GAPCHECK / READPOS 诊断逐行相同
	grep -E "^(READPOS|GAPCHECK)" "$logf" > "$logf.diag"
	grep -E "^(READPOS|GAPCHECK)" "$logp" > "$logp.diag"
	if ! diff -u "$logf.diag" "$logp.diag" > "$WORKDIR/v9-$label.diag.diff"; then
		head -20 "$WORKDIR/v9-$label.diag.diff" | sed 's/^/  /' >&2
		fail "$label: GAPCHECK/READPOS 诊断不一致（见 v9-$label.diag.diff）"
	fi

	# 判据 2：无效块逐 subint 相同（.sp 与 stream.bin 是同一份 flags 位图）
	python3 "$SCRIPTDIR/sp_valid.py" \
		"$WORKDIR/fengine/$BATCHID"/*/"$STATION/ds_0/band_00.sp" --machine \
		> "$WORKDIR/v9-$label-f.invalid"
	python3 "$SCRIPTDIR/stream_valid.py" \
		"$WORKDIR/fengine/$BATCHID"/*/"$STATION/ds_0/stream.bin" --machine \
		> "$WORKDIR/v9-$label-p.invalid"
	if ! diff -u "$WORKDIR/v9-$label-f.invalid" "$WORKDIR/v9-$label-p.invalid" \
		> "$WORKDIR/v9-$label.invalid.diff"; then
		head -20 "$WORKDIR/v9-$label.invalid.diff" | sed 's/^/  /' >&2
		fail "$label: 无效块分布不一致（见 v9-$label.invalid.diff）"
	fi

	local nbad
	nbad=$(awk -F: '$1=="total"{print $2}' "$WORKDIR/v9-$label-f.invalid")
	echo "  $label: 诊断 $(wc -l < "$logf.diag") 行逐行一致；无效块 total $nbad 一致"
}

# 场景表：label|FXSIM_* 赋值（默认值取自各 run_*.sh 的变量默认）
while IFS='|' read -r label spec; do
	[ -n "$label" ] || continue
	echo "--- $label ${spec:+($spec)} ---"
	if [ -n "$spec" ]; then
		make_data "$spec"
	else
		make_data
	fi
	run_pair "$label"
done <<'SCENARIOS'
ctrl|
gap_a_0.52_30|FXSIM_GAPS=0.52:30
gap_b_0.48_30|FXSIM_GAPS=0.48:30
filler_1.0_40|FXSIM_GAPS=1.0:40
filler_1.0_40_f400|FXSIM_GAPS=1.0:40:f400
window_0.6_40_f400|FXSIM_GAPS=0.6:40:f400
startoffset_18|FXSIM_STARTOFFSET=18
mixed_m1_f|FXSIM_GAPS=0.50:28:f229,0.62:22:f180
mixed_m1_g|FXSIM_GAPS=0.50:28,0.62:22
mixed_m2_f|FXSIM_GAPS=0.45:63:f508
mixed_m2_g|FXSIM_GAPS=0.45:63
pattern_f|FXSIM_GAPS=0.50:28:f229
pattern_p|FXSIM_GAPS=0.50:28:p229
pattern_h|FXSIM_GAPS=0.50:28:h229
SCENARIOS

# invalid 位场景（run_invalid.sh 的形态）：无中断数据 + 后处理把帧 125..353
# 的 word0 最高位置 1（帧号与时间轴不动），然后 f 与 fp 各跑一次
echo "--- invalid_125_229 (无 gaps + 后处理置位) ---"
make_data
VDIF="$WORKDIR/raw/$STATION/${STATION}_${BATCHID}.vdif"
python3 - "$VDIF" 125 229 <<'PYEOF'
import struct, sys
path, start, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, 'r+b') as f:
    framebytes = (struct.unpack('<I', f.read(32)[8:12])[0] & 0xFFFFFF) * 8
    for i in range(start, start + n):
        f.seek(i * framebytes)
        w0 = struct.unpack('<I', f.read(4))[0]
        f.seek(i * framebytes)
        f.write(struct.pack('<I', w0 | 0x80000000))
PYEOF
run_pair invalid_125_229

echo
echo "PASS: 15 个场景上 fxcorr-fp 与 fxcorr-f 的诊断逐行一致、无效块逐 subint 一致"
