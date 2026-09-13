# fxcorr V2 算法改进需求与设计

V1 按最小科学闭环切分（f：解包/模型/通道化落盘，x：XMAC/积分出 SWIN），上游 mpifxcorr 的部分功能停在半路，且去 MPI 单进程后上游"进程网格并行"带来的算力不复存在。本文定义各项改进的**动机分类、要解决的问题、预期效果、设计要点**与优先级。定稿 2026-09-13，随实施更新。

## 动机分类（贯穿全文的两类原因）

- **功能未迁移**：上游 mpifxcorr 有该功能，V1 按边界裁剪未迁移（pcal 只落 pcal.bin 不出文本；zoom / 多相位中心 / 脉冲星 binning 上游有、V1 不支持；difxmessage 上游有、拆分后无进程发送）。
- **串行环境新变化**：去 MPI 后单进程串行，上游靠 D×B 进程网格取得的并行性消失，需要新的并行手段（多 x 子集、多线程）；容器 / scalebox 编排环境引入新约束与新机会（组播受限、网络流输入）。

## 优先级

排序依据（2026-09-13 调整）：**V2 只做 mpifxcorr 串行算法的迁移与完善——先补齐数据链路完整性（P0/P1，成本低、科学产出与可观测性受损），再科学功能（P4、P6-P8 按改动量递增），再监控消息（P9）、输入格式（P10）、reader 细节（P11）；并行化（P2/P3）与流式新能力（P5）挪 V3（README：模块级 OpenMP / 按需 GPU，并行与流式阶段）**。

| 优先级 | 改进项 | 动机分类 |
|---|---|---|
| P0 | PCAL_*.pcal 文件生成 | 功能未迁移 |
| P1 | difxmessage 状态/STA 消息 | 功能未迁移 + 环境变化 |
| P2 | 多 x 子集并行 | 串行环境新变化（⤴ V3，2026-09-13 挪出） |
| P3 | 多线程（f/x 进程内并行） | 串行环境新变化（⤴ V3，2026-09-13 挪出） |
| P4 | zoom band → 多相位中心 → 脉冲星 binning | 功能未迁移 |
| P5 | 网络输入 / 数据流化 | 串行环境新变化（新能力，⤴ V3，2026-09-13 挪出） |
| P6 | SwitchedPower（TCAL 噪声功率） | 功能未迁移 |
| P7 | 交叉极化自相关（WRITE AUTOCORRS） | 功能未迁移 |
| P8 | 相位阵频率域加权合并 | 功能未迁移 |
| P9 | Kurtosis STA + STA 频域平均分支 | 功能未迁移 |
| P10 | 输入格式补齐（Mark5B/LBA/其余 + 多线程 VDIF corner-turn） | 功能未迁移 |
| P11 | f 侧 reader 语义补全（valid flag 跨段续接、延迟中途重对齐） | 语义等价 |

P6-P11 为 2026-09-13 mpifxcorr 完整审查新发现项，详细设计待后续在本文补节（按 P0/P1 相近方式细化后再实施）。已确认上游死代码、不迁移：FILTERBANK USED/PROCESSING METHOD、dumplta/ltachannels、checkData（`#if 0`）、相位阵 TIMESERIES 输出；硬件访问（StreamStor/Mark6）不迁移。

对拍原则：每项尽量与 mpifxcorr 基准对拍（run_bench.sh 产出），P0 文本 diff、P4 各功能 SWIN 对拍、P6/P8 SWIN 对拍、P2/P3（V3 实现时）与单 x 全量结果全等。

---

## P0：PCAL_*.pcal 文件生成（✅ 2026-09-13 完成）

### 动机分类

功能未迁移。上游 fxmanager 写线程每写一个 dump（时长 = intTime）SWIN 的同时，把该 dump 的 pcal 行追加到实验级文本文件 `vis/<exp>.difx/PCAL_<mjd>_<sec>_<station>`（visibility.cpp writeSWIN，数据行 987-1051；文件头 initialisePcalFiles 107-136）。V1 的 f 只落盘 subint 级 pcal.bin（data-spec 5.3），文本无人生成，链路停在半路。

### 要解决的问题

相位校准（pcal）是 VLBI 标准观测流程：下游 pcal 分析软件按上游文本格式解析 tone 幅度/相位。没有 PCAL_*.pcal，difx2fits 之外的校准链（pcal 提取、bandpass 校准）断裂，科学产出不完整。

### 预期效果

每实验每站一个 PCAL_*.pcal：追加式、每 intTime 一行、与 mpifxcorr 基准**逐字节对拍通过**（diff bench/<exp>.difx/PCAL_*）；多 batch 连续跑正确追加，重跑 batch 幂等（不产生重复行）。

### 设计

**落点：fxcorr-f 写**。pcal 是 station 级数据（f 是 station-based）；tone 复数就在 f 内存的 writePcal 挂点，无需回读 pcal.bin；x 侧不动。

- **文件头**：文件不存在或为空时写 5 行注释（下游软件会解析，原样保留上游格式）：
  ```
  # DiFX-derived pulse cal data
  # File version = 1
  # Start MJD = <mjd>
  # Start seconds = <sec>
  # Telescope name = <station>
  ```
- **路径与目录**：从 .input OUTPUT FILENAME 解析 `vis/<exp>.difx/`（复用 fxcorr-x 的解析），f 先跑负责 mkdir。
- **intTime 聚合（关键差异）**：pcal.bin 是 subint 级记录，PCAL 行是 intTime 级。上游 results 的 pcal 段每 subint 累加（core.cpp:1131 copyPCalTones）、每 dump 写一行后清零。f 侧同构：writePcal 每 subint finalisepcal 后把 tone 累加进聚合器（pcal.bin 写入不变），聚合顺序 = subint 顺序，与上游逐位一致；累计满 intTime 时写行并清零。
- **数据行格式（逐字节照抄上游）**：
  `站名  pcalmjd(%17.11f)  intTime/86400(%13.11f)  dsindex  n_recorded_bands  max_tones` + 每 tone ` 频率MHz(%.12g)  pol(%c)  re(%12.5e)  im(%12.5e)`；
  pcalmjd = intTime 起点 + intTime/2（用 .input START MJD/SECONDS 换算绝对时间）；某 band 实际 tone 数不足 max_tones 时补 ` -1 0 0 0`；**USB 的 im 取负、LSB 写原值**（extractor 对 USB 输出 -j，写盘时修正）；nonzero 条件照抄（任一 tone 的 re≠0 且 im≠0 才写行）。
- **重跑幂等（已定稿，策略 A）**：写行前读文件、删掉与当前行同一 intTime 时间戳的旧行，全部数据行按 pcalmjd 排序后重写。文件小（每 intTime 一行，10 小时实验约百 KB），全量重写毫秒级；任何顺序重跑 batch 均幂等，行序保持时间序。
- **zoom 的 pcal 缩放**（visibility.cpp:664 的 data-contribution 缩放）：V1 无 zoom，随 P4a 一并补。
- **验证**：单 batch 对拍（diff mpifxcorr 基准 PCAL）；多 batch 验证追加（行数 2×、pcalmjd 递增）；重跑 batch 验证幂等；容器模式回归（PCAL 落在挂载的 vis/ 内）。
- **文档同步**：data-spec.md 5.4 节补 PCAL_*.pcal 文本格式说明（现注"列入 V2"改为已实现）。

### 算法详解（2026-09-13 实施完成）

#### 上游数据链路全景（mpifxcorr 如何产出 PCAL 行）

