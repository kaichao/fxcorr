# 运行台账

按时间顺序记录**当前口径**（单任务：一个 (batch, ds 组)）相关的实验、失败与修复。
正式结果索引在 `results/README.md`。

**历史归档**（早期全批单进程口径，仅作对照）：

- [`archive/band-block-parallelism.md`](archive/band-block-parallelism.md) —— band 并行、
  ds×block 并行（劣化/未验证，未采用）；
- [`archive/full-batch-omp-scans.md`](archive/full-batch-omp-scans.md) —— 2 站/4 站全批
  OMP 扫描、目录整理；
- [`archive/thread-scan-20261108.md`](archive/thread-scan-20261108.md) —— 2 站全批线程
  扫描（已被单任务标定取代）。

## 2026-11-15 xmac OMP 扩展性分析：线程落点/NUMA 是主因

### 1. 先排除任务不均与固定开销

- 32 个输出频率 × 1 个 xmac stride = **32 个 pass**；每个 pass 为 24 条有效
  基线 × 1 个极化积 × 10 个 buffered FFT × 128 通道。用 `.input` 的
  BASELINE 表统计：32 个频率的基线数、极化积数**逐项都是 24**，任务等成本；
  OMP=2/4/8 分别 16/8/4 个 pass/线程，`schedule(static)` 无余数。
- 每 subint 调 `xmacBatch` **128 次**（blockspersend=1280、numbufferedffts=10），
  200 subint = **25,600 个并行区**。空 32 迭代并行区实测
  2.68/4.66/10.19 µs（OMP=2/4/8），合计 0.07/0.12/0.26 s；
  `fxpasses` 串行构造 25,600 次实测 0.171 s。均不足 xmac 的 2%。

### 2. 落点实验（OMP=4，同数据、同镜像）

| 绑定 | wall | station | xmac | wacc |
|---|---|---|---|---|
| `close`（全在 node0） | 67.85 | 45.08 | **18.91** | 1.48 |
| 默认（不绑核） | 82.41 | 50.63 | 24.93 | 2.26 |
| `spread`（2+2 跨 NUMA） | 91.98 | 52.39 | **34.62** | 2.11 |

- libgomp 默认亲和性 `0-383`，不绑核；`spread` 时采样到线程在 CPU
  0/48/96/144——node0 两个、node1 两个。
- 同一份代码/数据，`spread` 对 `close`：xmac **1.83×**、wacc 1.43×、
  station 1.16×、wall 1.36×。正式两轮的 T=4 xmac 31.1/36.9 s 与 `spread`
  同量级；默认 T=2/T=8 的 probe 也出现 xmac 40.9/15.2 s 的极端值。
- **`numactl ... docker run` 无效**：容器由 daemon 启动，实测容器内
  `Cpus_allowed_list` 仍为 `0-383`、`Mems_allowed_list` 仍为 `0-1`；
  要用 `OMP_PROC_BIND/PLACES` 或 docker `--cpuset-cpus/--cpuset-mems`。

### 3. 绑定后的真实曲线（docker cpuset 0..N-1 + mems=0）

| OMP | wall | station | xmac | wacc | wall 相对 T1 |
|---|---|---|---|---|---|
| 1 | 209.39 | 174.09 | 28.47 | 4.43 | 1.00× |
| 2 | 116.70 | 89.65 | 21.51 | 2.97 | 1.79× |
| 4 | 65.82 | 44.99 | 16.87 | 1.43 | 3.18× |
| 8 | 36.04 | 22.91 | 9.82 | 0.78 | 5.81× |

- xmac 变为**单调但次线性**：1.32× / 1.69× / 2.90×（OMP=2/4/8）；
  station 为 1.94× / 3.87× / 7.60×，接近线性。
- 与不绑核的正式值相比：T=4 wall 85.9/91.5 → 65.8（−23%～−28%），
  T=8 50.7/55.8 → 36.0（−29%～−35%）。
- 同内核微基准（taskset 单节点）：T=1/4/8 = 16.1/11.2/5.8 s（T=2 在
  17.2–24.2 s 间波动，受宿主负载影响），T=8 也只有约 2.8×，与 app 的
  2.90× 同量级——说明 xmac 还有**内核级**的带宽/延迟上限，绑核只能消除
  落点波动，不能把它变成线性。

### 4. 结论与下一步

- **扩展不单调的主因是线程/内存落点抽签**，不是任务不均、OpenMP 固定开销
  或串行 `fxpasses` 构造；同 OMP 下不同落点可差 1.8×。
