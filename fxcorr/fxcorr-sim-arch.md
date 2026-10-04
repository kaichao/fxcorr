# fxcorr-sim 分布式架构（单二进制 `fxcorr-sim`）

**最后更新**：2026-10-04（§8.1 追加「单节点并行度提升：优先级重排」——块时长参数化为**主线**
（**已实施并验证**），多 ds 复用 + OMP 降为附条件优化（未实施）；含重复生成事实、数据不变
论证与独立性边界；此前 2026-09-27 §1 补"多 datastream 站每 ds 一个文件"）

本文定 fxcorr-sim 的分布式形态与公共信号模型；命令行手册（参数/环境变量/示例）见 `usage.md`，源文件地图/实现要点见 `applications/fxcorr-sim/CLAUDE.md`，验证记录见 `applications/fxcorr-sim/VERIFICATION.md`，common/ 产物格式见 `data-spec.md`。

## 1. 定位与目标

用一个可执行文件 `fxcorr-sim` 生成对接 fxcorr 流水线的多站基带数据，支持单机一键与多节点分布式：

- 输出：`raw/<station>/<station>_<batch_id>[_ds<N>].vdif`（file-per-batch；**多 datastream 站每 ds 一个文件**，后缀只在多 ds 站出现）。**已实施（2026-09-27）**：`station` 子命令带 `ds_index` 位置参数、站噪声种子含 ds 索引（ds=0 的种子与改前相同）、默认模式遍历全部站的全部 ds。改造记录与验证见 `v5-plan.md` P6「多 datastream 生成」与 `test/multids/README.md`
- 配置与时间：`batches/<batch_id>.json`、`config/`（`.input`、按需 `.im`）
- 中间：`common/<batch_id>/`（公共信号，权威一份）
- V1：单线程；加速靠多任务/多节点跑站级，不用 OpenMP

分布式由编排层（V2 scalebox）驱动 `common` / `station` 两个入口，程序内不引入 MPI。datasim（MPI 一体式）与本设计的关系：公共信号 Bcast 换成共享存储单份落盘、全站一次生成换成逐站任务、MPI 并行换成 (batch, station) 任务分片——信号合成算法（频域公共信号 + 分层注入）照 datasim 移植。

## 2. 单一入口，两种用法

```text
fxcorr-sim station <batch_id> <station> [workdir] [ds_index] [tone_mhz ...]
fxcorr-sim         <batch_id> [workdir]
```

| 调用 | 行为 |
|------|------|
| **`station`** | 生成**一个站的一个 datastream** → `raw/<station>/<station>_<batch_id>[_ds<N>].vdif`（`ds_index` 位置参数，缺省 0）。**公共信号在这个任务内部合成**（V6 S2.5：算全部、只留自己 band 那段，不读也不写任何公共文件）；tone_mhz 位置参数 = **legacy 模式**触发器（旧时域合成路径，字节对拍回归专用，不经公共信号） |
| **无子命令（默认）** | **本机串行**：对 `.input` 全部站依次 `station`（一键多站；tone 参数不允许出现在此入口） |

**`common` 子命令、`sim-common/` 目录、`FXCORR_SIM_COMMON_ROOT` 根已在 V6 S2.5（2026-09-27）删除**——理由与量化依据见第 6 节开头的改正注。实现上只剩 `run_station()`。旧 4 参调用 `fxcorr-sim <batch> <station> [workdir]` 废止（改 station 子命令），编排脚本同步迁移。

## 3. 数据流

```
共享存储 workdir/
  config/  batches/<batch_id>.json      ← seed 在这里（唯一的公共参数）

[每个 (batch, station, ds) 一任务，可多节点并行]
  fxcorr-sim station B S W <ds>
        │
        │  公共信号在此进程内生成（SliceStream，不落盘）
        ▼
  raw/<station>/<station>_B[_ds<N>].vdif
```

单机默认模式 = 对 `.input` 全部站 × 全部 ds 顺序跑同一件事。

## 4. 一致性与部署

