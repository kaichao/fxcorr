# fxcorr V9 计划

**日期**：2026-10-13（设计轮；**启动**——拍板：FFT 从 f 侧移到 x 侧，新建两个对应工具
`fxcorr-fp` / `fxcorr-xp`，**不动现役 fxcorr-f / fxcorr-x**，便于新旧链对比。
**同日实施**：阶段 0（fxcorr-fp，判据 1/2/7）与阶段 1（fxcorr-xp，判据 3/5）完成，
实测见 `applications/fxcorr-fp/CLAUDE.md` 与 `applications/fxcorr-xp/CLAUDE.md`）
**主题**：**中间数据减量重构**——把 FFT（连同相位旋转、分数样本校正等频域步骤）从
f 侧下移到 x 侧，f/x 之间只传 2-bit 原始数据（**1×**），与 mpifxcorr 的 core 侧现算同构。
**前置**：V8 已收尾（`v8-plan.md` §5；§5.5 为主题声明）。
**范围**：本计划只覆盖 **fxcorr 工具链本身**——两个新工具、数据契约、判据与实施阶段。
**相关（均在本仓库）**：`data-volume.md` §1.4/§2/§4（中间数据 16× 公式与实测、在制量核算）、
`v7-plan.md`（P4/P5a：mpifxcorr 对照、"跨节点传的是 raw 而不是频谱"实测）、
`data-spec.md` §5（fengine / vis 产物规范）、`mpifxcorr/`（`core.cpp` / `fxmanager.cpp` /
`datastream.cpp` 源码对照）。

## 1. V9 的位置

V1–V8 把 mpifxcorr 拆成工具链（sim → f → x → merge），拆分划界选在 **FFT 之后**：f 做站级
处理 + FFT 并写 fp32 谱（`band_XX.sp`），x 读谱做基线。这条划界有两个结构性代价，本仓库
文档早已记明：

- **中间数据 16× 膨胀**：2-bit 0.25 B/样本 → fp32 复半谱 4 B/样本；`data-volume.md` §1.4
  的核算公式直接写着 "**fengine = 16 × raw**"，§2 附实测（raw 31.4 MiB/ds/批、fengine
  502 MiB/ds/批，0.512 s 档）；
- **中间物必须落盘且两次过盘**（f 写、x 读）：fengine 的体量是本仓库容量核算的主项
  （`data-volume.md` §4.2"在制量 = 在制组数 × 组体量"、§5.1"fengine 必须落 tmpfs"），
  中间数据的存与搬因此成为节点部署的硬约束。

而 mpifxcorr 本身不是这样——**core 侧现算**：

- **datastream（f 角色）不做 FFT、不解包**：只读帧、整数对齐、按 subint 搬 2-bit 原始样本
  （`datastream.cpp` 中 unpack/FFT 字样仅出现在字节计数处；每样本字节 = `numbits/8`）；
- **core（x 角色）做全部站级处理**：`core.cpp:798` 每 subint 对每个 ds 调 `Mode::process`
  （unpack + pcal + FFT + 旋转），随后交叉乘；`fxmanager.cpp:333` 时间片轮转保证每份数据
  FFT 恰好一次、无重复计算；
- **v7-plan P5a 实测印证**：mpifxcorr 跨节点传的是 **raw（4.11 GB/s）而不是频谱**。

**同一份 FFT 放在哪一侧，决定中间数据差 16 倍**——这就是本变体的全部动机。fxcorr 与
mpifxcorr 共用同一份 `Mode::process` 源码（fxcorrcommon），所以**位一致可期**；把划界移到
FFT 之前是纯工程改动——障碍是站级输入迁移、任务结构重设计、正确性回归、产物语义四类，
本计划逐项处理。

**工具形态（拍板，2026-10-13）**：**不动现役 `fxcorr-f` / `fxcorr-x`**，新建两个对应工具：

