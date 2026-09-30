#!/bin/bash
# slurm.sh —— 阶段作业封装（V7 §12 的「作业封装」，即 §13 第 8 条）
#
# **只做一件事**：把**已经存在**的阶段脚本提交成 Slurm 作业，并（可选）等它跑完。
# **判据不在这里实现**——每个阶段脚本自己打印自己的数字与门禁（`v7-plan.md` §13），
# 本脚本只管「提交、时限、内存、核数、日志」，以及把作业的**真实节点列表**交给那些
# 需要跨节点的脚本。这是 §12 定的"脚本要薄，不要长成另一个 scalebox"。
#
# 用法：./slurm.sh <phase> [选项]
#   phase          p1 | p2 | p3 | p4 | p5a
#   --batch ID     p2/p3 要跑的 batch_id（必填）
#   --nodes N      节点数（默认 1）
#   --np N         p5a 的总 rank 数；给了它才展开 FXCORR_MPI_PPN（= ceil(NP/NODES)）
#   --time T       时限（默认 01:00:00）
#   --mem M        每节点内存（默认 120G）
#   --workdir DIR  项目根（默认 .）
#   --env K=V      透传给作业的环境变量（可多次，如 FXCORR_PARALLEL=3）
#   --wait         提交后等作业结束并回显日志尾部
#   --dry-run      只打印 sbatch 内容，不提交
#
# **为什么需要它**（2026-09-30 的教训）：P5a 当时手工提了**两个独立的 1 节点作业**，
# 于是 `FXCORR_MPI_PPN` 的自动展开与 `run_bench.sh` 里的 `srun` 采样分发**两条路都
# 用不上**，只能 ssh 到对端手工起采样器——当天两个坑（Open MPI 不转发环境变量、
# heredoc 里 mpirun 抢 stdin）都是"没有这一层"的直接代价。**提一个真正的 N 节点作业**，
# 这两条路径就都通了。
#
# 判据怎么读：作业跑完，日志尾部就是对应阶段的判据行（`run_bench.sh` 的内存/流量、
# `run_batch.sh` 的 SWIN 与 merge、`cmp_swin.py` 的逐条对拍）——本脚本不重复它们。
set -uo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ENV_SCRIPT=${FXCORR_ENV_SCRIPT:-/public/home/cstu0036/fxcorr/env.sh}

usage()
{
	sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
	exit "${1:-2}"
}

[ $# -ge 1 ] || usage
PHASE=$1; shift
case "$PHASE" in p1|p2|p3|p4|p5a) ;; *) echo "slurm.sh: unknown phase '$PHASE'" >&2; usage 2 ;; esac

NODES=1 TIME=01:00:00 MEM=120G NP= BATCH= WORKDIR=. WAIT=0 DRY=0
ENVS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--batch)   BATCH=${2:?}; shift 2 ;;
		--nodes)   NODES=${2:?}; shift 2 ;;
		--np)      NP=${2:?}; shift 2 ;;
		--time)    TIME=${2:?}; shift 2 ;;
		--mem)     MEM=${2:?}; shift 2 ;;
		--workdir) WORKDIR=${2:?}; shift 2 ;;
		--env)     ENVS+=("${2:?}"); shift 2 ;;
		--wait)    WAIT=1; shift ;;
		--dry-run) DRY=1; shift ;;
		*) usage 2 ;;
	esac
done
for v in NODES TIME; do
	[[ ${!v} =~ ^[0-9]+(:[0-9]{2}){2}$|^[0-9]+$ ]] || { echo "slurm.sh: bad $v='${!v}'" >&2; exit 2; }
done
WORKDIR=$(cd "$WORKDIR" && pwd)

# ---- 阶段 → 命令 ----
case "$PHASE" in
	p1)  CMD="bash $SCRIPTDIR/make_testdata.sh $WORKDIR" ;;
	p2|p3)
		[ -n "$BATCH" ] || { echo "slurm.sh: $PHASE needs --batch <id>" >&2; exit 2; }
		CMD="bash $SCRIPTDIR/run_batch.sh $BATCH $WORKDIR"
		;;
	p4)  CMD="bash $SCRIPTDIR/run_bench.sh $WORKDIR" ;;
	p5a)
		CMD="bash $SCRIPTDIR/run_bench.sh $WORKDIR"
		# 多节点才展开：FXCORR_MPI_PPN 让 run_bench.sh 去问 Slurm 要节点列表，
		# 而它之所以问得到，正是因为**这个作业本身**有 N 个节点（今天缺的就是这一环）
		if [ "$NODES" -gt 1 ] && [ -n "$NP" ]; then
			ENVS+=("FXCORR_MPI_PPN=$(( (NP + NODES - 1) / NODES ))")
			ENVS+=("NP=$NP")
		fi
		;;
esac

# ---- 生成 sbatch ----
LOGDIR="$WORKDIR/meta/slurm"
mkdir -p "$LOGDIR"
SB="$LOGDIR/$PHASE-$(date +%Y%m%d-%H%M%S).sbatch"
{
	echo "#!/bin/bash"
	echo "#SBATCH --job-name=fxcorr-$PHASE"
	echo "#SBATCH --nodes=$NODES"
	echo "#SBATCH --exclusive"
	echo "#SBATCH --time=$TIME"
	echo "#SBATCH --mem=$MEM"
	echo "#SBATCH --output=$LOGDIR/%j.out"
	echo
	echo "set -uo pipefail"
	echo "source $ENV_SCRIPT"
	echo "cd $WORKDIR"
	echo 'echo "=== fxcorr '"$PHASE"'  job=\$SLURM_JOB_ID  nodes=\$SLURM_JOB_NUM_NODES ==="'
	echo 'echo "节点: \$(scontrol show hostnames \$SLURM_JOB_NODELIST | tr "\n" " ")"'
	echo 'echo "开始: \$(date)"'
	for e in ${ENVS+"${ENVS[@]}"}; do echo "export $e"; done
	echo 'echo "命令: '"$CMD"' </dev/null"'
	echo "time $CMD </dev/null"
	echo 'echo "结束: \$(date)"'
} > "$SB"

if [ "$DRY" = 1 ]; then
	echo "=== dry-run: $SB ==="
	cat "$SB"
	exit 0
fi

command -v sbatch >/dev/null 2>&1 || { echo "slurm.sh: sbatch not found (are we on the login node?)" >&2; exit 2; }
JOBID=$(sbatch --parsable "$SB")
[ -n "$JOBID" ] || { echo "slurm.sh: sbatch returned no job id" >&2; exit 1; }
echo "slurm.sh: $PHASE 已提交 -> 作业 $JOBID"
echo "         脚本   $SB"
echo "         日志   $LOGDIR/$JOBID.out"

if [ "$WAIT" = 1 ]; then
	echo "slurm.sh: 等待 $JOBID ..."
	# 提交后立刻查可能还没进队列，先等一下再轮询
	while squeue -h -j "$JOBID" 2>/dev/null | grep -q .; do sleep 15; done
	echo "=== $LOGDIR/$JOBID.out 尾部（判据在这里）==="
	tail -40 "$LOGDIR/$JOBID.out" 2>/dev/null || echo "（日志还没落盘，稍后再看）"
fi