- 所有 OMP 扫描与正式对比必须固定绑定（`OMP_PROC_BIND=close
  OMP_PLACES=cores` 或 docker cpuset）并在结果中记录；现有未绑核的
  OMP=4/8 正式数字不能直接用于扩展性判断。
- xmac 自身在 8 线程仍只有 2.90×，是下一个优化对象：优先做**循环交换**
  （pass 在最外层并行、fftloop 在内层，每线程只碰自己的 pass/累加器，
  并行区从 25,600 个降到 200 个、改善 L2 复用）、**conj 与 mul-add 融合**
  （去掉 `conjbuf` 的写回读）、预计算 pass/基线描述并把配置查找移出内层。

## 2026-11-15 xpp 绑核 + xmac 循环交换（实施与实测）

### 1. 绑核（只改 xpp 启动脚本）

- `run_xpp.sh` 新增 `--bind close|spread|none`（默认 `close`）、
  `--cpuset auto|none|<显式>`（默认 `auto`；OMP ≤ 96 时
  `--cpuset-cpus=0..N-1 --cpuset-mems=0`）、`--mems`、`--no-bind`。
- `OMP_PROC_BIND` / `OMP_PLACES` / `OMP_DISPLAY_AFFINITY` 透传进容器；
  `manifest.json` / `metrics.json` 记录 `omp_proc_bind`、`omp_places`、
  `cpuset_cpus`、`cpuset_mems`。
- `run_xp.sh` 未改：xp 保持参照原样（不绑核）。

### 2. xmac 循环交换（只改 fxcorr-xpp）

- 新增 `XmacEngine::xmacBatchPeriod(fftloop0, fftloop1, ...)`：
  `xmacBatch` 改为薄包装，共用 `xmacBatchRange`。
- 非 pulsar 路径：`#pragma omp parallel for` 仍按 pass 分线程，但每个线程在
  内层按 fftloop 升序处理整个 uvshift 周期；`resultindex` 每个 fftloop
  从 `pass.baseoffset` 重置，所以每个累加器的加法顺序不变。
- pulsar 路径：`bins` 是共享 scratch，多 fftloop 范围自动退回逐 fftloop
  处理（行为与改动前相同）。
- `main.cpp` 按 `maxxcblocks` 的 uvshift 周期组织：每周期先
  `xmacBatchPeriod`，再逐 fftloop `accumulateWeights`，最后
  `uvshiftAndAverage`；尾部不足一个周期同样处理。

### 3. 实测（4 站 1.024 s，绑核 cpuset 单节点，SWIN md5 全同）

对照 = 同一 workdir/镜像下、未改 xpp 但用 cpuset 绑定的基线；
新增 = 循环交换版二进制（`run_xpp.sh -b` 挂载），同样 cpuset 绑定。

| OMP | wall 基线→新 | Δwall | station 基线→新 | xmac 基线→新 | Δxmac | wacc 基线→新 |
|---|---|---|---|---|---|---|
| 1 | 209.39→204.52 | −2.3% | 174.09→173.96 | 28.47→24.68 | **−13.3%** | 4.43→3.39 |
| 2 | 116.70→111.16 | −4.7% | 89.65→88.85 | 21.51→17.27 | **−19.7%** | 2.97→2.46 |
| 4 | 65.82→64.21 | −2.4% | 44.99→44.51 | 16.87→15.72 | −6.8% | 1.43→1.40 |
| 8 | 36.04→35.78 | −0.7% | 22.91→22.70 | 9.82→9.69 | −1.3% | 0.78→0.82 |

- 四次运行的 SWIN md5 均为 `49551190c5f3ef8d2da790dcdaa7367a`；
  OMP=1/2/4/8 与 xp 逐字节一致。
- 循环交换对 xmac 的收益集中在中低线程数（OMP=1/2 约 −13%/−20%）；
  OMP=8 时 xmac 已只占 wall 27%，且本身接近带宽/延迟上限，收益约 −1%。
- wall 的改善被 station（OMP=8 占 63%）稀释为 0.7%–4.7%。
- 本次测试的二进制在 difx 宿主上构建后用 `-b` 挂载，**镜像
  `fxcorr/fxcorr:latest` 尚未重建**；正式归档前需重建镜像并复跑。

### 4. 镜像重建

- 2026-11-15 14:41（CST）重建 `fxcorr/fxcorr:latest`，镜像 ID
  `sha256:fdd3e867…`（manifest list）。镜像内
  `/usr/local/difx/bin/fxcorr-xpp` 已核对含 `xmacBatchPeriod` /
  `xmacBatchRange` 符号（宿主 `nm`），不再是旧二进制。
