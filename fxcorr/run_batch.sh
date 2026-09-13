#!/usr/bin/env bash
# run_batch.sh —— fxcorr 流水线（规格见 fxcorr/impl-plan.md 2.4）
#
# 步骤：① 读 batches/<batch_id>.json + .input，前置校验对齐（batch 起点 subint
# 边界、batch 时长 INT TIME 整数倍、INT TIME 为 subint 整数倍，容差同 fxcorr-f/x
# 程序内校验；不通过直接报错退出，不依赖工具兜底）
# → ② DATA TABLE 软链重指本 batch 的 VDIF（make_testdata.sh 多 batch 时软链停在
# 最后 batch，跑其他 batch 前必须重做；raw 数据不存在即报错）
# → ③ 置 status=running（batch.json status 字段）→ ④ 逐站 fxcorr-f（任一失败 →
# status=failed、非 0 退出，不跑后续站）→ ⑤ mkdir .input OUTPUT FILENAME 所在目录
# → ⑥ fxcorr-x（失败同 ④）→ ⑦ 成功 → status=done，追加 meta/batches.index 一行
# <batch_id>,done,<时间戳>。
#
# 用法：./run_batch.sh <batch_id> [workdir]
#   batch_id  批量标识（batches/<batch_id>.json 须已写好，可用 make_testdata.sh 生成）
#   workdir   项目根目录（默认 .）
set -euo pipefail

# 容器模式开关：FXCORR_RUN_MODE=container 时工具经 docker run 调用（见下方 fxc）
FXCORR_RUN_MODE="${FXCORR_RUN_MODE:-host}"

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ "$FXCORR_RUN_MODE" != "container" ] && ! command -v fxcorr-f >/dev/null 2>&1 && [ -f "$SCRIPTDIR/../setup.bash" ]; then
	# setup.bash 的 PurgePath 引用可能未设置的变量（PERL5LIB 等），
	# 与 set -u 冲突，source 时临时放开
	set +u
	. "$SCRIPTDIR/../setup.bash"
	set -u
fi

# ---- 容器模式执行前缀：工具→镜像映射，workdir 整体挂载、cwd 与宿主直跑一致 ----
run_in_container()
{
	local tool="$1"; shift
	local img
	case "$tool" in
		fxcorr-f)   img=fxcorr-f ;;
		fxcorr-x)   img=fxcorr-x ;;
		fxcorr-sim) img=fxcorr-sim ;;
		vex2difx|difxcalc|difx2fits) img=difx-tools ;;
		*) echo "fxc: no image for tool $tool" >&2; exit 2 ;;
	esac
	local envargs=()
	[ -n "${FXSIM_NOISE+x}" ] && envargs+=(-e FXSIM_NOISE="$FXSIM_NOISE")
	[ -n "${FXSIM_SEED+x}" ] && envargs+=(-e FXSIM_SEED="$FXSIM_SEED")
	docker run --rm "${envargs[@]}" -v "$WORKDIR:$WORKDIR" -w "$(pwd)" "$img:latest" "$tool" "$@"
}
fxc()
{
	if [ "$FXCORR_RUN_MODE" = "container" ]; then
		run_in_container "$@"
	else
		"$@"
	fi
}