| 规则 | 说明 |
|------|------|
| 公共参数 | **只剩 `seed`**，随 `batches/<batch_id>.json` 走；网格与 batch 的 slice 总数都是 `.input` 的纯函数，各站就地重算、必然逐位相同 |
| 站级 | 各站独立合成公共信号，**互不通信**；跨站相干靠"同一 seed + 同一 PRNG 顺序"保证 |
| 数据一致性 | 判据是 raw 与原落盘路径**逐字节相同**（S2.5 验收：32/32） |
| 禁止 | 同 batch 的各 station 任务**必须读到同一个 seed**；不一致则跨站相干**静默消失**，事后无法检测 |
| 站噪声种子 | `f(seed, station_id, ds_index)`，与公共种子分离（ds 维度 2026-09-27 加，见 usage.md 的 `ds_index`） |
| 消费后清理 | 无公共中间产物可清——`raw/` 是唯一产出（`data-spec` 12 节） |

**优先：** 单任务 `common` + 多任务 `station`。**退路：** 单机默认串行。**不优先：** 每节点各自重算 common（见第 8 节成本分析；同架构节点确定性复现在科学上等价，但失去"权威单份"的单源一致性，且重复计算无收益）。

> **⚠ 2026-09-27 改正（V6 S2.5）：下面这条判断已作废，`common` 子命令与 `sim-common/` 目录都已删除。**
>
> 那条"重复计算无收益"建立在 `test 配置`（1×4 MHz、8 Ms/s）的成本估算上；换成 t25362 的真实参数后结论反转——公共信号要按**覆盖跨度**生成 7072 MHz，而各站只读其中 1024 MHz，**85% 是白做的**：写 57.93 GB/batch、读侧跳读放大 221×、单 ds 从 2 分钟涨到 **34 分钟**。现在公共信号由每个 `station` 任务在本地合成（**算全部、只留自己 band 那段**），**算力换 I/O，wall time 不增**（各组并行）。公共参数只剩 `seed`，随 `batch.json` 走。
>
> **注意"只生成自己 band 那段"为什么做不到**：公共信号是流式顺序 PRNG 且跨 slice 连续，取 slice 内第 k 个点必须先算前 k 个；改成可寻址 PRNG 会改变全部数值、作废所有对拍基准。详见 `fxcorr/v6-plan.md` S2.5、`data-spec` 5.8。

选"权威单份"而非"确定性复现"的核由：单源读取使跨站一致性与节点架构解耦——确定性复现的逐位一致只在同架构节点成立（fftw 不同 SIMD 路径可能有末位浮点差异），共享存储单份没有此限制。

## 5. 信号模型

**Common（`run_common`）**

- 输入：batch 时间窗、`.input` 全站 band 布局、通量或谱模型（P2）、全局种子
- 输出：**频域**公共信号 S（已拍板）：每 `stime = 1/specRes` µs 一个 `numSamps` 复频谱 slice，float32 复数对（datasim gencplx 语义，STDEV=1）；`specRes` = 全站 band 频率差/带宽的 GCD 网格（0.5 MHz 起、二分至 1/2^10，datasim getSpecRes 移植）、`numSamps = maxChanFreq/specRes`（全站 band 覆盖跨度）；覆盖 `[minStartFreq, maxStartFreq+maxBW]`
- 确定性：S 只与 (seed, batch) 有关，与站无关；seed = FXSIM_SEED（默认固定），编排统一写
- 不含站噪声、不含站相关随机数、不做量化

**Station（`run_station`）**

```
V_i = g_i · S(t − τ_i) · e^{jφ_i} + n_i
```

