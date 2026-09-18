#!/usr/bin/env bash
# run_bench.sh —— mpifxcorr 基准（对拍基准生成器，规格见 fxcorr/v1-plan.md 2.4）
#
# 步骤：① 定位 batch（DATA TABLE 软链 target 里的 batch_id，fallback batches/ 最新 json）
# → ② 从 batch.json + .input 推导 EXECUTE TIME（整秒字段，取 floor(initsec + N*intTime)
# + 1——多留一个整秒，mpifxcorr 才把 N 个积分都写完整，见下方 ② 的注释）
# → ③ 复制 config .input → bench/，sed EXECUTE TIME / OUTPUT FILENAME → bench/<exp>.difx
# → ④ mpirun mpifxcorr 出基准 SWIN → ④½ 截掉 EXECUTE TIME 多留出的第 N+1 个积分
# → ⑤ 打印 cmp_swin.py 对拍提示。
#
# 用法：./run_bench.sh [workdir]
#   workdir  项目根目录（默认 .，须含 make_testdata.sh 布局：config/ + batches/ + DATA TABLE 软链）
#   环境变量：NP（mpirun 进程数，默认 4）；FXCORR_WORKDIR（项目根目录，位置参数优先）
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if ! command -v mpifxcorr >/dev/null 2>&1 && [ -f "$SCRIPTDIR/../setup.bash" ]; then
	# setup.bash 的 PurgePath 引用可能未设置的变量（PERL5LIB 等），
	# 与 set -u 冲突，source 时临时放开
	set +u
	. "$SCRIPTDIR/../setup.bash"
	set -u
fi

usage()
{
	cat >&2 <<'EOF'
用法：./run_bench.sh [workdir]
  workdir  项目根目录（默认 .，须含 make_testdata.sh 布局：config/ + batches/ + DATA TABLE 软链）
  环境变量：NP（mpirun 进程数，默认 4）；FXCORR_WORKDIR（项目根目录，位置参数优先）
EOF
	exit "${1:-2}"
}