- 随后用该镜像、`run_xpp.sh --formal` 默认绑核跑正式 OMP=1/2/4/8 扫描，
  run-id 带 `-bind`，结果落 `results/20261115/`。

### 5. 正式绑核扫描（新镜像）

条件：镜像 `sha256:fdd3e867…`；`run_xpp.sh --formal` 默认 `--bind close` +
`--cpuset auto`（OMP=1/2/4/8 → cpuset 0-0/0-1/0-3/0-7，mems=0）；
每档 1 次；四次 SWIN md5 全为 `49551190c5f3ef8d2da790dcdaa7367a`。

| OMP | wall | station | xmac | wacc | 旧 cpuset 基线 xmac | Δxmac |
|---|---|---|---|---|---|---|
| 1 | 205.88 | 174.05 | 25.92 | 3.42 | 28.47 | −8.9% |
| 2 | 115.27 | 88.53 | 21.54 | 2.68 | 21.51 | +0.1% |
| 4 | 66.93 | 44.87 | 17.83 | 1.58 | 16.87 | +5.7% |
| 8 | 35.18 | 22.49 | 9.22 | 0.90 | 9.82 | −6.1% |

### 6. 编译器差异排查（同日，结论：不存在可复现的 g++14 劣化）

同一份源码，difx 宿主 g++ 13.3 构建的 xpp（`-b` 挂载）为 xmac
24.68/17.27/15.72/9.69；镜像内 Debian 13 g++ 14.2 构建的版本为
25.92/21.54/17.83/9.22，OMP=2/4 上镜像版慢约 25%/13%，曾疑为代码生成问题。

- **静态对比**：反汇编 `xmacBatchRange._omp_fn.0`（宿主 1085 行/镜像 1144 行）
  显示两者均为 `-O2 -march=x86-64`、纯 SSE（无 ymm/zmm）、同一套
  4×`mulps`+`addps` 复数乘结构，指令 mix 基本一致；`DW_AT_producer` 差异只有
  宿主版多 Ubuntu 默认的 stack-protector/clash/CET、镜像版为 `-no-pie`。
- **运行时 A/B**：同一新镜像、同绑核（cpuset 0..N-1、mems=0）下交替跑两种
  二进制，OMP=2/4 各 2 轮（scratch 结果，非正式）：

| OMP | host g++13 xmac r1/r2 | image g++14 xmac r1/r2 | station 四轮 |
|---|---|---|---|
| 2 | 21.93 / 19.85 | 18.61 / 18.17 | 88.31 / 88.03 / 88.09 / 88.13 |
| 4 | 15.71 / 16.91 | 16.77 / 17.90 | 44.40 / 44.47 / 44.06 / 44.79 |

- 两档方向相反（OMP=2 镜像快约 12%，OMP=4 镜像慢约 6%），差异幅度与轮间波动
  同量级（OMP=2 的 xmac 单次波动约 2 s、约 10%；station 稳定）。运行期间机器上
  另有 5 个容器、load 从 0.4 升到 4.3。
- **结论**：此前的 25%/13% 是测量波动，不是编译器代码生成差异；正式表的 xmac
  单档数据应视为 ±5–10% 的单次采样。八次运行 SWIN md5 全同（`49551190…`）。
## 2026-11-15 单任务标定（Step 1）

口径：单任务 = (batch `00000001`, ds 组 0) = 8 ds（4 站 × 2 极化）；
`workdir-4st-b1024ms-n3`；分片模式写 `vis-parts/00000001/ds0.part`；
xpp 用当时镜像内二进制（循环交换版），绑核 cpuset 0..P-1、mems=0；每档 1 次。

| 实现 | P | wall | station | xmac | wacc | 相对 xp1 |
|---|---|---|---|---|---|---|
| xp | 1 | 50.13 | 40.75 | 6.76 | 1.52 | 1.00× |
| xpp | 1 | 47.03 | 39.03 | 5.85 | 1.37 | 1.07× |
| xpp | 2 | 27.79 | 20.15 | 5.85 | 1.01 | 1.80× |
| xpp | 4 | 17.71 | 10.55 | 5.77 | 0.68 | 2.83× |
| xpp | 8 | 12.68 | 6.47 | 4.90 | 0.46 | 3.95× |
| xpp | 16 | 24.85 | 15.76 | 7.23 | 0.94 | 2.02× |