- 读同一份 S，切自己 band 的频段（startIdx/blksize 由站 band 频率/带宽对 specRes 网格换算）
- 加本站噪声 n_i（SEFD/σ，datasim fabricatedata 语义：×√F → +√SEFD 站噪声 → ÷√(F+SEFD)；FXSIM_FLUX > 0 启用（替代 FXSIM_NOISE 路径），SEFD 单值或按 datastream 序逗号列表，默认 1000；P0 的 FXSIM_NOISE σ 语义保留为 flux ≤ 0 路径）
- Ormsby 频域边缘滤波（0, 1/2, 4/5, 1…1, 4/5, 1/2）→ IDFT 出复基带缓冲
- τ_i 注入（已实现，datasim updatevalues + processdata 语义）：每帧 .im 模型求 delay/rate，fracsamperror 累积超半复样本整样本移位（帧窗口在滚动基带缓冲上移动），频域亚样本校正（e^{j·2π·bandwidth·idx/vpsamps·fracerr}），时域条纹旋转（band 起始频率，fraction_of 小数相位）；FXSIM_DELAY=0 时校正链恒等（字节回归判据）
- 复转实：Hermitian 2N IDFT 取实部 → 2bit 量化打包（复用 quantise2bit + FXSIM_ADAPTIVE）→ VDIF 写盘
- pcal 注入（station 端，realc 量化前 = datasim applyphasecal 同构位置，延迟链之后）：.input PHASE CAL 网格 tone（幅度 0.7、batch 起点连续相位，与 legacy 共用网格读取）＋ FXSIM_PCAL 梳齿（datasim `-p` 语义：k·interval MHz、1/500 幅度、帧边 taper）——P4 已完成

### 数据量事实（设计依据，2026-09-14 分析）

公共信号 float32 复基带 = 8 字节/复样本 × 覆盖带宽；单站 2bit VDIF = 0.5 字节/s × 记录带宽。比值恒 **16 倍**（32bit/复样本 ÷ 2bit/实样本，与带宽无关）：

| 场景 | common 数据率 | 单站 VDIF | 说明 |
|---|---|---|---|
| test 配置（1×4MHz，8Ms/s 2bit） | 32 MB/s | 2 MB/s | batch 4.096s → common 131 MB vs 单站 8.4 MB，batch 粒度绝对量可控 |
| 异带多站（8 站各 4MHz 分布 40MHz 跨度） | 320 MB/s | 2 MB/s/站 | common = 全站总量（16 MB/s）的 20 倍，共享存储持续读写需评估 |

- 缓解手段（预留，不实现）：dtype 降 int16（比值 8 倍，96dB 动态范围对噪声信号无碍）；覆盖范围按 batch 实际用到的频段裁剪。**起步 float32，meta.json 带 dtype 字段留降级口。**
- 结论：common 落盘量 ≥16× 单站数据，但 batch 粒度下绝对量可控、消费后可删；与"每节点各自重算 common（省 I/O、每站多算 ~10% 计算，见第 8 节）"相比，权威单份的一致性收益压倒 I/O 代价，定为唯一形态。**（⚠ 该结论已作废，改正见本节开头的 2026-09-27 注："绝对量可控"只在 test 配置成立，t25362 参数下是 57.93 GB/batch。）**
- **代入真实观测参数后的体量核算见 `data-volume.md`**（2026-09-21 起）：t25362 那种宽跨度下 common 的**生成量**是 57.9 GB/(1.024 s batch)，与站数无关——本节只给模型与比值。（V6 S2.5 起公共信号不落盘，该数字读作**生成量当量**；本节表格中的"数据率"同理。）

## 6. 目录约定

```
workdir/
├── config/                 # .input / .im
├── batches/<batch_id>.json
├── common/<batch_id>/      # common 子命令输出（新产物类型，data-spec 第 2/3/5 节同步）
│   ├── meta.json           # 版本/dtype/specRes/numSamps/minStartFreq/块表/seed/status
│   └── data_XX.bin         # 每 0.5s 块一个文件，float32 频域 slice 顺序
└── raw/<station>/
    └── <station>_<batch_id>.vdif
```

公共信号**不再有落盘格式**（V6 S2.5）：它只在 `station` 进程内存在，`SliceStream` 的块布局是内部实现细节，没有跨进程契约，因此既不需要格式版本、也不需要 README。规范只剩"参数"那一层——`seed` 随 `batch.json` 走，见 `data-spec` 5.8。

## 7. CLI 要点