| 新工具 | 读作 | 角色（对照 mpifxcorr） | 对照现役 |
|---|---|---|---|
| **`fxcorr-fp`** | f′ / f-**p**assthrough | datastream：读帧 + 对齐 + 坏帧/缺口簿记 + 写 1× 数据流 | fxcorr-f（去掉站级 DSP） |
| **`fxcorr-xp`** | x′ | core：读 1× 数据流 + 站级栈（unpack/pcal/FFT/旋转/frac）+ XMAC + 集成 | fxcorr-x（并入站级栈） |

两者与现役工具的**对比**是 V9 的验收方法：同一份数据、同一个 batch，新旧链各跑一遍，
**SWIN 逐字节一致** + fengine 中间物体积 16× 缩减（见 §8）。

**边界**：本变体不解决共享盘供给瓶颈（raw 读量不变）；不会追上 mpifxcorr 的 MPI 内存
流水线形态（"谱不落盘"才是它相对 fxcorr 的真正优势）；重跑语义从"读盘"变"重算 FFT"
（以计算换存储的显式定价，见 §10 R5）。

## 2. 设计定案清单

| # | 决策 | 内容 |
|---|---|---|
| D1 | 工具形态 | 新建 `fxcorr-fp` / `fxcorr-xp` 两个二进制（applications/ 下新目录，模板照 fxcorr-f/x）；现役 f/x **零改动**，双链并存可对比 |
| D2 | 起步变体 | **B（稳妥）**：fp 输出 1× 重排数据流、xp 读它——格式/对齐/坏帧集中在 fp，xp 的"数据输入"接口干净。**A（激进）**（xp 直读 raw、fp 取消）留后置（§11） |
| D3 | 中间格式 | 新格式 **`stream.bin`**（自定义、固定记录长、顺序读写；§4）——不落 VDIF、不落 fp32，内容 = `DataReader::readSubint` 的产物序列（槽排布后数据 + 有效位 + 时间戳） |
| D4 | 布局 | **不变**：`fengine/<batch>/<g>/<station>/ds_<N>/stream.bin`——fengine 的目录约定、组级 purge 的删除单元、根解析（`FxcorrPath`）全部零改动，只是目录内容物换了 |
| D5 | 站级栈去向 | unpack、pcal 提取、FFT、条纹旋转、frac sample、autocorr、kurtosis、STA **全部跟 FFT 走（xp）**——它们在 `Mode::process` 内部一体实现，xp 用自己的 `Mode` 即零额外工作；fp **零 DSP**（不解包） |
| D6 | 两处旁路的去向 | **SWITCHEDPOWER 留 fp**（读取路径的旁路统计、不解包，喂的是原始帧字节）；**PCAL 文本 + pcal.bin 由 xp 写**（tone 在 xp 的 Mode 内存里；产物名/位置/幂等语义不变） |
| D7 | 取消的产物 | `band_XX.sp`、`autocorr.bin` **取消**（谱与自相关在 xp 内存直取）；`pcal.bin` **保留**（xp 写，对拍锚点，无链上消费者——消费面已核） |
| D8 | 并行结构（位一致机制） | **xp 站级阶段 = ds 级并行**：每线程独占一个 ds、线程内**块序串行** process——单 ds 的加法序列与现状完全一致，**不需要 f 的 per-block 归约那一套**；组内 ds 数 < 线程数时再议块并行（后置）。XMAC 阶段照 x 现状（基线并行，结果区独立） |
| D9 | 谱输入接口 | XMAC 引擎零改动：给 xp 做一个 **SpReader 兼容的内存 reader**（`spectra()/flags()/weights()/numChannels()` 从 Mode 的 `fftoutputs`/`getDataWeight` 取），`xmacBatch(fftloop, readers, …)` 签名不动 |
| D10 | 对比方法 | 同一 workdir、同一 batch，**两链各跑一遍**（v8 链 f+x 与 v9 链 fp+xp），产物逐字节比（§8 判据锚）；两链互斥——同一 batch 不混跑（先清产物，见 R9） |
| D11 | 起步范围 | 单相位中心（挡多相位中心）、无 pulsar binning、无相位阵（与分片模式同款挡法）；**zoom 顺带支持**（列为验证项）、crosspol 支持（`autocorrwidth` 全链逻辑不变） |
| D12 | 判据锚 | **SWIN 逐字节一致**（v9 链 vs v8 链、同数据同批）；次级锚 = `.part` 逐字节、PCAL 文本逐字节、fengine 体积 = raw 1× |
| D13 | difxmsg 角色 | fp 扮演 datastream（mpiId = dsindex+1、Diagnostic）、xp 扮演 manager（mpiId = 0、Running/Ending/Done）——与 v8 的分工逐一对应 |
| D14 | 组级 purge | 语义不变：xp 成功后哑执行（删该组 `fengine` 目录——现在删的是 stream.bin + pcal.bin）；raw 的批末清理不变；路径下发机制不变，工具侧只做哑执行 |
| D15 | 重跑纪律 | 组被 purge 后**从链头重跑**（fp 重读 raw、与 f 同）；xp 重跑 = 重读 stream.bin（1× 量，便宜）——既有重跑纪律延续 |

