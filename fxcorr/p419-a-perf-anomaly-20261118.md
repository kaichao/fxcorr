# p419 方案 A 性能异常排查记录（2026-11-17 ~ 11-18）

**状态**：未结案。首要嫌疑已定位到"wrapper 里的 `taskset` 覆盖 rankfile 逐 rank 绑定"，
但**尚未做判别实验**。本文把现象、口径、已排查项、证据与反证完整记录，供后续分析接手。

**读者**：接手本问题的人或模型。所有数字都可用 §9 的路径复算。

---

## 1. 摘要

- **现象**：p419 上方案 A（原生 `mpifxcorr`）的 wall 比 2026-11-08/09 的历史基线慢
  **16%–95%**，且慢的程度随节点数变化（1n 最严重）。数据、节点、镜像、`mpifxcorr`
  二进制、`run_bench.sh` 均已排除。
- **首要嫌疑**：A 的两套驱动脚本在 wrapper 上不一致——历史基线用的那份**不做** `taskset`，
  今天用的这份在 wrapper 里加了 `taskset -c 1-22,24-29`，把 `mpirun --rankfile` 已经设好
  的**逐 rank 单核亲和覆盖成整个 28 核掩码**。
- **未解释**：8 节点每节点 28 rank / 28 核（比例 1.00）却仍慢 19%，单靠"均摊超订"解释不通，
  怀疑还有 NUMA 迁移因素。
- **已排除**：数据内容、共享盘 I/O 等待、CPU 频率、节点集合（8n 逐台相同）、镜像与
  `mpifxcorr` md5、`run_bench.sh` 版本、启动段开销。
- **误判记录**：曾判定"OpenMPI 5.x 不支持 rankfile"，据此把镜像从 debian 13（OpenMPI 5.0.x）
  改成 debian 12（OpenMPI 4.1.4）。该判定**不成立**（见 §6），改动已回滚。

---

## 2. 现象与观测

### 2.1 A 的 wall 对照（同数据、同 NP、同 `run_bench.sh`）

单位秒；"内部"= mpifxcorr 自己打印的 `Total wallclock time`，与 a-side 的 `real` 差值为启动段。

| 配置 | 历史基线 | 日期 | 今天 | 日期 | 比值 | 今天内部 |
|---|---:|---|---:|---|---:|---:|
| 1n 2.048 | **1343.0** | 11-08 | **2622** | 11-17 | 1.95 | — |
| 4n 0.256 | **1394.0**（内部 1354.63） | 11-09 | **1619**（内部 1577.41） | 11-17 08:31 | 1.16 | 1577.41 |
| 4n 0.512 | 1382.5 | 11-09 | 1615（内部 1577.78） | 11-17 09:25 | 1.17 | 1577.78 |
| 4n 1.024 | 1370.5 | 11-09 | 1617 | 11-17 09:52 | 1.18 | — |
| 4n 2.048 | 1364.7 | 11-09 | 1613（内部 1587.07） | 11-17 10:19 | 1.18 | 1587.07 |
| 8n 2.048 | **1362.2** | 11-09 | **1736** | 11-17 | 1.27 | — |
| 8n 4.096 | **1368.5** | 11-09 | **1634** | 11-17 | 1.19 | — |
| 4n 0.256（4.1.4 镜像） | — | — | 2370.2 | 11-17 14:08 | — | — |
| 4n 0.256（恢复 5.0.7 镜像） | — | — | 1667（内部 1596.34） | 11-17 15:03 | 1.20 | 1596.34 |

历史基线全表（六档）见附录 A。**启动段没有变化**：历史 1394.0−1354.63 = 39.4 s，
今天 1619−1577.41 = 41.6 s。差异全部在 mpifxcorr 内部。

### 2.2 可比性（为什么这些对比成立）

- **数据相同**：`raw/` 下 VDIF 文件的 mtime 是 2026-10-21，11-09 之后未重造。
- **工作量相同**：两次 4n 0.256 的 mpifxcorr 收尾统计 `Min/Mean/Max number of subints
  processed is … 500.49 …`，**均值逐位相同**（500.49）；记录数均 983040/983040。
- **流量相同**：a-side 采样 `Σrx 1 774 543 MB / Σtx 806 960 MB`（历史） vs
  `1 776 036 / 807 840 MB`（今天），单 rank 峰值 370 vs 369 MB。
- **节点相同（8n）**：两次 8n 都是同一组 8 台：`h03r3n07 h03r3n11 h04r1n07 h04r2n05
  h04r3n03 h04r3n07 h04r4n03 h04r4n17`。4n 换了 1 台（`h03r4n01` 已下线，用 `h04r1n07`），
  但 1 台换机解释不了 8n 的 +19%。
