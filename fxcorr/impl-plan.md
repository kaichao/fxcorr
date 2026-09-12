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
- 容器化与 scalebox 编排（V2：scalebox 的 Module 需容器镜像，镜像制作是接编排的前提；上游 docker/ 为 EOL CentOS8，不沿用）
- 仿真数据分布式生成与流水线组装（V2：串行生成器 + scalebox 按（站, batch）分片调度）
- 流式监视（原计划 watch_and_dispatch.sh 砍掉，流式场景推迟到 V2 由 scalebox 承担）

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

### 2.4 fxcorr/（bash 编排与仿真数据）

**编排三脚本**（本目录，配套 data-spec 目录布局；规格细化如下，实现时照此做）：

**make_testdata.sh** —— 构建 data-spec 布局的标准测试数据。

```
./make_testdata.sh [workdir] [tone_mhz ...]     # 环境变量 FXSIM_NOISE / FXSIM_SEED 透传
```

步骤：① config/ 的 .vex/.v2d → `vex2difx` → `difxcalc` 出 .input/.calc/.im（产物已存在则跳过，幂等）；② 从 .input 推导 batch 参数（START MJD/SECONDS、subintNS），`batch_id = MJD_秒`（START 秒向下取整）；③ 写 `batches/<batch_id>.json`（全字段一次写全：时间/结构 + stations + 可选 station_groups + baselines/integration_sec/n_channels/polarizations/difx_dir，start_mjd 写精确 repr）；④ 逐站 `fxcorr-sim <batch_id> <station> <workdir> [tone_mhz...]` 生成 raw VDIF；⑤ 软链 `raw/<station>/<station>_<batch_id>.vdif` 到 .input DATA TABLE 文件名；⑥ stdout 打印 batch_id。多 batch（验证 SWIN 跨 batch 追加，data-spec 12 节）用 `-n N` 选项生成连续 N 个 batch 的 batch.json，数据生成逐 batch 重复 ④。

**make_testdata.sh 实施记录（2026-09-12 完成，fxcorr/make_testdata.sh）**：