1. **提取（station-based，core 进程）**：`Mode::process` 每处理一个 FFT 块，把解包时域数据喂给 `PCalExtractor::extractAndIntegrate`（mode.cpp:765-769）。提取器对实数据做解析信号构造（下变频 + Hilbert），按 tone 频率（`getDRecordedFreqPCalToneFreqHz`，RF 系）提取各 tone 复幅度，内部 DFT 输出**未经归一化**（幅度 ∝ 信号幅度 × 积分采样数）。
2. **subint 累加**：每 subint 结束，`Core::copyPCalTones`（core.cpp:1131-1153）调 `mode->finalisepcal()`（冻结提取器输出）→ `getPcal(j,t)` 累加进 results 数组的 pcal 段（f32 += f32，band-major 依序）。一个 dump（= intTime）内多个 subint 的 tone 值在此段内累加。
3. **校准缩放（manager 进程）**：写 SWIN 前，`Visibility::writedata` 的 "calibrate the pulse cal" 段（visibility.cpp:652-676）把 pcal 段整体乘一个**正实数 scale**（公式见下节），使输出归一化到单位强度。
4. **写行**：`writeSWIN`（visibility.cpp:987-1051）把缩放后的 pcal 段格式化成一行，追加写入 `PCAL_<mjd>_<sec>_<station>`；文件头由 `initialisePcalFiles`（visibility.cpp:107-136）在实验开始创建。

#### f 侧实现对应表

| 上游环节 | 上游位置 | f 侧落点 |
|---|---|---|
| 提取（extractor） | mode.cpp:765（fxcorrcommon 复用零改动） | 同（Mode 复用） |
| subint 冻结 + 写 pcal.bin | copyPCalTones core.cpp:1131 | fenginewriter.cpp writePcal（已有） |
| intTime 聚合 | results pcal 段累加 + dump 清零 | `PcalTextWriter::accumulate`（cf32 聚合器，band-major 同序） |
| 权重累加（校准用） | copyACWeights / averageAndSendAutocorrs core.cpp:1331 | `PcalTextWriter::accumulateWeight` |
| 校准缩放 | visibility.cpp:655-672 | `PcalTextWriter::flush` 内 bandscale |
| 行格式化 + 追加 | visibility.cpp:989-1051 | `PcalTextWriter::flush` |
| 文件头 | initialisePcalFiles visibility.cpp:107-136 | flush 首次写时补头（lazy） |
| dump 边界驱动 | fxmanager 每 intTime 调 writedata | main.cpp 按 subint 计数判断 intTime 边界 |

#### 逐字节一致的六个关键点

1. **intTime 聚合顺序**：pcal.bin 是 subint 级记录，PCAL 行是 intTime 级。上游 results 的 pcal 段每 subint `+=`（f32），dump 写完行后清零（下个 dump 重新累加）。f 侧聚合器同样 f32 +=、按 subint 顺序累加、flush 后清零——浮点求和顺序一致才能逐位一致。
2. **校准缩放公式**（visibility.cpp:670）：
   ```
   scale = 1/(acw × meansubintsperintegration × (float)(blockspersend×2×freqchannels×chanstoavg))
   acw   = f32( intTime 内 Mode::getWeight 累加 / fftsperintegration )    // visibility.cpp:455 存 f32
   ```
   逐位一致要求复现全部类型截断点：acw 是 f32 存储、`(float)(...)` 显式截断、scale 经 `vectorMulC_f32_I` 的 f32 参数再截断一次、tone 乘法是 f32×f32。f 侧同式（`PcalTextWriter::flush` 的 bandscale 计算），实测与基准逐位一致。注意代入 fftsperintegration = meansubints×blockspersend 后可消元，但 f 侧保留上游原式计算式以规避消元引入的舍入差。
3. **权重读取时机**：`Mode::getWeight` 返回的权重会被 `zeroAutocorrelations()` 清零（mode.cpp:1467）。上游在 `averageAndSendAutocorrs` 里、每次 autocorr 批次写盘**之前**读权重（读后即清零）；f 侧须在每个 autocorr 批次写盘前调 `accumulateWeight`（main.cpp 两个批次点）。初版在 subint 尾（最后一次 zero 之后）读，恒得 0 → acw=0 → 不缩放，输出为未归一化的 1.7e6 量级（与基准差 8.3e6 倍）。
4. **边带符号**（visibility.cpp:1019-1033）：**LSB 写原值、USB im 取负**。原因：提取器对实信号的解析信号构造固定输出 -j（对 sin tone），USB 数据需写盘时取负修正相位；初版按相反方向实现（LSB 取负），症状 = 幅度逐位一致、im 全体反号（re 不变，即整体共轭）。定位过程：两侧 pcal.cpp 与 extractor 代码 diff 均空 → 用同一段合成数据分别链接两侧 pcal 编译独立实验，证实 extractor 输出两侧相同（im 均为负）→ 差异只可能在上游写盘层 → 重读 visibility.cpp 发现 LSB/USB 分支写反了记忆。
5. **pcalmjd 分解计算**（visibility.cpp:983-989 同式）：
   ```
   dumpmjd     = startMJD + intTime起点当日秒/86400      （整型除法，先整日后余数）
   dumpseconds = intTime起点当日秒%86400 + 起点ns/1e9 + intTime/2.0
   pcalmjd     = (double)dumpmjd + dumpseconds/86400.0
   ```
   MJD ~5.9e4 时 %17.11f 的 16 位有效数字贴近 f64 精度极限，必须按上游的"先整日后余数"分解计算，直接 `jobstart + (绝对秒+intTime/2)/86400` 的等价式会产生 1e-11 天级差异导致对拍失败。f 侧在 main.cpp 以 long long ns 计算 intTime 起点（`batchstartabsns + k×subintsperint×subintns`，切批约束保证 batch 起点对齐 intTime 边界），传入 flush 后按上游分解式计算。
6. **行格式细节**：`%s %17.11f %13.11f %d %d %d` + 每 tone ` %.12g %c %12.5e %12.5e`；tonefreq 保持 float 类型（上游 `float tonefreq = 1e-6*getDRecordedFreqPCalToneFreqHz(...)`，double 会改变 %.12g 输出）；某 band 实际 tone 数不足 max_tones 时按上游补 ` -1 0 0 0`；nonzero 条件照抄（任一 tone 缩放后 re≠0 且 im≠0 才写行，全零仅清零聚合器不写）；intTime 边界由 subint 计数判断（`(s+1) % subintsperint == 0`，subintsperint = round(inttime×1e9/subintns)，避免 double 秒的累积误差），batch 结束聚合器非空即报错退出（切批约束违反，fail-fast）。

#### 幂等追加策略（3 版演进）

目标：实验级文件跨 batch 追加、任意 batch 重跑不产生重复行、行序保持时间序。

- **v1（误删后续 batch）**："删掉 pcalmjd ≥ 当前行时间戳的旧行"——重跑中间 batch 时，后续 batch 的行时间戳更大，被一并误删。
- **v2（误删本 batch 已写行）**："删掉落在 [batch 起点, batch 终点) 窗口内的旧行"——每 flush 一次就把本 batch 前面已写的行删了，每 batch 只剩最后一行。
- **v3（定稿）**："删掉与当前行同一 intTime 时间戳的旧行（|oldmjd − pcalmjd| < 1e-8 天容差），全部数据行按 pcalmjd 排序后重写"。一行一 intTime，时间戳相等即精确替换键；文件小（每 intTime 一行，10 小时实验约百 KB），全量读改写毫秒级；容差 0.86ms 吸收 %17.11f 往返误差、远小于半 intTime。

#### 连带修复与文档修正

- **fxcorr-sim pcal 注入幅度 0.1 → 0.7**（signalgen.cpp）：pcal 信号按 `0.1×sin` 注入，2bit 量化 `rint(v×2)+2` 的边界在 ±0.5——0.1 幅度的 2v ∈ (−0.2, 0.2) 永不跨档，信号被量化器完全抹平（pcal.bin/PCAL 全零，mpifxcorr 基准同样提取不到，两边一致地"没有数据行"）。0.7 与主 tone 同幅度后跨档正常。
- **pcal.bin 布局文档修正**（data-spec 5.3）：V1 实现为 freq+pol 交错（每 tone 9B），原 data-spec 文本写成分组数组——按实现修正文本（P0 不改二进制格式）。
- **测试资产新增**（fxcorr/test/pcal/）：`test-pcal.vex`（$PHASE_CAL_DETECT 段 def 内直接追加 tone 列表 `2 : 3 : 4 : 5`——VEX 词法无 `tones` 关键字，数值列表紧跟 `phase_cal_detect = &NoCal` 引用之后）、`test-pcal.v2d`（phaseCalInt = 0 → 1，MHz）。复制成 config/test.vex / test.v2d 后复用 make_testdata.sh 全流程；vex2difx 将 tone 序号 × 1MHz + base(0) 生成 .input PHASE CAL（difx 0-based 序号 1-4 → 201-204MHz，band 4MHz 内 4 tone）。注意 .input 的 PHASE CAL INDEX 行（freq table 级）与 configuration.cpp 自动生成的 tone 网格（bandedge 起按 interval 步进、与 INDEX 行无关）是两回事——提取与 PCAL 行都走后者。

