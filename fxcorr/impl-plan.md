# fxcorr V1 实施方案

需求与架构见 `README.md`，数据接口规范见 `data-spec.md`（v1.1）。本文把 V1 落到可执行的组件划分、代码搬移与改造点。

---

## 1. V1 范围（边界）

**做**：两个串行程序跑通单 batch；正确性优先；与 mpifxcorr 逐位对拍（SWIN 同格式直接比）。

**不做**（V2+）：
- 多相位中心（uvshift 仅单相位中心路径，uvshiftAndAverage 简化）
- zoom band（.sp 只落 recorded band，x 侧不做切片）
- 脉冲星 binning（binloop 恒 1）
- 多线程（numprocessthreads=1）、网络输入、数据流化
- `PCAL_*.pcal` 文件生成（pcal 数据由 f 落盘 pcal.bin，文件生成留 V2）
- ASCII 输出（只 SWIN）；difxmessage 状态/STA 消息（组播可留 V2，写盘主路径不含）

---

## 2. 组件划分与源码清单

### 2.1 libraries/fxcorrcommon（共享库，新建）

照 `libraries/mark6meta` 模板（configure.ac + Makefile.am + fxcorrcommon.pc.in + src/Makefile.am）。

**源文件**（全部从 `mpifxcorr/src/` 拷贝，= 现 libfxcorr.a 清单，Makefile.am:106-117）：

| 文件 | 用途 | 需改 |
|---|---|---|
| configuration.{h,cpp} | .input 解析（非 MPI 构造 configuration.cpp:98-137 已现成） | 去掉 MPI 构造分支 |
| model.{h,cpp} | .calc/.im 延迟模型（UVW 求值） | 无（零 MPI） |
| mode.{h,cpp} | 解包/条纹旋转/小数采样/FFT/自相关 | 落盘改造（见 3.2） |
| mk5mode.{h,cpp} | Mark5/VDIF 解包 | 无 |
| pcal.{h,cpp} | 脉冲校准提取 | 无（零 MPI） |
| polyco.{h,cpp} | 脉冲星权重 | 无 |
| visibility.{h,cpp} | SWIN 写盘（visibility.cpp 仅 include mpi.h，无 MPI 调用，可整体复用） | 去 mpi include |
| mathutil.* / sysutil.* | 工具 | 无 |
| datamuxer.{h,cpp} | 数据流复用 | 无（依赖 mark5access） |
| alert.{h,cpp} | 日志 | 无 |

**附带头**：`architecture.h.in`（向量宏/类型：cf32、vectorAlloc/AddProduct 等）、`mpifxcorr.h`（FLAGS_PER_INT=30 等常量）一并进 fxcorrcommon 的 include。

**依赖**（configure.ac 的 PKG_CHECK_MODULES，按实际编译需求收敛）：difxmessage >= 2.9.0、mark5access >= 1.7、vdifio >= 1.6、**codifio >= 0.2**（configuration.cpp 无保护使用 `CODIF_HEADER_BYTES`，实测必需）；mark6sg/mark5ipc/dirlist/streamstor 在 11 源文件零引用，不探测；IPP 可选否则 fftw3+fftw3f（架构宏沿用 architecture.h.in 的映射）。

### 2.2 applications/fxcorr-f（新 C++ 应用）

模板照 `applications/difxfilterbank`（多依赖 C++）。

| 文件 | 内容 | 来源/改造 |
|---|---|---|
| main.cpp | 参数解析（batch_id、station、config 目录）；循环 scan/subint 驱动 | 新写 |
| datareader.{h,cpp} | 无 MPI 数据读取：顺序读本地 raw 文件、按 subint 切块、**粗延迟**按采样偏移 | 简化改造 datastream.cpp（V1 不含环形缓冲/pthread/网络）；参照 mk5mode.cpp 的 unpack 入口 |
| fenginewriter.{h,cpp} | .sp / pcal.bin / autocorr.bin 写盘（格式见 data-spec 5.3） | 新写 |

**主流程骨架**：