（参数/环境变量/示例的完整手册见 `usage.md` fxcorr-sim 段；此处只记设计约束）

- 位置参数风格与 fxcorr-f / fxcorr-x 一致；workdir 语义沿用（位置参数 > `FXCORR_WORKDIR` > `.`）
- 站列表（默认模式）：来自 `.input` 全部 datastream（与 run_batch.sh 同语义）
- `station` 模式：公共未就绪（meta.json 缺失或 status≠done）则报错退出
- `station` 模式带 tone_mhz 位置参数 = legacy 模式：完全旧合成路径（tone/pcal/FXSIM_DELAY/FXSIM_FLUX/FXSIM_SEFD/FXSIM_ADAPTIVE 全部原样），供字节对拍回归；旧路径不再接新特性

## 8. 计算与并行

| 项 | 设计 |
|----|------|
| common : station 计算比 | common 段（公共噪声生成 + 切频段 + IDFT + normalize）≈ 单站总量的 **10%**（test 配置估算：~200M flops/s vs 站总 ~2G flops/s，大头在站级帧校正链）；specRes 变细（多站频率差 GCD 小或 FXSIM_SPECRES 缩放）时公共段占比上升（specRes 减半 → 占比翻倍），这是"每节点各自重算 common 不优先"的量化依据 **（⚠ 前提已作废：那是 test 配置的估算；t25362 参数下 common 要覆盖 7072 MHz 而实际只读 1024 MHz，"每站多算 10%"变成"多算 6.9 倍的白数据"，见本节开头的 2026-09-27 注）** |
| 并行 | 多进程/多节点跑 **`station`**（任务粒度 = (batch, station)，与 fxcorr-f 同构）；V1 程序内单线程 |
| OpenMP | V1 不需要；P3 已评估（2026-09-14）结论不实施，见阶段表；**2026-10-04 修订背景见 §8.1** |

### 8.1 单节点并行度提升：优先级重排（2026-10-04；第 1 项已实施并验证，第 3 项未实施）

**优先级结论**（bio：4 核 / 15 GB，mini 配置；g = 公共信号生成段、p = 站处理段，
时间按 g = p 归一）：

| 方案 | 峰值内存 | 用核 | 完成时间 | 改动面 |
|---|---|---|---|---|
| 现状：`-p 2`、0.5 s 块 | 8.8 GB | 2 | 16 | — |
| **0.25 s 块 + `-p 4`（逐 ds，主线，已实施）** | ≈8.0 GB（4 × 1.99 实测） | 4 | 8 | `FXSIM_BLOCK_US`（数据不变） |
| 0.25 s 块 + 合并 8 ds + OMP 2（可选） | ≈6.2 GB | 4 | 5 | fxcorr-sim + 平台四处 |

1. **块时长参数化（主线，已实施 2026-10-04）**：`FXSIM_BLOCK_US`（默认 500000 = 原行为）；
   单独完成"≈8.8 GB/2 核 → ≈8 GB/4 核"（内存减半 → 并发翻倍），零平台改动、数据不变；
   验证：默认参数与改造前镜像逐字节一致、0.5/0.25 s 逐字节一致、RSS 3942 → 1989 MB；
   `data-volume.md` §6 杠杆 9；
2. **实测 g:p**（生成段 vs 站处理段占比）——决定第 3 项值不值；
3. **多 ds 复用 + ds 级 OMP（附条件优化）**：独有收益 = 消除重复生成（16 → 2 次）+
   内存再低 ≈3 GB + 站处理线程化；代价 = 平台四处改动（见下）；且进程数 = 站数
   （4 站时 ≈12.3 GB，bio 放不下）——**站越多，内存劣势越大**。

**事实：同一 batch 的公共信号被逐个任务重复生成。** 公共参数只有 `seed`（batch 级，§4），
网格与 slice 总数是 `.input` 的纯函数——同 batch 的每一个 `station` 任务都在本地合成
**逐位相同**的一份公共信号（S2.5 的"算全部、只留自己 band 那段"）。bio 场景
（2 站 × 8 ds）每 batch = **16 次重复生成**，而进程级并行度 = 站数（2 路），4 核节点只用
2 核。默认串行模式（无子命令）同样逐 ds 调 `run_station`，每 ds 各自生成一遍。