- 全部 run 的 `.part` md5 为 `56039479bdd341ef3072118c82019748`（281,088 B），
  与 xp 一致。
- **xmac 在单任务内基本串行**：并行维度是"已用频率 × xmac stride"，该组只有
  1 个 pass；P=2..8 的变化来自噪声/自旋（P=8 的 user 时间中约 35 s 是空闲线程
  自旋），不是有效加速。
- **station 在 P=8 达到 6.3×**（40.75→6.47 s）。
- **P=16 反而变慢**（24.85 s）：`schedule(dynamic)` 在 32 个迭代（24 个立即
  `continue`）上分配不均，有线程拿到两个真实 ds；用 cpuset 0-15 与 16-31 复现，
  排除核区争用。

## 2026-11-15 Step 2：xmac 任务并行 + station 紧凑调度

改动（只改 fxcorr-xpp）：

- `main.cpp`：station 阶段先构造 active ds 紧凑列表，再 `schedule(static)`；
- `xmac.cpp`：非 pulsar 路径把每个 pass 拆成 (baseline, pol product) 任务，
  预计算每个任务的 `threadcrosscorrs` 偏移；每个累加器的加法顺序不变；
  pulsar 路径保留原 pass 并行（共享 `bins`）。

实测（同 Step 1 条件，宿主 g++13 构建、`-b` 挂载；P=1/2/3/4/8 各 2 轮）：

| P | wall（均值） | station | xmac（均值） | wacc | 相对 xp1 |
|---|---|---|---|---|---|
| 1 | 47.51 | 39.03 | 6.42 | 1.35 | 1.06× |
| 2 | 27.37 | 19.80 | 5.96 | 0.86 | 1.83× |
| 3 | 20.22 | 14.73 | 4.11 | — | 2.48× |
| 4 | 14.53 | 9.90 | 3.37 | 0.52 | 3.45× |
| 8 | 8.64 | 5.18 | 2.40 | 0.36 | 5.80× |
| 16 | 9.71（1 次） | 5.87 | 2.22 | 0.77 | 5.16× |

- 全部 `.part` md5 仍为 `56039479…`，逐字节一致。
- station 修正生效：P=16 从 24.85→9.71 s；P=8 6.5→5.2 s。
- xmac 串行 6.4 s → P=8 2.4 s（2.7×），但 P=2 只有 1.08×、P=4 1.9×，
  扩展不足；任务数应为 24（6 基线 × 4 极化），下一步先诊断（任务数/串行前导/
  内存或同步开销），再决定 AVX 与 wacc 的顺序。
- 端到端：xpp P=8 8.64 s，相对 xp OMP=1 的 50.13 s 为 **5.80×**。

## 2026-11-15 Step 3：xmac 内核诊断与 AVX2（已验证无效，已回滚）

- 诊断（Step 2 的扩展不足）：`FXCORR_XPP_DEBUG=1` 打印出每次调用
  `tasks=192`、`pass_setup≈0`、`task_setup≈0`，并行区耗时逐 subint 稳定
  （P=1 中位数 31.97 ms、P=8 12.11 ms），排除任务数不足与串行前导。
  机器频率实测 3.04–3.06 GHz（P=1 与 P=8 相同），排除降频；cpuset
  0-7/0-63/96-103 结果一致，排除核区/缓存切片。
- 单独 kernel 微基准（192 任务、200 subint、cpuset 0..P-1）：
  SSE 5.75/3.91/2.53/1.48 s（P=1/2/4/8，8 线程 3.9×）；
  AVX2 6.02/3.12/1.97/1.09 s（8 线程 5.5×，P=8 绝对时间 1.36×）。
  **kernel 本身也撞内存墙。**
- AVX2 实现检查：`target("avx2")` 克隆确实生成了 256 位代码
  （反汇编 53 处 ymm、8 处 vmulps），且运行时确认选中（run.log 有
  `xMAC kernel = avx2`）。整程序结果：P=1 xmac 7.45 s、P=8 2.37 s，
  对 SSE 的 6.25/2.31 无收益且 P=1 慢 19%。
- **排除实现问题**：把 `xmac.cpp` 整文件用 `-mavx2` 重编（不是 target
  克隆）再测：P=1 xmac 7.23–7.26 s（仍比 SSE 慢），P=8 2.20–2.24 s
  （约 4%，在波动内）。整程序口径下 AVX2 无效，不是克隆写法的锅。
