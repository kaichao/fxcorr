# fxcorr V2 算法改进需求与设计

V1 按最小科学闭环切分（f：解包/模型/通道化落盘，x：XMAC/积分出 SWIN），上游 mpifxcorr 的部分功能停在半路，且去 MPI 单进程后上游"进程网格并行"带来的算力不复存在。本文定义各项改进的**动机分类、要解决的问题、预期效果、设计要点**与优先级。定稿 2026-09-13，随实施更新。

## 动机分类（贯穿全文的两类原因）

- **功能未迁移**：上游 mpifxcorr 有该功能，V1 按边界裁剪未迁移（pcal 只落 pcal.bin 不出文本；zoom / 多相位中心 / 脉冲星 binning 上游有、V1 不支持；difxmessage 上游有、拆分后无进程发送）。
- **串行环境新变化**：去 MPI 后单进程串行，上游靠 D×B 进程网格取得的并行性消失，需要新的并行手段（进程内多线程，P3；多 x 子集方案 P2 已取消——并行维度改为时间片 batch，见 P2 节）；容器 / scalebox 编排环境引入新约束与新机会（组播受限、网络流输入）。

## 优先级

排序依据（2026-09-13 调整；2026-09-15 V3 定案同步）：**V2 只做 mpifxcorr 串行算法的迁移与完善——先补齐数据链路完整性（P0/P1，成本低、科学产出与可观测性受损），再科学功能（P4、P6-P8 按改动量递增），再监控消息（P9）、输入格式（P10）、reader 细节（P11）；并行化（P2/P3）与流式新能力（P5）挪 V3（README：模块级 OpenMP / 按需 GPU，并行与流式阶段）**。V3 定案（2026-09-15）：P2 取消（时间片 batch 并行取代，见 P2 节）、P3 实施（V3 唯一实现项）、P5 不做（留 scalebox 阶段）。

| 优先级 | 改进项 | 动机分类 |
|---|---|---|
| P0 | PCAL_*.pcal 文件生成 | 功能未迁移 |
| P1 | difxmessage 状态/STA 消息 | 功能未迁移 + 环境变化 |
| P2 | 多 x 子集并行 | 串行环境新变化（✂ 2026-09-15 取消：时间片 batch 并行取代，见 P2 节） |
| P3 | 多线程（f/x 进程内并行） | 串行环境新变化（⤴ V3，2026-09-13 挪出） |
| P4 | zoom band → 多相位中心 → 脉冲星 binning | 功能未迁移 | ✅ 2026-09-13（P4a zoom 对拍 12/12、P4b 多源对拍 8/8、P4c 非 scrunch 14/14 + scrunch 6/6，各无开关回归 6/6；详见 P4 节） |
| P5 | 网络输入 / 数据流化 | 串行环境新变化（新能力，⤴ V3，2026-09-13 挪出） |
| P6 | SwitchedPower（TCAL 噪声功率） | 功能未迁移 | ✅ 2026-09-13（f 侧 SwitchedPower 类 + SWITCHEDPOWER_* 落盘；前 2 个完整整秒窗与 mpifxcorr 逐位全等、SWIN 回归 6/6、无 tcal 回归 6/6；详见 P6 节） |
| P7 | 交叉极化自相关（WRITE AUTOCORRS） | 功能未迁移 | ✅ 2026-09-13（autocorr.bin v2 + crosspol 段全链路；单 pol + WRITE AUTOCORRS 对拍 6/6、无开关回归 2/2、位序 BYTE-IDENTICAL；详见 P7 节） |
| P8 | 相位阵频率域加权合并 | 功能未迁移 | ✅ 2026-09-13（x 侧 BeamEngine 波束加权求和 + beam.bin 落盘（上游输出端死代码、格式自定）；两种权重逐位 PASS、回归 6/6；详见 P8 节） |
| P9 | Kurtosis STA + STA 频域平均分支 | 功能未迁移 | ✅ 2026-09-13（f 侧 FXCORR_KURTOSIS=1 触发 + STA 平均分支；CHANS TO AVG 1/4 两轮 STA+kurtosis 对拍 318 条逐位全等、无开关回归 6/6；顺手修 P1 遗留 stride bug） |
| P10 | 输入格式补齐（Mark5B/LBA/其余 + 多线程 VDIF corner-turn） | 功能未迁移 | ✅ 2026-09-14（五路径 reader；Mark5B/多线程 VDIF 对拍 6/6、LBA 自洽验证 PASS、五格式审查、VDIF 回归 6/6；详见 P10 节实施记录） |
| P11 | f 侧 reader 语义补全（valid flag 跨段续接、延迟中途重对齐） | 语义等价 | ✅ 2026-09-14（locate 加 delay 重对齐（跳块/tosubtract 含上游 quirk/整数 ns 对齐）+ fillValidFlags count 语义；delay≠0 对拍 6/6 全等、cmp5 回归 6/6；详见 P11 节实施记录） |
| P12 | 真实观测的病态数据（文件起点偏移、记录中断、filler 帧） | 串行环境新变化 | ⚠ 2026-09-16~18 部分完成（A 起点 / B 缺口 / C filler 三类共 13 条已修并验证；**B5「缺口跨 subint 边界」未修**，见 reader-model.md 4.5；详见 P12 节） |

P6-P11 为 2026-09-13 mpifxcorr 完整审查新发现项，均已补节细化并实施（P6-P10 2026-09-13，P11 2026-09-14）。**P12 的来源不同——它是 2026-09-16 起由首个真实观测 t25362 暴露的读取路径缺陷，不是上游代码审查的发现项**（P0-P11 的全部对拍都用 fxcorr-sim 的理想数据，病态形态从未被覆盖）。已确认上游死代码、不迁移：FILTERBANK USED/PROCESSING METHOD、dumplta/ltachannels、checkData（`#if 0`）、相位阵 TIMESERIES 输出；硬件访问（StreamStor/Mark6）不迁移。

对拍原则：每项尽量与 mpifxcorr 基准对拍（run_bench.sh 产出），P0 文本 diff、P4 各功能 SWIN 对拍、P6/P8 SWIN 对拍、P3（V3 实现时）与单线程全量结果全等。

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

## P2：多 x 子集并行（✂ 2026-09-15 取消）

### 动机分类

串行环境新变化。上游 XMAC 由 B 基线进程网格并行（每个 baseline 进程串行做若干基线）；拆分后单进程 fxcorr-x 串行全部基线。XMAC 是算力热点，大站网/高通道数下单核处理不完。

### 取消结论（V3 讨论定案，2026-09-15）

原方案（基线集合切 N 子集、N 个 fxcorr-x 进程并行、各子集写独立子目录后合并）**取消**。定案理由：

- **并行维度改为时间**："时间片"就是 batch——batch 即并行/调度单元，多 batch 并发由 scalebox 编排承担（编排外置，不在本项目）；空间维度切分（站组/基线子集）不再需要。
- **同实验 batch 串行（路线 B）**：同一实验的 batch 按时间序串行处理，实验级共享文件（PCAL_* 文本读改写、SWITCHEDPOWER_* 追加、SWIN 跨 batch 追加）不存在并发写点，无需子集目录与合并逻辑（data-spec 12 节）。
- **进程内并行交给 P3**：单进程算力不足由模块级 OpenMP 解决（P3 节），不靠多进程切分。

### 遗留记录

原设计（基线切分、独立子目录、合并、baselines 切分表元数据、run_batch.sh wait 收口）作废，不实现。若未来 scalebox 编排层遇到"同实验多 batch 并行追赶积压"场景，实验级文件的并发语义（PCAL 读改写竞态、SWITCHEDPOWER 行序、SWIN 时间序）届时再评估。

---

## P3：多线程（f/x 进程内并行）——V3 唯一实现项

### 动机分类

串行环境新变化。上游核心计算（channelization/XMAC）单线程，靠进程网格并行；拆分后进程数 = 1，进程内无并行手段。

### 要解决的问题

单进程只能用一个核：f 侧 channelization/FFT 在大数据量下也是热点；x 侧 XMAC 串行全部基线。

### 预期效果

f 侧 FFT 批并行、x 侧基线循环并行，近线性加速（受内存带宽约束）。P2 取消后（见 P2 节），P3 是本项目唯一的并行实现项；batch 间并行由 scalebox 编排承担（多 batch 并发）。

### 设计（2026-09-15 定稿，实测热点驱动）

性能剖析（fxcorr/test/p3/README.md，4 站 61s batch，测试机 8 核）：f 侧热点 = 条纹旋转（apply/apply_dit）> FFT > 写盘 > unpack；x 侧 = 读 .sp fread 与 XMAC 大致对半（4MHz/4096 配置，nChan 放大后 XMAC 占 ~90%）。基线耗时 f 7.8s/站、x 7.2s。

**f 侧：Mode 副本 + 连续段分块 + 块序归约**（fxcorrcommon 零改动）：

- 每 fftloop 的块循环（≤ numbufferedffts 块）按连续段分给 T 个线程，每线程一个 Mode 副本（同 config/datastream 构造参数，工作缓冲独立分配）。每块 process(i, b) 的输入独立（unpackedarrays 槽 + interpolator 只读 + FFTW plan 并发 new-array execute 线程安全）、输出 fftoutputs[j][subloop] 槽独立 → 并行计算结果与串行逐位一致。
- 归约按块序：副本 fftoutputs 槽拷回主 Mode（writeSpectra 零改动）+ autocorrelations/weights/pcalresults/kurtosis 累积按块序累加进主 Mode。连续段分块保证全局累积序列 = 串行块序 → 结果逐位一致（浮点加法不可交换，块序必须保）。
- 旁路（writeSpectra / pcaltext 累积 / STA / autocorr 批次 / zeroAutocorrelations）全部串行作用于归约后的主 Mode，节奏与 V2 一致。

**x 侧：基线循环并行 + scratch 线程私有化**：