**第 3 项（可选）的形态**：`station` 缺省 ds = 本站全部——进程内公共信号块只生成一次、
各 ds 复用同一 blockbuf；ds 级 `#pragma omp parallel for`（线程数沿用 `OMP_NUM_THREADS`，
与 fxcorr-f 一致）；`make_testdata.sh` 任务粒度改为 (batch, station)，`-p 2` 即
"2 进程 × 2 线程"。

- **数据逐字节不变**（验收 = 同参数与现二进制全文件 `cmp`）：每 ds 的 `FreqStationGen`
  完全独立——噪声种子 `f(seed, station, ds, band)`、各自 temp/baseband/FFT plan/输出
  文件；跨 ds 共享的只有**只读** blockdata；OMP 只切 ds 维度、每 ds 内部保持原顺序
  （fftw 不同 plan 并发 execute 安全；plan 创建都在串行 init 段）。
- **内存**：合并后单进程 = blockbuf + Σ baseband ≈ 4.1 + 8 × 0.26 ≈ **6.2 GB**（mini；
  两项均 ∝ 覆盖跨度 × 块时长，见 `data-volume.md` §3.3）——2 进程 ≈ 12.4 GB 对 15 GB
  节点偏紧，须与"减小块时长"（`data-volume.md` §6 杠杆 9；0.25 s → ≈3.1 GB/进程）叠加。
- **落点**：`serial` 已是一个"遍历全 ds"的循环（main.cpp 默认模式，注释明说现状每 ds
  自生成）——块生成提到循环外即可；`station` 带 `ds_index` 的现有语义与输出不动。
- **边界（重要）**：逐 (batch, station, ds) 独立生成是**平台分发粒度**的设计基础
  （router 逐 ds 展开任务、模块 body 三元）；复用只在"少节点/本地造数"场景有收益，
  应实现为**可选形态**。启用合并粒度需同步改：router 的 ds 展开、`run.sh` body、
  造数模式 `counter:sim-ready` 初值（ds 总数 → 站数）、`make_testdata.sh` 的跳过判据
  （幂等改为逐 ds 检查存在即跳过）。下游（f/x/merge/fits、`rawDataReady` 判据）零感知
  （产物字节相同）。
- **对 P3"OpenMP 不实施"结论的修订背景**：P3 的前提（test 配置 + 进程级并行已覆盖多核）
  在"站数少、核多"的场景不成立；复用方案同时消除重复计算并把站内 ds 并行起来。

## 9. 实现结构（单二进制内）

```
fxcorr-sim
├── main                    # 解析子命令/默认模式、环境变量、legacy 分派
├── run_common()            # 频谱生成 + 分块写盘 + meta.json（rename 完成可见性）
├── run_station()           # 读 common → 频域链 → 量化打包 → VDIF（新路径）
├── SignalGen               # 分层：公共信号层（新）/ 站专属层（噪声、延迟注入）/ 量化打包层（共用）
│                           #   legacy 路径原样保留在站专属层旁
└── VDIFWriter              # 帧封装（不动）
```

公共文件读写独立成 `commonsignal.{h,cpp}`（格式版本化），与 SignalGen 解耦。

## 10. 与现有组件/特性的关系

| 组件/特性 | 关系 |
|------|------|
| 当前 `fxcorr-sim`（tone/噪声/pcal 时域合成） | 演进为本设计；旧路径整体保留为 station 子命令的 legacy 模式（字节对拍回归依赖） |
| tone 位置参数 | legacy 专属；新路径下 tone 由 P2 谱线机制（common 端频域注入）提供，跨站天然相干 |
| FXSIM_FLUX/FXSIM_SEFD | station 端 SEFD 定标（datasim fabricatedata 语义），已实现（flux > 0 启用） |
| FXSIM_DELAY | 新路径：频域链延迟注入（procptr/fracsample/条纹旋转）默认开、`0` 关；legacy 保留纯相位版 |
| FXSIM_ADAPTIVE / pcal / 量化打包 | station 端，沿用（pcal 新路径 P4 已实现，见阶段表） |
| `datasim` | 物理参考；逻辑拆进 `run_common` / `run_station`，IPP→fftw3f |
| `fxcorr-f` / `fxcorr-x` | 只读 `raw/` + config，零感知 |
| 编排脚本（make_testdata.sh / run_batch.sh） | 数据生成步骤改两段式：common 一次 → 逐站 station（P0） |