- **已回滚**：删除 `xmac_core_avx2`、`FXCORR_XPP_SIMD` 开关与分派；
  保留 `spectra()`/`numChannels()` 的循环外提（默认路径 xmac
  P=1 6.20 s、P=8 2.36 s，较回滚前 6.42/2.40 略好）。回滚后 P=1/P=8
  的 `.part` md5 仍为 `56039479…`。
- 结论：xmac 的下一步不是加宽 SIMD，而是减少访存冗余（任务粒度/数据复用）；
  station 仍是 wall 主体（P=8 时 5.2/8.6 s）。

## 2026-11-15 Step 4：station 段内部 profile

- LD_PRELOAD 探针（容器内，P=1，g=0）：station 39.0 s 中
  `fftwf_execute` 3.13 s（16.18M 次）、`mark5_unpack_with_offset` 2.62 s
  （2.02M 次）、PCal 提取器 3.10 s（16.16M 次），合计 **22%**；
  xmac 侧同口径另有 5.9 s。
- `main.cpp` 的 STPROF（`FXCORR_XPP_PROF=1`，默认关）：P=1 时
  process **33.29 s（85%）**、归约 3.84 s（10%）、collect 1.78 s（4.5%）、
  setup/tail ≈0.05 s；P=8 时 CPU 总量相同（process 33.28 s）。
- process 扣掉 FFT/unpack/pcal 后仍有约 24.5 s：per-band 的 FFT in/out
  memcpy、frac 旋转、conj、autocorr 与标量簿记，没有单一的大头。
- fxcorrcommon 用 `-O3` 重编（/tmp 试验，不改仓库构建）：process
  33.3→31.1 s（−6.5%）、归约 3.84→2.67 s（−30%）、station 39.0→35.7 s
  （−8.5%），`.part` md5 不变。**单靠编译优化不够。**
- 结构结论：station 的代价分散在每 (block, band) 的若干 128 点小循环里；
  进一步优化需要动共享库 `fxcorrcommon`（融合 conj+autocorr、减少
  per-band 拷贝），这会影响 xp 参照的性能口径——待定。

## 2026-11-15 Step 5：串行直累加 + pcal 单独停放（库新增接口）

发现：旧 xpp 即使串行也启用 per-block 累加器，每个 block 清零 + 写块槽，
每批约 2.05M block × 8 band，process 33.3 s；其中缩减（3.84 s）与清零/写槽
（约 9.8 s）都是可以省掉的。

实现（保持 xp 行为不变）：

- `fxcorrcommon` 新增 **`Mode::enableBlockPcal(slotcount)`** 与
  `blockpcalslots`：只分配 pcal 的每块停放槽；`clearBlockAccumulators`
  改为 ac 与 pcal 各自按自己的槽数清零。默认路径（`enableBlockAccumulators`）
  行为不变。
- xpp 串行路径（`blockthreads==1`）：**不启用 ac/权重块累加器**，直接按块序
  累加到 `autocorrelations`/`weights`（等价 f 的 nthreads=1）；pcal 仍按块停放
  并按块序归约——因为 xp 本身启用了完整块累加器，pcal 的加法顺序必须保持一致。

验证（4 站 1.024 s、g=0）：

- `.part` md5 `56039479…`、`pcal.bin` 8 个文件、PCAL 文本 8 个文件，三者都与
  `fxcorr-xp` **逐字节一致**。
- P=1：station 38.99→31.76 s，process 33.29→27.90 s，归约 3.84→2.02 s；
  P=8：station 5.25→4.12 s，wall 约 8.5→7.4 s（xmac 2.56、wacc 0.41）。
- 内存峰值从 396 MiB 降到约 90 MiB（不再分配整 subint 的块累加器）。
- 说明：更激进的"pcal 也直累加"版本（process 23.5 s、P=8 wall 6.65 s）
  pcal.bin 与 xp 不一致，已放弃。

## 2026-11-15 正式矩阵：4 站 1.024 s、g=0..3、P=1/2/4/8

条件：镜像 `sha256:71decf5e…`（含 Step 5 的直累加 + pcal 停放）；
`workdir-4st-b1024ms-n3`、batch `00000001`；每组单任务分片；
xp OMP=1 参照；xpp P=1/2/4/8；每档 2 轮；run-id 前缀 `mt-`。
每组 `.part` md5：g0 `56039479…`、g1 `16ea34aa…`、g2 `3b601e13…`、
g3 `3ed2d79d…`，xp 与 xpp 全部一致。

wall（s，两轮均值）：