#### 验证方法与结果

✅ 2026-09-13：单 batch 与 mpifxcorr 基准**逐字节一致**；多 batch 追加/重跑幂等验证通过；容器模式落盘与基准一致。完整验证方法与结果记录见 `fxcorr/test/pcal/README.md`。

---

## P1：difxmessage 状态/STA 消息

### difxmessage 是什么

DiFX 自带的**状态广播库**（`libraries/difxmessage`）：进程把状态/告警/进度以 XML 消息组播到网络（默认 `224.2.2.1:50201`，UDP 尽力而为），观测室的 difxwatch/errormon 等监控工具监听显示。它**不承载科学数据**、不是进程间数据通道（fxcorr 的 f→x 数据走目录接口）、也不是日志系统（run.log/batches.index 才是）。

**消息类型**：
- **Status**（状态机）：Starting → Running → Ending → Done；错误时 Aborting。
- **Alert**（告警）：FATAL/SEVERE/ERROR/WARNING/INFO 分级，带错误文本。
- **Diagnostic**（诊断）：数据消费字节数、输入速率、缓冲状态。
- **STA**（二进制）：station-based 自相关功率谱（独立组播通道），属内容监控而非进度。

**进度粒度**（三层）：
- Running：**每积分一条**（intTime 级，典型 1-4 秒）——"当前相关到第几秒"，附积分中心 MJD 与各站权重。
- Diagnostic：f 每 subint 两条（典型 0.1-0.5 秒）——吞吐速率与累计字节。
- STA：每 autocorr 批次一条谱图（10ms 级）——内容，不是进度。

比 batch 级细（batch 内可见推进）、比 FFT 块级粗。

**用途边界（为什么 P1 设计成默认全静默）**：difxmessage 是纯对外的监控接口，**唯一价值前提是有外部消费者**——UDP 组播没人收即消失。串行相关器自身不需要它。不设 `DIFX_MESSAGE_GROUP/PORT` 环境变量时零开销零输出（与上游 inUse 语义一致），纯批处理场景完全无感。与 scalebox 编排的关系：task 级状态（排队/运行/完成/失败）与 Status 状态机**重叠**（Starting/Done/Aborting 对编排层冗余）；**不重叠**的只有三点——task 内部秒级推进（区分"正常长跑"与"卡死/IO 挂起"，scalebox 只能靠超时兜底）、数据速率异常（吞吐掉零但进程没死）、失败原因文本（Alert 比 exit code + 翻 stderr 结构化）。是否需要开启取决于编排策略：超时 + exit code + batch.json status 够用则永远不开启；需要卡死检测时开环境变量即可。

### 动机分类

功能未迁移 + 环境变化。上游 mpifxcorr 经 `libraries/difxmessage`（上游自带独立库，install-difx 已注册，L106/215/249/477-478，无需新注册）组播广播状态，观测室监控工具（difxwatch/errormon）依赖它。V1 只有 batch.json 的 status 落盘。且容器内组播受限（需 host 网络），编排层（scalebox）也需要状态信号。

### 要解决的问题

实时监控缺失：job 进度、ETA、异常无人可见；scalebox 编排无法感知任务内部状态，只能轮询 batch.json。

### 预期效果

f/x 按上游 DifxMessage 格式发送状态（启动/进度/完成/异常），现有 difx 监控工具零改造可接；容器模式降级为落盘 difxmessage 格式文件，由编排层转发，两种模式共用同一套消息结构（字节级一致，可互相对拍）。

### 设计

**落点：fxcorrcommon 新增 `difxmonitor.{h,cpp}` 封装**（fxcorrcommon 已 PKG_CHECK difxmessage，f/x 经它传递链接，无需新增依赖；difxmonitor.h 进安装头清单、`src/Makefile.am` 的 libfxcorrcommon_la_SOURCES 加 difxmonitor.cpp）。封装自生成 XML（与 difxsend.c 格式逐字节一致）、统一分发：host = 组播（`MulticastSend`，difxmessage 公开函数），container = 落盘追加文件。f/x 的 main.cpp 调用，不直接碰 difxmessage。

**角色映射**（无 MPI 后进程角色重新分配，对齐上游语义）：
- **x = manager 角色**（mpiId=0）：发 RUNNING（上游在 manager 进程发，visibility.cpp:1140 multicastweights）。
- **f = datastream+core 融合角色**（mpiId=dsindex+1）：发 DIAGNOSTIC（上游 datastream.cpp:631-634）与 STA 二进制（上游 core.cpp:1198）。**f 不发 RUNNING**——RUNNING 是 visibility 进度语义，f 先跑完 x 再跑，f 发 RUNNING 会让进度在 f→x 交接时回退；f 的进度由 DIAGNOSTIC 表达（与上游分工完全一致）。
- **identifier**：.input 文件 basename（去掉路径与 `.input` 后缀，同上游 generateIdentifier mpifxcorr.cpp:217-239）；f/x 同 identifier，监控器按它聚合同一 job。`difxMessageSetInputFilename` 同上游设置（alert/status body 带 `<input>` 标签）。

**消息清单与发送节奏**（逐点对照上游）：

| 消息 | 上游位置 | fxcorr 落点 | 节奏 |
|---|---|---|---|
| STARTING（difxMessageSendDifxStatus） | fxmanager ctor fxmanager.cpp:73 | f/x main（.input 解析、校验通过后） | 每进程 1 次 |
| RUNNING（difxMessageSendDifxStatus3，附 mjd/weight/jobstart/jobstop） | visibility.cpp:1140 | x：integrator.addSubint 返回 true（一次积分写盘）时 | 每积分 |
| DIAGNOSTIC（BufferStatus/InputDatarate/DataConsumed） | datastream.cpp:631-634 | f：每 subint readSubint 之后 | 每 subint |
| STA 二进制（DifxMessageSTARecord） | core.cpp:1198（averageAndSendAutocorrs 内） | f：每 autocorr 批次、writeAutocorrelationBatch 之前（可选，FXCORR_STA=1） | 每批次 |
| Alert | alert.cpp:54 | f/x：错误 exit 路径（统一 helper：cerr + Alert + ABORTING） | 错误时 |
| ENDING → DONE / ABORTING | fxmanager.cpp:268-290 | f/x 正常结束（先 ENDING 后 DONE，上游 terminate() 节奏）/ 错误退出（ABORTING） | 每进程 1 次 |

**发送方式开关**：
- host（默认，FXCORR_RUN_MODE 未设或 ≠container）：组播。group/port 取 `DIFX_MESSAGE_GROUP`/`DIFX_MESSAGE_PORT` 环境变量（setup.bash 默认 224.2.2.1:50201）；port 未设（= -1）时静默不发送——与 difxmessage 库 `difxMessageSend2` 的 inUse 语义一致（difxsend.c:88-98，零风险默认关闭）。
- container（FXCORR_RUN_MODE=container）：落盘 `meta/difxmsg/`。每进程一个文件（避免多进程并发追加竞态，NFS 上 O_APPEND 单 write 也不保证原子）：f → `meta/difxmsg/<exp>_<batch>_<station>.xml`、x → `meta/difxmsg/<exp>_<batch>.xml`；每条消息 = 完整独立 XML（`<?xml?><difxMessage>…</difxMessage>`），与组播字节一致（同一生成路径）；文件 O_TRUNC 打开（重跑 batch 幂等，覆盖自己的旧文件）。编排层轮询该目录转发。
- **STA 开关**：上游由 difxmessage 命令线程动态设 `dumpsta=true`（默认 false）+ `stachannels`（默认 32）。fxcorr 批处理进程无命令线程 → 环境变量 `FXCORR_STA=1` 开启（V1 不做 LTA/kurtosis）；STA 走独立二进制通道 `DIFX_BINARY_GROUP`/`DIFX_BINARY_PORT`（同上游 difxsta.c），container 模式落盘 `meta/difxmsg/<exp>_<batch>_<station>.sta`（DifxMessageSTARecord 原始结构追加）。