- **硬件相同**：全部 `Hygon C86 7185 32-core`、32 核、4 NUMA、123 GB；
  运行中 `/proc/cpuinfo` 恒 2000 MHz（performance governor），load ≈35，无其他用户进程。

---

## 3. 环境与口径

### 3.1 A 的启动链路（多节点正式口径）

```
宿主 mpirun（HPC-X v2.11.0，Open MPI 4.1.4rc1）
  ├─ --rankfile <per-host rankfile>        ← 逐 rank 绑核在这一层完成
  ├─ --bind-to core:overload-allowed
  ├─ --mca mpi_yield_when_idle 1
  └─ 每 rank 一个 wrapper：singularity exec <sif> /usr/local/difx/bin/mpifxcorr
```

- rankfile：manager + 32 个 datastream rank → 前 4 个 I/O 核（`1-4`）；
  core rank → 24 个计算核（`5-22,24-29`）。生成逻辑见 `p419/a-side.sh` 的 python 段。
- **容器内 mpirun 不参与**——容器只跑单 rank 的 `mpifxcorr` 可执行文件。
  这一点是 §6 判定"镜像与 rankfile 支持无关"的关键。
- 1n 例外：`A_MPI_LAUNCH=inproc` 时走容器内 mpirun（诊断路径）；
  11-08 起 1n 也默认走宿主路径。

### 3.2 两套驱动脚本

| 脚本 | 位置 | wrapper 是否 taskset | 产出 |
|---|---|---|---|
| 历史版（11-08，md5 `70e84e94…`） | `~/fxcorr/fxcorr/p419/a-side.sh` | **否** | **全部历史 A 基线**（`rev-*.txt` 27 个 tag：2n/4n/8n/16n） |
| 现役版 | `scripts/exp/lib/a-side.sh`（app-fxcorr 仓库，经 `run-a.sh` 调用） | **是**（自 commit `6a992c1`，2026-11-07） | 今天全部 A 测量 |

两版**其余部分等价**：rankfile 生成 python 段逐行相同（diff 为空）、
`FXCORR_MPI_RANKFILE` / `FXCORR_MPI_BIND` / `FXCORR_MPI_EXTRA`（含 yield）/
`OMPI_MCA_plm_rsh_args` 相同、`REPO` 与 `run_bench.sh` 相同。

### 3.3 镜像与版本

| 文件 | 内容 | 备注 |
|---|---|---|
| `fxcorr.sif`（当前） | difx `fxcorr/fxcorr:latest`（2026-11-15 22:18），**Open MPI 5.0.7** | 192 573 440 B；`mpifxcorr` md5 `00f51a4f3a29b302c75a9e26ef51708c` |
| `fxcorr.sif.bak-20261117-openmpi4.1.4` | 自建 debian 12（bookworm），**Open MPI 4.1.4** | 122 023 936 B；为误判而建，A 用它更慢（2370.2 s） |
| `fxcorr-agent.sif`（当前） | 由 `fxcorr/fxcorr:hist` 重建的 agent，5.0.7，含 `fxcorr-xpp` | 206 897 152 B |
| `fxcorr-agent.sif.bak-20261117-1106` | difx 11-06 agent，5.0.7，**不含 `fxcorr-xpp`** | 205 697 024 B |
| `fxcorr-agent.sif.bak-20261117-openmpi4.1.4` | 上述 4.1.4 配套 agent | 136 351 744 B |

> ⚠ **2026-11-17 13:58 之前的原 `fxcorr.sif` 已被覆盖且没有备份**，其 Open MPI 版本
> 无法再复核（只能从 §6.2 的报错形态反推它不是 5.0.7 也不是 4.1.4）。
> difx 上 10-14 / 11-06 / 11-15 三代镜像的 `mpifxcorr` md5 全同，但 **md5 只覆盖可执行
> 文件，不覆盖运行时 `libmpi`**。

---

## 4. 已排查项与结论