| g | xp1 | P=1 | P=2 | P=4 | P=8 |
|---|---|---|---|---|---|
| 0 | 49.37 | 40.22 | 23.42 | 12.98 | 7.38 |
| 1 | 50.03 | 39.80 | 22.53 | 12.50 | 7.21 |
| 2 | 47.50 | 39.77 | 22.47 | 12.42 | 7.18 |
| 3 | 49.04 | 40.34 | 23.92 | 13.16 | 7.41 |
| 均值 | 48.99 | 40.03 | 23.09 | 12.77 | 7.30 |

- 相对 xp OMP=1：P=1 1.22×、P=2 2.12×、P=4 3.84×、P=8 **6.72×**。
- P=8 阶段均值：station 4.04、xmac 2.33、wacc 0.39、read 0.21 s。
- 说明：首次启动的矩阵在 g=3 r1 的 P=1 后被手动停止（怀疑镜像未更新，
  实为运行镜像缺 `nm` 导致的误判），续跑补齐剩余 23 个 run；正式结果共
  40 个 run，run-id 无重复。

## 2026-11-15 Step 6：process 三合一融合（试过，已回滚）

把每 band 的 frac 旋转、共轭、自相关合成一遍（新增
`fusedRotConjAc`，`setFusedAc(true)` 只在 xpp 串行路径打开）。`.part` md5
仍为 `56039479…`，但更慢：

| 配置 | process | station |
|---|---|---|
| 原路径（P=1） | 27.90 s | 31.76 s |
| 融合（P=1） | 31.70 s | 35.60 s |
| 原路径（P=8） | — | 4.12 s |
| 融合（P=8） | — | 4.71 s |

融合循环在 `-O2` 库构建下没有拿到与 `genericMul/Conj/AddProduct` 同等的
向量化，标量版本反而慢约 14%。**已回滚**（库与 xpp 均恢复），恢复后
P=8 station 4.16 s、md5 不变。

## 2026-11-15 Step 7：FFTW 直写目标数组（试过，已回滚）

思路：`genFFT_*` 每次 FFT 有一次 in/out 拷贝（约 2M block × 8 band × 1 KB
× 2），改用 FFTW new-array execute 直接算到 `fftoutputs`。

- 先做位级验证：普通 plan 在新分配的数组上 `fftwf_execute_dft_r2c`
  与 plan 数组结果 memcmp=0（含 1 float 偏移的错位数组也一致）；
  但 `FFTW_UNALIGNED` plan 与原 plan 结果不同，故不能用它。
- 实现 `Mode::setDirectFFT(true)`（默认 off，仅 xpp 串行路径），只替换
  RtoC 路径。实测 `.part` md5 仍为 `56039479…`，但**更慢**：
  process P=1 27.90→28.24 s、station 32.09 vs 31.76 s，
  P=8 station 4.19 vs 4.12 s——原地 memcpy 本来就在缓存里，省不掉多少。
- **已回滚**，库与 xpp 恢复。

## 2026-11-15 Step 8：wacc 预计算描述符（采用）

`accumulateWeights` 原实现把 config 查表放在每 block 的内层（96 baselines ×
32 freqs × 10 buffered FFT，每次调用重算），32 线程下也只有 3.4×。
改为一次性构造 (freq, baseline, pol) 描述符表（`WDesc`），预计算 ds/record-band
索引，按描述符并行；每个 `baselineweight[f][0][j][p]` 仍只按 fftsubloop 升序
累加，逐位不变。

| 配置 | wacc 前 | wacc 后 |
|---|---|---|
| P=1 | 1.35 s | 0.22 s |
| P=8 | 0.39 s | 0.16 s |

`.part` md5 仍为 `56039479…`；P=1 wall 40.0→39.2 s。

xmac 访存复用（按共享 band 合并极化积）暂不做：4 站每组只有 24 个任务，
与 P=8 整除；按 2 个极化合并后变 12 个任务，粒度损失约 33%，会抵消约 25%
的访存收益。仅在大站数（8/16 站，任务数 112/480）时才可能有利，待相应数据
再评估。

## 2026-11-15 正式矩阵（含 wacc 优化）

镜像 `sha256:cdccaed4…`；其余条件同上一轮（4 站 1.024 s、g=0..3、
xp OMP=1 + xpp P=1/2/4/8、每档 2 轮）。run-id 前缀 `mt3-`，共 40 个 run；
各组 `.part` md5 与上一轮相同。wall 均值：