- 实现偏差与决策：① config 资产缺才从 `fxcorr/test/` 复制（test.vex/test.v2d），vex2difx/difxcalc 在 config/ 内跑（vex= 路径相对 cwd）；**单 batch 直接用 difxcalc 原产物 test.input**（SUBINT 0.524288s、INT TIME 1.048576 = 6/6 对拍同配置），**仅 `-n N` 多 batch 时 sed 出 test-sim.input 变体**（SUBINT 128000000、INT TIME 0.256）——多 batch 连续切分起点须 VDIF 帧边界，0.524288s 网格与帧网格公倍数 65.5s 不可用，128ms = 32 帧对齐（v2d 的 subintNS/tInt 无法直接指定：vex2difx 不认 subintNS 关键字、tInt 会被 difxcalc nudge）。② 幂等：前处理产物存在跳过、VDIF 存在跳过；前处理工具输出重定向 stderr（stdout 只留 batch_id）。③ `-n N`：batch_i 起点 = scan 起点 + i×duration，n_subints 自动提升到每 batch 时长 ≥ 1s（batch_id 秒 floor 唯一）；软链指向最后 batch 并提示。④ batch.json 的 calc_file/im_file 从 .input 的 CALC FILENAME 推导（sed 变体不改它）。
- 实现 -n 多 batch 时发现并修复的三个既有 bug：**fxcorr-sim 整秒 snap 放宽**为帧边界整除（vdifwriter 帧号从秒内偏移起算，1µs 内近整秒仍归整秒——simcmp BYTE-IDENTICAL 回归通过）；**fxcorr-f 数据文件起点 = batch 起点**（原按 scan 起点定位，多 batch 偏移后读错位置；修复后 batch 1 对拍回归 6/6）；**fxcorr-x executeseconds 加 initsec 偏移**（原停写判定按 scan 起点基准，batch 起点偏移时静默不写盘）。
- 实测踩坑（均写入 applications/*/CLAUDE.md 或 memory）：**mpifxcorr vdifmux 读不了带噪 2bit 数据**（FXSIM_NOISE>0 时 databytesperpacket 错乱、0 积分输出，NOISE=0 正常）——对拍数据须 `FXSIM_NOISE=0` 生成；f 数据块时间与 batch 起点换算的坐标系混用（当日秒系 vs scan 相对系）曾导致 .sp weight 全 0。
- 验收（测试机，全部通过）：全新目录跑通单 batch 全流程（config/raw/batches/软链/batch.json 字段全对）；`-n 2` 两个 batch.json 时间连续（start_mjd 差 1.024s）、n_subints 自动 8、软链指最后 batch；batch 2（起点 scan+1.024s）f/x 全链路 4 积分 12 条；NOISE=0 数据 test.input 配置与 mpifxcorr cmp_swin **6/6 全等**；fxcorr-sim simcmp 位序 BYTE-IDENTICAL 回归。

**run_bench.sh** —— mpifxcorr 基准（对拍基准生成器，不是独立产品线）。

```
./run_bench.sh [workdir]
```

步骤：① 复制 `config/<exp>.input` → `bench/`，EXECUTE TIME 截断为完整覆盖段（≤ 数据时长 − 1 subint，避开 mpifxcorr vdifmux 滞后，见 fxcorr-f CLAUDE.md）；② `mpirun --allow-run-as-root -np 4 mpifxcorr bench/<exp>.input`；③ 基准 SWIN 落 `bench/<experiment>.difx/`；④ 打印对拍提示（`cmp_swin.py` 用法）。

**run_bench.sh 实施记录（2026-09-12 完成，fxcorr/run_bench.sh）**：

- batch 定位：DATA TABLE 软链 target 解析 `<station>_<batch_id>.vdif`（-n 多 batch 时软链指最后 batch，精确对应数据），无软链时 fallback batches/ 下 mtime 最新 json。读 batch.json 取 config_file/n_subints/subint_ns/integration_sec，与 fxcorr 侧同配置是两侧积分数一致的前提。
- **EXECUTE TIME 截断公式**（mpifxcorr writedata 以积分起点 + scanstartsec ≥ executeseconds 停写，且 EXECUTE TIME 为整秒字段）：`exec = floor(initsec + (N−1)×intTime) + 1`，N = batch 时长/intTime（不整除即报错退出）。积分 1..N 起点 < exec 全写、积分 N+1 起点 ≥ exec 停；与 6/6 对拍实测 EXECUTE TIME=2 反推一致。**batch 时长 < 1s 时无整秒解**（如 0.512s 场景 mpifxcorr 会多写 weight-0 积分）——默认配置（n_subints=4 × 0.524288s = 2.097s）无此问题。
- OUTPUT FILENAME sed 改指 `bench/<exp>.difx`（与 fxcorr 侧 OUTPUT 目录分开落盘）；DATA TABLE 相对路径靠 cwd 在 workdir 内跑；mpifxcorr 拒绝覆盖已有 SWIN，重跑前 rm -rf 输出目录（幂等）。
- **发现 vdifmux 帧号 bit7 错读**（mpifxcorr 读端，fxcorr 无）：128ms 帧对齐 subint 数据在帧号 0x80..0xFF（128..255，256 帧周期 = 1.024s）段被误判乱序 → 丢帧错乱（伴随 frameheadersize 32→16 误检测、databytesperpacket 垃圾值），帧号 0x100 恢复。实验证据：坏段随 batch 起点帧号平移（帧号 0 起坏段在 [0.512,1.024]s、帧号 32 起在 [0.384,0.896]s）、与生成器/tone/噪声无关；0.524288s 非帧对齐 subint 网格不触发（6/6 全等）。**决策（方案 A）**：对拍统一 test.input 配置，fxcorr-sim 时长帧整除校验放宽为生成到帧边界取整（文件尾部多半帧，f 只读 batch 段）；128ms 变体仅用于 -n 多 batch（fxcorr 侧自测，不与 mpifxcorr 对拍）。
- 验收（测试机，全部通过）：mktd3 默认单 batch（test.input 配置、525 帧）run_bench EXECUTE TIME=2 + fxcorr 全链路 cmp_swin **6/6 全等**；mktd4 `-n 2` 回归（test-sim 变体、batch.json 连续、batch 2 全链路 4 积分）；nonsec 坏段平移实验定稿根因。

**run_batch.sh** —— fxcorr 流水线（V1 静态数据集，手工/编排器均可调）。

```
./run_batch.sh <batch_id> [workdir]             # station 列表自动取 .input 全部 datastream
```

步骤：① 读 `batches/<batch_id>.json` + .input，前置校验对齐（batch 起点 subint 边界、时长 intTime 整数倍，容差同 fxcorr-f；不通过则报错退出，不依赖 fxcorr-f 兜底）；② 置 status=running（batch.json status 字段，无独立 status.txt）；③ 逐站 `fxcorr-f <batch_id> <station> <workdir>`，任一失败 → status=failed、非 0 退出，不跑后续站；④ mkdir .input OUTPUT FILENAME 所在目录；⑤ `fxcorr-x <batch_id> <workdir>`，失败同 ③；⑥ 成功 → status=done，追加 `meta/batches.index` 一行 `<batch_id>,done,<时间戳>`。

**run_batch.sh 实施记录（2026-09-12 完成，fxcorr/run_batch.sh）**：

- 规格 6 步照做，两个实现偏差：① **DATA TABLE 软链每次重指本 batch 的 VDIF**（make_testdata.sh 多 batch 时软链停在最后 batch、其注释承诺由本脚本重做，规格未列但多 batch 正确性必需；raw 数据不存在即报错，跑 fxcorr-f 前就挡）；② 前置校验在规格两条之外加一条 **INT TIME 为 subint 整数倍**（fxcorr-x 的 offsetnsperintegration==0 硬校验，脚本端提前挡）。站列表取 batch.json stations（缺失时从 .input DATASTREAM 解析）。
- 前置校验实现与程序内校验同语义：batch 起点 subint 边界 1µs 容差照 fxcorr-f main.cpp:175-182（`floor(initsec×1e9+0.5) % subintns` 距 0/subintns ≤ 1000ns）；batch 时长为 intTime 整数倍照 run_bench.sh 公式。
- status 流转：running→done/failed 写回 batch.json（json.dump 保字段序与 make_testdata.sh 一致）；done 时追加 `meta/batches.index` 一行 `<batch_id>,done,<UTC 时间戳>`（追加语义，每次成功一行）。
- 验收（测试机 mktd5，全部通过）：正常路径 status running→done、两站 fxcorr-f、fxcorr-x 2 积分、batches.index 追加；失败路径①坏 start_mjd 前置校验报错退出非 0（偏移 7167726 ns 被检出）；失败路径②假 fxcorr-f（stub exit 1）→ status=failed、exit=1、不跑后续站；重跑恢复流转 failed→done。**闭环对拍**：run_bench.sh 基准 vs run_batch.sh 产出 SWIN 前 6 条 cmp_swin **全等**（fxcorr-x 追加写盘语义，重跑同 batch 记录累积，对拍取前 N 条或清 OUTPUT 目录）。

`watch_and_dispatch.sh` **砍掉**：V1 数据集为静态构建，无"轮询 raw/ 发现新数据"场景；流式监视与多节点调度推迟到 V2 由 scalebox 承担（届时 run_batch.sh 的调用改由编排器发出，脚本本身不变）。

**仿真数据生成器 fxcorr-sim**（`applications/fxcorr-sim`，独立 C++ 串行应用；上游 datasim 因 subband.{h,cpp} 硬编码 IPP 无法 --noipp 构建，此为其替身）：

- **应用落位与复用**：autotools 应用模板照 fxcorr-f（configure.ac PKG_CHECK_MODULES: fxcorrcommon），install-difx 注册 4 处；读 .input 复用 fxcorrcommon 的 Configuration（非 MPI 构造）——与 fxcorr-f 同一份解析语义，分批次对齐的根基。
- **编排器无关**：纯文件/目录接口，不感知调度器——手工、xargs -P、scalebox 三种方式均可驱动。
- **分片参数化**：按（站, batch 时间窗）参数化，天然分片单元 = 未来 scalebox 的 Task 粒度。
- **分批次对齐**：读 .input 的 subint 结构，batch 时间窗与 subint 网格对齐（data-spec 12 节约束前置到数据产生端）；输出命名 `<station>_<batch_id>.vdif`。
- **分布生成正确性**：多节点并行生成时，各分片的 VDIF 帧时间戳/帧号必须全局连续——作为程序内校验点，不依赖调度器保证。
- V1 范围：tone + 高斯噪声 + pcal tone 注入（pcal.bin 链路对拍的前提，该链路至今未验证）+ 多 band。
- **位序验证**：2bit 打包位序沿用 gen_test_vdif.py 已验证约定（对齐 mark5access lut2bit），C++ 实现与其同参数输出逐字节对拍。
- 上游 datasim 的 IPP 修复（subband.h 13 处类型/签名 + subband.cpp 5 处调用，DFT/复乘换 fftwf）可作为独立小贡献，不绑进主路线。

**实施记录（2026-09-12 完成，applications/fxcorr-sim 已建成）**：

- 源码三件套：main.cpp（batch.json/对齐/帧时序/pcal 网格）+ signalgen.{h,cpp}（tone/噪声/pcal 合成 + 2bit 量化打包）+ vdifwriter.{h,cpp}（帧封装与时序连续性），vdif_header 位域头复用 vdifio.h（仅头文件，不链接 vdifio 库）。
- 实测踩坑（均写入 applications/fxcorr-sim/CLAUDE.md）：① 采样率须从帧结构反推（getFramePayloadBytes×4×getFramesPerSecond），FREQ 表带宽≠采样率（4MHz band 以 8Ms/s 记录）；② FREQ 表 getter 单位是 MHz、pcal tone getter 单位是 Hz，须 ×1e6 统一；③ bytespersample 校验含 band 数因子（num/denom = nbands/4）。
- 验收（测试机，全部通过）：位序与 gen_test_vdif.py 同参数输出 **BYTE-IDENTICAL**（1024 帧，对拍同时修复 Python 版帧号公式 bug）；单 band 全链路（fxcorr-sim→f→x）与 mpifxcorr cmp_swin **6/6 全等**；pcal 链路首次跑通（PHASE CAL INT 1MHz → 4 tones 201-204MHz 检出）；多 band（test2b.vex/v2d，2×4MHz）f/x 全链路跑通、tone 峰落位与可见度峰正确——mpifxcorr 读不了 2 band VDIF（vdifmux 帧结构识别异常，读端限制），2 band 对拍以物理验证为准。

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
3. **回归**：`pcal.cpp` 的 -DUNIT_TEST 独立测试通过（模式样板）；`libraries/` 其他库不受影响（install-difx 常规构建全绿）。**（✓ 已执行 2026-09-12：① pcal UNIT_TEST——`g++ -DUNIT_TEST -DPCAL_DEBUG -I. -I$DIFXROOT/include pcal.cpp mathutil.cpp -lfftw3f -lfftw3 -lpthread -lm` 编译通过，`pcaltest auto` 108 正向用例全过、2 个 failed 用例为上游设计的负向测试（注释 "ought to fail"：offset=0 强制 implicit extractor，预期提取失败），exit=0；② install-difx --noipp 常规构建全绿——默认组件 8 库（difxio/codifio/difxmessage/dirlist/mark5access/vdifio/python/fxcorrcommon）+ mpifxcorr + 10 应用全部构建安装（39 次 make install，日志无 error/failed/Traceback，Done 正常打印；doxygen 缺失被优雅跳过仅影响文档生成；difxcalc11 安装名为 difxcalc；默认 False 组件 datasim/mark6sg 等不在常规构建范围））**
4. **对齐校验**：batch 起点非 subint 边界时 fxcorr-f 明确报错退出（防御 data-spec 12 节）。

---

## 5. 风险与备选

| 风险 | 影响 | 备选 |
|---|---|---|
| datareader 粗延迟移植误差 | 对拍失败 | 对照 datastream.cpp 逐行移植 + 构造已知延迟的单 tone 测试信号 |
| .sp 内存占用（整 batch 常驻） | 大 batch OOM | V1 流式逐 subint 读 .sp（x 侧），文件布局已支持顺序读 |
| visibility 构造依赖 FxManager 的缓冲预算 | 编译/运行差异 | 缓冲长度按 fxmanager.cpp:107-139 公式移植 |
| IPP/FFTW 架构宏随 architecture.h.in 迁移 | 编译错误 | 保持与 mpifxcorr 同款 configure 探测（configure.ac 移植） |