[ $# -le 1 ] || usage
WORKDIR="${FXCORR_WORKDIR:-.}"
if [ $# -gt 0 ] && [ -d "$1" ]; then
	WORKDIR=$1	# 位置参数优先于环境变量
	shift
fi
[ $# -eq 0 ] || usage
WORKDIR=$(cd "$WORKDIR" && pwd)
if [ ! -d "$WORKDIR/batches" ] || [ ! -d "$WORKDIR/config" ]; then
	echo "run_bench.sh: $WORKDIR does not look like a make_testdata.sh workdir (need config/ and batches/)" >&2
	exit 2
fi
command -v mpifxcorr >/dev/null 2>&1 || { echo "run_bench.sh: mpifxcorr not found (source setup.bash or install mpifxcorr)" >&2; exit 2; }
command -v mpirun >/dev/null 2>&1 || { echo "run_bench.sh: mpirun not found" >&2; exit 2; }
if ! [[ ${NP:-4} =~ ^[1-9][0-9]*$ ]]; then
	echo "run_bench.sh: NP must be a positive integer" >&2
	exit 2
fi

# ---- ①② 定位 batch、推导 EXECUTE TIME 截断 ----
# python3：json 读取 + .input 解析 + 浮点（bash 无浮点算术）
OUT=$(mktemp)
trap 'rm -f "${OUT:-}"' EXIT
python3 - "$WORKDIR" <<'PYEOF' > "$OUT"
import glob, json, math, os, re, sys

workdir = sys.argv[1]
batchdir = os.path.join(workdir, 'batches')

# ① batch 定位：优先从 DATA TABLE 软链 target（<station>_<batch_id>.vdif）解析，
#    与数据精确对应（-n 多 batch 时软链指向最后 batch）；无软链（手工布局）时
#    用 batches/ 下 mtime 最新的 json
def softlink_bid():
    for j in glob.glob(os.path.join(batchdir, '*.json')):
        b = json.load(open(j))
        cfg = os.path.join(workdir, b['config_file'])
        if not os.path.exists(cfg):
            continue
        m = re.search(r'^FILE \d+/\d+:\s*(\S+)\s*$', open(cfg).read(), re.M)
        if not m:
            continue
        lnk = os.path.join(workdir, m.group(1))
        if not os.path.islink(lnk):
            continue
        base = os.path.basename(os.readlink(lnk))
        if base.endswith('.vdif'):
            base = base[:-len('.vdif')]
        bid = base.split('_', 1)[1] if '_' in base else base
        if os.path.exists(os.path.join(batchdir, bid + '.json')):
            return bid
    return None

bid = softlink_bid()
if bid is None:
    jsons = sorted(glob.glob(os.path.join(batchdir, '*.json')), key=os.path.getmtime)
    if not jsons:
        sys.exit('run_bench.sh: no batch.json in %s' % batchdir)
    bid = os.path.basename(jsons[-1])[:-len('.json')]
    print('run_bench.sh: no symlinked batch_id, using latest batch.json %s' % bid,
          file=sys.stderr)

bj = json.load(open(os.path.join(batchdir, bid + '.json')))
cfgrel = bj['config_file']
cfg = os.path.join(workdir, cfgrel)
if not os.path.exists(cfg):
    sys.exit('run_bench.sh: config_file %s not found' % cfgrel)
text = open(cfg).read()

def one(key):
    m = re.search(r'^%s:\s*(.*)$' % re.escape(key), text, re.M)
    if not m:
        sys.exit('run_bench.sh: %s not found in %s' % (key, cfgrel))
    return m.group(1).strip()

mjd0 = int(one('START MJD'))
sec0 = float(one('START SECONDS'))
exp = os.path.basename(one('OUTPUT FILENAME').rstrip('/'))
if exp.endswith('.difx'):
    exp = exp[:-len('.difx')]

# ② EXECUTE TIME：mpifxcorr 的 writedata 在积分满 intTime 时按积分起点
# currentstartseconds + scanstartsec >= executeseconds 判定停写（mpifxcorr EXECUTE
# TIME 语义，fxcorr-x CLAUDE.md 已实证），EXECUTE TIME 又是整秒字段。
#
# 不能取 floor(initsec + (N-1)*intTime) + 1（曾经如此，按"积分 N+1 起点 >= 截断值即停"
# 推导）：mpifxcorr 不只在积分起点处判停，它把数据读到 EXECUTE TIME 就停，末积分因此
# 只累积到 EXECUTE TIME 为止。t25362 真实数据实测（2026-09-18）：EXECUTE TIME=11 时
# 末积分 [24621.752, 24622.776) 的 weight 只有 0.7356，而基准应为 0.9956——差值恒为
# 0.26，与"mpifxcorr 读到 24622.5 前后停止累积"吻合。合成测试未暴露此问题，因为
# batch 起点即 START SECONDS、末积分恰好压在整秒上（6/6 对拍时 ET=2 与 ET=3 结果相同）。
#
# 取 floor(initsec + N*intTime) + 1：N 个积分全部落在 EXECUTE TIME 之前，mpifxcorr 会
# 把它们写完整；代价是多写第 N+1 个积分（起终点都在 batch 窗口之外），跑完由 ④½ 截掉。
nsub = int(bj['n_subints'])
subint = int(bj['subint_ns'])
inttime = float(bj['integration_sec'])
initsec = (bj['start_mjd'] - mjd0 - sec0 / 86400.0) * 86400.0
batchdur = nsub * subint / 1.0e9
nint = round(batchdur / inttime)
if abs(nint * inttime - batchdur) > 1e-6:
    sys.exit('run_bench.sh: batch duration %g is not an integer multiple of INT TIME %g '
             '(integrals would not line up for comparison)' % (batchdur, inttime))
exec_ = int(math.floor(initsec + nint * inttime)) + 1

print('BID=%s' % bid)
print('CFGIN=%s' % cfgrel)
print('EXEC=%d' % exec_)
print('NINT=%d' % nint)
print('EXP=%s' % exp)
print('OUTDIR=%s/bench/%s.difx' % (workdir, exp))
print('NCHAN=%d' % int(bj['n_channels']))
PYEOF

BID= CFGIN= EXEC= NINT= EXP= OUTDIR= NCHAN=
while read -r line; do
	[ -n "$line" ] || continue
	k=${line%%=*}
	v=${line#*=}
	case "$k" in
		BID) BID=$v ;;
		CFGIN) CFGIN=$v ;;
		EXEC) EXEC=$v ;;
		NINT) NINT=$v ;;
		EXP) EXP=$v ;;
		OUTDIR) OUTDIR=$v ;;
		NCHAN) NCHAN=$v ;;
	esac