| 配置 | wall (s) | 相对 xp1 |
|---|---|---|
| xp OMP=1 | 48.71 | 1.00× |
| xpp P=1 | 38.73 | 1.26× |
| xpp P=2 | 22.15 | 2.20× |
| xpp P=4 | 12.25 | 3.98× |
| xpp P=8 | 7.05 | 6.91× |

阶段耗时（g=0..3、2 轮均值，s）：

| 实现 | P | wall | station | read | xmac | wacc |
|---|---|---|---|---|---|---|
| xp | 1 | 48.71 | 39.74 | 0.29 | 6.54 | 1.50 |
| xpp | 1 | 38.73 | 31.73 | 0.20 | 6.30 | 0.22 |
| xpp | 2 | 22.14 | 16.02 | 0.19 | 5.44 | 0.18 |
| xpp | 4 | 12.25 | 8.03 | 0.20 | 3.53 | 0.18 |
| xpp | 8 | 7.05 | 4.04 | 0.20 | 2.32 | 0.17 |
与上一轮（无 wacc）相比：P=8 7.30→7.05、P=4 12.77→12.25、P=2 23.09→22.15、
P=1 40.03→38.73，wacc 从 0.35–0.43 降到 0.13–0.22。

## 2026-11-16 p419-n0 复测（Hygon 30 核）

环境：`ssh p419-n0`（login04）→ 空闲计算节点 `h04r4n03`（Hygon C86 7185，
30 核 / 123 GiB）；该集群无 docker，用宿主二进制 + `env.sh`
（`LD_LIBRARY_PATH` 含 fftw/GSL/install/lib）。

准备：

- p419 仓库原本没有 `fxcorr-xpp`，已同步
  `applications/fxcorr-xpp`、`libraries/fxcorrcommon/src/{mode.*}`、
  `install-difx`，并在 `build.sh` 的 `--doonly` 里加入 `fxcorr-xpp`；
  构建产出 `~/fxcorr/install/bin/fxcorr-xpp`。
- 数据：`/work2/cstu0036/mydata/fxcorr/workdir-b1024ms`（4 站 × 8 ds、
  4 组、batch 00000001）。建实验 workdir
  `~/p419-wd/workdir-4st-b1024ms`（config/batches/raw 软链，其余新建），
  用 `fxcorr-fp` 并发生成 32 个 `stream.bin`（并发必须用后台作业，
  `xargs` 写法会失败）。
- sanity：xp OMP=1 g0 137.0 s，xpp P=8 g0 18.4 s，`.part` md5 相同
  （`c6ee3ece…`）。

矩阵（g=0..3、xp OMP=1 + xpp P=1/2/4/8、每档 2 轮，共 40 run）：

| 实现 | P | wall | station | read | xmac | wacc | 相对 xp1 |
|---|---|---|---|---|---|---|---|
| xp | 1 | 139.81 | 104.12 | 2.46 | 27.73 | 4.44 | 1.00× |
| xpp | 1 | 119.78 | 91.07 | 0.49 | 26.80 | 0.58 | 1.17× |
| xpp | 2 | 61.59 | 46.05 | 0.29 | 14.59 | 0.32 | 2.27× |
| xpp | 4 | 32.55 | 23.43 | 0.29 | 8.32 | 0.18 | 4.30× |
| xpp | 8 | 18.57 | 12.10 | 0.29 | 5.69 | 0.16 | 7.53× |

- 每组 10 次运行 `.part` md5 完全一致：g0 `c6ee3ece…`、g1 `7872b32c…`、
  g2 `e78e011c…`、g3 `1437332…`。
- 与 difx 相比：p419 单核更慢（xp1 139.8 vs 48.7 s），但 xmac 扩展更好
  （27.7→5.7 s，4.9×；difx 为 6.5→2.3 s，2.8×），P=8 加速比 7.53× 略高于
  difx 的 6.91×。

## 2026-11-19 p419-n1 全 OMP 扫描（补 xp 的 P=2/4/8）

目的：此前 xp 只有 OMP=1 参照，xpp 的优势在 P≥2 时没有同节点对照。本轮
在同一节点把 xp 与 xpp 的 P=1/2/4/8 全部测一遍，两侧统一绑核。

环境：`ssh p419-n1` → `ssh -p 50022 h04r1n07`（Hygon C86 7185，32 核
4 NUMA × 8、`nproc` 30，123 GiB；无 docker，宿主二进制 + `env.sh`）。
与 11-16 的 p419-n0（h04r4n03）同型。

准备（p419 源码/构建补齐）：