usage()
{
	cat >&2 <<'EOF'
用法：./run_batch.sh <batch_id> [workdir]
  batch_id  批量标识（batches/<batch_id>.json 须已写好，可用 make_testdata.sh 生成）
  workdir   项目根目录（默认 .）
EOF
	exit "${1:-2}"
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage
BID=$1
shift
WORKDIR=.
if [ $# -gt 0 ] && [ -d "$1" ]; then
	WORKDIR=$1
	shift
fi
[ $# -eq 0 ] || usage
WORKDIR=$(cd "$WORKDIR" && pwd)
if [ ! -f "$WORKDIR/batches/$BID.json" ]; then
	echo "run_batch.sh: $WORKDIR/batches/$BID.json not found" >&2
	exit 2
fi
if [ "$FXCORR_RUN_MODE" = "container" ]; then
	command -v docker >/dev/null 2>&1 || { echo "run_batch.sh: docker not found (FXCORR_RUN_MODE=container)" >&2; exit 2; }
else
	command -v fxcorr-f >/dev/null 2>&1 || { echo "run_batch.sh: fxcorr-f not found (source setup.bash or install)" >&2; exit 2; }
	command -v fxcorr-x >/dev/null 2>&1 || { echo "run_batch.sh: fxcorr-x not found (source setup.bash or install)" >&2; exit 2; }
fi
mkdir -p "$WORKDIR/meta"

# ---- ①② 读 batch.json + .input、前置校验、DATA TABLE 软链重做 ----
OUT=$(mktemp)
trap 'rm -f "${OUT:-}"' EXIT
python3 - "$WORKDIR" "$BID" <<'PYEOF' > "$OUT"
import json, math, os, re, sys

workdir, bid = sys.argv[1], sys.argv[2]
bjpath = os.path.join(workdir, 'batches', bid + '.json')
bj = json.load(open(bjpath))
cfgrel = bj['config_file']
cfg = os.path.join(workdir, cfgrel)
if not os.path.exists(cfg):
    sys.exit('run_batch.sh: config_file %s not found' % cfgrel)
text = open(cfg).read()

def one(key):
    m = re.search(r'^%s:\s*(.*)$' % re.escape(key), text, re.M)
    if not m:
        sys.exit('run_batch.sh: %s not found in %s' % (key, cfgrel))
    return m.group(1).strip()

# ① 前置校验（与 fxcorr-f/x 程序内校验同语义，提前挡）：
#   - batch 起点在 subint 边界（1µs 容差，同 fxcorr-f main.cpp:175-182，
#     吸收 start_mjd 的 f64 表示误差）
#   - batch 时长为 INT TIME 整数倍（跨 batch 追加不碎片化，data-spec 12）
#   - INT TIME 为 subint 整数倍（fxcorr-x 的 offsetnsperintegration==0 硬校验）
mjd0 = int(one('START MJD'))
sec0 = float(one('START SECONDS'))
subint = int(bj['subint_ns'])
inttime = float(bj['integration_sec'])
initsec = (bj['start_mjd'] - mjd0 - sec0 / 86400.0) * 86400.0
batchstartns = int(math.floor(initsec * 1.0e9 + 0.5))
rem = batchstartns % subint
if rem > 1000 and (subint - rem) > 1000:
    sys.exit('run_batch.sh: batch start is not on a subint boundary (%d ns into a %d ns subint)' % (rem, subint))
batchdur = int(bj['n_subints']) * subint / 1.0e9
nint = round(batchdur / inttime)
if abs(nint * inttime - batchdur) > 1e-6:
    sys.exit('run_batch.sh: batch duration %g is not an integer multiple of INT TIME %g' % (batchdur, inttime))
trem = inttime * 1.0e9 % subint
if min(trem, subint - trem) > 1.0:
    sys.exit('run_batch.sh: INT TIME %g is not a multiple of SUBINT %d ns' % (inttime, subint))

# 站列表：batch.json stations（make_testdata.sh 已写全）；缺失时从 .input
# DATASTREAM 解析（TELESCOPE INDEX + TELESCOPE TABLE，同 make_testdata.sh）
stations = bj.get('stations')
datafiles = re.findall(r'^FILE \d+/\d+:\s*(\S+)\s*$', text, re.M)
if stations is None:
    telnames = re.findall(r'^TELESCOPE NAME \d+:\s*(\S+)\s*$', text, re.M)
    ds_tel = re.findall(r'^TELESCOPE INDEX:\s*(\d+)\s*$', text, re.M)
    stations = [telnames[int(t)] for t in ds_tel]
if len(stations) != len(datafiles):
    sys.exit('run_batch.sh: station/file count mismatch in %s (%d stations, %d files)' % (cfgrel, len(stations), len(datafiles)))

# ② DATA TABLE 软链重指本 batch 的 VDIF（fxcorr-f 按 DATA TABLE 文件名读数据）；
# raw 数据不存在即报错（跑 fxcorr-f 前就挡）。软链 target 用相对 workdir 路径。
for st, fn in zip(stations, datafiles):
    src = 'raw/%s/%s_%s.vdif' % (st, st, bid)
    if not os.path.isfile(os.path.join(workdir, src)):
        sys.exit('run_batch.sh: raw data %s not found (run make_testdata.sh first)' % src)
    tgt = os.path.join(workdir, fn)
    if os.path.islink(tgt) or os.path.exists(tgt):
        os.unlink(tgt)
    os.symlink(src, tgt)

outdir = os.path.join(workdir, one('OUTPUT FILENAME').rstrip('/'))
print('CFGIN=%s' % cfgrel)
print('OUTDIR=%s' % outdir)
print('--')
for st, fn in zip(stations, datafiles):
    print('%s %s' % (st, fn))
PYEOF

CFGIN= OUTDIR=
while IFS= read -r line && [ "$line" != "--" ]; do
	[ -n "$line" ] || continue
	k=${line%%=*}
	v=${line#*=}
	case "$k" in
		CFGIN) CFGIN=$v ;;
		OUTDIR) OUTDIR=$v ;;
	esac
done < "$OUT"
[ -n "$CFGIN" ] || { echo "run_batch.sh: failed to parse batch $BID" >&2; exit 2; }
declare -a DSTATION
while read -r st fn; do
	DSTATION+=("$st")
done < <(awk 'f{print} /^--$/{f=1}' "$OUT")

# ---- ③④⑤⑥⑦ status 流转与逐站/基线执行 ----
# status 写回 batch.json（json.dump 保字段序与 make_testdata.sh 一致）；
# done 时顺带追加 meta/batches.index（规格⑥⑦）
mark_status()
{
	python3 -c '
import json, os, sys
from datetime import datetime
workdir, bid, st = sys.argv[1], sys.argv[2], sys.argv[3]
p = os.path.join(workdir, "batches", bid + ".json")
b = json.load(open(p))
b["status"] = st
with open(p, "w") as f:
	json.dump(b, f, indent=2)
	f.write("\n")
if st == "done":
	with open(os.path.join(workdir, "meta", "batches.index"), "a") as f:
		f.write("%s,done,%s\n" % (bid, datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")))
' "$WORKDIR" "$BID" "$1"
}
mark_status running

# 逐站 fxcorr-f：任一失败 → status=failed、非 0 退出，不跑后续站（规格③④）
for st in "${DSTATION[@]}"; do
	echo "run_batch.sh: fxcorr-f $BID $st" >&2
	if ! fxc fxcorr-f "$BID" "$st" "$WORKDIR"; then
		echo "run_batch.sh: fxcorr-f failed for station $st" >&2
		mark_status failed
		exit 1
	fi
done

mkdir -p "$OUTDIR"    # 规格⑤（fxcorr-x 自身也会建，先建无害）
if ! fxc fxcorr-x "$BID" "$WORKDIR"; then
	echo "run_batch.sh: fxcorr-x failed for batch $BID" >&2
	mark_status failed
	exit 1
fi

mark_status done
echo "run_batch.sh: batch $BID done, SWIN in $(basename "$OUTDIR")"