| # | 假设 | 证据 | 结论 |
|---|---|---|---|
| 1 | 数据变了（重造 / 切片不同） | raw mtime 2026-10-21；`subints` 均值 500.49 逐位相同；记录数全对 | **排除** |
| 2 | 共享盘 I/O 变慢 | 运行中 `vmstat`：us 79–80%、sy 11–14%、**wa 0**；节点上 `read_bytes` 无持续增长；文件系统 ParaStor300S | **排除（无 I/O 等待）** |
| 3 | CPU 降频 / 邻居负载 | 8 台均 2000 MHz 恒定；load ≈35（28 核作业 + 33 rank）；`ps` 无其他用户 | **排除** |
| 4 | 节点集合不同 | 8n 两台次逐台一致；4n 仅换 1 台 | **8n 排除**；4n 未量化（影响 <10%） |
| 5 | 镜像 / `mpifxcorr` 二进制退化 | 三代镜像 md5 全同；换成 4.1.4 后更慢（2370）；恢复 5.0.7 后仍慢（1596 内部） | **排除** |
| 6 | `run_bench.sh` 版本漂移 | 历史 rev 文件 `RUN_BENCH_REV=20261107-yield`，md5 `92c3bd7a…`；今天用同一文件 | **排除** |
| 7 | 启动段（展开 FILE 行、容器冷启）变慢 | 历史 39.4 s vs 今天 41.6 s | **排除** |
| 8 | 时段 / 白天负载 | 08:31 / 09:25 / 09:52 / 10:19 / 15:03 五个时点，内部墙钟 1577–1596，稳定 | **排除（无时段效应）** |
| 9 | wrapper `taskset` 覆盖 rankfile 绑定 | 见 §5 | **首要嫌疑，未验证** |
| 10 | `env -u OPAL_PREFIX -u OMPI_HOME`（11-17 新增） | 同一脚本 09:xx vs 15:03：内部墙钟 1577.41 → 1596.34（+1.2%） | **影响很小（≤1.2%，且可能是噪声）** |
| 11 | 容器内 OpenMPI 版本 | 见 §6 | **与 A 无关**（容器 mpirun 不参与多节点口径） |

---

## 5. 关键线索：wrapper `taskset` 覆盖 rankfile 绑定

### 5.1 证据链

1. 全部历史 A 基线由 `p419/a-side.sh`（11-08 版）产出，其 wrapper 是
   `exec singularity exec … mpifxcorr "$@"`——**没有 taskset**，注释明写
   "绑定由 mpirun 的 rankfile 给出，这里不做 taskset"。
2. 今天所有 A 测量由 app-fxcorr 的 `scripts/exp/lib/a-side.sh` 产出，其 wrapper 是
   `exec taskset -c '1-22,24-29' singularity exec … mpifxcorr "$@"`。
3. `taskset` 在 `exec singularity` **之前**执行，会把宿主 `mpirun` 按 rankfile 设好的
   **单核掩码替换成整个 28 核掩码**。两种日志的 `--report-bindings` 输出看起来一样
   （那是 mpirun 设置时的视图），实际亲和已被覆盖，磁盘上看不出来。

### 5.2 定量吻合度

| 配置 | 每节点 rank / 28 核 | 实测 wall 比 |
|---|---:|---:|
| 1n（NP=57） | 57/28 = **2.04** | **1.95** |
| 4n（NP=129） | 32–33/28 = **1.14–1.18** | **1.16–1.18** |
| 8n（NP=225） | 28/28 = **1.00** | 1.19–1.27 |

前两行与"均摊超订"模型吻合；**8n 不吻合**（比例 1.00 却慢 19%），说明除超订外还有
全局性因素——最可能是**跨 NUMA 迁移**：节点 4 个 NUMA 域，rankfile 把 rank 钉在某个核
（同时钉住了它的局部内存域），taskset 放开后线程被调度器来回迁移，远端内存访问比例上升。

### 5.3 反证与它的问题

现役脚本里"taskset 必须加"的依据是一组 2026-11-06 的对照：

```
p22（11-05，wrapper taskset）：1324.7 s
T5 （11-06，rankfile 无 taskset）：2612.3 s
```

但 `run_bench.sh` 自己的注释写明：**2026-11-06 起 A 的 wall 翻 2.1× 的根因是
`run_bench.sh` 当时不读 `FXCORR_MPI_EXTRA`，参数被静默丢弃，缺 `mpi_yield_when_idle`**，
11-07/08 才修。两条改动同期发生，归因被混淆。

更直接的证据是 11-09 的同一脚本六档连跑：**无 taskset** 下 4n = 1364.7–1403.8 s、
8n = 1362.2–1455.3 s，完全正常。因此"无 taskset 会 2 倍变慢"不成立。

---

## 6. 误判记录：OpenMPI 与 rankfile

### 6.1 时间线

1. 2026-11-17 11:22 / 12:30，1n `A_MPI_LAUNCH=inproc` 两轮在 7 s 内失败，日志报
   `--map-by rankfile:file=…` 不被接受。