- xmacBatch 基线循环（xmac.cpp:288）、uvshiftAndAverage 的 freq×baseline 循环（xmac.cpp:469-476）、accumulateWeights 基线循环并行化；基线间写区独立（threadcrosscorrs 按 getThreadResultBaselineOffset 布局、baselineweight/subintresults 独立条目），无数据竞态。
- 共享 scratch（conjbuf / pulsarscratchspace / rotator）改为按线程数的多副本，并行区内按 omp_get_thread_num() 取用；每基线运算序列与串行一致 → 逐位一致。
- 结果偏移：基线循环内以 per-(f, x) 预计算的基线偏移表替代顺序累加 resultindex（threadcrosscorrs 布局不变，populateResultLengths 语义照旧）。

**线程数与默认行为**：线程数取 OMP_NUM_THREADS，未设 = 1（启动时 omp_set_num_threads(1)）——默认串行，V2 回归不破对拍。构建：configure.ac 加 AC_OPENMP、src/Makefile.am 用 OPENMP_CXXFLAGS。

**线程安全前置（审计结论）**：Configuration/Model 解析后只读共享 ✓；FFTW new-array execute 线程安全、plan 创建在构造期串行 ✓；Mode/XmacEngine 的写状态（工作缓冲/累积数组）以上述副本/私有化方案隔离，不改 fxcorrcommon 的 Mode 内部结构。

**验证**：OMP_NUM_THREADS=N 与未设（串行）SWIN cmp_swin.py 全等（f/x 两侧）+ 加速比测试（8 站 nChan 32768 放大场景，fxcorr/test/p3/）。

### 实施记录（2026-09-15）

**x 侧**：xmacBatch 重构为 used (freq, xmac-pass) 对预收集（baseoffset 照原 resultindex 步进重放，含 localfreqindex<0 不占位）→ `#pragma omp parallel for` 基线循环；uvshiftAndAverage 主循环 collapse(2)；accumulateWeights 基线循环最外层（循环交换后每基线累加序列不变）；conjbuf/pulsarscratchspace/chanfreqs/rotator/rotated/argument 改 [nthreads] 副本（omp_get_thread_num 取用，串行时索引 0）；shifterrorcount atomic；main 开头 OMP_NUM_THREADS 未设 = omp_set_num_threads(1)。对拍：串行回归 580/580 全等（4 站 61s batch）、OMP_NUM_THREADS=4 与串行 580/580 逐位全等；4 线程 1.76×、8 线程 2.09×（4MHz/4096 配置读盘主导，接近 Amdahl 上限）。

**f 侧**：Mode 副本 + 每 subint 一个并行区（连续段分块 process，主线程 t==0 块序归约，worker 各拷各段槽、副本清零与主线程写盘并行）。fxcorrcommon 仅 mode.h 加 7 个归约接口（getFreqsWrite/setDataWeight/addWeight/addPcal/getKurtosisProducts1/2/addKurtosisProducts），Mode::process 零改动。踩坑：① 初版每 fftloop 一个并行区，fork/join 开销吃光收益（gdb 采样 gomp_barrier 主导、user 6×串行）——改每 subint 一个并行区 + barrier 协调；② AC_OPENMP([CXX]) 探测的是 C 编译器（输出 "gcc option"）——须 AC_LANG_PUSH([C++]) 包裹；③ SWIN 追加语义使重跑对拍必须清 vis 目录（3 次运行 = 3×580 条）。对拍：串行全链路 580/580 全等、OMP_NUM_THREADS=4 全链路 580/580 逐位全等。4MHz/4096 配置加速有限（写盘 ~40% 串行，Amdahl 上限低），加速比以放大场景为准（fxcorr/test/p3/README.md）。

**放大场景加速比**（8 站 28 基线 + nChan 32768、61s batch，SWIN 2088 条；对拍：4/8 线程全链路 2088/2088 逐位全等）：

| 侧 | 串行 | OMP 4 | OMP 8 | 受限因素 |
|---|---|---|---|---|
| f（每站） | 8.8s | 4.1s（2.15×） | 4.0s（2.2×，见顶） | 读 122MB + 写 2GB .sp 串行 I/O 主导 |
| x | 30.0s | 13.3s（2.26×） | 10.3s（2.92×） | 读 7.8GB .sp 串行 fread + uvshift 串行段；user 37.8s 含 spin 浪费（active 默认；OMP_WAIT_POLICY=passive 降 user 到 30.7 但 wall 11.0 不改善——futex 唤醒开销抵消） |

结论：正确性全达标（默认串行回归 + 多线程逐位全等）；加速比受串行 I/O 限制（f ~2.2×、x ~2.9×），并行计算部分本身的提速由 user 时间印证（f 4 线程 user 14.4 ≈ 2×7.4 计算并行 + spin）。进一步提速需异步写盘/预读（超出 P3 范围，留待有真实观测数据量需求时评估）。

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

## P5：网络输入 / 数据流化（✂ 2026-09-15 V3 定案：不做）

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
- **定案（2026-09-15）**：不做，留到 scalebox 阶段——真实采集环境联调依赖编排层，届时再评估（上游 vdifnetwork.cpp 现成实现可参照）。

---

## P6：SwitchedPower（TCAL 噪声功率）

### 是什么

按**整秒窗口**统计每站原始 2bit 数据的**高电平状态比例**，按 TCAL 开关频率分 on/off 两组相位，输出噪声二极管功率曲线文本 `SWITCHEDPOWER_<mjd>_<sec>_<dsid>`（每行 mjd0 mjd1 + 每通道 powerOn sigmaOn powerOff sigmaOff），用于 Tsys/增益校准。

### 动机分类

功能未迁移。上游 DataStream 节点读原始数据时同步统计（SwitchedPower::feed，switchedpower.cpp），V1 拆分后无人统计，链路停在半路。

### 要解决的问题

TCAL 观测（噪声二极管按频率切换，如 VLBA 80Hz）的功率曲线缺失，Tsys 校准做不了。

### 预期效果

f 侧支持 .input DATASTREAM 段的 `TCAL FREQUENCY`（Hz，>0 启用），出 SWITCHEDPOWER 文本与 mpifxcorr 逐行一致；TCAL FREQUENCY=0（默认）路径零改动。

### 设计

**上游数据链路全景**：

1. 触发：.input DATASTREAM 段 `TCAL FREQUENCY`（可选行，Hz；v2d antenna 级 `tcalFreq` 经 vex2difx 生成）。Configuration 解析已迁移（configuration.cpp:1575-1579，fxcorrcommon 零改造），0 = 关闭。
2. 构造（vdiffile.cpp:50-58）：spf>0 时每 datastream 一个 SwitchedPower(conf, mpiid)，datastreamId = mpiid-1，filepath = OUTPUT FILENAME，frequency = spf。
3. 喂入（vdiffile.cpp:945-964）：diskToMemory 每读一段（readbytes = databufferfactor×maxbytes/numdatasegments，帧对齐；datarate<512Mbps 时 switchedpowerincrement=1）→ 构造 mark5_stream_memory(段字节) + new_mark5_format_generic_from_string(formatname) → mark5_stream_fix_mjd → feed。formatname 由 genMk5FormatName 生成（vdiffile.cpp:568；nthreads=1 时 vdifmux outputFrameSize=输入帧大小）。
4. 统计（switchedpower.cpp:191-276）：mark5_stream_get_frame_time 取块起点时间；phase 从起点秒内 ns×frequency×2e-9 起编号（phase%2==0 为 on），每半周期 mark5_stream_count_high_states（format_vdif 的 lookup 表统计 2bit 高态数），块尾不满半周期处 break。
5. 窗口（interval=1s 硬编码，init 赋值、无 .input 键）：feed 顶部（sec/interval 变化）与相位循环内（整秒边界）两处 flush 前窗口再开新窗口；close 时 flush 尾窗。
6. 输出（flush，switchedpower.cpp:124-187）：一行 = mjd0（precision 14）mjd1 + 每通道 powerOn sigmaOn powerOff sigmaOff（precision 8）；power = high_state_fraction_to_power(f)（mark5access 2bit 功率换算），sigma = binomial df ± 换算；nOn/nOff 不足 0.5 个半周期输出 0 0 0 0。文件名 `SWITCHEDPOWER_%05d_%06d_%d`（0-based dsid），ios::app。

**fxcorr 落点表**：

| 上游环节 | 上游位置 | fxcorr 落点 |
|---|---|---|
| TCAL FREQUENCY 解析 | configuration.cpp:1575-1579 | 零改造（已迁移） |
| SwitchedPower 类 | switchedpower.{h,cpp} | fxcorrcommon（去 MPI：构造 (conf, dsindex)，datastreamId = mpiid-1 = dsindex；mark5access 依赖已就位） |
| formatname 生成 | vdiffile.cpp:568（genMk5FormatName） | fxcorrcommon：SwitchedPower 构造时生成（format, nrecordedbands, bw, nbits, sampling, getFrameBytes, decimation, alignment, nthreads） |
| 段字节喂入 | vdiffile.cpp:945-964 | f：main 每 subint 读入后按 readbytes 块切分喂 feed（块缓冲累计） |
| mark5_stream 构造 | vdiffile.cpp:954-961 | fxcorrcommon：feed(u8*, nbytes) 内部构造 memory stream（f 零 mark5access 依赖） |
| 输出路径 | switchedpower.cpp:59 | f：OUTPUT FILENAME 目录（P0 PcalTextWriter 同模式 mkdir -p） |

**关键点（易错，实施必读）**：

1. **喂入节奏是逐位一致的前提**：上游每 readbytes = (databufferfactor/numdatasegments)×maxbytes 段喂一次（test 配置 256/64 = 4×maxbytes ≈ 4 subint 量，帧对齐），V1 subint 读入量是 sendbytes（1×）——**必须独立切块、每满一块喂一次，不能按 subint 喂**。feed 内 phase 从块起点帧头时间算起，半周期统计分组由切分点决定，切分点不同则 nOn/nOff 计数不同、sigma 必炸。
2. **块起点时间从帧头读**（mark5_stream_get_frame_time），非外部传入——字节连续 + 块起点帧对齐即与上游同源；首块 = batch 起点帧（粗延迟修正后，与上游 bufferindex 同起点）。
3. **尾部**：batch 尾残余块（< readbytes）照上游尾段一样喂一次 short buffer；close() 由 SwitchedPower 析构 flush 尾窗。
4. **interval 恒 1s**、frequency=0 时整个类不创建；V1 格式仍限 VDIF/VDIFL（datareader 已拒其他）。
5. **对拍窗口**：fxcorr-f 跑 batch（2.097s）vs mpifxcorr EXECUTE TIME=2 截断 → 时间窗不同；逐行 diff 只比较共同覆盖的整秒行，fxcorr 多出的尾窗行单独验证（时间戳/数值合理性）。
6. **输出同目录冲突**：SWITCHEDPOWER 与 PCAL 一样走 OUTPUT FILENAME 目录且 ios::app——对拍两侧错开目录（mpifxcorr sed OUTPUT FILENAME 到 bench/，同 P0 对拍做法）。