done < "$OUT"
[ -n "$BID" ] || { echo "run_bench.sh: failed to locate a batch" >&2; exit 2; }

# ---- ③ 复制 config .input → bench/，sed EXECUTE TIME 截断 + OUTPUT FILENAME ----
# OUTPUT FILENAME 改指 bench/<exp>.difx：基准 SWIN 与 fxcorr 侧（.input OUTPUT
# FILENAME 目录）分开落盘，对拍时互不覆盖。CALC FILENAME 为绝对路径，无需改。
mkdir -p "$WORKDIR/bench"
sed -e "s/^EXECUTE TIME (SEC):[[:space:]]*[0-9]*$/EXECUTE TIME (SEC): $EXEC/" \
    -e "s|^OUTPUT FILENAME:[[:space:]]*.*$|OUTPUT FILENAME:    $OUTDIR|" \
    "$WORKDIR/$CFGIN" > "$WORKDIR/bench/$(basename "$CFGIN")"
rm -rf "$OUTDIR"    # 幂等重跑
mkdir -p "$OUTDIR"

# ---- ④ mpirun mpifxcorr ----
# DATA TABLE 文件名为相对路径（软链在 workdir 根），故 cwd 在 workdir 内跑。
# mpifxcorr 需要 LD_LIBRARY_PATH 找 libmark5access（install-difx 的 bin/lib 目录）。
cd "$WORKDIR"
export LD_LIBRARY_PATH="${DIFXROOT:-/usr/local/difx}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
MPIARGS=()
if [ "$(id -u)" = 0 ]; then
	MPIARGS+=(--allow-run-as-root)
fi
mpirun "${MPIARGS[@]}" -np "${NP:-4}" mpifxcorr "bench/$(basename "$CFGIN")"

# ---- ④½ 截掉 EXECUTE TIME 多留出的第 N+1 个积分 ----
# ② 多留一个整秒的代价：mpifxcorr 会多写一个积分（起终点都在 batch 窗口之外，fxcorr
# 侧不产出）。按 sec 分组保留前 NINT 组、把尾部多余记录从基准文件里截掉，使两侧记录数
# 一致，cmp_swin.py 无需 maxrecords 参数。无多写时不动文件。
python3 - "$OUTDIR" "$NINT" "$NCHAN" <<'PYEOF'
import glob, os, struct, sys

outdir, nint, nchan = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rec = 74 + nchan * 8          # SWIN 记录长度（74 字节头 + nchan 复数）
SECOFF = 16                   # 头内 sec 字段偏移（2I sync/ver + 2i bl/mjd 之后）
for path in sorted(glob.glob(os.path.join(outdir, 'DIFX_*'))):
    size = os.path.getsize(path)
    nrec = size // rec
    if nrec * rec != size:
        sys.exit('run_bench.sh: %s size %d not a multiple of record size %d'
                 % (path, size, rec))
    with open(path, 'rb') as f:
        data = f.read()
    keep, seen, prev = nrec, 0, None
    for i in range(nrec):
        sec = struct.unpack_from('<d', data, i * rec + SECOFF)[0]
        if prev is None or abs(sec - prev) > 1e-9:
            seen += 1
            if seen > nint:
                keep = i
                break
            prev = sec
    if keep < nrec:
        with open(path, 'r+b') as f:
            f.truncate(keep * rec)
        print('run_bench.sh: %s: %d -> %d records (dropped %d integration(s) outside '
              'the batch window)' % (os.path.basename(path), nrec, keep, seen - nint))
PYEOF

# ---- ⑤ 对拍提示 ----
# 两侧 SWIN 逐基线文件对拍：fxcorr 侧由 run_batch.sh 产出（.input OUTPUT FILENAME
# 目录），基准侧在本脚本的 bench/<exp>.difx/。2 站 1 基线为单文件
# DIFX_<mjd>_<6 位秒>.s0000.b0000。
echo "run_bench.sh: batch $BID, bench/$EXP.difx holds $NINT integration(s) (EXECUTE TIME $EXEC)"
echo "对拍（run_batch.sh 产出 fxcorr 侧 SWIN 后，逐基线文件）："
echo "  python3 $SCRIPTDIR/test/cmp_swin.py <fxcorr SWIN>/DIFX_*.s0000.b0000 bench/$EXP.difx/DIFX_*.s0000.b0000 $NCHAN"