**difxmonitor 接口**（fxcorrcommon，C++ 封装）：
```cpp
class DifxMonitor {
    DifxMonitor(int mpiId, const string &identifier, const string &inputFilename, const string &containerPrefix);
    void status(enum DifxState, const string &msg, double visMJD, int nDS, const float *weight,
                double mjdStart, double mjdStop);   // 状态（含 STARTING/RUNNING/DONE/ABORTING）
    void alert(const string &msg, int severity);    // DIFX_ALERT_LEVEL_*
    void diagnostic(DifxDiagnosticType, long long bytes, double rateMbps);  // 数据消费/速率/缓冲
    void staSend(const DifxMessageSTARecord *record, int nbytes);           // BINARY_STA
};
```
containerPrefix 如 `meta/difxmsg/test_60512_45000_T1`（.xml/.sta 由封装按消息类型补后缀）；host 模式忽略。XML 生成照抄 difxsend.c（difxmessageinit.c:138-152 的 XML 模板 + difxsend.c:940-1148 的 body 格式），seqNumber 每进程从 0 自增（同库语义）。

**STA 记录组装（f 侧）**：DifxMessageSTARecord 全字段对照 core.cpp:1195-1253（注意：该块在 `averageAndSendAutocorrs` 函数内，**每 autocorr 批次发送一次**，数据窗口 = 本批次，不是整个 subint）：
- messageType=STA_AUTOCORRELATION、dsindex、coreindex=0、threadindex=0、identifier=getJobName 截断 31 字节、nChan=getSTADumpChannels()（freqchannels < nChan 时收缩）。
- 时间戳：scan=0；sec = scan 起点当日秒 + subint 偏移秒（代码照 core.cpp:1208-1214：`getScanStartSec(...) + offsets[1]`，当日秒系，注释与代码不一致以代码为准）；ns = offsets[2] + nsoffset（nsoffset = 批次中心偏移 = (acshiftcount×maxacblocks + 批次块数/2)×blockns，≥1e9 进位）；nswidth = 批次宽度（满批次 maxacblocks×blockns，尾批 acblockcount×blockns）。
- data = 实功率谱前 nChan 通道：每通道 = chans_to_avg 个相邻实部之和（非平均）× renorm，renorm = 1/(2×freqchannels×getWeight)（datastreamsaveraged 时再 /getFChannelsToAverage）；f 侧无 averageFrequency（V1 不降频），恒走非 averaged 分支。
- **最低权重门槛**（core.cpp:1218-1220）：weight < 0.333×stasamples/(2×freqchannels)（stasamples = 0.001×nswidth×2×bandwidth）时该 band 不发送（dodgy packet 保护）。
- 每 datastream 每 band 一条记录；单站 f 进程即每 band 一条。f 侧取数时点在 writeAutocorrelationBatch（内部 averageFrequency）**之前**、本批次 zeroAutocorrelations 之前。

### 算法详解

#### 上游数据链路全景（mpifxcorr 如何发消息）

1. **初始化**（mpifxcorr.cpp:296-298）：identifier = .input basename（generateIdentifier 去路径、去 `.input` 后缀）→ `difxMessageInit(mpiId, identifier)` → `difxMessageSetInputFilename(argv[1])`。difxMessageInit 读 `DIFX_MESSAGE_GROUP`/`DIFX_MESSAGE_PORT`（difxmessageinit.c:97-136），任一缺失即 inUse=0 静默不发送。
2. **STARTING**：fxmanager 构造时发（fxmanager.cpp:73）。
3. **RUNNING**：manager 写线程每写一个 dump（= intTime）前，`Visibility::multicastweights`（visibility.cpp:1100-1146）计算各站 band 平均权重（`weight[i] = Σ autocorrweights[i][0][j] / weightcount`，仅 used frequencies），发 `difxMessageSendDifxStatus3(RUNNING, "", 积分中心mjd, numdatastreams, weight, jobstartMJD, jobstopMJD)`。
4. **DIAGNOSTIC**：datastream 进程每批数据读入时发 BufferStatus/InputDatarate/DataConsumed（datastream.cpp:631-634）。
5. **STA**：core 进程每 subint（processdata 内，averageFrequency 前），dumpsta 开关开时按上节字段组装 DifxMessageSTARecord，`difxMessageSendBinary(..., BINARY_STA, bytecount)` 组播到 DIFX_BINARY_GROUP/PORT（difxsta.c，port 未设静默 -1）。
6. **Alert**：错误时 difxMessageSendDifxAlert(msg, level)（alert.cpp:54）。
7. **收尾**：正常 DONE、SIGINT TERMINATING→TERMINATED、错误 ABORTING（fxmanager.cpp:268-290）。

#### f/x 实现对应表

| 上游环节 | 上游位置 | fxcorr 落点 |
|---|---|---|
| init（identifier/mpiId/inputFilename） | mpifxcorr.cpp:296-298 | DifxMonitor 构造（f/x main） |
| STARTING | fxmanager.cpp:73 | main 校验通过后 |
| RUNNING + weight 计算 | visibility.cpp:1100-1146 | x：Integrator 内 writedata 后、increment 前（Integrator 构造收 DifxMonitor*）；weight 照抄 1116-1146：autocorrweights[i][0][j] = `vis_->floatresults`（public 字段）`[getCoreResultACWeightOffset(configindex,ds)*2+j] / fftsperintegration`，used band 平均 |
| DIAGNOSTIC | datastream.cpp:631-634 | f main：readSubint 返回字节数后 |
| STA 组装 + 发送 | core.cpp:1195-1253 | f main：writeAutocorrelationBatch 后（FXCORR_STA=1 时） |
| Alert | alert.cpp:54 | f/x 错误 exit 统一 helper |
| DONE/ABORTING | fxmanager.cpp:268-290 | f/x main 尾部/错误路径 |

#### 关键点（易错，实施必读）

1. **XML 逐字节一致**：difxmessage 的 XML 模板（difxmessageinit.c:138-152）与 body 格式（difxsend.c:940-1148）直接照抄：header 的 `<from>` = 本机 hostname、`<mpiProcessId>`、`<identifier>`、`<type>Status/Alert/...`；Status body = `<difxStatus><input>…</input><state>Running</state><message>…</message><visibilityMJD>%9.7f</visibilityMJD><jobstartMJD>…<jobstopMJD>…<weight ant="%d" wt="%5.3f"/>…</difxStatus>`；expandEntityReferences 转义 `<>&`。container 落盘文件与组播包字节一致，对拍可 diff。
2. **weight 公式定稿**：上游 RUNNING 的 weight[i] = 各 used band 的 autocorrweight 平均，其中 `autocorrweights[i][j][k] = floatresults[getCoreResultACWeightOffset(configindex,i)*2+k]/fftsperintegration`（visibility.cpp:455），`fftsperintegration = meansubintsperintegration × getBlocksPerSend`（visibility.cpp:1316）。x 侧同式：`vis_->floatresults`（public 字段）的 acweight 段 ÷ (subintsperint×blockspersend)，f32 除法顺序一致即逐位一致。**时序**：Integrator::addSubint 里 writedata 后、increment 前发送（increment 清零 floatresults，之后取数为 0；上游同序——fxmanager loopwrite 也是 writedata() 后 multicastweights()）。numdatastreams > 20 截断、weight < 0 不发该条（difxsend.c:957-970 同款）。
3. **RUNNING 时间戳**：visibilityMJD = 积分中心（intTime 起点 + intTime/2，同 P0 的 dumpmjd 分解式计算）；jobstart/jobstop MJD = 实验 START MJD/SECONDS 与 + executeseconds（x 侧 executeseconds 已按 batch 偏移修正，用 batch 的起止时刻）。
4. **f 不发 RUNNING**：f 的 subint 进度用 DIAGNOSTIC（DataConsumed/InputDatarate），避免 f→x 交接时监控器进度回退。
5. **STA 的 renorm 与门槛**：renorm = 1/(2×freqchannels×getWeight)（f 侧无 averageFrequency 恒走此分支）；weight < 0.333×stasamples/(2×freqchannels) 的 band 跳过；data 是实部之和（非平均）——照抄 core.cpp:1244-1249，勿"顺手"改成平均。
6. **STA 时间戳是当日秒系**（core.cpp:1209 `getScanStartSec(...) + offsets[1]`），非注释所说的 scan 相对——以代码为准。
7. **默认零行为**：host 模式未设 DIFX_MESSAGE_GROUP/PORT 时全部静默（port=-1 不发），与上游一致；container 模式仅 FXCORR_RUN_MODE=container 时落盘。两种模式互斥、默认全关，对现有对拍零影响。
8. **容器落盘幂等**：O_TRUNC 覆盖本进程自己的文件（文件名含 batch_id + station），重跑 batch 无重复行；跨 batch 的 f 任务文件名不同（batch_id 不同），天然追加语义交给编排层按 batch 聚合。