```
Configuration config(input, 0)            // 非 MPI 构造（现成）
Model model（config->getModel()）          // 读 .calc + .im
读 batch.json → 定 subint 范围（对齐校验见 data-spec 12 节）
for subint in batch:
  for datastream in station:
    reader 按粗延迟偏移读 blockspersend 个 FFT 块（每块 fftchannels 采样）
    填 controlbuffer 语义：[0]=scan [1]=sec [2]=ns [3..]=valid flags
    mode[j]->setOffsets/setData/setValidFlags      // 与 core.cpp:699-701 同构
    for fftsubloop: mode[j]->process(i, fftsubloop) // = core.cpp:798，内部已含小数采样/条纹旋转/FFT/自相关
  fenginewriter: getFreqs()[band][subloop] 逐块落盘 .sp
                  finalisepcal() + getPcal() → pcal.bin
                  averageFrequency() 后 getAutocorrelation() → autocorr.bin
  batch.json 写 status
```

要点：
- **粗延迟**：现 DataStream 在读取层按 .input 的延迟做采样级移位（`datastream.cpp` 的 sendbytes 切分逻辑），fxcorr-f 的 datareader 按相同公式算起始采样偏移后顺序读——对照 `datastream.cpp` 的延迟计算移植。
- **Mode 改造**（在 fxcorrcommon 的 mode.cpp 内）：`process()` 不变，新增/调整落盘接口：f 侧每 subloop 从 `fftoutputs[band][subloop]` 拷出 `recordedbandchannels` 个 cf32 写出；x 侧读回后按需 `vectorConj_cf32` 得到共轭（.sp 不存共轭副本）。
- valid flags 位域（30 位/字）与 dataWeight 语义完全沿用现实现（mode.cpp:656-669、mode.h:158）。

**实施记录（2026-09-11 完成，与计划偏差见下）**：

- **Mode 零改造**：`Configuration::getMode()` 工厂与 `getFreqs`/`getPcal`/`getAutocorrelation`/`getDataWeight` 均现成 public，fxcorrcommon 的 mode 未做任何落盘改造。
- **process 槽复用**：`process(i, subloop)` 的 subloop 是 NUM BUFFERED FFTS 缓冲槽而非 FFT 序号，主循环必须照 core.cpp:786-801 双循环（fftloop × numBufferedFFTs），每 fftloop 结束立即写盘该批槽（槽会被下一批覆盖）；fenginewriter 因此拆为 `writeSubintHeader`（scan/sec/ns + flags + weights）+ 每 fftloop 一次 `writeSpectra`。
- **datareader V1 边界**：本地 VDIF、单 mux thread、无 fanout、帧粒度 1、文件序号=scan（单 scan 实验）；sendbytes 直接用 `getDataBytes`（已帧对齐含 guard）；定位公式照 datastream.cpp:381-394 + vdiffile.cpp:417-444（framesin = 延迟校正采样偏移/payloadbytes 向下取整，文件偏移 = framesin×framebytes）。
- **对齐校验**：batch 起点必须落在 subint 边界（容差 1µs，吸收 start_mjd 的 f64 表示误差），非对齐报错退出（验收标准 4）。
- 验收标准 1 的 f 侧已达成（2 站 4 秒数据 7 subint 跑通，tone 峰位置正确）；对拍（验收 2）待 fxcorr-x。

### 2.3 applications/fxcorr-x（新 C++ 应用）

模板同上。**核心计算从 `Core::processdata()` 拆出**（core.cpp）：

| 来源段 | 内容 | 去向 |
|---|---|---|
| core.cpp:814-982 | XMAC 主循环（f→x stride→baseline→subloop→pol，vectorAddProduct_cf32 :963） | xmac.cpp |
| core.cpp:1005-1052 | baselineweight 累加（weight1×weight2） | xmac.cpp |
| core.cpp:1431 起 uvshiftAndAverage | 单相位中心简化版（无 differentialdelay 旋转，仅频谱平均） | xmac.cpp |
| core.cpp:985-1003 / 1166-1378 | maxxcblocks/maxacblocks 触发、自相关平均与 SWIN 自相关段 | integrate.cpp |
| fxmanager.cpp:168-185 / 650-698 | Visibility 缓冲管理、addData/writedata 循环（写盘循环的串行替代） | integrate.cpp |
| visibility.cpp（整文件） | SWIN 写盘（已在 fxcorrcommon，直接调用） | 不改 |

**主流程骨架**：