## 3. 承接：V8 遗留（2026-10-13 整理）

**主线声明**：V9 的主线是 **FFT 从 f 侧移到 x 侧**（§4–§11）；下表各项**不阻塞、不改变主线
范围**——与主线有交互的随主线一并处理，其余维持挂起，不借 V9 扩面。来源：`v8-plan.md`
§5.4 / §3 表（本仓库遗留项）。

| 项 | 来源 / 现状 | V9 落位 |
|---|---|---|
| **S5.3 真实数据四项**（FILL_PATTERN / invalid 位 / filler 修正量 / 真实 `.calc`） | `v6-plan.md` 挂起（待第二份真实观测） | 维持挂起；**交互**：fp 复用 datareader（含上述路径）——真实数据一到，新旧两链一并受益 |
| **S5.5 `executeseconds` 时间基准**（`getScanStartSec() != 0` 时静默不写盘） | V1 单 scan 挡着 | 维持；**交互**：xp 沿用现口径，**放开多 scan 前必须先修**（两链同真） |
| **S5.1 挂起项、"已知但有意未做"三条** | 处置见 `v6-plan.md` 末节 | 维持 |
| **判据 9b 的显式跨节点流量计数**（fxcorr 侧跨节点流量 ≈0 的量测） | `v8-plan.md` §5.2：架构成立 + 多轮全绿已实证，**显式计数未做** | 维持（需多节点环境；V9 不改"计算单元同节点"的部署形态） |
| **判据 8 遗留：集群读速率随作业采集机制** | `v8-plan.md` §3 表末 | 维持（环境依赖） |

## 4. 数据契约：fp → xp 的中间格式（`stream.bin`）

**语义**：内容 = 现 f 主循环里喂给 `Mode::setData` 的东西，逐 subint 落盘。即
`DataReader::readSubint` 的完整产物：**帧对齐、filler 丢弃、缺口占槽、坏帧标无效后的
"时间槽排布"数据**（含帧对齐 guard 重叠的 sendbytes 字节）+ 每 FFT 块的 valid flags +
时间戳。xp 顺序读、直接 `setValidFlags`/`setData`/`setOffsets`，**不做任何重新对齐**。

**格式（v1）**：

```
header（固定长）:
  magic      "FXCORR9S"（8 B）
  version    u32 = 1
  batch_id   char[32]
  station    char[16]
  ds_index   u32            ← 站内 datastream 序号（与 raw 文件名/_ds<N> 同口径）
  subintns   u32
  blockspersend u32, numbufferedffts u32
  nsubints   u32
  sendbytes  u32            ← 每 subint 数据块字节数（固定）
  framebytes u32, framespersec u32   ← 诊断/SWP 用
record（每 subint 一条，固定长）:
  status     u32            ← 0 = ok；1 = 无效 subint（readSubint 返回 0）
  validbytes u32            ← readSubint 返回值原样（可 < sendbytes——muxed VDIF /
                              Mark5B / 窗口不完整路径；xp 重放 setData 的字节数）
  scan u32, sec i32, ns u32 ← setData 的 (scan, datasec, datans)——reader 返回值原样
  offsetsec u32, offsetns u32   ← setOffsets 的（= batch 起点 + s×subintns 换算，冗余存防口径漂移）
  flags      u32[flagwords] ← valid flags，布局与现 f 相同（每 FFT 块一位、s32 字打包）
  data       u8[sendbytes]  ← 帧对齐数据；[validbytes, sendbytes) 补 0
```