### 验证方法与结果（2026-09-13 实施完成）

- **默认零行为回归**：不设任何状态环境变量时 f/x 跑批完全静默（组播 port 未设即不发，同上游 inUse 语义），SWIN 与 mpifxcorr 基准 cmp_swin.py 对拍 6/6 全等（P1 对科学输出零影响）。
- **节奏与字段对拍（host 组播，测试机 cmp5）**：`run_bench.sh` 跑 mpifxcorr 基准与 fxcorr host 模式各抓一组消息（python 组播接收器），对比：
  - 消息序列：mpifxcorr Starting → Running×2 → Ending → Done；fxcorr-x 完全一致（Starting → Running×2 → Ending → Done）；fxcorr-f（每站）Starting → Diagnostic×2×subints → Ending → Done。
  - RUNNING 字段逐位一致：visibilityMJD = 58948.2916727/58948.2916849、weight ant0/1 = 0.989/0.989、1.000/1.000、jobstartMJD = 58948.2916667。jobstopMJD 不同（fxcorr 58948.2917014 vs 基准 58948.2916898）——fxcorr 的 executeseconds 按 batch 偏移修正（+1s 语义，见本目录关键实现要点），jobstop 随之平移，预期差异非缺陷。
- **STA**（FXCORR_STA=1，单 band test 配置）：4 subints 共 207 条记录（每 autocorr 批次一条，与上游 averageAndSendAutocorrs 节奏一致）；头字段自检通过——batch 起点 sec/ns 正确、尾批 nswidth 2048000ns 正确、sec 进位正确、nChan=32、identifier=jobname、renorm 后功率 ~8053 量级合理。
- **container 模式**：FXCORR_RUN_MODE=container 跑批，`meta/difxmsg/test_58948_25200.xml` 4 条、T1/T2 各 10 条（含 8 条 Diagnostic）、`.sta` 41400 字节（207×200B）；落盘 XML 与组播抓包逐字节一致；**重跑幂等**（构造时截断，两次重跑仍 4 条——初版用 fopen "a" 追加导致重跑翻倍，已修为构造时 O_TRUNC）。
- **错误路径**：删站触发 cannot read → 组播抓到 Alert（severity=2 ERROR）+ Aborting 状态 ✓。
- **文档同步**（已做）：data-spec.md 5.6 meta/ 布局加 `meta/difxmsg/`；usage.md 三工具环境变量表加 FXCORR_STA / DIFX_MESSAGE_GROUP/PORT / FXCORR_RUN_MODE；f/x 的 CLAUDE.md 调用方式章节同步；v2-plan.md 第 5 节 P1 行更新为 ✅。
- 未做项（已确认可接受）：difxwatch 监控工具实接（utils 未注册 install-difx，需手工构建——组播抓包已直接验证 XML 格式）；mpifxcorr 基准 STA 对拍（上游 dumpsta 需经 difxmessage 命令线程动态开启，批处理场景不便——以字段自检 + 数值量级为准）。

---

## P2：多 x 子集并行

### 动机分类

串行环境新变化。上游 XMAC 由 B 基线进程网格并行（每个 baseline 进程串行做若干基线）；拆分后单进程 fxcorr-x 串行全部基线。XMAC 是算力热点，大站网/高通道数下单核处理不完。

### 要解决的问题

x 侧吞吐不足成为流水线瓶颈；单机多核（测试机 100 核）与多节点无从利用。

### 预期效果

基线集合切 N 个子集、N 个 fxcorr-x 进程并行，x 侧吞吐约 ×N（受共享存储读 .sp 带宽与 SWIN 写带宽约束）；子集可分布到多节点。

### 设计

- **切分**：基线集合按算力均衡切成子集（.input BASELINE TABLE 驱动，round-robin 或按基线通道数加权分配）；每子集一个 fxcorr-x 进程。
- **输出合并**：各子集写独立子目录（如 `vis/<exp>.difx.s<N>/`），difx2fits 前合并成实验级 `vis/<exp>.difx/`（SWIN 追加保序，与 data-spec 12 节约定一致，实施前同步细节）。
- **元数据**：batch.json 增子集描述字段（baselines 切分表），编排脚本与 x 进程共用。
- **编排**：run_batch.sh 本地多进程并发（wait 收口）；scalebox 环境下改为分发（各子集一个 module 实例）。
- **验证**：N 子集结果合并后与单 x 全量输出 cmp_swin.py 全等；性能测试测吞吐增益（fxcorr-sim 生成大数据量）。

---

## P3：多线程（f/x 进程内并行）

### 动机分类

串行环境新变化。上游核心计算（channelization/XMAC）单线程，靠进程网格并行；拆分后进程数 = 1，进程内无并行手段，P2 之外仍有上限。

### 要解决的问题

单进程只能用一个核：f 侧 channelization/FFT 在大数据量下也是热点；x 侧即使 P2 子集化，单子集仍是单核。

### 预期效果

f 侧 FFT 批并行、x 侧基线循环并行，近线性加速（受内存带宽约束）；与 P2 叠加使用。

### 设计

- f 侧：OpenMP 并行 FFT 批（上游 datamuxer corner turn 已有 OpenMP 先例，线程数取 OMP_NUM_THREADS）。
- x 侧：基线循环并行化（基线间无依赖，embarrassingly parallel；注意 XMAC 工作区按线程私有化）。
- 顺序：P2 进程级先行（隔离简单、可跨节点），P3 是进程内补充，两者不互斥。

---

## P4：科学功能补齐（zoom band → 多相位中心 → 脉冲星 binning）

三项均为功能未迁移（上游有、V1 不支持），按改动量递增排序。共同格局（2026-09-13 完整审查上游后确认）：**f 侧近乎零改动**（zoom 只需 autocorr.bin 补段，多相位中心与 pulsar 完全不涉及 f）——因为 zoom 频谱是 Mode 内的父 band 切片（getMode 工厂已算好）、多相位中心与 pulsar 全是基线级处理；**x 侧为改动主体**；configuration / model / polyco / visibility 四个零改造迁移组件已把解析、结果预算（thread/coreresult 均含 bin 与相位中心因子，configuration.cpp:2347/2365/2410）、写盘循环（多 .s/.b 文件、PULSAR BIN 字段、sourceindex）全部备好。

### P4a：zoom band（✅ 2026-09-13 实施完成）

#### 是什么

观测整个宽带、只输出其中若干**窄子带的高分辨率频谱**（谱线巡天典型需求）。zoom 不是独立 FFT：zoom 窄带是 freq table 里的独立频率项，其频谱就是父 band 频谱数组的指针切片。

#### 动机分类

功能未迁移。V1 的 .sp 只落 recorded band、autocorr.bin 只落 recorded band，x 侧无 zoom 输出。

#### 要解决的问题

频谱线观测需要高分辨率窄带输出，V1 无法做谱线科学。

#### 预期效果

x 侧支持 .input ZOOM FREQ 定义、出 zoom SWIN；f 侧 autocorr.bin 补 zoom 段；无 zoom 配置行为与 V1 逐位一致（对拍回归保证）。