2. 据此判定"OpenMPI 5.x 移除了 rankfile"，于是改 Dockerfile：`debian:13` → `debian:12`
   （OpenMPI 5.0.x → 4.1.4），并在 wrapper 里加 `env -u OPAL_PREFIX -u OMPI_HOME`
   （4.1.4 会被宿主注入的 `OPAL_PREFIX` 带偏，报 `orte_init:startup:internal-failure`）。
3. 14:08 用该镜像跑 4n 0.256：**2370.2 s**，比原镜像的 1619 s 更慢。
4. 15:02 从 difx 恢复 5.0.7 镜像，15:03 复跑 4n 0.256：**1667 s**——差距仍在。
5. 11-18 复核：**误判**，两个文件已回滚。

### 6.2 实测反驳

| 版本 | `--rankfile <file>` | `--map-by rankfile:file=<file>` |
|---|---|---|
| 容器 5.0.7 | 可用（提示 deprecated，自动转成后者） | **可用**（实测起 2 个 rank 成功） |
| 容器 4.1.4 | 可用 | **不可用**：`unrecognized modifier` |

5.0.7 的 `--map-by` 有效 mapper 列表里明确含 `rankfile`：
`slot:hwthread:core:l1cache:l2cache:l3cache:numa:package:node:seq:ppr:rankfile:pe-list=`。

而 11:22 那次的报错是第三种形态：
`The map-by directive contains an unrecognized qualifier: file=…; Valid qualifiers:
pe=,span,oversubscribe,nooversubscribe,nolocal,hwtcpus,corecpus,inherit,noinherit,file=,ordered`
——既不是 5.0.7 的接受行为，也不是 4.1.4 的 `unrecognized modifier`。**结论：原镜像里的
OpenMPI 是第三个构建（很可能是早期 5.0.x），版本号无从复核（原 sif 无备份）。**

### 6.3 影响面界定

- **A 的多节点正式口径完全不受影响**：rankfile 由宿主 HPC-X `mpirun` 4.1.4rc1 解析，
  容器内 mpirun 从不执行。
- 只有 **1n inproc 诊断路径**会踩到；且 5.0.7 下该路径本可用。
- 因此"容器 OpenMPI 支不支持 rankfile"**不可能**是 A 性能异常的原因；
  把它当原因去换镜像，属于把诊断路径的问题误扩到正式口径。

---

## 7. 候选原因总表

| 候选 | 状态 | 证据强度 |
|---|---|---|
| wrapper `taskset` 覆盖 rankfile 绑定 | **未验证，首要嫌疑** | 中—强（时间同步、8n 同节点、1n/4n 定量吻合） |
| 跨 NUMA 迁移（taskset 放开后） | 未验证，与上一条同源 | 中（用于解释 8n 的 +19%） |
| `env -u OPAL_PREFIX -u OMPI_HOME` | 已量化，影响 ≤1.2% | 弱（已回滚） |
| 4n 换 1 台节点 | 未量化 | 弱 |
| 数据 / I/O / CPU / 镜像 / 脚本其余部分 | **已排除** | 强 |

---

## 8. 建议的判别实验（按优先级）

### E1（决定性，约 28 min）4n 0.256 + `A_NO_TASKSET=1`

```sh
# 在 p419-n1（cstu0036），其余参数与 2026-11-17 08:31 那轮完全一致
NODES="h03r3n11 h04r1n07 h04r2n05 h04r4n03" N=4 \
W=/work2/cstu0036/mydata/fxcorr/workdir-b0256ms SSH_PORT=50022 \
A_NO_TASKSET=1 bash scripts/exp/run-a.sh 960 0.256 4n-a-0256-notaskset
```

- **判定**：≈1390–1450 s → taskset 是主因，结案；
  ≈1600 s → taskset 无关，转 E2。

### E2（NUMA 证据，与 E1 同时或事后）运行中采计数器

```sh
numastat -p $(pgrep -x mpifxcorr | head -1)     # 每个 rank 的本地/远端内存比
perf stat -e node-load-misses,node-loads -p <pid>  # 跨 NUMA 访存比例
```
两种绑定各采一次，比较远端命中率。

### E3（灵敏度对照，约 25 min）1n 2.048 + `A_NO_TASKSET=1`

1n 的差异最大（1.95×），信噪比最高；**但注意 1n 默认走 inproc 路径**，
需要 `A_MPI_LAUNCH=host` 才与历史 1343.0 s 同口径。

### E4（复核历史脚本）直接用 `~/fxcorr/fxcorr/p419/a-side.sh` 重跑 4n 0.256