**尺寸** = 92 + nsubints × (28 + 4×flagwords + sendbytes) ≈ raw 的 1×（guard 重叠与
flags 是 1× 之上的常数开销；header 定长 92 B、record 定长，见 `data-spec.md` 5.3.1 的
逐字段偏移表）。**为什么自定义而不复用**：

- 不复用 VDIF 帧：重排后的流不再是"文件字节的一段"（filler 已丢、缺口是空槽），
  再套 VDIF 会引入假的帧连续性；
- 不复用 `DataReader`：它带定位/缺口/filler 全套逻辑，对已排好的流是死重且可能二次
  修正（损坏数据）；xp 的 reader 应是 ~200 行的顺序读。

**不变量（fp 的写出纪律）**：写盘判错照 f 的纪律（`fflush`→`ferror`→`fclose` 三步，
失败即任务失败——`v7-plan.md` 17.2）；`status=1` 的 record 也写（xp 端据此走
`Mode::process` 的 INVALID_SUBINT 路径，行为与现状一致）。

## 5. 产物语义变化表

| 产物 | v8 现状 | V9（B） |
|---|---|---|
| `fengine/<b>/<g>/<st>/ds_N/band_XX.sp` | f 写（fp32 谱，16×） | **取消** |
| `…/autocorr.bin` | f 写、x 读 | **取消**（xp 内存直取，进 SWIN 的自相关记录照旧） |
| `…/pcal.bin` | f 写 | **xp 写**（同格式同位置；组 purge 后与 v8 同样消失） |
| `…/stream.bin` | — | **新增**（fp 写、xp 读，1×） |
| `vis/<exp>.difx/PCAL_<mjd>_<sec>_<st>_ds<N>` | f 写 | **xp 写**（同格式同位置同幂等语义；每 (站,ds) 一文件，组内无冲突） |
| `vis/<exp>.difx/SWITCHEDPOWER_…` | f 写 | **fp 写**（原始帧统计，不解包） |
| `vis-parts/<b>/ds<G>.part`、SWIN、FITS | x/merge/fits | **不变**（判据锚） |

## 6. 改动清单

### 6.1 fxcorr 侧（`fxcorr/` 仓库）

**`applications/fxcorr-fp/`（新）**——从 fxcorr-f 裁剪：

- 保留：参数解析（batch/station/workdir/ds_index）、batch.json、`.input`+Model 加载
  （时间换算与 DataReader 需要）、子整 subint 校验、`resolveDataFiles`（`real_path` >
  命名规则）、`DataReader` 全部、GAPCHECK/READPOS 诊断与 `FXCORR_LOGLEVEL`、时间戳三档
  （采集点重排）、DifxMessage Diagnostic、SWITCHEDPOWER 喂入（含重叠剔除与
  `lastReadContiguous` 判据，照抄）；
- 移除：`Mode` 构造与 process、FFT/归约/槽副本、`FEngineWriter`、`PcalTextWriter`、
  STA/kurtosis 分支；
- 新增：`StreamWriter`（§4 格式；`mkdir -p` 组目录同现布局；写盘判错照 f）；
- 行数预期：main.cpp 比 f 显著薄（subint 循环只剩 读→(SWP)→写 record）。

**`applications/fxcorr-xp/`（新）**——从 fxcorr-x 生长：