#### 设计

**上游数据链路全景**：

1. .input DATASTREAM 段：`NUM ZOOM FREQS` → 每组 `ZOOM FREQ INDEX j`（freq table 窄带项）+ `NUM ZOOM POLS j` → `ZOOM BAND k POL/INDEX`（INDEX 指向 local zoom freq 序号）。
2. Configuration 解析（configuration.cpp:1702-1753）：按频域包含关系自动找父 recorded band（zoomfreqparentdfreqindices），算 `zoomfreqchanneloffset = (zoom lowedge − parent lowedge)/parent_bw × parent_nchan`（:1733）。
3. Mode 构造（getMode 工厂，configuration.cpp:948 等）：传 numzoombands；zoom 频谱 = 父 band 频谱 `[channeloffset]` 起的指针（mode.cpp:184-195），zoom 自相关同理切片（mode.cpp:374-390）。`getFreqs(nrecorded+l, subloop)` 直接返回切片。
4. 基线表（configuration.cpp:1098-1145）：zoom band 按独立 band 参与基线（bandindex = nrecorded+zoom 序、polpairs 用 zoombandpols、freq 映射用 zoomfreqtableindices）。
5. XMAC（core.cpp:896-968）：`getBDataStream1BandIndex` 返回 ds 内 total band 序，getFreqs 拿到 zoom 切片，与普通 band 无差别参与 XMAC；**权重仍取 recorded band 序**（getBDataStream1RecordBandIndex → getDataWeight(父 band)），zoom 权重即父权重。
6. autocorr/acweight（core.cpp:1273-1302、1314-1339）：averageAndSendAutocorrs 遍历 total bands，getAutocorrelation 已含 zoom 切片；**zoom 的 acweight 从父 band 取**（k≥nrecorded 时按 parentfreqindex+pol 匹配 recorded band 的 getWeight，:1324-1339）。
7. pcal：copyPCalTones 只遍历 recorded bands（core.cpp:1141-1153），zoom 无独立 pcal；PCAL 文本 zoom 无 matching autos 时 acw=1.0（visibility.cpp:664，P0 已实现于 f 侧 pcaltextwriter.cpp:93）。
8. 写盘：zoom band 作为独立 freq（targetfreqindex = zoom freq）正常出 SWIN，无特殊分支。

**f/x 实现对应表**：

| 上游环节 | 上游位置 | fxcorr 落点 |
|---|---|---|
| zoom 解析（parent/offset） | configuration.cpp:1702-1753 | 零改造（已迁移） |
| Mode zoom 频谱切片 | mode.cpp:184-195 | 零改造（getMode 工厂现成） |
| zoom autocorr 落盘 | core.cpp:1273-1302 | f：FEngineWriter 写盘循环扩 numtotalbands |
| zoom acweight 父 band 映射 | core.cpp:1324-1339 | f：autocorr.bin 的 weight 段换算（x 侧只见结果） |
| XMAC 读 zoom 谱 | core.cpp:908-912 | x：SpReader 加 zoom 视图（切片） |
| uvshift/频谱平均 | core.cpp:1605+ | x：零改造（freq 表驱动） |
| addAutocorrs | core.cpp:1273-1302 | x：Integrator::addAutocorrs 扩 total bands |
| pcal | core.cpp:1141-1153 | 无改动（zoom 无 pcal） |

**关键点（易错，实施必读）**：

1. **.sp 不新增文件**：zoom 视图 = 同一父 band .sp、从 channeloffset 起读 zoomfreq 的 nchan（data-spec:250 已约定"x 侧切片"方向）。SpReader 需支持切片视图（构造加 channeloffset/nchan，或读入后返回偏移指针）。
2. **band 序号两套语义**：XMAC 的 `getBDataStream1BandIndex` 是 ds 内 total band 序（≥nrecorded 即 zoom），而 .sp 文件名 `band_XX.sp` 是 recorded band 序——SpReader 映射：band<nrecorded → 对应 .sp 全文；band≥nrecorded → 父 .sp 的 channeloffset 切片。`getBDataStream1RecordBandIndex` 恒为 recorded band 序（权重取父 .sp 的 weights），**accumulateWeights 零改动**。
3. **autocorr.bin 头 nbands 扩到 getDNumTotalBands**：现有头每 band (bandindex, nchan)，bandindex 用 ds 内 total band 序、nchan = zoomfreq 的 nchan；x 侧 addAutocorrs 的 nbands 校验从 getDNumRecordedBands 改为 getDNumTotalBands，累加循环不变（weight 已是 f 侧换算后的值）。
4. **zoom 的 acbuffer 尺寸**：addAutocorrs 的 acbuf 现按第一个 recorded band 分配，zoom 的 nchan 可能更大——改按 total bands 的最大 nchan 分配。
5. **退化保证**：无 zoom 时 numtotalbands==numrecordedbands、所有新分支不进（条件结构与上游一致），V1 对拍回归不漂移。
6. **频率映射断言**：uvshiftAndAverageBaselineFreq 的 `assert(targetfreqchannels == 0.5+bandwidthoftarget/bandwidth×freqchannels)` 对 zoom→zoom 映射天然成立，无需改动。

#### 验证方法

- 自造小配置（1 父 band 64MHz 4096ch + 1 zoom 2MHz 128ch，.input 照 ma008_1.input 的 ZOOM 段语法）生成 VDIF：mpifxcorr 基准 vs fxcorr 对拍，zoom SWIN 逐记录全等（可见度 <1e-6、weight 逐位一致）。
- autocorr.bin 头自检（nbands、bandindex、nchan）+ zoom 自相关峰值落位。
- 无 zoom 配置回归：test 配置 6/6 对拍不变。

**检验操作步骤**：可复现命令与验收判据见 `fxcorr/test/zoom/README.md`（make_testdata.sh 造数据 → `gen_test_zoom.py` 生成 test-zoom.input + mpi2 截断变体 → f/x 链路与 mpifxcorr 基准各跑一遍 → `cmp_swin_zoom.py` 按 freqindex 分拆对拍）。

#### 验证结果（2026-09-13 实施完成）

✅ 2026-09-13：zoom 对拍 **12/12 记录全等**（主带 6 + zoom 6，可见度 max rel 0.00e+00）、无 zoom 回归 6/6；zoom 的 XMAC/uvshift/accumulateWeights 零改动、f 侧 Mode 零改动。完整验证记录与可复现步骤见 `fxcorr/test/zoom/README.md`；实施中实测的两个坑已入 fxcorr-x CLAUDE.md 关键要点。

### P4b：多相位中心

#### 是什么

同一次相关**同时覆盖视场内多个天体**：所有源共用一次解包/FFT（f 侧零成本共享），基线级按各源相对指向中心的差分延迟做频域相位旋转（rotator），每源一套独立可见度（SWIN 多套 .s 文件，源序号进 SWIN 头）。

#### 动机分类

功能未迁移。V1 的 uvshiftAndAverageBaselineFreq 是删掉 rotator/decorr 段的单相位中心简化版，main.cpp 对 getNumPhaseCentres>1 直接报错。

#### 要解决的问题

in-beam 多源观测（巡天多源同波束、邻近双星）需要一次相关出多套源结果，V1 只能单源。

#### 预期效果

x 侧支持 .input 多 SOURCE 相位中心（.im 的 NUM PHASE CENTRES 驱动），一次相关出多套 .s 文件（源序号/SWIN 头 sourceindex/UVW 头按相位中心）；单源配置路径与 V1 逐位一致。

#### 设计

**上游数据链路全景**：