验证历史基线在今天的环境里仍能复现 ~1394 s。若不能，说明环境也变了，
需要重新审视 §4 的结论。

**注意**：E4 会把 wrapper 写进 `$BENCHWRAP`（默认 `~/bench-wrap-26/bin`），
与现役脚本共用同一目录，跑之前用 `A_BENCHWRAP=` 指到独立目录，避免互相覆盖。

---

## 9. 复现资料

| 内容 | 路径 |
|---|---|
| 历史脚本（无 taskset） | `~/fxcorr/fxcorr/p419/a-side.sh`（p419-n1，md5 `70e84e948ff9539caf45a21638d44cd1`） |
| 现役脚本（有 taskset） | `~/app-fxcorr-xpp/scripts/exp/lib/a-side.sh` |
| 基准驱动 | `~/fxcorr/fxcorr/run_bench.sh`（md5 `92c3bd7acb2b0c5e14b3c7b1913b7496`，`RUN_BENCH_REV=20261107-yield`） |
| 运行日志 | `/work2/cstu0036/mydata/fxcorr/a-logs/a-<tag>.log` |
| 结果表 | `/work2/cstu0036/mydata/fxcorr/a-logs/a-side.csv`（历史，含 startup_s/io_rank_ratio/args_ok） |
| 缺口测试结果 | `~/s1-logs/a-gap-results.csv`（今天） |
| 版本存根 | `/work2/cstu0036/mydata/fxcorr/a-logs/rev-<tag>.txt`（只有历史脚本写） |
| rankfile | `/work2/cstu0036/mydata/fxcorr/a-logs/rankfiles/` |
| 镜像 | `/public/home/cstu0036/singularity/fxcorr/` |
| 数据 | `/work2/cstu0036/mydata/fxcorr/workdir-b0{128,256,512,1024,2048,4096}ms` |

---

## 附录 A：历史 A 六档基线（`p419/a-side.sh`，无 taskset）

**1n（2026-11-08，h03r4n01，host 路径 NP=57）**

| 批长 | 0.128 | 0.256 | 0.512 | 1.024 | 2.048 | 4.096 |
|---|---:|---:|---:|---:|---:|---:|
| wall (s) | 1343.8 / 1334.8 | 1343.2 | 1342.5 | 1344.4 | 1343.0 | 1334.6 / 1339.5 |

**4n（2026-11-09，h04r4n03 / h04r2n05 / h03r3n11 / h03r4n01，NP=129）**

| 批长 | 0.128 | 0.256 | 0.512 | 1.024 | 2.048 | 4.096 |
|---|---:|---:|---:|---:|---:|---:|
| wall (s) | 1403.8 | 1394.0 | 1382.5 | 1370.5 | 1364.7 | 1367.3 |

**8n（2026-11-09，8 台 x12，NP=225）**

| 批长 | 0.128 | 0.256 | 0.512 | 1.024 | 2.048 | 4.096 |
|---|---:|---:|---:|---:|---:|---:|
| wall (s) | 1455.3 | 1398.7 | 1397.9 | 1383.8 | 1362.2 | 1368.5 |

**16n（2026-11-10，NP=417）**：2.048 = 1362.2（与 8n 同档接近，说明 A 的 wall 由
"每节点数据量"决定，与节点数弱相关）。

## 附录 B：今天的测量清单（现役脚本，有 taskset）

| tag | 配置 | wall (s) | 内部 (s) | 记录 | 备注 |
|---|---|---:|---:|---|---|
| 8n-a-2048-gap | 8n 2.048×240 | 1736 | — | PASS | |
| 8n-a-4096-gap | 8n 4.096×120 | 1634 | — | PASS | |
| 4n-a-0256-gap | 4n 0.256×960 | 1619 | 1577.41 | PASS | |
| 4n-a-0512-gap | 4n 0.512×480 | 1615 | 1577.78 | PASS | |
| 4n-a-1024-gap | 4n 1.024×240 | 1617 | — | PASS | |
| 4n-a-2048-gap | 4n 2.048×120 | 1613 | 1587.07 | PASS | |
| 1n-a-2048-gap2 | 1n 2.048×30（host 路径 NP=57） | 2622 | — | PASS | |
| 1n-a-2048-inproc3 | 1n 2.048×30（单容器、无 rankfile） | 2539.9 | — | PASS | 手动对照 |
| 4n-a-0256-newimg | 4n 0.256×960（4.1.4 镜像） | 2370.2 | — | PASS | 镜像实验 |
| 4n-a-0256-hist | 4n 0.256×960（恢复 5.0.7） | 1667 | 1596.34 | PASS | 镜像回滚验证 |
