# fxcorr-xp 目录说明

**最后更新**：2026-10-13（**新建**——v9 链的 core 侧；设计见 `fxcorr/v9-plan.md`，
阶段 1 判据 3/5 已实测通过）

v9 链的站级+基线一体化工具（**x′**）：无 MPI，从 fxcorr-x 生长而来并**并入站级栈**。
每个 datastream 一个 `Mode`，数据来自 fxcorr-fp 的 **`stream.bin`**（1× 时间槽排布数据，
fxcorr/v9-plan.md §4）——原样重放 `setValidFlags`/`setData`/`setOffsets`，自己跑
`Mode::process`（unpack/pcal/FFT/旋转/frac sample/autocorr/kurtosis 全在 Mode 内部），
再走原样搬来的 **XmacEngine + Integrator** 出分片 `.part` / SWIN。所以 v8 链（f+x）与
v9 链（fp+xp）在同一数据上产出**逐字节相同**（判据锚，见测试节）。角色对照 mpifxcorr 的
core（站级处理 + 交叉乘），`fxmanager.cpp:333` 的"每份数据 FFT 恰好一次"在 xp 里表现为
**每 ds 单线程块序串行 process**。

## 调用方式

```
fxcorr-xp <batch_id> [workdir] [ds_group]      # 全量（写 SWIN）/ 分片（写 .part）
fxcorr-xp merge <batch_id> [workdir]           # batch 级归并 → merged.part
fxcorr-xp merge --experiment [workdir]         # 实验级归并 → SWIN（唯一写入者）
```

- 命令形态、参数语义、根解析（`FxcorrPath` 三根）、batch.json / `.input` 加载、
  `FXCORR_X_GROUPS_ONLY` 诊断出口、SWIN 互斥检查、`FXCORR_X_MERGE_FORCE`、
  `FXCORR_X_ALLOW_EMPTY` 空输入防呆、ds 组划分与分片掩码、DifxMessage manager 角色
  （mpiId=0、Starting → 每积分 RUNNING → Ending/Done）——**全部与 fxcorr-x 相同**。
- **数据输入**：`<FENGINE_ROOT>/<batch>/<g>/<station>/ds_<N>/stream.bin`（fp 写），
  不再是 `band_XX.sp`；header 与 `.input` 交叉校验（batch_id/station/ds/subintns/
  blockspersend/numbufferedffts/nsubints），口径同 x 当年校验 `.sp` header。
- **站级产物由本工具写**：`ds_<N>/pcal.bin`（fengine 目录内）与
  `vis/<exp>.difx/PCAL_<mjd>_<sec>_<station>_ds<N>`（PCAL 文本）——同格式、同位置、
  同幂等语义，原来由 fxcorr-f 写（v9-plan D6/D7）。`autocorr.bin` 与 `band_XX.sp`
  **取消**（谱与自相关在内存直取）。
- **挡掉的配置**（启动即报错，不静默；v9-plan D11）：多相位中心、pulsar binning、
  相位阵（阶段 2 后补）；`FXCORR_STA` / `FXCORR_KURTOSIS` 置位同样报错（STA/kurtosis
  的发送端从 f 搬来、阶段 2 移植）。zoom 与 crosspol 的代码已完整搬入（MemReader
  切片语义 / ac 记录 crosspol 段），阶段 2 专项验证。
- `FXCORR_LOGLEVEL` 同 fxcorr-x（log.h 为拷贝，改一处必须同步另一处）。

## 文件与来源对照

