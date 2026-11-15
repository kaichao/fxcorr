# 归档：全批单进程 OMP 扫描与目录整理（2026-11-08 ~ 11-15）

> **归档说明（2026-11-15）**：这些实验的口径是"一个进程跑整批（4 组串行）"，
> 与当前单任务口径（一个 (batch, ds 组) 一个进程）不同，仅作历史对照。
> 绑核/NUMA 的结论仍然有效，见 `../log.md`。

## 2026-11-08 建立工作区与首轮基线

### 1. 目录与脚本

- 新建 `fxcorr/xp-opt/`：`prepare_workdir.sh`（实验 workdir）、`run_xp.sh`
  （docker 运行 + 计时）、`results.tsv`。
- 实验 workdir：
  `/data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-2st-b1024ms-n3`
  ——`config/` `batches/` `fengine/` 软链到源 workdir，`meta/` `vis/` `product/`
  `vis-parts/` `raw/` 新建。
- 源 workdir：`/data/scalebox/mydata/fxcorr/workdir-2st-b1024ms-n3`
  （2 站 × 8 ds = 16 ds、4 组、128 通道、1.024 s）。

### 2. 失败 1：容器内缺 workdir 挂载

现象（rc=1，0.009 s）：

```text
fxcorr-xp: cannot open .../xp-opt/workdir/workdir-2st-b1024ms-n3/batches/00000001.json
```

根因：`run_xp.sh` 只挂了软链目标（源 workdir），没挂实验 workdir 本身；docker
`-w <workdir>` 指向不存在路径时静默建了空目录。

修复：挂载数组初始加入 `-v "$WD:$WD"`。修复后同一命令 rc=0（111.9 s）。

### 3. 失败 2：root 产物宿主删不掉

现象（OMP=8 首跑，rc=1）：清理 `vis/` 时大量

```text
rm: cannot remove '.../vis/test_1.difx/...': Permission denied
```

根因：容器以 root 运行，`vis/` 产物属 root；宿主用户对 `vis/test_1.difx/` 目录
无写权限。清理失败后 SWIN 未删，xp 触发互斥检查退出。

修复：`clean_outputs()` 改为在容器内执行 `rm -rf` + `mkdir -p`。

### 4. 基线（OMP 1 / 8 / 16）

条件：全量模式（16 ds、4 组）、`fxcorr/fxcorr:latest`、每次清空 `vis/`。

| OMP | wall (s) | user (s) | sys (s) | 峰值 RSS (MiB) | station (s) | read (s) | xmac (s) | wacc (s) | 相对 OMP=1 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 119.8 | 117.3 | 2.0 | 783 | 111.1 | 0.96 | 6.12 | 0.75 | 1.00× |
| 8 | 22.4 | 146.9 | 6.7 | 768 | 15.6 | 1.01 | 4.22 | 0.59 | 5.35× |
| 16 | 14.6 | 175.0 | 5.2 | 765 | 9.23 | 1.22 | 2.61 | 0.56 | 8.20× |

- 三次 SWIN md5 相同：`82eb2c23e93d4a843e5e719cea87b231`（281,088 B）——ds 级
  并行位一致。
- `station` 是主体（OMP=1 占 93%）；OMP≥8 后 `xmac` 串行段成为次瓶颈。
- 首轮临时 runs（旧脚本）同一数据的 wall：111.9 / 20.6 / 14.7 s——与正式值相差
  约 0–8%，属宿主负载/页缓存的正常波动；以 `results/` 为准。

### 4.1 测量口径修正：内存峰值

首轮 OMP=1 报 2826 MiB，重跑报 786 MiB。根因不是程序行为：旧脚本用容器
cgroup v2 `memory.peak`，它**包含页缓存**（读 `stream.bin` 的缓存），随宿主
缓存状态变化，跨运行不可比。正式脚本改为在容器内用 `python3` wrapper 取
`RUSAGE_CHILDREN.ru_maxrss`（真实进程峰值 RSS），同时保留 cgroup 值作参考
（`metrics.json` 的 `mem_cgroup_peak_mib`）。修正后 783/768/765 MiB，与
`fxcorr-xp/CLAUDE.md` 的 ~55 MB/ds 量级一致。

