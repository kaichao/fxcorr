# 环境与数据

## 机器（ssh difx）

| 项 | 值 |
|---|---|
| 主机名 | `gjnode5`（`ssh difx` 入口） |
| CPU | Intel Xeon 6966P-C，2 socket × 96 物理核（HT 后 `nproc` = 384） |
| NUMA | 2 个节点（node0: CPU 0-95/192-287；node1: CPU 96-191/288-383），各约 385 GiB |
| CPU 调频 | `powersave` |
| 内存 | 754 GiB |
| 数据盘 | `/data/scalebox/mydata/fxcorr/`（28 TB 卷，2026-11-08 已用 46%） |
| docker | `/usr/bin/docker`；镜像 `fxcorr/fxcorr:latest`（ID `sha256:48c0d…`，2026-11-08） |

容器内 `fxcorr-xp` 位于 `/usr/local/difx/bin/fxcorr-xp`；镜像含 `python3`，
不含 GNU `/usr/bin/time`（计时用 bash `time`，内存用 cgroup v2 `memory.peak`）。

## 数据资产

根：`/data/scalebox/mydata/fxcorr/`

| workdir | 站数 | 批长 | batch 数 |
|---|---|---|---|
| `workdir-2st-b1024ms-n3` … `b16384ms-n3` | 2 | 1.024 / 2.048 / 4.096 / 8.192 / 16.384 s | 3 |
| `workdir-{4,8,16}st-b1024ms-n3` … `b16384ms-n3` | 4/8/16 | 同上 | 3 |

- 每套 workdir 含 `config/`、`batches/`、`raw/`、`fengine/`；**xp 的输入是
  `fengine/<batch>/<g>/<station>/ds_<N>/stream.bin`**（fxcorr-fp 产出，v9 链）。
- `workdir-2st-b1024ms-n3`：2 站 × 8 ds = 16 datastream、4 个 ds 组；1.024 s、
  subint 5.12 ms、每 ds 128 通道；fengine 2.0 GB/batch。
- 2 站的 `raw/` 是指向 `workdir-16st-b1024ms-n3/raw` 的相对软链；xp 不读 raw。
- batch 元数据：`batches/<batch_id>.json`（站列表、时长、config/im 路径）。

## 实验 workdir

源 workdir 直接跑会被拦：

1. `vis/` 已有上一轮 SWIN → **SWIN 互斥检查**拒绝重写；
2. `meta/roots/<batch>.json` 记录的是 scalebox 平台路径
   （`workdir=/cluster_data_root/...`、`raw=/tmp/fxcorr/raw`）→ 宿主直跑根检查
   报 `disagrees`。

`scripts/prepare_workdir.sh` 构造实验 workdir：

```text
/data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-<...>/
  config   -> 源 workdir/config     （软链，只读）
  batches  -> 源 workdir/batches    （软链）
  fengine  -> 源 workdir/fengine    （软链；xp 幂等写回 pcal.bin）
  meta/ vis/ product/ vis-parts/ raw/  新建空目录
```

`meta/` 为空是有意的：无根记录 → `FxcorrPath::checkRoots` 放行。

## 容器运行

```text
docker run --rm -w <workdir> -v <workdir>:<workdir> -v <源 workdir>:<源 workdir>
  -v <run>/logs:/out -e OMP_NUM_THREADS=N -e FXCORR_TIMING_BRIEF=/out/timing-brief.tsv
  fxcorr/fxcorr:latest fxcorr-xp <batch> <workdir>
```

- 源 workdir 的挂载来自 `config/batches/fengine` 软链目标的父目录，脚本自动解析。
- 优化版二进制：`-b DIR` → `-v DIR:/opt/xp-opt/bin:ro`，PATH 前置。
- 容器默认 root：`vis/` 产物宿主用户删不掉，`run_xp.sh` 的清理在容器内执行。

### 单任务口径（2026-11-15 起）

- 任务 = 一个 (batch, ds 组)：`fxcorr-xpp <batch> <workdir> <G>`，分片写
  `vis-parts/<batch>/ds<G>.part`；进程数 = 任务数，由计算框架调度。
- 每组 ds 数 = 2 × 站数（每站每频段 X/Y 两极化）：2/4/8/16 站 → 4/8/16/32；
  每批组数 = 每站 ds 数 / 2（本数据为 4）。