## 11. datasim 特性差距（2026-09-14 定稿）

定位差异：datasim 仿真「可相关出条纹的 VLBI 观测」（科学信号 + 几何模型注入）；fxcorr-sim 新架构以频域公共信号 + 分层注入对齐此语义，落盘形态按 (batch, station) 任务模型重排。特性归属（datasim → fxcorr-sim）：

| datasim 特性 | 归属 | 状态 |
|---|---|---|
| 公共频域信号 + 子带切分（跨站相干来源） | common 端（频域 S 落盘共享） | **P0 已完成**（commonsignal） |
| 站噪声 + SEFD/通量定标（fabricatedata） | station 端 | **P2 已完成**：flux > 0 走 datasim 链（×√F → +√SEFD gencplx 噪声 → ÷√(F+SEFD) 折入 scale），flux ≤ 0 保留 σ 语义（scale = 0.5/(√(1+σ²)·√blksize)）；两路径在 F=1/SEFD=0/σ=0 逐位一致 |
| 几何延迟/条纹注入（procptr+fracsample+条纹旋转） | station 端 | **P2 已完成**：datasim updatevalues + processdata 语义，默认开、FXSIM_DELAY=0 关（tone 纯相位版保留于 legacy） |
| 谱线 -l（gengaussianfilter） | common 端 | **P2 已完成**（FXSIM_LINE，gengaussianfilter 照抄：√amp·exp(−π²δ²/2rms²)、re=im 同乘、越界报错） |
| specres -r（specRes 缩放） | common 端 | **P2 已完成**（FXSIM_SPECRES，datasim `specRes /= specres` 语义，缩放后一致性检查照跑） |
| pcal 注入 | station 端 | **P4 已完成**（.input PHASE CAL 网格两路径共用，幅度 0.7；新路径另有 FXSIM_PCAL datasim 梳齿 1/500 + 帧边 taper） |
| 量化阈值自适应（quantize d_tmul） | station 端打包层 | 已实现（FXSIM_ADAPTIVE，四电平版） |
| 测试模式 -t / MPI 并行 / 多站一次生成+zipper/cat | — | 等价覆盖：batch.json 定时长、batch 编排分片、(batch,station) 任务 + band 交织帧直接生成 |
| 依赖（GSL/IPP/MPI） | — | mt19937+Box-Muller / fftw3f / 无 MPI（fxcorr-sim 已链接 fftw3f，零新依赖） |

**复核（2026-09-15，源码级逐项比对）**：通读 datasim.cpp 主流程 + subband.h 接口，结论 = **功能已等价、无表外遗漏**。表中"等价覆盖"项的源码依据：vdifzipper（多 band 帧合并，datasim.cpp:605-619）→ fxcorr-sim band 交织帧直接生成；catvdif（时间分片拼接，:627-646）→ batch 切分每 batch 独立文件；`-t` 测试模式（:319 强制 1 秒）→ batch.json 定时长。有意差异 4 处（非遗漏，均有验证背书）：① 量化映射——datasim 三电平 ±thresh×sign，fxcorr-sim 四电平 rint（实测弱信号下三电平效率低 4.5 倍，不照抄）；② 随机数——gsl_rng vs mt19937+Box-Muller（同 seed 确定性可复现，但不与 datasim 逐位一致）；③ CLI 形态——datasim 单命令一次全站 vs fxcorr-sim 两段式 common+station（单节点串行默认模式一键等价）；④ 依赖——GSL/IPP/MPI vs 零新增（仅 fftw3f）。datasim 的 MPI 并行（子带分发/时间分组/Bcast）对应 (batch,station) 任务模型 + P1 编排，scalebox 承接（第 13 节约束不变）。未验证项：与 datasim 本身逐位对拍（datasim 因 IPP 依赖无法构建，对拍基准 = gen_test_vdif.py 字节对拍 + mpifxcorr 科学对拍，已覆盖）。