```
Configuration config(input, 0)；Model model
读 batch.json + 各站 .sp header → 建站级缓冲（读全部 subint 进内存或流式逐 subint 读）
分配 threadcrosscorrs（长度 = config 的 threadresultlength 预算，configuration.cpp:2429-2453 现成）
分配 Visibility（构造参数照 fxmanager.cpp:177，numvis/dbuffer 自行预算）
for subint in batch:
  读各站各 band 数据块 → 构造 vis1/vis2 指针
  XMAC 循环（照 core.cpp:816-982，含 stride 切分、subloop 循环、pol 循环）
  baselineweight 累加（照 core.cpp:1005-1052）
  xcblockcount 达 maxxcblocks → 平均（vectorAddProduct 结果 / nfft）→ 进 Visibility 的 results
  自相关段 → 平均 → 进 results（照 core.cpp:1166-1378）
  时间推进（nsincrement = getSubintNS）→ addData → 满 intTime → writedata()
写 batch.json（x 版）+ status
```

要点：
- **UVW 与 SWIN 头**：`writeSWIN` 内部已用 `model->interpolateUVW()` 在积分中点求值（visibility.cpp:855），x 只需保证 Model 已构造（.calc + .im），主计算路径不碰 UVW——与现实现一致。
- **共轭**：`vectorAddProduct_cf32(vis1, vis2, ...)` 中 vis2 原本取自 `getConjugatedFreqs()`；fxcorr-x 读入 .sp 后整段 `vectorConj_cf32` 再取用。
- **积分边界**：`subintsthisintegration = intTime*1e9/subintNS`（visibility.cpp:96）；batch 时长 = intTime 整数倍（data-spec 12 节）保证跨 batch 追加不碎片化。
- **结果数组布局**：`threadresultfreqoffset / threadresultbaselineoffset / completestridelength` 预算全部沿用 configuration.cpp:2429-2453——x 侧不重造索引体系，只替换"数据来源"（MPI 消息 → .sp 文件）。

**实施记录（2026-09-11 完成，与计划偏差见下）**：

- **文件划分**：spreader.{h,cpp}（.sp 读取，每站每 band 一个 SpReader，顺序 fseek 读 subint）、xmac.{h,cpp}（XMAC 批循环 + baselineweight + uvshiftAndAverage 简化版，照 core.cpp:814-1052、1005-1052、1431-1938 删 pulsar/多相位中心/锁/线程切分）、integrate.{h,cpp}（单 Visibility 管理 + autocorr.bin 累加，替代 FxManager 的环形缓冲+写线程）、main.cpp（batch.json、逐 subint 驱动）。
- **单 Visibility 零改造**：构造照 fxmanager.cpp:177（numvis=1）；todiskbuffer 预算照 fxmanager.cpp:114-133；写盘时序 = addData 满 intTime → writedata → increment；可见度/自相关校准与 SWIN 落盘全部复用（writedata 内从 floatresults 现算 baselineweights/autocorrweights）。
- **自相关批次化（关键修正）**：mpifxcorr 的自相关按 maxacblocks 批次（core.cpp:993-1003）平均并累加，而非每 subint 一次——原计划"每 subint 一条 autocorr"与上游节奏不等价。改为 autocorr.bin 每 subint 存 ac_batches = ceil(blockspersend/maxacblocks) 条记录（header 增加 ac_batches 字段），f 侧每批次 averageFrequency+落盘+zeroAutocorrelations，x 侧逐条累加（integrate.cpp）。
- **fxcorr-f weights 槽语义 bug（本阶段修复）**：`Mode::getDataWeight(band, slot)` 的 slot 是 NUM BUFFERED FFTS 缓冲槽且仅在 process 后有效，fxcorr-f 原实现按全局 FFT 序在 process 前写盘（越界读+时点错误）→ 改为 writeSpectra 时按槽收集、subint 尾 flushWeights() fseek 回填占位区（.sp 布局不变）。
- **V1 边界（main 启动检查）**：intTime 为 subintNS 整数倍（offsetnsperintegration==0，dump 网格 = subint 网格）；单相位中心；maxproducts≤2（无 cross-polar autocorr）；无 pulsar/phased array。
- **验收 1、2 达成（2026-09-11）**：2 站 4 秒小实验（vex2difx+difxcalc → fxcorr-f → fxcorr-x → difx2fits 出 FITS）。对拍：同数据同配置 mpifxcorr，SWIN 前 6 条记录（2 个完整积分）逐记录全等——头字段全等、可见度相对误差 <1e-6、weight 精确一致（0.9892578125 逐位吻合）。对拍工具 `fxcorr/test/cmp_swin.py`。
- **mpifxcorr mux 滞后（对拍发现，非 fxcorr 错误）**：mpifxcorr 的 vdifmux 流式管线存在确定性可见滞后（读线程+mux 与 main 竞争），数据后段边界 subint 被标 invalid（本次实验 subint 5 只 50/512 块有效、subint 7 208 块，两次运行完全可复现）；fxcorr 的 fseek 直读无此滞后。对拍应在数据完整覆盖的积分段进行（batch 时长 ≤ 数据时长 − 一个 subint 余量）。