### 5. 目录重构（同日）

参照 `app-fxcorr/experiments` + `scratch` + `scripts/exp` 的组织：

- `scripts/`：正式脚本（`prepare_workdir.sh`、`run_xp.sh`、`sync_results.sh`）；
- `scratch/`：临时脚本与临时结果；
- `results/<日期>/<run-id>/`：正式 run 产物（manifest/result/metrics/command/logs）；
- `calibration/`：线程、内存、I/O 标定；
- `docs/`：环境、计划、台账（本文件）。

`run_xp.sh` 升级：`--formal` 生成标准 run 产物；默认输出到 `scratch/results/`。

### 6. 正式重跑（同日）

用升级后的脚本、`--formal`、RSS 口径重跑三轮，结果在
`results/20261108/<run-id>/`，汇总 `results/summary.tsv`：

| run-id | OMP | wall (s) | 峰值 RSS (MiB) | SWIN md5 |
|---|---|---|---|---|
| `workdir-2st-b1024ms-n3-00000001-omp1` | 1 | 119.8 | 783.1 | `82eb2c23…` |
| `workdir-2st-b1024ms-n3-00000001-omp8` | 8 | 22.4 | 768.0 | `82eb2c23…` |
| `workdir-2st-b1024ms-n3-00000001-omp16` | 16 | 14.6 | 765.1 | `82eb2c23…` |

每个 run 目录含 `manifest.json`（镜像 ID、主机、脚本 sha256、batch 元数据）、
`result.tsv`、`metrics.json`（阶段耗时、RSS/cgroup 内存、SWIN 指纹）、
`command.sh`、`logs/`。结果已用 `scripts/sync_results.sh` 拉回仓库。

后续接续：`app-fxcorr/docs/experiments/difx.md` 的固定单元是 S=16、1.024 s、
3 批；下一步先补该基线，再 profile `station` 内部与 `xmac` 串行段。

## 2026-11-15 4 站 1.024 s：xpp OMP 扫描（1/2/4/8）

- 条件：`workdir-4st-b1024ms-n3`、batch `00000001`（4 站 × 8 ds = 32 ds、4 组、
  1.024 s、200 subint、128 通道）、全量模式、镜像
  `sha256:bdff7cfb…`（2026-11-15 11:11 构建）、每档 1 次。
- 参照：同批先跑 `fxcorr-xp` OMP=1。
- **正确性**：五次运行 SWIN md5 全为
  `49551190c5f3ef8d2da790dcdaa7367a`（1,124,352 B）——xp 与 xpp、以及
  xpp 的 OMP=1/2/4/8 逐字节一致。

| 实现 | OMP | wall (s) | user (s) | sys (s) | 峰值 RSS (MiB) | station (s) | read (s) | xmac (s) | wacc (s) | 相对 xp OMP=1 |
|---|---|---|---|---|---|---|---|---|---|---|
| xp | 1 | 214.98 | 210.20 | 3.72 | 1551 | 178.29 | 1.11 | 29.15 | 4.38 | 1.00× |
| xpp | 1 | 214.17 | 209.41 | 3.71 | 1560 | 178.03 | 1.12 | 28.74 | 4.34 | 1.00× |
| xpp | 2 | 120.40 | 231.38 | 6.09 | 1557 | 88.94 | 1.05 | 26.24 | 2.87 | 1.78× |
| xpp | 4 | 85.93 | 317.61 | 8.69 | 1554 | 49.71 | 1.21 | 31.13 | 2.11 | 2.49× |
| xpp | 8 | 50.69 | 355.02 | 8.50 | 1557 | 27.88 | 1.16 | 18.61 | 1.33 | 4.23× |

其余阶段（各档合计，秒）：`zero` 0.04–0.05、`uvshift` 0.03–0.04、
`cbw` 0.004–0.012、`auto` 0.03–0.04、`addsub` 0.04–0.06，均小于 0.1 s。

- **station 主导并稳定扩展**：178.0 → 88.9（2.00×）→ 49.7（3.58×）→ 27.9（6.39×）；
  OMP=8 时仍占 wall 的 55%。