| 本目录 | 作用 | 来源 |
|---|---|---|
| main.cpp | 命令形态 / batch.json / 根解析 / merge 两子命令（**未改**）、挡掉项、站级阶段 + XMAC 阶段主循环、三档时间戳（采集点 station/read/zero/xmac/wacc/uvshift/cbw/auto/addsub，TSV prog 列 = `fxcorr-xp`） | 从 fxcorr-x/main.cpp 生长：删 beam 分支（相位阵挡掉）、删 `.sp` reader 构造与并行读段；新增站级阶段（见下）与产物收尾 |
| spreader.h | **纯接口**（`SpReader`：`numChannels/spectra/weights` 三个虚方法 + 虚析构） | 从 fxcorr-x/src/spreader.{h,cpp} 改造：文件读实现删除，只留 XmacEngine 实际调用的接口面——**xmac.cpp 因此一行未改** |
| memreader.{h,cpp} | `MemReader : SpReader`——一个 (ds, total band) 的谱视图，从 Mode 的槽直取 | 新写（对应 fxcorr-x 的 SpReader 文件读；带宽/槽语义与 f 的 `writeSpectraBand` 一致） |
| streamreader.{h,cpp} | `stream.bin` 顺序读：header 校验 + 每 subint 定长 record（status/validbytes/时间戳/flags/data） | 新写；格式 = fxcorr-fp StreamWriter 的镜像（两处 static_assert 定长） |
| acbatch.{h,cpp} | `AcBatchCollector`——每 (ds, subint) 的 ac 批次记录的**内存形态**（`averageFrequency` + 按 total band 取 ac/权重，zoom 权重映射到父带） | 新写；逐行对应 fxcorr-f 的 `FEngineWriter::writeAutocorrelationBatch` |
| integrate.{h,cpp} | `Integrator`：SWIN 积分/写盘、RUNNING 状态（**未改**）；`addAutocorrs` 改为**收内存记录**（`std::vector<std::vector<AutocorrBatch>>`），数值路径（resultindex/weightindex 递推、used-frequency 门、crosspol 续段）与文件读版逐行相同 | fxcorr-x/src 拷贝 + addAutocorrs 接口改造（v9-plan R4） |
| pcalwriter.{h,cpp} | `pcal.bin` 写盘（header + 每 subint tone 块），写盘判错三步照 f | 新写；实现从 fxcorr-f 的 `FEngineWriter::writePcal*` 摘出 |
| pcaltextwriter.{h,cpp} | PCAL 文本（intTime 累加、幂等替换、缩放公式） | **fxcorr-f/src 逐字节拷贝**（2026-10-13）；**改一处必须同步另一处**，回归 = 判据 5 |
| xmac.{h,cpp} | XmacEngine（交叉乘 + uvshift/average + 权重累加）——**xmac.cpp 一行未改**；xmac.h 仅注释更新 | fxcorr-x/src 拷贝 |
| log.h / ompcompat.h | 同 fxcorr-x | 拷贝 |
| （无 beamengine.{h,cpp}） | 相位阵被挡掉，beam 分支未搬入（fxcorr-x 保留原样） | — |

## 关键实现要点（易错，改前必读）

### 站级阶段 = fxcorr-f 的 [t=0 路径 + 归约段] 合并（位一致的根）

每 subint、每 ds（`#pragma omp parallel for` over ds，**每线程独占一个 Mode**）：

1. 重放 fp 的 record：`setValidFlags` → `setData(data, validbytes, scan, sec, ns)` →
   `setOffsets(scan, offsetsec, offsetns)`（顺序与 f 的 setup 段一致；`sec` 在无效
   subint 时是 `Mode::INVALID_SUBINT`，原样透传）；
2. 每 fftloop：`process(fftloop×nbf + b, b)` **块序串行**（slot = 全局块号，
   `setBlockSlotBase(0)`、`enableBlockAccumulators(blockspersend)` 在构造处调用——
   与 f 的 nthreads=1 路径同款）；
3. 紧随的**归约段**是 f 并行区归约的单线程版：ac/权重按 (crosspol, band) 的
   `getBlockAc`/`getBlockWeight` **逐块序**累回 `autocorrelation`/`getWeightReduced`；
   pcal 用 `getBlockPcal` → `addPcalBlock` 同序回填（extractor 不支持 per-block 时
   二者自动空转，process 已直接累加——与 f 的降级路径同语义）；
4. `MemReader::collectBand(fftloop)`（= f 的 `writeSpectraBand` 位置，槽被复用前）；
5. ac 批次边界（`maxacblocks`，公式同 f）：`pcaltext->accumulateWeight` →
   `AcBatchCollector::collect` → `zeroAutocorrelations`；subint 尾处理尾部 partial；
6. subint 尾（有 pcal 时）：`PcalWriter::writeSubint`（内部 `finalisepcal`）→
   `pcaltext->accumulate` → intTime 边界 `flush`（时间基准 = `batchstartabsns`，
   与 f 同式）。

**D8 的另一半**：XMAC 阶段（`xmacBatch`/`accumulateWeights`/`uvshiftAndAverage`/
`copyBaselineWeights`）是 x 现役代码原样，在每个 subint 的站级阶段**之后**串行执行；
`addAutocorrs` 消费本 subint 的 `acrecords`。

### 其它易错点

- **槽索引 vs 块索引**：`getFreqs`/`getDataWeight` 的第二个参数是**槽号**（b），
  `getBlockAc`/`getBlockPcal` 的 block 是**全局块号**（index）——混用即静默错数据。