### 验证方法

- test.v2d antenna 段加 `tcalFreq=80` → vex2difx 出 TCAL FREQUENCY 行 → mpifxcorr 基准（EXECUTE TIME=2 截断）与 fxcorr-f 两站对拍：各站 SWITCHEDPOWER_* 文本逐行 diff（共同时间窗），power/sigma 全等（文本 diff，同 P0 判据）。
- 无 tcal 回归：test 配置（无 TCAL FREQUENCY）SWIN 6/6 对拍不变。

✅ 2026-09-13：switched power 对拍**前 2 个完整整秒窗逐位全等**（两站 ds0/ds1 文本 diff 无差异；尾窗按各自数据终点，mpifxcorr vdifmux 多读 12ms）、SWIN 回归 6/6、位序对拍 BYTE-IDENTICAL、无 tcal 回归 6/6。检验资产（test-tcal.v2d + README）见 `fxcorr/test/tcal/`。实施中实测的坑：① subint 读入的帧对齐 guard 重叠必须剔除（否则块内帧号不连续、mark5access validate 全 fail）；② 测试数据生成器（gen_test_vdif.py/fxcorr-sim）帧头两个 bug——legacy 位误置、字布局用 VDIF 官方 spec 而非 vdifio/mark5access 布局（主路径 vdifmux 不查这些字段故从未暴露），已修复并回归。

---

## P7：交叉极化自相关（WRITE AUTOCORRS / maxproducts>2）

### 是什么

dual-pol 观测（如 RCP+LCP 同频率）时，除平行自相关（RR/LL）外还输出**交叉极化自相关**（RL/LR）：同一 FFT 块内 R 频谱 × conj(L 频谱) 与 L × conj(R) 的累加谱，与平行自相关一起进 SWIN 文件（自相关伪基线 `257*(telescope_index+1)`，polpair 记录 RR/LL/RL/LR）。

### 动机分类

功能未迁移。上游 Mode 已随 fxcorrcommon 迁移时**自带 crosspol 计算**（getMode 工厂把 `conf.writeautocorrs` 直接作为 calccrosspolautocorrs 传入，计算/频率平均/清零全部按 autocorrwidth 循环，零改造），但 f 的 autocorr.bin 只落平行段、x 的 V1 检查直接拒绝 maxproducts>2——链路断在 f/x 两侧的文件接口上。

### 要解决的问题

偏振校准需要的交叉极化自相关缺失，dual-pol 实验无法处理。

### 预期效果

.input CONFIG 段 `WRITE AUTOCORRS TRUE` 且 dual-pol（maxproducts>2）时，fxcorr 全链路 SWIN 含平行 + 交叉自相关记录，与 mpifxcorr 逐记录对拍全等；WRITE AUTOCORRS 默认 FALSE 路径零改动、无 crosspol 回归不变。

### 设计

**上游数据链路全景**：

1. 触发：.input CONFIG 段 `WRITE AUTOCORRS`（TRUE/T 开启）→ `configs[i].writeautocorrs`（configuration.cpp:1290-1291，fxcorrcommon 已迁移零改造）。**Mode 构造**：getMode 工厂直接把 `conf.writeautocorrs` 作为 calccrosspolautocorrs 传 Mode（configuration.cpp:916 等，已迁移）→ `autocorrwidth = 2`（mode.cpp:364-368）。
2. 计算（mode.cpp:1333-1352，零改造）：Mode::process 内每个 FFT 块，`matchingRecordedBand(i, j)`（freq 索引 == band 的 recordedbandlocalfreqindices，configuration.h:140-141）收集 count 个同频率 band（indices[]）；**count>1 时**（dual-pol 同频率）累加 `autocorrelations[1][indices[0]] += fft[ind0]×conj(conjfft[ind1])` 与对称项；`weights[1][idx]` 累加 dataweight（perbandweights 时取两 band 权重乘积）。单 pol 时 count 恒 =1，crosspol 数组与权重恒 0（天然安全）。
3. 频率平均/清零按 autocorrwidth 循环（mode.cpp:1385-1400 / 1460-1469，零改造）；zoom 的 crosspol 谱 = 父 band crosspol 数组切片（mode.cpp:379-388 在 autocorrwidth 循环内）。
4. 拷贝（core.cpp:1288-1301 / 1342-1369）：averageAndSendAutocorrs 在平行段拷贝后，若 `writecrossautocorrs(=writeautocorrs) && maxproducts>2` 再拷 crosspol 段——results 自相关区布局 = **[平行段 total bands][crosspol 段 total bands]** 串联（resultindex 连续递增，isFrequencyUsed 跳过）；weight 区同构（floatresults）。zoom 的 crosspol weight 从父 recorded band 取（getWeight(true, l)，父匹配 = localfreqindex 同 + pol 同，与平行同构）。
5. Visibility 写盘（visibility.cpp:903-947 / 591-647，fxcorrcommon 零改造）：writeautocorrs 时每 datastream 按 autocorrwidth 循环写自相关 SWIN 记录——j=0 平行（polpair=[p,p]）、j=1 交叉（polpair=[p, getOppositePol(p)]），baselinenumber = 257*(telescopeindex+1)，weight>0 才写；校准段（TSYS=0 走 weights 归一）crosspol 用 getDMatchingBand 的平行 calib 做除数。**结果长度预算已含 crosspol 段**（populateResultLengths 的 bandsperautocorr=2，configuration.cpp:2304/2503-2527，coreresultlength 自动覆盖，f/x 两侧缓冲分配零改动）。

**fxcorr 落点表**：

| 上游环节 | 上游位置 | fxcorr 落点 |
|---|---|---|
| WRITE AUTOCORRS 解析 | configuration.cpp:1290-1291 | 零改造（已迁移） |
| Mode crosspol 计算/平均/清零 | mode.cpp:364/1333-1352/1385-1400/1460-1469 | 零改造（已迁移，getMode 工厂传 writeautocorrs） |
| crosspol 段拷贝 | core.cpp:1288-1301/1342-1369 | **f：FEngineWriter::writeAutocorrelationBatch** 平行段后加 crosspol 段（getAutocorrelation(true, j)/getWeight(true, j)，zoom weight 父 band 逻辑同平行） |
| autocorr.bin 布局 | data-spec 5.3 | **f：header 加 u32 crosspol flag + version 升 2**；crosspol 段记录与平行同构（每 band cf32[nchan] + f32 weight） |
| V1 拒绝 | fxcorr-x main.cpp:158-159 | **删除**（maxproducts>2 放行） |
| autocorr 累加 | integrate.cpp addAutocorrs | **x：按 flag 读 crosspol 段**，resultindex 从平行段 walk 结束处继续（同 core.cpp 连续递增语义），累加进 subintresults 自相关区与 floatresults weight 区 |
| SWIN 自相关写盘 + 校准 | visibility.cpp:903-947/591-647 | 零改造（已迁移，autocorrwidth=2 路径现成） |

**关键点（易错，实施必读）**：

1. **crosspol 段存在条件 = writeautocorrs && maxproducts>2**（core.cpp:1288 判 modes[0]->writeCrossAutoCorrs() 即 writeautocorrs；maxproducts>2 是配置性质——dual pol 时 BASELINE 段 numpolproducts=4）。f 写段与 x 读段必须用同一条件（x 按 autocorr.bin header flag 判，不与 .input 重复推导）；单 pol 时 crosspol 恒 0、weight 恒 0（count 永 =1），不写段也不产生 SWIN 记录。
2. **crosspol 结果区偏移**：x 侧累加 crosspol 段时 resultindex 必须从**平行段 walk 结束处**继续（= getCoreResultAutocorrOffset + Σ平行 used-band nchan，isFrequencyUsed 跳过与平行同序），weightindex 同理——results 布局是平行/cross 串联，不是独立 offset 区。
3. **zoom crosspol**：谱是父 band crosspol 数组切片（getAutocorrelation(true, j) 直接可用）；weight 从父 recorded band 取（getWeight(true, l)，父匹配条件同平行：localfreqindex 相同 + pol 相同，core.cpp:1342-1369 同构）。
4. **无独立 valid flags**：crosspol weight=0 即无数据（visibility 判 weights>0 才写），单 band 无效时 dataweight=0 自然传染 crosspol，无需特判。
5. **对拍数据需 dual-pol 2 band VDIF**：test.vex 单 pol，需新资产（chan_def 加 Lcp 行）。mpifxcorr vdifmux 读 2 band 有历史问题（test2b 2026-09-12 记录读端错乱只出 1 积分），但 P6 帧头修复（2026-09-13）后未重测——**实施第一步先重测 2 band + mpifxcorr 链路**；若仍读不了则对拍降级为 fxcorr 链路自洽性验证（SWIN 自相关记录数值/落位合理性）并记录。
6. **autocorr.bin 兼容**：version 升 2 + u32 crosspol flag；x 侧读 header 时 version 1 按无 crosspol 段处理（旧 f 产物仍可读）、version 2 按 flag 判——单 pol 无 WRITE AUTOCORRS 的 v2 文件 flag=0，记录布局与 v1 完全一致。
7. **SWIN 对拍含自相关记录**：mpifxcorr 在 writeautocorrs 时同样写自相关伪基线记录，cmp_swin.py 全记录比较直接覆盖，无需工具改动。

### 验证方法