- **xmac 是第二瓶颈**：OMP=1 占 13%，OMP=8 占 37%（18.6 s）；其扩展不单调
  （OMP=2 26.2 s、OMP=4 31.1 s、OMP=8 18.6 s），OMP=4 的点是单次运行的异常值，
  复测结果见下（第二轮 OMP=4 为 36.9 s，异常复现）。
- **wacc 扩展正常**：4.34 → 1.33 s（3.26×）。
- 并行效率（wall）：OMP=2 0.89、OMP=4 0.62、OMP=8 0.53；station 的 6.39× 与
  理想 8× 之间的差距（约 5.6 s）以及 xmac 是下一步的主要优化对象。
- 结果目录：`results-detail/20261115/`（gitignored）；汇总行已追加到
  `results/summary.tsv`。

### 第二轮复测（同日）

同一 workdir、同一 batch、同一镜像，五档各重跑一次；run-id 带 `-r2`。

| 实现 | OMP | wall r1 (s) | wall r2 (s) | Δwall | station r1→r2 | xmac r1→r2 | wacc r1→r2 |
|---|---|---|---|---|---|---|---|
| xp | 1 | 214.98 | 212.25 | −1.3% | 178.30→176.89 | 29.15→28.51 | 4.38→4.29 |
| xpp | 1 | 214.17 | 212.79 | −0.6% | 178.03→176.80 | 28.74→28.93 | 4.34→4.39 |
| xpp | 2 | 120.40 | 123.44 | +2.5% | 88.94→88.43 | 26.24→29.26 | 2.87→2.79 |
| xpp | 4 | 85.93 | 91.52 | +6.5% | 49.71→49.50 | 31.13→36.92 | 2.11→2.20 |
| xpp | 8 | 50.69 | 55.80 | +10.1% | 27.88→29.03 | 18.61→21.85 | 1.33→1.36 |

- 十次运行（含两轮 xp/xpp）SWIN md5 仍全部相同：`49551190…`。
- 峰值 RSS 两轮一致，1551–1560 MiB。
- **OMP=1 复现性最好**：wall 差 −1.3%/−0.6%，station 差 <1%。
- **OMP≥2 的轮间差异几乎全在 xmac**：xmac 变化 +11.5%（OMP=2）、
  +18.6%（OMP=4）、+17.4%（OMP=8），而 station 在 OMP=1/2/4 的变化均 ≤0.8%，
  只有 OMP=8 为 +4.1%。
- **OMP=4 的 xmac 高于 OMP=2 不是单次异常**：两轮都是 31.1/36.9 s（OMP=4）
  对 26.2/29.3 s（OMP=2）。宿主机是 2 socket × 96 核、2 个 NUMA 节点，
  xmac 内部的 OpenMP 循环可能受线程落点/跨 NUMA 访存影响；需要用
  `FXCORR_TIMING_TRACE` 与绑核实验进一步定位。
- 第二轮 OMP≥2 的 wall 比第一轮高 2.5%–10.1%，但同镜像同源码，差异集中在
  xmac 而非 station，不能归因于代码变化。

## 2026-11-15 目录整理：正式/详细/临时分离

- 本地仓库只保留：`scripts/`（正式脚本）、`results/`（`README.md` +
  `summary.tsv` 正式汇总）、`docs/`、`calibration/`。
- 详细 run 产物移到 `results-detail/<日期>/<run-id>/`，新增
  `fxcorr/xp-opt/.gitignore` 忽略该目录；`sync_results.sh` 已改为把详细结果
  拉到 `results-detail/`、只把 `summary.tsv` 同步进 `results/`。
- 仓库内 `scratch/` 已移除；临时脚本与临时结果放仓库外
  `/Users/kaichao/WORK/fxcorr-scratch/xp-opt/{scripts,results}`（远端对应
  `/data/scalebox/mydata/fxcorr/xp-opt/scratch/`）。
- 已跟踪的 `results/20261108/*` 与 `scratch/README.md` 的物理移动会在
  git status 中显示为删除，由用户自行暂存。