历史分析（A/B 类分类、方案 A 修上游 datasim 的讨论）见第 10 节与 git 历史。

## 12. 阶段

| 阶段 | 内容 |
|------|------|
| P0 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/VERIFICATION.md）**：子命令框架 + 默认串行；频域 S 落盘（common）；station 读 S 加噪量化出 VDIF；legacy 模式挂接（字节对拍 BYTE-IDENTICAL）；编排脚本两段式；文档同步；测试机验证（跨站相干两站 FXSIM_NOISE=0 逐位一致、σ=1.0 相关系数 0.444 vs 理论 0.5、新路径全链路 SWIN） |
| P1 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/VERIFICATION.md）**：make_testdata.sh `-p P` 本地并行 + `--nodes` ssh 节点映射分发 station 任务；测试机验证（p1reg）——3 batch × 2 站并行生成、远程/本地逐位一致、失败传播非零退出、并行产物全链路 SWIN 12 记录 |
| P2 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/VERIFICATION.md）**：SEFD/通量定标（station 端，datasim fabricatedata 链）；延迟注入完整链（procptr/fracsample/条纹旋转，默认开）；谱线 FXSIM_LINE + specres FXSIM_SPECRES（common 端） |
| P3 | **已完成（2026-09-14，评估不实施 + 附带修复）**：性能基线实测（test 配置 2.097s batch）：common 0.84s、station 2.45s/站（0.86× 实时）、fxcorr-f 0.46s、fxcorr-x 0.09s——sim station 虽是流水线最慢单任务（f 的 5.3×），但离线造数无压力、P1 的 (batch,station) 进程级并行已覆盖多核场景，**进程内并行判定不需要**；datasim 功能对照（通读 2748 行源码逐项比对）结论：核心功能全覆盖，遗漏 2 项——新路径 pcal（→P4）、带间隙多 band 网格（本阶段已修：deriveGrid span 改为 max(freq+bw)−min(freq) 实际覆盖，datasim 原算法对间隙布局静默越界不照抄；附带修 per-band 帧结构 ÷nbands bug，test2b 全链路验证通过、单 band 回归 BYTE-IDENTICAL，详见 applications/fxcorr-sim/CLAUDE.md） |
| P4 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/VERIFICATION.md）**：新路径 pcal 相位校准注入双语义——.input PHASE CAL tone 网格（0.7 幅度、batch 起点连续相位，网格读取与 legacy 共用）＋ FXSIM_PCAL datasim `-p` 梳齿（k·interval MHz、1/500 幅度、帧边 taper）；注入点 = realc 量化前（datasim applyphasecal 同构，延迟链之后）。验证：网格 4 tones（201-204MHz）PCAL 与 mpifxcorr 同一 raw 提取**逐字节一致**、SWIN 6/6 全等；梳齿 1MHz 峰两站两积分 4/4 检出、taper 525 帧首样本恒 2；无 pcal 默认回归 BYTE-IDENTICAL |

## 13. 约束

- 一个二进制，逻辑两阶段；分布式靠编排调用 `common` / `station`，程序内不用 MPI
- 默认模式仅限单机（或明确单任务）一键生成
- 一致性靠**共享存储上单份 common**，不靠多节点重复 common
- 公共文件格式单独定版本；改格式必须先同步 `data-spec.md`（新增 common/ 产物节后即为规范来源）
- legacy 模式只做回归，不做新特性

## 14. 一句话

**`fxcorr-sim` 单入口：`common` 写共享公共信号，`station` 按站读公共并写 raw；不加子命令则本机串行 common+多站；集群上 common 一次、station 多节点并行。**