- 保留：全部命令行形态（批量/分片/两级 merge——**merge 两子命令不改**）、
  batch.json/config/Model 加载、`deriveDsGroups` 分片、validblocks 空输入防呆、
  `XmacEngine`、`Integrator`、Visibility/两级归并、DifxMessage manager 角色、
  时间戳三档；
- 新增（站级栈接入）：
  - **每 ds 一个 Mode**（`config.getMode(0, dsindex)`，xp 本就有 Configuration/Model——
    站级输入迁移无缺口）；组内 ds 列表来自分片（同现状）；
  - **数据源**：读组内各 ds 的 `stream.bin`（新 reader，顺序读；校验 header 与 config
    一致——subintns/bps/nchan 等，同现状 .sp 的头部校验口径）；
  - **subint 驱动**（照 f 的 main 循环骨架 + x 的 subint 循环合并）：每 subint
    对每个 ds `setValidFlags/setData/setOffsets/resetpcal/zeroAutocorrelations`;
  - **站级并行（D8）**：`#pragma omp parallel for`（ds 维度），每线程处理自己的 ds 的
    全部块（块内串行，位置型/归约型都天然保序）；随后 XMAC 阶段照现状（其内部基线并行不变）；
  - **谱输入（D9）**：`MemReader`（SpReader 兼容：`spectra()/flags()/weights()`
    从 Mode 取；weight 从 `getDataWeight(band, slot)` 按槽收集，同 f 的
    `writeSpectraBand` 口径）；
  - **autocorr 接入**：`Mode::averageFrequency` + `getAutocorrelation` 的 ac 批次逻辑
    从 f 的 `writeAutocorrelationBatch` 移植为**内存形态**，直接喂
    `Integrator::addAutocorrs`（接口从"读 autocorr.bin"改为"收内存记录"，数值路径不变）；
  - **PCAL 文本**：`PcalTextWriter` 移植（含 `accumulateWeight`、intTime 边界 flush、
    幂等替换）；**pcal.bin**：从 `FEngineWriter::writePcal` 摘出写盘函数；
  - STA/kurtosis（`FXCORR_STA`/`FXCORR_KURTOSIS`）：随 autocorr 跟进
    （sendSTA/sendKurtosis 从 f 移植，DifxMonitor 在 xp 已有）——**后置到阶段 2**，
    起步阶段这两个 env 置位时报错挡掉（不做静默忽略）；
- 挡掉的配置（D11）：多相位中心 / pulsar binning / 相位阵，启动报错（照分片模式的挡法）。

**`fxcorrcommon`**：**零改动**（这是本设计可行的关键——同一份 `Mode::process` 两个工具都
链接得到）。`FEngineWriter`/`PcalTextWriter` 类留在 fxcorr-f 内（不进库；xp 按需复制或
抽公共小函数——实施时定，偏向"各留一份、判据对拍兜底"）。

**`fxcorr/data-spec.md`**：新增 §5.3-补（`stream.bin` 格式与语义、产物变化）；`usage.md`
加两工具节；`CLAUDE.md` 目录表加两行。

### 6.2 对比实验方法（工具级）

同一 workdir、同一 batch、同一数据：

- **A/B 对拍**：先用 v8 链（f+x）跑一遍留底（SWIN / PCAL / `.part`），清产物后换 v9 链
  （fp+xp）再跑，逐字节比（§8）；或分 workdir 并行留档；
- **性能 A/B**：两链各 1–2 批（用本仓库 `run_batch.sh` 系工具），比 f/x 段计时、
  fengine 体积、整链时长（阶段 3）。

## 7. 性能与资源账

**结构性事实（本仓库已文档化）**：