- 资产 `fxcorr/test/crosspol/`：test-pols.vex（chan_def 加 Lcp 行）+ test-pols.v2d（.input 生成 WRITE AUTOCORRS TRUE）+ README。
- 2 band（RCP+LCP 同频率）数据生成（fxcorr-sim，tone 参数两 band 给不同频率利于 crosspol 非平凡）→ mpifxcorr 基准（EXECUTE TIME=2 截断）与 fxcorr 全链路对拍：cmp_swin.py 全记录（含自相关伪基线）逐位全等。
- 无 crosspol 回归：test 配置（单 pol、无 WRITE AUTOCORRS）SWIN 6/6 不变；autocorr.bin 无 crosspol 段时 x 侧照旧。
- 位序回归：生成器输出 BYTE-IDENTICAL 不变（P6 修复后基线）。

✅ 2026-09-13：**dual-pol 全链路跑通**（test-pols 资产：RCP+LCP 同 200MHz、POL PRODUCTS 4、WRITE AUTOCORRS TRUE）：autocorr.bin v2 crosspol=1（记录 = 平行 2 band + crosspol 2 band，weight>0）；SWIN 72 条 = 6 积分 × 12（1 基线 × 4 pol 互相关 + 自相关伪基线 257/514 各 RR/LL/RL/LR 4 条，顺序同 visibility.cpp:913-944），自相关谱峰全部落 1.5MHz（tone 频率）、交叉谱功率 = 平行谱功率（同 tone 完全相关，物理判据）、weight=1.0。**单 pol + WRITE AUTOCORRS 对拍**：fxcorr 与 mpifxcorr 前 2 积分 6/6 记录逐位全等（含 4 条自相关记录：基线号/时间戳/polpair/weight/可见度——autocorr.bin v2 读法、x 侧累加、Visibility 自相关写盘与上游一致）。**无 WRITE AUTOCORRS 回归**：autocorr.bin v2 crosspol=0、SWIN 对拍 2/2 全等（退化路径不漂移）。**位序回归**：fxcorr-sim 单 band 输出与 gen_test_vdif.py BYTE-IDENTICAL（8032000 字节公共前缀）。**对拍数据链路前置发现（非 P7 代码问题）**：① test2b.vex 的 $TRACKS `track_frame_format = VDIF/8032/2` 帧长写错（2 band 实为 16032）→ vex2difx 生成 DATA FRAME SIZE 8032 → vdifmux 帧长不匹配错乱；改为 16032 后 vdifmux 警告清零、SWIN 头字段正常，但 mpifxcorr 读 2 band 样本交织帧仍只有首积分有效（vdiffile.cpp:538-550 的 setvdifmuxinputchannels(2) corner-turn 路径输出 32032B 帧，与 mark5access 的 2channel 交织解码期望不匹配，上游此路径本不可用），**mpifxcorr 对拍 2 band 仍不可行**，crosspol 对拍以单 pol 场景覆盖（自相关写盘路径共享）+ dual-pol 链路自洽验证；② fxcorr-sim 的 nbands 用 getDNumRecordedFreqs（不同频率数）——dual-pol 同频率时 freq 数=1 而 band 数=2，2bit 校验与 tone/pcal 网格全部按 band 语义使用，改为 getDNumRecordedBands（多频场景两值相等，行为不变；位序回归 BYTE-IDENTICAL 佐证）。

## P8：相位阵（phased array）频率域波束形成（✅ 2026-09-13 完成）

### 是什么

相位阵模式（.input CONFIG 段 `PHASED ARRAY TRUE` + `PHASED ARRAY CONFIG FILE`）不做站间互相关，而是把各站频谱按配置权重加权求和成一个波束频谱（beam forming，core.cpp:818-865），按 `ACC TIME (NS)` 窗口输出。典型用途：把相控阵（如 VGOS/WIDAR 子阵）的各单元数据合成单口径波束。

### 动机分类

功能未迁移。

### 要解决的问题

.INPUT 的相位阵配置解析与预算（configuration.cpp 的 PHASED ARRAY 段、processPhasedArrayConfig、populateResultLengths 的相位阵分支、subintns%paaccumulationns 与 accffts 整数校验）随 fxcorrcommon 零改造就位，但 fxcorr-x 在 V1 启动检查直接拒绝（main.cpp:156 "phased arrays are not supported in V1"）。上游 mpifxcorr 的相位阵链路是**半成品死代码**：f 侧算波束和写入 threadcrosscorrs（布局 = Σ_freq Σ_pol nchan，无 baseline 维），但消费端 uvshiftAndAverage 仍按 freq×baseline offset 解包（相位阵时 baseline offset 表未分配，configuration.cpp 相位阵分支只设 threadresultlength/coreresultlength），输出端（padomain/paoutputformat/DIFX/VDIF/TIMESERIES）全程无消费者——上游无法跑通，也无输出格式可对照。

### 预期效果

fxcorr-x 支持相位阵模式：删 V1 拒绝，实现波束加权求和并按 ACC TIME 分窗落盘（自定格式，同步 data-spec），与上游算法逐位对齐（波束和 = Σ_ds DWeight[ds] × spectrum_ds，累加不归一化）。无相位阵配置时路径零变化（回归对拍不变）。

### 设计

**算法（core.cpp:818-865 迁移，f 侧 .sp 多站频谱现成）**：

- per freq f（freq table 序号，numpafreqpols[f] > 0 的 freq 才输出，上游不查 isFrequencyUsed）：
  - per papol（getFPhasedArrayNumPols/getFPhaseArrayPol）：
    - per FFT 块（subloop）：per datastream ds：在 recorded bands 找 freq==f && pol==papol 的 band（getDRecordedFreqIndex/getDRecordedBandPol，同上游 dsfreqindex 匹配）→ 取该 FFT 块频谱；找不到再在 zoom bands 找（getDZoomFreqIndex/getDZoomBandPol）；找到则 `波束[f] += DWeight[f][ds] × 频谱`（vectorMulC + vectorAdd，DWeight 由 getFPhasedArrayDWeight，上游校验 ≥0）。
- **通道数用 getFNumChannels(f)**——上游 core.cpp:821 误传 configindex（getFNumChannels 的参数是 freq 序号，上游把 config 序号传进去，单 freq 配置碰巧 configindex==0==f 才正确），fxcorr 按正确语义修复并在文档记录。
- **不查 valid flags**：上游相位阵分支直接取 getFreqs 加权，不判有效位；fxcorr 侧无效 FFT 块频谱在 f 侧落盘即全零（fxcorr-f 保证），直接加权自然等价。
- **acc 分窗**：paaccumulationns 的窗口 = 整数个 FFT 块（Configuration 校验：subintns%paaccumulationns==0 且 accffts = paaccumulationns/ffttime 为整数且 %numbufferedffts==0），即整数个 fftloop 批。fxcorr-x 按 fftloop 计数累积（accfftloops = accffts/numbufferedffts），满一组落盘并清零；每窗口一条记录，时间戳 = subint 起点 + acc 序号×accns（scan 相对系，同 .sp）。
- 相位阵与 pulsar/多相位中心互斥（上游 if/else），与自相关并存（f 侧 .sp/autocorr.bin 照常落盘）。

**输出格式（上游无对照，自定；数据规范同步 data-spec 新节）**：

- 目录 `beam/<batch_id>/beam.bin`（batch 粒度，与 fengine/<batch_id> 对称；不是 vis/ 的 SWIN 语义）。
- Header（照 .sp/autocorr.bin 风格）：magic "FXCBM"、version 1、n_subints、n_accs_per_subint（= subintns/paaccumulationns）、acc_ns、n_freqs；per freq：freq_index、n_pols、pols[n_pols]、nchan。DWeight 不落 header（.input 可查，落盘易与配置漂移）。
- 每 subint：n_accs 条记录：i32 scan / i32 sec / i32 ns（窗口起点时间戳，scan 相对）+ per (freq, pol) cf32[nchan]（波束频谱，累加值）。无 weight 段（上游无此概念）。
- **相位阵 batch 不写 SWIN**：上游互相关消费端无相位阵路径（见动机），fxcorr-x 相位阵时跳过 XmacEngine/Integrator 路径、只出 beam.bin；intTime 整数倍校验随之跳过（不写 SWIN 即无积分网格需求）；batch 起点 subint 边界校验保留（beam 时间戳网格依赖）。

**fxcorr-x 结构**：

- main.cpp：删 V1 拒绝；相位阵时走独立分支（不构造 XmacEngine/Integrator、不分配 subintresults——相位阵的 coreresultlength 预算语义是字节数且无 baseline 布局，不可复用）。
- 新文件 beamengine.{h,cpp}：构造（freq/pol/band 映射表，一次建好）+ per subint 处理（读各站频谱 → per (freq,papol) 加权求和 → acc 分窗落盘）。频带映射沿用 main.cpp 现成的 readers 构造逻辑（recorded + zoom 视图）。
- 落盘 writer 照 autocorr.bin 风格（fseek 回填不适用——记录定长，顺序写）。

### 验证方法

- 资产 `fxcorr/test/phasearr/`：gen_test_phasearr.py 从 test.input 生成相位阵变体（CONFIG 段插 `PHASED ARRAY TRUE` + `PHASED ARRAY CONFIG FILE` 两行 + 相位阵配置文件：OUTPUT TYPE FILTERBANK / OUTPUT FORMAT DIFX / ACC TIME (NS) = 整数个 FFT 块 / COMPLEX OUTPUT FALSE / OUTPUT BITS 32 / 每 freq NUM FREQ 与 pol 列表 + 每 ds 权重）；README 写检验步骤。
- **数值验证（上游死代码无法对拍）**：① 单站 DWeight=1（另一站权重 0）：beam.bin 波束谱与 fxcorr-f 的 .sp 频谱逐位全等（Σ 只有一项）；② 两站 DWeight=(0.5,0.5)：波束 = (T1+T2)/2，与 Python 手算参考逐位核对；③ DWeight=(1,0)：T2 频谱无贡献；④ tone 峰落位（1.5MHz 通道）；⑤ numaccs=2 变体：窗口时间戳 = subint 起点 + acc×accns、分段累加值正确。
- 无相位阵回归：test 配置（无 PHASED ARRAY 段）SWIN 对拍 6/6 不变；位序对拍 BYTE-IDENTICAL 不变。