- **MemReader 的 zoom 语义**：Mode 内部 `fftoutputs[zoomband]` 就是父 recorded band
  的切片指针，所以 MemReader 直接用 `getFreqs(band, slot)`、nchan 取 zoom freq 的
  通道数——**不需要** x 当年 SpReader 的 channeloffset 逻辑。
- **ac 记录 = autocorr.bin 的内存镜像**：`AutocorrBatch` 的 ac/acx 是 total band 顺序
  的扁平拼接、w/wx 每 total band 一个（zoom 已映射到父带权重）；`addAutocorrs` 的
  resultindex/weightindex 递推与 crosspol 续段照 X 现役文本逐行移植。
- **pcal.bin 与 PCAL 文本的次序**：`accumulateWeight` 必须在 `zeroAutocorrelations`
  之前；`pcaltext->accumulate` 必须在 `writeSubint`（即 `finalisepcal`）之后——与 f
  的 main 逐处对应。
- **`getWeightReduced`/`setWeightReduced`** 读写 `weights[cp][band]`（ac 聚合权重），
  与 `getDataWeight`（写 .sp/谱视图用的每块权重）**不是同一个数组**。
- **挡掉的位置**：restriction 段在多相位/pulsar/相位阵/STA/kurtosis 上启动即 fail——
  注意它**在 reader 构造之前**，但根一致性检查与 batch.json 读取在其前。
- **时间戳探针**：`g_probes` 的行号必须与 `STAGE_LAP` 站点同步（brief 档输出
  `file:line`）；加行/删行后重新 grep 回填。

## 构建与注册

- `configure.ac` 照 fxcorr-x（fxcorrcommon + fftw3f，**含 AC_OPENMP**——ds 级站级并行；
  无 mark5access，不读 VDIF）。
- install-difx 注册 4 处（components 字典、setNormalComponentsFalse、apptargets、
  --doonly 帮助文本）——2026-10-13 已加。
- 测试机上裸构建：`autoreconf -i && ./configure --prefix=/usr/local/difx && make
  && make install`。镜像（`fxcorr/docker/Dockerfile`）的 COPY 行待镜像重建时加。

## 测试（2026-10-13，v9 阶段 1）

样本副本均在 `/root/fxcortest/`（每个 = 原 workdir 的 `cp -r`，清产物后两条链各跑
一遍；`.input` 的 `OUTPUT FILENAME` 已改指副本自己的 `vis/`——原值是指向源 workdir 的
绝对路径，直接跑会把 PCAL 写回去）。两条链均 `OMP_NUM_THREADS=1`。

- **判据 3（分片 .part 逐字节，v8 x 分片 vs v9 xp 分片）**——三个样本全过：
  - `v9pcal`（p4 副本：2 站 T1/T2 单组、4 tones pcal、4 subints）：
    `ds0.part` 一致（md5 `f7769313…`）；
  - `v9mds`（mds 副本：每站双极化 2 ds、共 4 ds、无 pcal）：一致；
  - `a1`（**病态数据**：含缺口/filler——病态重放路径一并覆盖）：一致。
- **判据 5（pcal.bin + PCAL 文本逐字节）**——`v9pcal` 上全过（两份 pcal.bin、两份
  PCAL 文本，均逐字节一致）。
- **判据 4 提前验证**（阶段 2 的锚，顺手跑掉）：`v9pcal` 全量模式（无 ds_group）的
  SWIN 与 v8 全量 x 的 SWIN **逐字节一致**（197,052 B）。
- **挡掉实测**：`FXCORR_STA=1` / `FXCORR_KURTOSIS=1` 报错退出（rc=1，消息含
  "not implemented in v9 stage 1"）；pulsar、相位阵配置在其 workdir 上启动即报
  "stage-2 item"。**多相位中心未实测**（测试机的多相位资产 .im 已被覆盖为单源，造
  合法双源配置需要 vex2difx——测试机没有；代码与 pulsar/相位阵同段同通路）。
  顺带确认：非法配置（pointing centre 不是第一个相位中心）下 xp 的 rc=139 段错误
  是 **fxcorr-x 既有行为**（同输入下 fxcorr-x 同样 rc=139），非 v9 引入。
- 未实测项（阶段 2）：zoom、crosspol、STA/kurtosis、ds 级并行的多线程位数一致性
  （阶段 1 判据均为 OMP=1；站级并行的位一致机理 = 每 ds 单 Mode 字块序，与线程数无关，
  但多线程实测留给阶段 2）、内存实测。