| 项 | v8 现状 | V9（B） |
|---|---|---|
| 单 ds 中间产物/批 | fengine **502 MiB**（0.512 s 档实测；`data-volume.md` §1.4） | ≈ raw 1×：**31.4 MiB**（同源实测）——**16× 减量** |
| 整批中间物（4 站 × 8 ds） | ≈ 16.1 GiB | ≈ 1.0 GiB（变体 A → 0） |
| 中间 I/O/批 | 写 16.1 + 读 16.1 GiB | 写 1.0 + 读 1.0 GiB |
| fengine 介质约束 | **必须落 tmpfs**（`data-volume.md` §5.1） | 落到 /tmp 盘亦从容（1× 量级）——约束大幅缓解 |
| 站级 proc CPU | 3.60 CPU s/ds·批（4st512、OMP=1、O3 后；`apps/fxcorr-f/CLAUDE.md`） | **同量，移至 xp**（同一份 `Mode::process`，总量不变） |
| f 单任务 WALL | OMP=8 **3.05 s** / OMP=1 6.64 s（同源） | 估 **~0.3–0.5 s**（去 DSP，只剩读 33 MB + 对齐 + 写 33 MB） |
| x 段 | xmac 21.0 s（4st512 全批、单线程、O3 后；`apps/fxcorr-x/CLAUDE.md`） | + 组内站级（8 ds × 3.60 ÷ 并行度）——**线程数需重新标定**（阶段 3） |

**待标定项（阶段 3，不预判）**：批节拍的净收益**不能只按上述账推**——现役批节拍的构成
未完全解析（流水线依赖与任务派发节奏在起作用）；fp/xp 的真实 WALL、xp 的最优线程数、
稳态批节拍以工具级标定为准。fp 的 raw 读量不变（1× 本来就一样）→ 共享盘供给瓶颈不解除
（`data-volume.md` §5.1 的存储侧结论保持）。

**内存**：xp 每 ds 一个 Mode（首版无副本）；`Mode::getEstimatedBytes()` 可报预算，
量级参考 f 的实测（4st512、1310 块配置单 Mode 工作集数百 MB 级，8 个 ≈ 2–3 GB）——
实测确认列入阶段 2 判据。

## 8. 正确性策略与判据

**机理**（为什么位一致可期）：xp 与 f 用**同一份 `Mode::process` 源码**；xp 站级阶段
**每 ds 单线程块序串行**（D8），单 ds 内的加法序列与现状完全相同（不需要复制 f 的
per-block 归约）；XMAC 阶段是 x 现状的代码原样（基线写区独立 + 序列不变）。风险残点：
xp 的 subint 驱动顺序（每 ds 的 setData/setOffsets 与 f 逐字对齐——按 f 的 main 循环移植，
含 `datasec/datans` 与 `offsetsec/offsetns` 两个时间口径的用法）。

| # | 判据 | 方法 |
|---|---|---|
| 1 | **fp 流内容正确** | 同一数据，fp 的 record 内容与 f 运行中 `setData` 的 databuf 序列一致（f 加临时 dump 或单测工具重组比对）；`status/flags` 与 f 的 validflags 一致 |
| 2 | **fp 尺寸 1×** | `stream.bin` 尺寸 = header + nsubints×(24+4×flagwords+sendbytes)，与 raw 同量级 |
| 3 | **xp 单组分片产物** | v9 的 `vis-parts/<b>/ds<G>.part` 与 v8 同组 **逐字节一致**（小样本 4st 先行） |
| 4 | **全链 SWIN** | v9 链（fp+xp）与 v8 链（f+x）（同数据同批）SWIN 主文件**逐字节一致**；FITS 仅差 HISTORY |
| 5 | **PCAL 文本 / pcal.bin** | 逐字节一致（xp 写 vs v8 f 写） |
| 6 | **中间物 16×** | fengine 目录体积 = v8 的 ≈1/16（0.512 档 31.4 MiB vs 502 MiB 每 ds） |
| 7 | **回归面** | zoom（`test/zoom` 资产 + `cmp_swin_zoom.py`）、crosspol（`test/crosspol`）、gaps 七用例（fp 用同一 datareader，全数重跑）、失败路径（stream.bin 写失败即任务失败可见） |
| 8 | **性能** | 阶段 3 工具级标定：fp/xp 计时分层、xp 线程数扫描、中间 I/O 实测 vs §7 表 |

## 9. 实施阶段