✅ 2026-09-13：**实施完成**（fxcorr-x 早退分支 + BeamEngine，fxcorrcommon 仅补一行 `getFPhasedArrayAccumulationNS` getter；beam.bin 布局同步 data-spec 5.5 节/D14）。**数值验证**：test 配置 SUBINT 改 81920000ns（40 块，numbufferedffts 整除——原 256 块过不了上游 accffts 校验）、ACC TIME 20480000ns → 每 subint 4 窗口：DWeight(0.5,0.5) 与 (1.0,0.0) 两种权重下 cmp_beam.py 50×4 窗口**逐位 PASS**（频谱 f32 相同累加序逐位一致、时间戳 = subint 起点 + acc×20480000ns）；波束谱峰 chan 1536 = 1.5MHz、不写 SWIN ✓。**无相位阵回归**：标准配置 SWIN 对拍 6/6 全等（intTime 校验限定非相位阵后重确认）。**实施坑**：① 相位阵配置文件行格式——getinputkeyval 从固定第 20 列取 val，短 key 的值须空格 pad（照 pulsar 资产的 kv() 先例）；② 相位阵分支最初做 if/else 包裹重构（提公共 lambda），标准配置回归出现堆损坏（subint 1 fread 触发），二分无果后改**早退分支**（旧路径逐字节不动、相位阵提前 return），回归恢复全等——早退隔离性最好，保留；③ 上游 core.cpp:821 的 getFNumChannels(configindex) 是 bug（参数应为 freq 序号），fxcorr 按正确语义修复。

---

## P9：Kurtosis STA + STA 频域平均分支（✅ 2026-09-13 完成）

### 动机分类

功能未迁移。上游 STA 监控有三个组件，P1 只做了自相关 STA 的非平均分支：① `averageAndSendAutocorrs`（core.cpp:1166-1265）每 autocorr 批次发 STA_AUTOCORRELATION；② 同函数内的**频域平均分支**（:1181-1187）——`minpostavfreqchannels >= STADumpChannels` 时（默认 4096≥32 恒真）STA 发送前先把各站频谱 `averageFrequency()`，STA 的 renorm 与通道数按平均后频谱修正（:1249-1254），autocorr.bin 落盘跳过重复平均（:1263-1268）；③ `averageAndSendKurtosis`（:1378-1434）每 subint 末发 STA_KURTOSIS（谱峰度，无 weight gate、无 renorm）。上游由 difxmessage 控制消息运行时开启（`dumpsta=true` / `dumpkurtosis=true`，mpifxcorr.cpp:88-97，独立命令线程阻塞接收）；fxcorr 用环境变量（`FXCORR_STA=1` P1 已有、`FXCORR_KURTOSIS=1` 本次新增）。

### 要解决的问题

STA 是实时监控（difxmonitor GUI）的数据源：autocorr STA 的谱形、kurtosis 的 RFI 峰度统计都是标准观测监控项。P1 只覆盖了 autocorr STA 的默认分支，且 CHANS TO AVG>1 的配置下数值与上游不同（非平均分支 renorm 未除 FChannelsToAverage），监控图与上游不一致。

### 预期效果

`FXCORR_STA=1` 时 STA_AUTOCORRELATION 消息与 mpifxcorr 基准**逐位全等**（含 CHANS TO AVG 4 的平均分支）；`FXCORR_KURTOSIS=1` 时 STA_KURTOSIS 消息逐位全等；无开关路径零变化（SWIN 回归 6/6、autocorr.bin 逐字节不变）。

### 设计

**落点：fxcorr-f 只动 main.cpp + fenginewriter 一处签名**。fxcorrcommon 的 Mode 已把上游 kurtosis 全链路移植好（`process()` 内 s1/s2 累积在 fracsample 修正前、`zeroKurtosis` 惰性分配、`calculateAndAverageKurtosis` 含折叠平均），f 侧只需接线。

- **Kurtosis 接线**：构造后 `mode->setDumpKurtosis(dokurtosis)`（process 内 `if(dumpkurtosis)` 分支已就位）；每 subint 循环 `if(dokurtosis) mode->zeroKurtosis()`（core.cpp:703-704 条件调用）；subint 尾（flushWeights 后）`sendKurtosis`：`calculateAndAverageKurtosis(blockspersend, STADumpChannels)` 后 per recorded band 发 STA_KURTOSIS——nChan = min(STADumpChannels, FNumChannels)，data = getKurtosis 前 nChan 直接拷贝（**无 weight gate、无 renorm**，与 autocorr STA 的区别），时间戳同 autocorr STA（当日秒系），nsoffset/nswidth = 整个 subint（blockspersend×blockns，非 ac 批次）。单线程 numblocks = blockspersend 与上游 core.cpp:2165 一致。
- **频域平均分支**：两处 ac 批次边界（满批 + 尾批）改为：`bool datastreamsaveraged = (dosta && getMinPostAvFreqChannels(0) >= getSTADumpChannels())`；true 时先 `mode->averageFrequency()` 再 sendSTA，`writeAutocorrelationBatch(mode, datastreamsaveraged)` 内部跳过自己的平均（新参数，默认 false 保持旧行为）。sendSTA 内部按上游 :1249-1254 顺序：weight gate 用**原始** freqchannels（gate 在平均调整前）→ renorm 初值 → datastreamsaveraged 时 `renormvalue /= FChannelsToAverage(freqindex)`、`freqchannels /= FChannelsToAverage(freqindex)` → nChan 截断 → chans_to_avg 折叠 → renorm 乘。
- **顺手补上游 MTU gate**（:1191-1192）：`sizeof(STARecord)+4×STADumpChannels > MTU` 时整体不发（P1 漏移植；默认 32 通道远小于 MTU，无实际影响）。
- **上游怪癖不修**：`calculateAndAverageKurtosis` 的返回 nonzero 只反映**最后一个 band**（上游 bug，单 band 配置碰巧正确）——fxcorr 照抄；多 band 时 sk 的 stale 值与上游行为一致（对拍无法覆盖，上游自己都不一致）。
- **difxsta 对拍设施**：difxmessage 库只有发送端 API 无现成抓包工具，测试资产自写 `sta_ctrl`（C，链 libdifxmessage）：`send` 模式发控制消息（difxMessageSendDifxParameter）、`recv` 模式 difxMessageBinaryOpen(BINARY_STA) 抓原始 record 流（与 fxcorr container 模式 .sta 落盘同布局，同一 cmp 解析）。

### 验证方法

- 资产 `fxcorr/test/sta/`：gen_test_sta.py（CHANS TO AVG 1→4 变体 + EXECUTE TIME=2 截断变体）、sta_ctrl.c（控制消息 + 抓包）、cmp_sta.py（按 (messageType, dsindex, bandindex, scan, sec, ns, nswidth) 分组逐位比对 + min_absns 过滤参数）、README（完整命令）。
- 对拍两轮：默认 CHANS TO AVG 1（平均分支等价回归）与 CHANS TO AVG 4（真平均分支）各 318 条（autocorr 312 + kurtosis 6）逐位全等；无开关 SWIN 回归 6/6；FXCORR_STA=1 时 autocorr.bin md5 与无开关一致。

✅ 2026-09-13：**实施完成**（main.cpp：FXCORR_KURTOSIS 开关 + setDumpKurtosis/条件 zeroKurtosis/sendKurtosis 新函数 + 两处批次边界的 datastreamsaveraged 判定 + sendSTA 平均分支与 MTU gate；fenginewriter：writeAutocorrelationBatch 加跳过平均参数）。**验证**：两轮对拍各 318 条**逐位 PASS**（含 kurtosis 6 条）；无开关 SWIN 回归 6/6 全等；autocorr.bin 开/关 STA 逐字节一致（md5）。**实施坑**：① **P1 遗留 stride bug**——sendSTA 折叠循环 `acdata[2*k*chans_to_avg].re` 直接照抄上游 f32* 的 ×2 stride（上游 acdata 是 `(f32*)getAutocorrelation()`，f32 stride 2 = cf32 stride 1），fxcorr 用 cf32* 时 ×2 导致隔块取样、STA 谱形完全错误；P1 对拍未覆盖 data 数值，P9 的逐位对拍暴露，修复为 `acdata[k*chans_to_avg].re`；② **基准控制消息时序**——mpifxcorr 命令线程在 .input 读完后才 spawn，先发的消息必丢，4 次×1s 连发只从第 3 个 subint 起生效；检验用 0.2s×40 轮连发（dump 从第 2 个 subint 起生效），首 subint 无基准记录、两边统一 min_absns=524288000 过滤（组播 fire-and-forget 固有行为，非 fxcorr 问题）。

## P10：输入格式补齐（Mark5B/LBA 家族/其余 + 多线程 VDIF corner-turn）

### 动机分类

功能未迁移。V1 fxcorr-f 的 datareader 只收 VDIF/VDIFL、强制 nummuxthreads=1（usage.md V1 边界）；上游 DataStream 工厂（mpifxcorr.cpp:431-469）按格式分派六类读取路径，其余全部未迁移。

### 要解决的问题

真实观测数据不只有单线程 VDIF：Mark5B 是主流磁带文件格式（LBA/CMI 等数据的标准文件载体），多线程 VDIF（INTERLACEDVDIF）是宽带数据标准形态。缺这些格式则 fxcorr 无法处理绝大部分历史与现役观测数据。

### 上游路径映射（调研结论，2026-09-13）