- 原树停在 10 月：无 `applications/fxcorr-xpp`，`install-difx` 无
  fp/xp/xpp 注册，`fxcorr-f` 仍在调用旧 API（`enableBlockAccumulators`
  签名变化、`addPcal` → `addPcalBlock`），与新 `fxcorrcommon` 不匹配，
  第一次构建在 fxcorr-f 处失败（退出码 1）。同步
  `applications/fxcorr-{f,x,sim,fp,xp,xpp}`、`libraries/fxcorrcommon`、
  `install-difx`，并把 `build.sh` 的 `--doonly=` 改为只含
  `fxcorrcommon,fxcorr-f,fxcorr-x,fxcorr-sim,fxcorr-fp,fxcorr-xp,fxcorr-xpp`
  后重建（退出码 0；`fxcorr-fp` 12:34、`fxcorr-xp` 12:35、
  `fxcorr-xpp` 12:35）。
- 数据：`/work2/cstu0036/mydata/fxcorr/workdir-b1024ms`（4 站 × 8 ds、
  4 组、batch 00000001）；实验 workdir `~/p419-wd/workdir-4st-b1024ms`
  （config/batches/raw 软链、fengine 空）；`fxcorr-fp` 每 8 个一批、
  后台作业生成 32 个 `stream.bin`。
- 坑：`env.sh` 在 `set -u` 下报 `PKG_CONFIG_PATH: unbound variable`——
  脚本先 `PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}` 再 source。
- sanity：xp OMP=1 g0 与 xpp P=8 g0，`.part` md5 `c6ee3ece…`（与 n0 相同）。

矩阵（g=0..3、xp/xpp 各 P=1/2/4/8、每档 2 轮，共 64 run；两侧统一
`taskset 0..P-1` + `OMP_PROC_BIND=close OMP_PLACES=cores`——xp 原脚本
不绑核，但 OMP>1 不绑会跨 NUMA 抽签，故本次两侧同口径）：

| 实现 | P | wall | station | read | xmac | wacc | 相对 xp1 |
|---|---|---|---|---|---|---|---|
| xp | 1 | 139.37 | 105.69 | 0.79 | 27.70 | 4.41 | 1.00× |
| xp | 2 | 85.00 | 52.75 | 1.73 | 27.58 | 2.26 | 1.64× |
| xp | 4 | 58.16 | 26.88 | 1.74 | 27.55 | 1.34 | 2.40× |
| xp | 8 | 32.86 | 14.10 | 1.50 | 15.75 | 0.91 | 4.24× |
| xpp | 1 | 122.66 | 93.07 | 1.66 | 26.79 | 0.59 | 1.14× |
| xpp | 2 | 63.97 | 46.80 | 1.71 | 14.56 | 0.32 | 2.18× |
| xpp | 4 | 34.38 | 23.72 | 1.72 | 8.21 | 0.18 | 4.05× |
| xpp | 8 | 20.00 | 12.21 | 1.46 | 5.67 | 0.15 | 6.97× |

- 64 次运行 `.part` md5 全部一致（g0 `c6ee3ece…`、g1 `7872b32c…`、
  g2 `e78e011c…`、g3 `1437332…`），且与 p419-n0 的 11-16 结果逐位相同。
- **xp 的 xmac 不随线程扩展**：27.70/27.58/27.55 s（P=1/2/4），P=8 才
  15.75 s；xp 的 wall 扩展主要来自 station（105.7→14.1，7.5×）。xpp 的
  xmac 近线性（26.8→14.6→8.2→5.7 s）。同 P 的 xpp/xp 比：
  1.14×/1.33×/1.69×/1.64×——xpp 优势随线程数放大。
- 内存：峰值 RSS xp 398 MiB、xpp 234 MiB（−41%）；P=8 的 user 时间
  xp 206.2 s、xpp 131.1 s。
- 与 p419-n0 对比：xp1 139.37 vs 139.81（−0.3%）；xpp P=1/2/4/8
  122.66/63.97/34.38/20.00 vs 119.78/61.59/32.55/18.57（+2.4%/+3.9%/
  +5.6%/+7.7%；P=8 两轮 21.4/19.4 s，在 xmac ±5–10% 的已知单次波动内）。
- 产物：远端 `~/p419-matrix-20261119/matrix.tsv`，已同步
  `results-detail/20261119-p419n1/`；矩阵脚本
  `~/p419-wd/{gen_streams,sanity,matrix_p419v2}.sh`（本地副本
  `fxcorr-scratch/xp-opt/scripts/20261119-p419/`）。