| 阶段 | 内容 | 判据 |
|---|---|---|
| 0 | **fp 工具**（裁剪 + StreamWriter + SWP + 诊断）；`stream.bin` 格式定稿 | 判据 1/2/7（gaps 全跑）——**2026-10-13 完成**，实测见 `applications/fxcorr-fp/CLAUDE.md` |
| 1 | **xp 工具**：站级栈 + MemReader + autocorr 内存接入 + PCAL/pcal.bin（单 ds 组、小样本 4st） | 判据 3/5（单组逐字节）——**2026-10-13 完成**，实测见 `applications/fxcorr-xp/CLAUDE.md`（三个样本：pcal 单组 / 4 ds 双极化 / a1 病态；判据 4 的全量 SWIN 逐字节亦顺带通过；挡掉项 pulsar/相位阵/STA/kurtosis 实测） |
| 2 | xp 并行定标（ds 级并行）+ zoom/crosspol + STA/kurtosis + 内存实测 | 判据 4/6/7 全绿 |
| 3 | 工具级性能标定（两链 A/B：计时分层、xp 线程数扫描、中间 I/O） | 判据 8 |
| 4（后置） | **变体 A**：xp 直读 raw（datareader 三层迁 `fxcorrcommon`、fp 取消） | 同判据 3/4；中间存储归零 |

## 10. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | xp 内存峰值（8 Mode + 谱缓冲） | 首版单副本/ds + `getEstimatedBytes` 预算；阶段 2 实测；超限再拆组（分片划分已在，粒度可调） |
| R2 | 任务结构变化（f 32 任务 → fp 32 轻 + xp 4 重） | 多任务编排的槽数/线程数定案需**重标定**（阶段 3）；工具接口（分片粒度）不变 |
| R3 | zoom 切片（谱在内存后，x 侧"父带切片视图"的来源改变） | MemReader 复制 SpReader 的切片语义（channeloffset/nchanoverride）——阶段 2 专项验 |
| R4 | xp 的 `Integrator::addAutocorrs` 接口改造（文件→内存）偏离现状 | 数值路径不动（只换来源），判据 4 笼罩；改造处写注释锁定口径 |
| R5 | 重跑/多产品：xp 重跑要重读 stream.bin（组 purge 后不可重跑） | 沿用既有重跑纪律（从链头重跑）；stream.bin 1× 使"重读"便宜 |
| R6 | fp 的 SWP 迁移细节（重叠剔除、`lastReadContiguous`） | 照抄 f 现逻辑；SWP 用例（`test/tcal`）重跑 |
| R7 | difxmsg 时序（fp/xp 两工具分饰 datastream/manager） | 照 v8 分工逐条对应（D13） |
| R8 | PCAL 文本并发写（同站多 ds 同目录） | 文件名含 `_ds<N>`（每 (站,ds) 独占）、flush 幂等替换自己的行——v8 已验，语义不变 |
| R9 | 双链并存期的产物混淆（同一 workdir 混跑 v8/v9） | 两链互斥（D10）：换链前清产物（沿用"同 workdir 重跑纪律"） |

## 11. 后置（明确不做 / 延后）

- **变体 A**（xp 直读 raw、fp 取消、中间存储归零）——阶段 4；前置是把 datareader 三层
  （reader/timeline/corrections）从 `fxcorr-f/src` 迁入 `fxcorrcommon`（纯搬移）。
- **F 阶段块并行**（组内 ds 数 < 线程数时用 f 的 P3 归约复刻）——性能数据出来再议。
- STA/kurtosis 之外的 P9 尾部（kurtosis 复归一化细节）以对拍为准，不额外设计。
- 共享盘供给（`data-volume.md` §5.1）、mpifxcorr 的内存流水线形态（谱不落盘的对齐）
  ——V9 边界外。
- 多相位中心 / pulsar / 相位阵的 v9 支持——按需后补（挡掉报错起步）。

---

**v9 的设计细案即本文**；实施按 §9 阶段推进，每阶段判据过线后进入下一阶段。