| 格式 | 上游类 | 读取机制 | fxcorr 落点 |
|---|---|---|---|
| VDIF/VDIFL | VDIFDataStream（vdiffile.cpp） | 自写帧解析 | V1 已迁移 |
| INTERLACEDVDIF（多线程 VDIF） | VDIFDataStream | 帧对齐按 multiplexed framebytes（payload×nthreads、fps÷nthreads，vdiffile.cpp:396-401）+ vdifio 的 vdifmux corner-turn | **改造项 1**：放开 nthreads 限制 + VDIFMuxer（fxcorrcommon 已含，上游 Mk5DataStream 的 VDIF+MK5MODULE 路径同款，输出同为 mk5access 期望的 muxed 布局） |
| MARK5B | Mark5BDataStream（mark5bfile.cpp） | summarizemark5bfile 读首帧时间 → 时间跳转 dataoffset（:341-356，帧/载荷比 10016/10000 修正）→ seek → 顺序读 + **mark5bfix** 修复 | **改造项 2**：同款（summarizemark5bfile + mark5bfix，mark5access 库函数） |
| MKIV/VLBA/VLBN/KVN5B/CODIF | Mk5DataStream（mk5.cpp） | mark5access 通用文件流：`new_mark5_stream_file` + `genMk5FormatName` 的 formatname 读首帧（frameoffset/mjd/sec/ns/framebytes/framens，mk5.cpp:315-390）→ 时间跳转 → seek → 基类 raw 顺序读（无修复） | **改造项 3**：同款通用路径，一条路径覆盖 5 格式 |
| LBA 家族（LBASTD/VSOP/8BIT/16BIT） | 基类 DataStream（工厂 else 分支） | **ASCII 时间头 + raw payload**（datastream.cpp:1825-1848：15 字符 `YYYYDOYHHMMSS:xx` 或 `TIME ` 关键字头，LBA 数据文件传统格式）+ 纯字节率定位（无帧概念）；解包走 mode.cpp 的 LBAMode/LBA8BitMode/LBA16BitMode（**fxcorrcommon 已含**，configuration 工厂 870-892 已接） | **改造项 4**：ASCII 头解析 + raw 字节定位；解包零改动 |
| K5VSSP/K5VSSP32 | Mk5Mode（mark5access） | **上游即不可用**：mark5access 的 MK5_FORMAT_K5 = "Not Yet Implemented" + genMk5FormatName 无 K5 分支（直接 fatal） | **不迁移**，与上游死代码同列记录 |
| StreamStor/Mark6 | NativeMk5/Mark6 类 | 硬件访问 | 不迁移（既定） |

### 设计

**落点：fxcorr-f 只动 datareader.{h,cpp}（+main.cpp 最小接线）**。datareader 构造时按 format 分派五条路径；locate/readSubint 语义保持（逐 subint 定位 + 顺序读，main.cpp 驱动不变）。

- **改造项 1（INTERLACEDVDIF）**：构造时放开 `nummuxthreads != 1` 限制（保留 format != VDIF/VDIFL 拦截）；nthreads>1 时创建 VDIFMuxer（构造参数照 datamuxer.h：conf, dsindex, id, nthreads, 单线程帧字节, rframes, fpersec, bitspersamp, threadmap）。帧对齐/字节定位用 multiplexed 参数（getMultiplexedFrameBytes / payloadbytes×nthreads / fps÷nthreads，对照 vdiffile.cpp:396-401）；readSubint 读入 muxed 帧到临时 buffer → `muxer->deinterlace(validbytes)` → `muxer->multiplex(databuf)` 输出 corner-turn 数据，valid flags 按 mux 后字节数照旧。单线程路径零变化（回归）。
- **改造项 2（MARK5B）**：构造时 `summarizemark5bfile` 读文件头（firstFrameOffset/startmjd/sec/ns/bitrate，对齐 mark5bfile.cpp:313-327）；locate 的时间→字节偏移用帧长/帧率直接算（batch 起点=帧边界，file-per-batch 干净数据），首帧时间仅做校验与锚定（对照 mark5bfile.cpp:337-373 的跳秒逻辑）；readSubint 顺序读 + `mark5bfix`（对齐上游 Mark5BDataStream 的 gap/fill 语义，逐位对拍的前提）。
- **改造项 3（MKIV/VLBA/VLBN/KVN5B/CODIF）**：一条通用路径——构造时 `new_mark5_stream_file(filename, 0)` + `new_mark5_format_generic_from_string(genMk5FormatName(...))`（fxcorrcommon 已含，mk5.cpp:315-390 同款）读首帧参数（frameoffset/mjd/sec/ns/framebytes/framens）→ 首帧时间锚定 batch 起点 → locate 按帧长算偏移 → readSubint seek + raw 顺序读（无修复，对齐 Mk5DataStream）。解包在 mark5access 库内，与上游同一库同一 formatname。
- **改造项 4（LBA 家族）**：构造时解析 ASCII 头（照 datastream.cpp:1825-1848：第一行 15 字符直接是时间、或循环找 `TIME` 关键字行，取 `YYYYDOYHHMMSS:xx` 的年月日时分秒+百分秒），头后偏移 = 首 payload 字节；时间→字节定位纯字节率（无帧对齐，对齐基类 calculateControlParams 语义）；解包已就位零改动。
- **formatname 一致性**：改造项 2/3 的 formatname 一律走 `config.genMk5FormatName`（fxcorrcommon 已有），保证与上游、与 mk5mode 解包侧同一字符串（CODIF 的 codifio 库头常量 CODIF_HEADER_BYTES 已由 configuration.cpp include，genMk5FormatName 的 CODIF 分支直接可用）。
- **K5VSSP 记录**：v2-plan 的 P10 行补充"K5VSSP 上游不可用不迁移"（mark5access K5 Not Yet Implemented、genMk5FormatName 无分支）。

### 验证方法（决策：重点对拍 + 通性验证，2026-09-13 用户定）

- **逐位对拍**（mpifxcorr 读同文件出基准 SWIN → cmp_swin.py）：① **MARK5B**（上游 Mark5BDataStream 路径）；② **INTERLACEDVDIF 多线程**（上游 VDIFDataStream+vdifmux 路径；测试机 fakemultiVDIF 造多线程数据）；③ **LBA 家族**（上游基类 DataStream raw 路径——先试对拍，若 mpifxcorr 基类路径自身不可用则降级 fxcorr 自洽验证并把结论写入本文与测试 README）。
- **通性验证**（跑通 + tone 落位 + 谱形/SWIN 物理正确 + 代码对照审查）：MKIV/VLBA/VLBN/KVN5B/CODIF——解包在 mark5access 库内与上游同一库，fxcorr 侧只验帧定位接线；基准数据难造（磁带格式无现成生成器），不做逐位对拍。
- **回归**：现 VDIF 单线程配置 SWIN 对拍 6/6 保持全等。
- 测试数据：Mk5B 帧生成器自写（Python，10016 字节帧 = 16 字节头 + 10000 payload，帧头布局照 mark5access 读端实现而非官方文档——见 memory vdif-header-layout 同类教训——先用测试机 m5bstate/fixmark5b 验证帧合法）；LBA raw 文件 = ASCII 头 + payload（2bit 采样，照 gen_test_vdif.py 的位序）；CODIF 用测试机 codif_write；多线程 VDIF 用 fakemultiVDIF。

### 验收判据

① Mark5B 对拍逐位全等；② 多线程 VDIF 对拍逐位全等；③ LBA 家族对拍逐位全等（或降级自洽验证 + 结论记录）；④ MKIV/VLBA/VLBN/KVN5B/CODIF 五格式 fxcorr-f 通性验证全过；⑤ 现 VDIF 回归 6/6。

### 实施记录（2026-09-14 完成）

- **Mark5B 对拍 PASS**（判据①）：fxcorr vs mpifxcorr Mark5BDataStream，SWIN 6/6 逐记录全等。
- **多线程 VDIF 对拍 PASS**（判据②）：INTERLACEDVDIF fanout 2 线程（自写 gen_test_ivdif.py，fakemultiVDIF 复制语义不可用），SWIN 6/6 全等。期间修复：VDIF word3 位布局（threadid bits16-25、nbits-1 bits26-30，旧生成器布局错）、datamuxer 输出 EDV4 头三坑（nchan 保持 0、validitymask=1、裸 word 写）、mode.cpp unpacked +8、mk5mode invalid 按线程数分配。
- **LBA 降级自洽验证 PASS**（判据③预案）：mpifxcorr 读 LBA+FILE 走基类 DataStream 路径，4 进程 100% CPU 失控死循环（上游陈年 bug 实锤）——基准不可用，降级 fxcorr 自洽验证（`fxcorr/test/p10/verify_lba.py`）：T1/T2 autocorr 峰位 1536/1024（1.5/1.0MHz）正确、SWIN 12 记录 weight 0.82-1.0。期间修复生成器位序 bug：**LBA 2bit 是低位先**（LBAMode lookup shift=0 起取 u16 低 2bit，与 VDIF/Mk5B 相同；生成器原 MSB-first 打包使 1.5MHz tone 被字节内时间反转调制到 fs/8 → 峰落 1MHz 位置）。
- **五格式审查**（判据④，决策 B 通性验证）：MKIV/VLBA/VLBN/KVN5B/CODIF 无基准数据可造（磁带格式），以代码对照审查为验收依据：构造分派（datareader.cpp:50-55）与上游 mpifxcorr.cpp:431-469 映射一致；genMk5FormatName + new_mark5_stream_file 锚定首帧 + 帧对齐定位 + raw 顺序读，与上游 Mk5DataStream（mk5.cpp:315-390）同构；解包在 mark5access 库内与上游同一库同一 formatname；报错路径完整（fanout<0 / 打开失败 / 文件起点晚于 batch 起点）。读入架构与已验证的 KIND_MARK5B 路径同构（raw 读 + Mode 解包）。
- **VDIF 单线程回归 PASS**（判据⑤）：cmp5 目录（标准 VDIF 配置）SWIN 6/6 全等，datareader 多格式改造 + mode/mk5mode/datamuxer 修复无回归。
- 调试 dump 代码（FXCORR_*DUMP）已全部清理。

---

## P11：f 侧 reader 语义补全（valid flag 跨段续接、延迟中途重对齐）

### 与 P10 的关系

P10/P11 是同一组件（fxcorr-f datareader）上先后两层的改造，均源自 2026-09-13 mpifxcorr 完整审查：

- **P10 补"读什么"（格式覆盖）**：上游 DataStream 工厂按格式分派六类读取路径，P10 补齐五路径（VDIF/muxed VDIF/Mark5B/mark5access 通用流/LBA 家族），落点是 datareader 构造分派 + 各格式定位/读取机制，已完成。
- **P11 补"怎么读"（读入语义精度）**：上游所有格式的 subint 定位都走基类 DataStream::calculateControlParams 的公共段（datastream.cpp:516-613），其中两处边界语义（延迟中途重对齐、valid flag 跨段续接）P10 未迁。P11 落点是 P10 五路径**共享**的 locate/fillValidFlags——改一次，五格式同时受益。
- **暴露条件**：P11 只在几何 delay≠0（真实观测）时暴露差异；合成测试 .vex 两站坐标重合 delay=0，故 P10 的全部对拍均未触达。