1. 相位中心来源：.im 文件 `NUM PHASE CENTRES` + 逐行 SOURCE index（model.cpp:507-513），指向中心单独存（pointingcentre），`getNumPhaseCentres(scan)` = 相位中心个数（不含指向中心）；scansourceindex 0 = 指向中心、1..N = 各相位中心。
2. 差分延迟（core.cpp:1636-1672，仅 numphasecentres>1 时）：指向中心延迟 = `calculateDelayInterpolator(scan, offsets[1]+(offsets[2]+nsoffset)/1e9, 1µs, 1, ant, 0, 1)`（scansourceindex=0，order=1 返 delay+rate）；每相位中心 s 同法取 scansourceindex=s+1；applieddelay = 相位中心延迟 − 指向中心延迟 + 几何 rate 修正（`applieddelay += applieddelay×指向中心rate`，:1663-1664）；differentialdelay[s][1] = applieddelay2−applieddelay1、[s][0] = rate 差。
3. rotator 生成（core.cpp:1697-1767，仅多相位中心）：chanfreqs 按 LSB/USB 排列填 rotatorlength = rotatestridelen + numstrides×rotatesperstride 个频率；applieddelay≠0 时 argument = (applieddelay×chanfreq + edgeturns) 取小数×2π（edgeturns = applieddelay×lofrequency 取小数，仅第一段带），vectorSinCos + RealToComplex 成 rotator 复向量。
4. 应用（core.cpp:1770-1815）：每相位中心 s、每 xmac stride、每 polpair：srcpointer（threadcrosscorrs 或 pulsaraccumspace）先 `vectorMul(rotator段, 每 rotatestridelen 分块)` + `vectorMulC(rotator标量段)` 进 rotated，srcpointer = rotated；后续频谱平均/累积与单相位中心同（:1817-1870）。
5. 结果区步进（core.cpp:1878）：每相位中心 coreindex += corebinloop×numpolproducts×targetfreqchannels/targetchannelinc（V1 版跳过此步进）。
6. decorr（core.cpp:1886-1923）：timesmeardecorr = sin(maxphasechange/2)/(maxphasechange/2)（maxphasechange = 2π×differentialdelay[s][0]×(nswidth/1000)×lofrequency）；delaydecorr = 1−|differentialdelay[s][1]|/delaywindow（delaywindow = nchan/bandwidth）；baselineshiftdecorr[freq][baseline][s] += nswidth×timesmeardecorr×delaydecorr（负值置 0 + shifterrorcount 告警）。
7. floatresults 写段：bweight 段之后追加 shiftdecorr 段（getCoreResultBShiftDecorrOffset，core.cpp:1090-1105）。
8. 写盘（visibility.cpp:840-888，零改造）：每相位中心 s → bin → pol 循环，weight = baselineweights×baselineshiftdecorrs[s]（>1 相位中心时），UVW = interpolateUVW(scan, t, ant1, ant2, **s+1**)，文件 .s%04d.b%04d、sourceindex = getPhaseCentreSourceIndex(scan, s)；自相关段多相位中心时 sourceindex 用指向中心（:896-898）。
9. 预算：coreresultblocksize 已含 maxconfigphasecentres 因子（configuration.cpp:2410，零改造；subintresults 分配 getCoreResultLength 已含，无需改）。

**f/x 实现对应表**：

| 上游环节 | 上游位置 | fxcorr 落点 |
|---|---|---|
| 相位中心解析 | model.cpp:507-513 | 零改造（config.getModel() 已用） |
| 差分延迟计算 | core.cpp:1636-1672 | x：XmacEngine::uvshiftAndAverageBaselineFreq（构造加 Model*） |
| rotator 生成 | core.cpp:1697-1767 | x：同上（工作区加 chanfreqs/rotator/rotated/argument） |
| 旋转+频谱平均+累积 | core.cpp:1770-1878 | x：同上 |
| decorr 累加 | core.cpp:1886-1923 | x：同上（baselineshiftdecorr 数组） |
| shiftdecorr 写段 | core.cpp:1090-1105 | x：copyBaselineWeights 尾部 |
| 写盘 | visibility.cpp:840-888 | 零改造（writedata 自动多 .s 文件） |
| 结果预算 | configuration.cpp:2410 | 零改造 |

**关键点（易错，实施必读）**：

1. **单相位中心退化必须逐位一致**：上游的 delay 计算/rotator 生成/decorr 段全部由 `getNumPhaseCentres>1` 条件包裹（core.cpp:1636/1699/1887），单源路径与 V1 现实现完全相同——新代码照同一条件结构，对拍回归 6/6 保证不漂移。
2. **工作区尺寸**（照 core.cpp:421-434 scratchspace 分配）：chanfreqs f64×maxrotatestrideplussteplength（= rotatestridelen + maxchan/rotatestridelen，取全配置最大）、rotator cf32 同长、argument f32×3 同长、rotated cf32×maxchan；differentialdelay 临时表（3×numphasecentres×2 double）。
3. **时间基准**：calculateDelayInterpolator 的 offsettime = offsets[1] + (offsets[2]+nsoffset)/1e9，scan 相对系——fxcorr-x 每 subint 已算 expectedsec/expectedns（main.cpp），直接传入；勿用当日秒系（那是 polyco 的系）。
4. **rotator 分段**：rotatestridelen 内带 edgeturns、外不带；先段向量 vectorMul 再标量 vectorMulC，顺序照抄 core.cpp:1800-1804。
5. **scansourceindex 语义**：0 = 指向中心、s+1 = 第 s 个相位中心（延迟与 UVW 两处一致，visibility.cpp:857）。
6. **decorr 段结果区布局**：floatresults 的 bshiftdecorr 段在 bweight 段之后、每 (freq,baseline) 连续 numphasecentres 个 f32——offset 体系照 getCoreResultBShiftDecorrOffset，勿手算。
7. **x 侧无锁**：上游 viscopylocks 是线程并行产物，fxcorr-x 单线程直接省略（V1 已如此）。

#### 验证方法

- 自造多源 .input/.im（2-3 相位中心、.im 的 NUM PHASE CENTRES + SOURCE 列表）：mpifxcorr 基准 vs fxcorr 对拍，每相位中心 .s0000/.s0001… 逐记录全等（含 sourceindex、decorr 后 weight、UVW 头）。
- 单源回归：test 配置 6/6 对拍不变（退化路径）。

✅ 2026-09-13：多源对拍 **8/8 记录全等**（.s0000 6/6 + .s0001 2/2，自相关按上游语义只写指向中心源文件）、源间差异确认（src/uvw/可见度 rel 1.5e-02/weight decorr 均不同，rotator 非平凡）、单源回归 6/6。检验资产（test-mpc.v2d 走 vex2difx 原生 addPhaseCentre 链路）与可复现步骤见 `fxcorr/test/mpc/README.md`；.im 的 SRC 索引语义（SRC 0 = 指向中心、SRC 1..N = 相位中心，PHS CTR 0 可能即指向中心）见该 README。

### P4c：脉冲星 binning

#### 是什么

按**脉冲相位把每个 FFT 块的互相关分到 N 个 bin 分别累积**：polyco 多项式预言到达时间 → 每 FFT 块算脉冲相位 → bin 号 → XMAC 结果分箱累积 → 每 bin 一套可见度（SWIN .b 文件）。可选 SCRUNCH OUTPUT：先分 bin 累加、uvshift 前按 bin 权重折叠回单 bin（剥离缓变 RFI）。

#### 动机分类

功能未迁移。V1 的 xmac.cpp 只有 non-pulsar 分支，main.cpp 对 pulsarBinOn 直接报错。

#### 要解决的问题

脉冲星门控/计时观测需要按脉冲相位分箱，V1 无法做。

#### 预期效果

x 侧支持 .input PULSAR BINNING + pulsar 配置文件，出 .b 文件（SCRUNCH 两种模式都支持）；无 pulsar 配置路径与 V1 逐位一致。

#### 设计

**上游数据链路全景**：