### 2.4 fxcorr/（bash 编排）

`run_batch.sh`：校验 batch 对齐 → 逐站调 fxcorr-f → 调 fxcorr-x → 更新 `meta/batches.index`。
`watch_and_dispatch.sh`：轮询 raw/，齐套时间窗后生成 batch_id（MJD_秒，对齐 subint）→ 调 run_batch.sh。

---

## 3. 关键改造点清单（按依赖序）

1. **fxcorrcommon 建库**：11 源文件拷贝 + architecture.h.in/mpifxcorr.h 头 + mark6meta 式 4 手写构建文件；configuration.cpp 删 MPI 构造与 MPI_Bcast 分支；mode/mk5mode/visibility 删 `#include <mpi.h>`；另删 3 个遗留 include（visibility.{h,cpp} 的 datastream.h、visibility.cpp 的 core.h、mk5mode.cpp 的 mk5.h），补显式 include（visibility.h→configuration.h；visibility.cpp→mode.h+cassert）。
2. **fxcorr-f datareader**：移植 datastream.cpp 的延迟/切块公式（对照 :745-756 sendbytes、采样时间 500.0/bandwidth µs），实现本地文件顺序读。**（已完成，见 2.2 实施记录）**
3. **fxcorr-f fenginewriter**：按 data-spec 5.3 的 .sp/pcal.bin/autocorr.bin 布局实现写盘与 header 填充。**（已完成，见 2.2 实施记录）**
4. **fxcorr-x xmac.cpp/integrate.cpp**：从 core.cpp 拷贝段改造——删 MPI 收发（core.cpp:596/302）、删 pulsarbin 分支（binloop=1）、删多线程切分（startblock=0, numblocks=blockspersend）、uvshiftAndAverage 取单相位中心路径。
5. **install-difx 注册**：components 字典 + setNormalComponentsFalse + libtargets（fxcorrcommon）+ apptargets（fxcorr-f、fxcorr-x）+ --doonly 帮助文本（4 处，见根 CLAUDE.md）。
6. **fxcorr/CLAUDE.md 同步**：核心约定节按 data-spec v1.1 更新（vis 布局、pcal/autocorr 文件）。

---

## 4. 验收标准（V1 完成判据）

1. **跑通**：小实验（2 站、单 band、单偏振、≥2 个 intTime 时长）单 batch 全流程：vex2difx+difxcalc → fxcorr-f → fxcorr-x → difx2fits 出 FITS，无崩溃、batch.json 状态正确。
2. **对拍**：同配置同数据跑原 mpifxcorr，SWIN 文件逐记录比较（header 字段全等；可见度复数与 weight 相对误差 < 1e-6，容浮点运算顺序差异）。
3. **回归**：`pcal.cpp` 的 -DUNIT_TEST 独立测试通过（模式样板）；`libraries/` 其他库不受影响（install-difx 常规构建全绿）。
4. **对齐校验**：batch 起点非 subint 边界时 fxcorr-f 明确报错退出（防御 data-spec 12 节）。

---

## 5. 风险与备选

| 风险 | 影响 | 备选 |
|---|---|---|
| datareader 粗延迟移植误差 | 对拍失败 | 对照 datastream.cpp 逐行移植 + 构造已知延迟的单 tone 测试信号 |
| .sp 内存占用（整 batch 常驻） | 大 batch OOM | V1 流式逐 subint 读 .sp（x 侧），文件布局已支持顺序读 |
| visibility 构造依赖 FxManager 的缓冲预算 | 编译/运行差异 | 缓冲长度按 fxmanager.cpp:107-139 公式移植 |
| IPP/FFTW 架构宏随 architecture.h.in 迁移 | 编译错误 | 保持与 mpifxcorr 同款 configure 探测（configure.ac 移植） |