### 动机分类

语义等价。V1/P10 的 datareader 只迁移了正常路径语义（数据连续、几何 delay≈0）；上游 DataStream::calculateControlParams 的两处边界语义未迁移，真实观测（几何 delay≠0）下 fxcorr-f 与 mpifxcorr 输出不等价。合成测试 .vex 两站坐标重合 delay=0，故此前所有对拍均未暴露。

### 要解决的问题

上游 getData（datastream.cpp:516-613，**所有格式经基类路径**；VDIF/Mk5/Mark5B 子类只在基类 bufferindex 上做帧对齐，vdiffile.cpp:348-353 同款调用）在"延迟修正后的数据起点早于所属数据段起点"时走 count>0 分支（:538-574）：

- **跳块**：按 FFT block（blockbytes）跳过 count 块直到数据起点进入段内；count ≥ blockspersend → 整个 subint INVALID_SUBINT。
- **tosubtract 补偿**：`count×(delayus2−delayus1)×1000/(sampletimens×blockspersend)` 采样，补偿 subint 内延迟线性变化（delayus2 = subint 终点延迟，:387-388）。**上游 quirk（:553-569，须照抄保对拍）**：仅当"补偿后压回段起点前"才扣 tosubtract，此时 count++ 再跳一块重算；补偿后仍在段内则**不扣**。
- **对齐**：2 字节边界（:518-519）+ 整数 ns 字节边界（bytesbetweenintegerns，:570-573）。
- **valid flags**：前 count 块强制 invalid（:568 判界从 count 起）。
- **时间重算**：数据时间 = 段起点时间 + segoffns（跳过后实际起点），不是请求的 offsetns（:576-583）。

触发场景：① 修正起点早于 scan 起点（**delay>0 时第一个 subint 必触发**；地球基线 delay ≤ ~21ms < subint 时长，只影响第一个 subint）；② 修正起点落在数据缺口段内（段 validbytes 截断、段前推循环 `nsdifference ≥ validns` 提前推进，:437-456）——缺口语义依赖 filldatasegment 的帧断档检测，**本项不含**（V1 数据无缺口）。

另两处相关语义：**nsdifference < −nsinc 早退**（:463-470：修正起点早于段起点超过一个段长 → 整个 subint INVALID_SUBINT，不跳块）；**valid flag 跨段续接子句**（:584-605：本段读满 + 下一段时间连续（精确 nsinc 衔接）+ 两段合计字节够 → 判界借用下段 validbytes）。

fxcorr 现状差距：locate 对修正起点 < batch 起点只做 `framesin=0` 截断（无块对齐、无补偿、无 count、时间语义不同）；fillValidFlags 无 count 参数；locate 只算 delayus1 未算 delayus2。

### 设计

**落点：fxcorr-f 的 datareader.{h,cpp}（+main.cpp 传 count）**。

- **等价映射**：fxcorr 无段缓冲、无缺口检测 ⇒ 段恒"读满"（validbytes = readbytes）⇒ 段前推条件 `nsdifference ≥ validns = nsinc` 在修正起点 ≥ 数据起点时恒成立（前推到所属段）、< 数据起点时恒不前推 ⇒ **段网格（readbytes = databufferfactor/numdatasegments×maxbytes）退化为单一"数据起点"网格**，count>0 判界即"修正字节位置 < 数据起点"（batch 起点；file-per-batch 单 scan 下 = scan 起点 = 段 0 起点）。
- **locate 改造**（五 kind 通用，插在现有 framesin 帧对齐之前，对照 :516-583）：
  1. delayus2 = calculateDelayInterpolator(subint 终点，dataspanns 算法同 mode.cpp setOffsets 的 timespan，:387-388)；
  2. 修正字节位置（batch 相对）< 0 → 按 blockbytes 跳块到 ≥ 0（count 上限 blockspersend，超限 → INVALID_SUBINT，:542-548）；修正起点 < −nsinc（nsinc = subintns×databufferfactor/numdatasegments，configuration.cpp:3025）→ 整个 subint INVALID_SUBINT（:463-470 早退）；
  3. count>0 时 tosubtract 补偿（照上游公式，采样→字节用 bytespersamplenum/denom），**含 quirk 照抄**（仅压回时扣 + count++ 重算，:553-569）；
  4. 2 字节对齐 + bytesbetweenintegerns 对齐（:570-573）；
  5. 各 kind 现有帧对齐与时间重算照旧（输入改为重对齐后的位置；KIND_LBA 无帧对齐）。重对齐后的数据起点时间 = 重对齐后字节位置换算（上游 = 段起点时间 + segoffns）。
- **fillValidFlags 加 count 参数**：前 count 块强制 0，其余按 gcount 判界（:568-613 慢路径本段判界）；"跨段续接子句"在 fxcorr 单文件连续读下自动等价（读窗口无段限制、文件连续 ⇒ gcount = sendbytes 时全 valid；文件尾部 = 上游"段未读满"单段判界），**不显式实现**，写入实施记录。
- **main.cpp**：readSubint 把 count 带出（返回参数或成员），fillValidFlags 取用；仅传参改动。
- **新成员/参数**：blockbytes 与 bytesbetweenintegerns 提为 datareader 成员（fillValidFlags 已算 blockbytes；bytesbetweenintegerns 照 datastream.cpp:757-761 累加算法）；delayus2 为 locate 局部量。
- **不迁移项**：帧断档检测（filldatasegment）、段缓冲（readbytes 网格）、跨段数据拼接（execute 回绕拷贝）——单文件连续数据下语义自动等价，写入 v2-plan 记录。

### 验证方法

- **delay≠0 测试资产**（`fxcorr/test/p11/`）：改 test.vex 站点坐标（T2 挪到地球尺度基线 → 几何 delay ~ms 级）→ vex2difx + difxcalc 全链路重跑（fxcorr 用 .calc、mpifxcorr 用 .im，同 .vex 几何同源）。delay 变化项：2.097s 观测内几何 delay 变化 ~µs 级（dτ/dt ~ 1.5µs/s）→ delayus2−delayus1 ≈ 3µs → tosubtract = count×3µs/0.524s ≈ 0 采样（round 0）→ **跳块语义独立可验，补偿项恒 0 时 quirk 无影响**。
- **对拍**：delay≠0 配置 mpifxcorr 出基准 SWIN → cmp_swin.py 全等（第一个 subint 触发 count>0，其余 count=0，无缺口）；delay=0 回归（cmp5）6/6 不变。
- **补偿项（tosubtract≠0）**：真实几何 delay 下 subint 内变化不足以产生 ≥1 采样补偿（需 Δ ≥ subintns/count，地球基线不可能，航天器场景才有）；公式与 quirk 以**代码逐行对照审查**为验收依据（对拍覆盖到 tosubtract=0 的同一条代码路径），写入实施记录。

### 验收判据

① delay≠0 对拍逐位全等（count>0 触发）；② delay=0 回归 6/6 全等；③ tosubtract 公式/quirk 与上游逐行对照记录；④ 跨段续接自动等价的论证写入实施记录。

### 实施记录（2026-09-14 完成）

- **delay≠0 对拍 PASS**（判据①）：p11 资产（`fxcorr/test/p11/gen_test_p11.py`：TEST2 挪到 T1 对跖点，基线 12746km）→ vex2difx + difxcalc 全链路，`.im` 实测 DELAY ANT0 ≈ +11.2ms / ANT1 ≈ −11.2ms（fxcorr 用 .calc、mpifxcorr 用 .im，同源几何）。SWIN 对拍 6/6 全等。**跳块语义确认触发**：两边 SWIN weight 分布一致——第一个 subint 的 T1 相关记录（bl 258/257）weight 0.989258（+11.2ms 修正起点落在数据起点前 → 跳块、前段块 invalid），T2 相关记录（bl 514）与后续 subint 全部 weight 1.0（负 delay 修正起点在数据起点之后、count=0），与"delay>0 只影响第一个 subint"的预期一致。
- **delay=0 回归 PASS**（判据②）：cmp5 目录 SWIN 6/6 全等（改造后二进制）。
- **tosubtract 逐行对照**（判据③）：公式 `count×1000×(delayus2−delayus1)/(sampletimens×blockspersend)` 采样 + `×bytespersamplenum/bytespersampledenom` 换字节，与 datastream.cpp:553/563 一致；quirk（仅"补偿压回数据起点前"时扣 + count++ 重算，否则不扣）照抄 :556-567；单位 sampletimens = 500/带宽（COMPLEX×2）照 :755-756；delayus2 取 subint 终点（offsetns+dataspanns，dataspanns = blockspersend×fftchannels×sampletimens）照 :384-388；2 字节对齐（:518-519）与整数 ns 边界（bytesbetweenintegerns，:570-573、累加算法照 :757-761）同序。对拍数据 delay 一阶项 −0.23µs/s → 2.097s 内变化 ~0.48µs → 补偿恒 round 0，补偿分支未触发（设计节预期，走代码对照）。
- **跨段续接自动等价论证**（判据④）：fxcorr 读窗口 = 单段（seek + 读 sendbytes），文件连续时 gcount == sendbytes ⇔ 上游"段读满 + 下段连续（nsinc 精确衔接）+ 两段合计够"的快路径（:584-594）；文件尾部 gcount < sendbytes ⇔ 上游"段未读满"单段判界（:600-604 第一子句）。上游跨段数据拼接（execute 回绕拷贝）与段缓冲（readbytes 网格）无对应物——单文件连续读下无段边界可切，语义自动等价，不显式实现。
- **实施细节**：等价映射成立的关键是"无缺口检测 ⇒ 段恒读满 ⇒ 前推循环 nsdifference ≥ validns=nsinc 恒成立 ⇒ 段网格退化为单一数据起点"；−nsinc 早退（datastream.cpp:463-470）用 nsinc = subintns×databufferfactor/numdatasegments（测试配置 256/64=4 → 2.097s，delay 11.2ms 远小于，不触发）；locate 的修正字节位置按上游 :516 采样四舍五入公式重算（替换原 floor(帧) 近似），帧对齐改为上游子类 `framesin = vlbaoffset/payloadbytes`（vdiffile.cpp:429-444）。
- **对拍过程踩坑**：① mpifxcorr 基准的 OUTPUT FILENAME sed 替换须带 4 空格（.input 列对齐，getinputkeyval 按固定列读值）——漏掉时输出目录被读成 `ch.difx`（值从第 17 列起截断），此坑 p10/README 已有记录，再次踩中；② vex2difx 从进程 cwd 找 `vex=` 文件（非 v2d 所在目录），test-delay.vex 须放工作目录根（已写入 p11/README 步骤）。

