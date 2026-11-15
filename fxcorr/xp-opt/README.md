# xp-opt：fxcorr-xpp 单任务性能优化

**建立**：2026-11-08。**当前口径（2026-11-15 起）**：只优化**单任务**——
一个 (batch, ds 组) 的一个 xpp 进程；多进程 × 多线程的组合由计算框架负责，
不在本目录。

## 目标与边界

- 指标：单任务 wall 相对 `fxcorr-xp OMP=1`（同 batch、同 ds 组）的加速比。
- 线程数 P ≤ 16；一个进程绑一个 NUMA 节点的连续核。
- 正确性：分片产物 `vis-parts/<batch>/ds<G>.part` 的 md5 与 xp 逐字节一致
  （SWIN 由 merge 写出）。
- 数据：4 站 `workdir-4st-b1024ms-n3`，每批 4 个 ds 组、每组 8 ds；
  锚点 md5 `56039479bdd341ef3072118c82019748`。
- 不做：band 并行（实测劣化）、单进程 P>16、多进程调度、block 模式（按需再评估）。

## 目录

```text
xp-opt/
├── README.md
├── docs/
│   ├── README.md        # 索引
│   ├── environment.md   # 机器、数据、镜像、测量口径
│   ├── plan.md          # 单任务优化路线（Step 1–5）
│   ├── optimizations.md # 优化项与效果（相对单线程 xp）
│   ├── parameters.md    # 单任务参数表（P、绑核、环境变量）
│   ├── log.md           # 当前口径的运行台账
│   └── archive/         # 历史口径与负结果（只作对照）
├── scripts/
│   ├── prepare_workdir.sh
│   ├── run_xp.sh        # 参照程序
│   ├── run_xpp.sh       # 优化版（默认 --bind close --cpuset auto）
│   └── sync_results.sh
├── results/             # 正式汇总（README + summary.tsv）
└── results-detail/      # 详细 run 产物（gitignored）
```

仓库外临时区：`/Users/kaichao/WORK/fxcorr-scratch/xp-opt/{scripts,results}`；
远端 `$XP_OPT_ROOT/scratch/`。

## 正式 / 临时 / 归档

- **正式**：`scripts/run_*.sh --formal`，落 `results/<日期>/<run-id>/` 与
  `results/summary.tsv`。
- **临时**：不带 `--formal` → 远端 `scratch/results/`，不同步进 git。
- **归档**：`docs/archive/`，只是历史记录，不能作为当前结论的依据。

## 快速开始（单任务）

```bash
ssh difx
cd ~/fxcorr/fxcorr/xp-opt
WD=/data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-4st-b1024ms-n3

./scripts/run_xp.sh  -j 1 -g 0 "$WD" 00000001     # 参照：xp OMP=1
./scripts/run_xpp.sh -j 8 -g 0 "$WD" 00000001     # 优化版，默认绑核
# 宿主构建的二进制：加 -b /home/scalebox/fxcorr/applications/fxcorr-xpp/src
```

环境、数据与判据见 [`docs/environment.md`](docs/environment.md)；路线见
[`docs/plan.md`](docs/plan.md)；**优化项与效果**见
[`docs/optimizations.md`](docs/optimizations.md)；**单任务参数表**见
[`docs/parameters.md`](docs/parameters.md)；实测见 [`docs/log.md`](docs/log.md)。