- 线程上限 P ≤ 16；一个进程绑一个 NUMA 节点的连续核（`--cpuset auto` 已按 P
  取 0..P-1、mems=0）。
- 判据：`.part` md5 与 xp 相同；正式 SWIN 由 merge 写出后核对。
- 框架预留：多进程时每个进程需要不相交的 cpuset 槽（16 核/槽，node0 六个、
  node1 六个）；槽位分配不属本目录。

### 线程绑定（2026-11-15 实测）

- **libgomp 默认不绑核**：`OMP_DISPLAY_AFFINITY=true` 显示所有线程的亲和性都是
  `0-383`，线程可在两个 NUMA 节点间迁移。
- 跨节点落点会显著改变 `xmac`/`wacc`/`station` 的耗时；同 OMP=4 同数据，
  `close`（全在 node0）xmac 18.9 s，`spread`（2+2 跨节点）34.6 s。
- `run_xpp.sh` 默认 `--bind close --cpuset auto`（OMP ≤ 96 时
  `--cpuset-cpus=0..N-1 --cpuset-mems=0`），xpp 的 manifest/metrics 记录绑定
  参数；`--no-bind` 可关闭。`run_xp.sh` 不加绑定，保持参照程序原样。
- 正式对比必须固定绑定：`OMP_PROC_BIND=close OMP_PLACES=cores`，或 docker
  `--cpuset-cpus=... --cpuset-mems=...`；结果里记录绑定方式。
- **`numactl` 前缀对 `docker run` 无效**：容器由 daemon 启动，`numactl ...
  docker run` 不会传给容器（实测容器内 `Cpus_allowed_list` 仍为 `0-383`、
  `Mems_allowed_list` 仍为 `0-1`）。要用 cpuset 参数或 OMP 绑定变量。

## 测量口径

| 指标 | 来源 |
|---|---|
| `wall_s` / `user_s` / `sys_s` | 容器内 bash `time` 包住 `fxcorr-xp`（`TIMEFORMAT='__TIME__ %R %U %S'`） |
| `host_wall_s` | 宿主侧 `docker run` 前后时间差（含容器启停） |
| `mem_peak_mib` | 容器内 `python3` wrapper 取 `RUSAGE_CHILDREN.ru_maxrss`（真实进程峰值 RSS） |
| `mem_cgroup_peak_mib` | 容器 cgroup v2 `/sys/fs/cgroup/memory.peak`（**含页缓存**，只作参考，跨运行不可比） |
| 阶段耗时 | `FXCORR_TIMING_BRIEF` → `logs/timing-brief.tsv`（station/read/zero/xmac/wacc/uvshift/cbw/auto/addsub） |
| SWIN 指纹 | `vis/**/DIFX_*` 的文件数、总字节、逐文件 md5 |
| `.part` 指纹 | 单任务分片的 `vis-parts/<batch>/ds<G>.part` 的 md5 与字节数（run 后立即记录，下次 run 会清空） |

## 结果与临时目录

- 正式 run（远端）：`/data/scalebox/mydata/fxcorr/xp-opt/results/<日期>/<run-id>/`。
- 本地详细结果：`fxcorr/xp-opt/results-detail/<日期>/<run-id>/`（gitignored，
  由 `scripts/sync_results.sh` 拉回）。
- 本地正式汇总：`fxcorr/xp-opt/results/summary.tsv`（进 git，同一个脚本同步）。
- 临时脚本/结果（远端）：`/data/scalebox/mydata/fxcorr/xp-opt/scratch/`；
  本地仓库外：`/Users/kaichao/WORK/fxcorr-scratch/xp-opt/`。两者都不进 git。

## 已知坑

1. **漏挂 workdir**：只挂软链目标不够，容器内 `<workdir>` 不存在时 docker 会静默
   建空目录，xp 报 `cannot open batches/<id>.json`。
2. **root 产物**：见上，清理必须在容器内做。
3. **fengine 软链可写**：xp 写回 `pcal.bin`，源数据需可写（幂等）。
4. **镜像与宿主数值差异**：跨构建对照不要用"逐字节"判据（容器与宿主基础库不同，
   约 2 ULP；见仓库 memory 与 `fxcorr/CLAUDE.md`）。本工作区的对拍都在同一镜像内。