---

## P12：真实观测的病态数据（文件起点偏移、记录中断、filler 帧）

### 与 P10/P11 的关系

P10/P11/P12 是同一组件（fxcorr-f 的 `datareader`）上先后三层的改造，差别在**暴露条件**：

| 项 | 补什么 | 暴露条件 | 完成 |
|---|---|---|---|
| P10 | 读**什么**（格式覆盖：Mk5B/LBA/多线程 VDIF/mark5access 通用流） | 换一种输入格式 | 2026-09-14 |
| P11 | **怎么读**（延迟中途重对齐、valid flag 判界） | 几何 delay ≠ 0 | 2026-09-14 |
| **P12** | 数据**不听话**时怎么办（起点偏移 / 缺口 / filler） | **真实观测数据** | 部分完成（见下） |

P10/P11 及此前的全部对拍都用 fxcorr-sim 生成的**理想数据**（帧连续、起点恰在 batch 起点、无 filler），这些前提在真实观测下一条都不成立——P12 是首个由**真实数据驱动**的 reader 改造。

### 动机分类

串行环境新变化。mpifxcorr 的**顺序读**模型把起点偏移、中间缺帧、filler 的后果全部**隐式吸收**（每段的起始时间取自数据内的帧时间戳；缺帧由 `vdifmux` 成流时标 invalid 位），因此这些量既没写进上游代码注释、也没写进任何规范，拆分时无从识别。fxcorr-f 的**定位读**模型（`字节位置 = (时间 − batch 起点) × 速率`）下，它们全部变成必须**显式求解**的量。

### 要解决的问题

首个真实 VGOS 观测 t25362（BA/S6 两站、11.264 s、BA 有 8 个 datastream）同时暴露三类病态形态，且方向相反：

| 类 | 现象 | 对应的显式量 |
|---|---|---|
| **A 起点偏移** | 文件起点晚于 batch 起点（BA 晚 79.312 ms） | `anchorbytes` |
| **B 中间缺帧** | 字节流**短**于时间轴（BA 每个 ds 有 7–8 处、合计 144–148 帧／12 秒） | `gapspan`（`gapshiftAt`）+ `gapinvalid` |
| **C filler 帧** | 帧头全零的占位帧，字节流**长**于时间轴（BA ds_2 有 1162 帧／8 段） | `fillershiftbytes` |

每种形态又派生出多个实现陷阱（去重键、补扫漏计、跨 subint 边界等），合计 14 条缺陷，2026-09-18 全部修完。

### 设计

**分析与设计见 `fxcorr/reader-model.md`**（fxcorr-f 读取路径的单一权威分析文档）：

- 两类读模型对照与三条被破坏的上游假设：第 2–3 节
- 缺陷根因总表（A 起点 / B 缺口 / C filler / D 有效性）与症状指纹：第 4 节
- 诊断判据（`GAPCHECK` / `READPOS`）与分层验收（L1 帧时间轴 / L2 有效区间 / L3 产物）：第 6–7 节
- 后续改造建议（reader 拆分为帧时间轴层 / 修正量层 / I/O 层）：第 7.2 节

**代码落点**：`applications/fxcorr-f/src/datareader.{h,cpp}` 的 `checkFrameContinuity` / `shiftFrameGaps` / `gapshiftAt` / `countFillerRange` / `locate` / `readSubint` / `fillValidFlags`——源码注释即按 P12 step 1 / 2a / 2b 分节。

### 实施记录（2026-09-16 ~ 2026-09-18，已完成）

| 组 | 缺陷 | 状态 | 关键点 |
|---|---|---|---|
| A | A1 `anchorbytes` 缺失、A2 取整用 C++ 截断、A3 起点所在 subint 整块判无效 | ✅ 已修 | A2 改 `floor(x+0.5)`（四条格式路径统一）；A3 按 `skippedblocks` 折进 `lastcount` 后从**文件头**读起 |
| B | B1 线性换算错位、B2 跨 buffer 假缺口、B3 去重键错误、B4 `gapinvalid` 未按 subint 重建 | ✅ 已修 | B2 缺口只在 buffer **内部**找；B3 去重键用**修正后**的读取位置 |
| C | C1 filler 帧号当缺口、C2 filler 未识别、C3 按距离推断超调、C4 补扫漏计、C5 段边界/接缝漏检、C6 链尾未回存 | ✅ 已修 | C3 必须**实扫**该区段（`countFillerRange`）；C5/C6 的修复四处缺一不可 |
| B | **B5 缺口按"已检测到"而非"时间位置在读取点之前"累计** | ✅ 2026-09-18 已修 | 两条：① 缺口记入 `gapspan`（缺口后第一帧的文件位置 + 帧数），`readSubint` 用 `gapshiftAt(locate 的帧序号)` 取"时间轴位置早于本 subint"的部分；② `shiftFrameGaps` 的起始槽由 buffer 首个数据帧的帧号与 locate 的帧号之差决定（缺口盖住起点时 buffer 帧号连续、`reorder` 不会置起，必须按偏移判断才调用它）。判据 `fxcorr/test/gaps/run_boundary.sh` 两个场景；细节见 reader-model.md 4.5 与 7.4 |

**t25362 实测（2026-09-18，B5 修复后重跑）**：

- 读取位置：BA ds_0 的 `READPOS firstfno` 由修复前的 4026 回到 **4098**（该 subint 起点应对应的帧号，正是 reader-model.md 4.5 记的期望值）；无效块落位由外部帧号扫描核对——.sp 835 标缺口 1、2、836 标缺口 3、4，与真实缺口位置吻合（`.sp 索引 = buffer 编号 + 19`，4.5 末段的「+18」已订正）。
- **对拍仍未归零，但根因已定位并修好（2026-09-18 续查 + 修复）**：576 条差异（积分 0、4、10 各 192 条，全在 BA 站）中积分 0 的 192 条是 5e-5 的边界取整残差，积分 4/10 的差异集中在 ds_2。文件帧头扫描给出真值——ds_2 只有 **143 帧**真正缺失（filler 1162 帧占字节不占时间）。缺陷两处：① `gapshiftAt` 把缺口的文件偏移换算成时间槽时漏扣缺口之前的 filler（长 filler 段之后的缺口晚 `filler_before` 个槽才生效）；② 跳过的区段在**读取之后**才补扫，而段里的缺口会缩短正要用的那个位置。修法见 `reader-model.md` 4.7。**效果**：ds_2 的无效块 7170→1749（积分 4）、17720→1734（积分 10），真值 70/73（ds_0 参照 1129/1142）；合成判据 `fxcorr/test/gaps/run_filler.sh` 修前红（1071 对 169）、修后绿（169 对 169）。**对拍方向随之翻转**：ds_2 相关记录 fxcorr 0.9931 对基准 0.9754（积分 4）、0.9931 对 0.9800（积分 10），基准自身偏低 1.8%/1.3% 而真值只需 0.43%——mpifxcorr 侧 `vdifmux` 丢弃 filler 字节、其块有效性又是纯字节数判据所致，故 t25362 不能用 `cmp_swin.py` 全等验收。fxcorr 侧残留 ~40 帧/积分（filler 段收尾一个 subint）**性质未定**，「重读」修法已试并否决（2026-09-19，见 `reader-model.md` 4.7）。

### 验证方法与验收判据

- **合成数据**（`fxcorr/test/gaps/`，`FXSIM_GAPS` 造记录中断）：T1（filler 形式）与 T2（缺口形式）在**同一位置**、**帧号范围相同**；判据 = `GAPCHECK summary` 的 `missing frames` / `filler frames` 与外部帧号扫描逐一致。
- **真实数据**（t25362）：`filler frames` 逐 ds 对齐（BA 8 个 ds 为 0/0/1162/0/0/0/0/0）、`missing frames` = 143；`READPOS` 的 `firstfno` 序列符合时间轴期望；最终以 `cmp_swin.py` 对拍为准。
- **回归**：无缺口、无 filler 的数据路径**必须逐字节不变**（S6 全部产物、cmp5 目录、全部合成数据对拍）。
- **原盲区已补（2026-09-18）**：旧 gaps 判据只覆盖"缺口**被计数**"，不覆盖"缺口的**时间位置被放对**"——B5 正落在两者之间。`fxcorr/test/gaps/run_boundary.sh` 补上定位判据：`READPOS firstfno`（跨边界不盖住读取位置）与 `.sp` 零权重块区间（盖住读取位置），合成数据即可、无需 mpifxcorr 基准。实测：两个场景修前报红、修后转绿，且 `fxcorr/test/gaps/` 的 T1/T2 计数判据与无缺口产物的 md5 都不变。
- **有缺口的数据不能用 `cmp_swin.py` 判**：两边的 subint 窗口差一个 delay 修正，缺口与窗口的交叠不同，weight 必然不同；`run_boundary.sh` 之外的真实数据判据应落在 `READPOS` / `.sp` 的落位核对上。

---

## 相关

- 优先级与验收总览：v2-plan.md 第 5/6 节
- **读取路径的完整分析**（读模型对照、缺陷根因总表、症状指纹、诊断判据、改造建议）：`reader-model.md`——P10/P11/P12 是同一组件（fxcorr-f datareader）上先后三层的改造，分析集中于此，本文只记动机与实施记录。
- 数据接口变更（涉及 data-spec.md 的项）：P0（补 PCAL 文本格式说明，不改二进制格式）、P2（已取消，无变更）、**P7（autocorr.bin 增 crosspol 段与 header 字段，5.3 节）**、**P8（新增 beam.bin 数据类型 D14 与 5.5 节，上游无对照格式自定）**