1. 触发与解析：.input `PULSAR BINNING TRUE` → `PULSAR CONFIG FILE`（pulsar 配置：NUM POLYCO FILES/POLYCO FILE n/NUM PULSAR BINS/SCRUNCH OUTPUT/BIN PHASE END n/BIN WEIGHT n；每 bin 结束相位 0..1 递增）。Configuration::processPulsarConfig（configuration.cpp:3382+，已迁移）经 mpiGetFileContent（非 MPI = fopen 读全文件，configuration.cpp:107）读 polyco 文件、构造 configs[c].polycos 数组。
2. currentpolyco 选择（core.cpp:496-507）：sec = startseconds + getScanStartSec(scan, startmjd, startseconds) + offsets[1] + offsets[2]/1e9（**当日秒系**）；`Polyco::getCurrentPolyco(configindex, startmjd, sec/86400, polycos, numpolycos, false)` + setTime；NULL 时 cfatal（配置校验阶段 configuration.cpp:3186 也会提前拦）。
3. bins 计算（core.cpp:803-812）：每 fftloop 每 fftsubloop：i = fftloop×numbufferedffts + fftsubloop；offsetmins = i×blockns/6e10；`currentpolyco->getBins(offsetmins, bins[fftsubloop])` 填每通道 bin 号（bins[fftsubloop][f][chan]）。
4. XMAC pulsar 分支（core.cpp:914-957）：vectorMul_cf32 进 pulsarscratchspace；非 scrunch：按 destbin = bins[fftsubloop][f][destchan] 累积 threadcrosscorrs（cindex = resultindex + (destbin×polpairs+p)×xmacstridelength + l），baselineweight[f][destbin][j][p] += w1×w2/**freqchannels**；scrunch：累积 pulsaraccumspace[f][x][j][0][p][destbin][l]，baselineweight[f][0][j][p] += bweight×binweights[destbin]（**仅 binweights>0 的 bin**，:938-939）。
5. resultindex 步进（core.cpp:974-975）：pulsar 非 scrunch 时 += polpairs×numpulsarbins×xmacstridelength（与 populateResultLengths 的 thread 预算一致）。
6. scrunch 折叠（core.cpp:1438-1475）：uvshiftAndAverage 开头，pulsaraccumspace 每 (freq,stride,base,pol,bin) 就地乘 binweights[k]（vectorMulC_f32_I）。
7. uvshift bin 展开（core.cpp:1626-1631、1781-1789）：threadbinloop = numpulsarbins；corebinloop = numpulsarbins（scrunch 时 1）；coreoffset = ((b×polpairs+k)×targetfreqchannels + x×xmacstridelength)/targetchannelinc；srcpointer = threadcrosscorrs（或 scrunch 折叠后的 pulsaraccumspace）。
8. bweight 段（core.cpp:1080-1087）：binloop 循环写 floatresults；pulsar 时 accumulateWeights 的非 pulsar 分支（:1005-1052）整体跳过（权重已在 XMAC 内更新）。
9. 写盘（visibility.cpp:1369+、851-886，零改造）：Visibility 构造时 pulsar 初始化（getCurrentPolyco 按积分起点当日秒系、binweightdivisor[0]=Σ getBinWeightTimesWidth 或每 bin 一个 = getBinWeightTimesWidth×fftsperintegration）；writedata binloop=numbins，baselineweights 归一 /(fftsperintegration×getBinWidth(b))（binloop>1）或 /(fftsperintegration×binweightdivisor[0])（scrunch）；SWIN 头 PULSAR BIN 字段、文件 .b%04d。
10. 预算：threadresult（configuration.cpp:2347/2365）与 coreresult（:2410）均含 bin 因子，零改造。

**f/x 实现对应表**：

| 上游环节 | 上游位置 | fxcorr 落点 |
|---|---|---|
| pulsar 配置解析（polyco 装载） | configuration.cpp:3382+ | 零改造（已迁移，非 MPI 读文件） |
| currentpolyco 选择/setTime | core.cpp:496-507 | x：main 每 subint 起点 |
| bins 计算 | core.cpp:803-812 | x：XmacEngine::xmacBatch 前（每 fftloop） |
| XMAC pulsar/scrunch 分支 | core.cpp:914-957 | x：XmacEngine::xmacBatch |
| scrunch 折叠 | core.cpp:1438-1475 | x：uvshiftAndAverage 开头 |
| uvshift bin 展开 | core.cpp:1626-1631/1781-1789 | x：uvshiftAndAverageBaselineFreq |
| bweight bin 循环 | core.cpp:1080-1087 | x：copyBaselineWeights |
| 写盘/预算 | visibility.cpp:1369+/851-886 | 零改造 |

**关键点（易错，实施必读）**：

1. **两套时间系勿混**：polyco 的 setTime/getBins 用**当日秒系**（core.cpp:497 的 sec 含 job 起点 + scan 偏移），bins 的 offsetmins 是 subint 内 FFT 序号×blockns/6e10（i 从 0 起，fxcorr-x 无 startblock）；rotator/delay 用 scan 相对系（见 P4b）——同一次 uvshift 内两种系并存，照上游各自取数。
2. **bweight 的 freqchannels 因子**：pulsar 分支 bweight = w1×w2/freqchannels（core.cpp:924/945），non-pulsar 分支不除（:1042）——fxcorr-x 现实现的 accumulateWeights 是 non-pulsar 版，pulsar 时改为 XMAC 内更新、跳过 accumulateWeights。
3. **scrunch 负权重语义**：负 binweight 的 bin 参与数据折叠但不进 baselineweight（剥离缓变信号），照抄 :938-939 的 `if(binweights[destbin] > 0.0)`。
4. **单 polyco 假设**：pulsaraccumspace 源槽恒 0（"forced to single pulsar ephemeris"，:928/1458/1584）；getCurrentPolyco 返回子 polyco 之一、多子 polyco 跨时段自动切换（按时刻匹配）。
5. **Visibility 的 polyco 时刻**：visibility.cpp:1369 按积分起点（expermjd + experseconds + scan 偏移 + currentstartseconds）选 polyco——x 侧 Visibility 构造的 initsec/initns 已含 batch 偏移，零改造正确。
6. **文件路径**：pulsar config 与 polyco 文件名按 cwd 相对解析（非 MPI fopen），run_batch.sh 的 cwd=workdir 与 .input DATA TABLE 同语义；.input 里 PULSAR CONFIG FILE 也是相对路径。
7. **scrunch 折叠时序**：uvshiftAndAverage 开头对 accumspace 就地乘 binweights，之后 corebinloop=1 走单 bin 路径；bins 计算照跑（getBins 无条件调用）。
8. **退化保证**：pulsarBinOn=false 时新分支全不进（条件结构照上游），V1 对拍回归不漂移。

#### 验证方法

- 自造小配置：PULSAR BINNING TRUE + 2-4 bins + 覆盖观测时段的短 polyco（自造 .polyco）+ SCRUNCH OUTPUT 两种取值：mpifxcorr 基准 vs fxcorr 对拍，各 .b 文件逐记录全等（含 PULSAR BIN 头字段、weight 归一）。
- 无 pulsar 回归：test 配置 6/6 对拍不变。

✅ 2026-09-13：非 scrunch 对拍 **14/14 记录全等**（.b0000 6/6 + .b0001-.b0003 各 2/2）、scrunch 对拍 6/6、binning 生效确认（pbin/weight/可见度各 bin 均不同）、无 pulsar 回归 6/6。检验资产（gen_test_pulsar.py：.input 变体 + pulsar config + 自造 tempo polyco）与可复现步骤见 `fxcorr/test/pulsar/README.md`；实施中实测的坑（scrunch 的 accumspace 须在 uvshiftAndAverage 尾部清零，否则可见度按积分序放大）已入 fxcorr-x CLAUDE.md 关键要点。

---

## P5：网络输入 / 数据流化

### 动机分类

串行环境新变化（新能力）。V1 的 datareader 只读本地 raw/ 文件；真实观测数据从采集机经网络到达，采集与处理同时进行。

### 要解决的问题

流式场景（边采边处理）无法用 V1 的"先落盘再处理"模式；scalebox 流式处理需要网络输入。

### 预期效果

f（与 fxcorr-sim）支持网络流输入（VDIF over UDP），固定时长 batch 边到边处理，batch 边界动态化。

### 设计

- datareader 加网络源抽象（上游 vdifmux 有网络输入先例，可参考其 socket 层）。
- 丢包统计与重传策略；batch 收齐校验后触发处理。
- 排最后：依赖真实采集环境联调，属新能力而非迁移补齐。

---

## 相关

- 优先级与验收总览：v2-plan.md 第 5/6 节
- 数据接口变更（涉及 data-spec.md 的项）：P0（补 PCAL 文本格式说明，不改二进制格式）、P2（子集目录与合并约定，12 节）
