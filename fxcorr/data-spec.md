# fxcorr 数据规范文档（完整版）

**版本**：1.1
**适用系统**：fxcorr-f / fxcorr-x 流水线（由 DiFX/mpifxcorr 重构）
**处理模式**：非实时、按时间批量、串行可手工执行

**v1.1 变更**（相对 v1.0）：
- 定义 `band_XX.sp` / `pcal.bin` / `autocorr.bin` 二进制格式（第 5.3 节）
- 可见度输出改为 **SWIN 直出**（`vis/<experiment>.difx/`，difx2fits 免改造直读），删除 `vis_<batch_id>.bin` 自定义格式
- fxcorr-x 输入增加 D4（.calc）、D6（.im）——UVW 由 x 侧用延迟模型求值
- 新增时间轴映射（第 7 节）、通道/频率/偏振映射（第 8 节）、切批与重跑约束（第 12 节）

---

## 1. 概述

本文档定义 fxcorr 系统中**所有数据**的类型、目录结构、命名规则、文件内容和模块输入输出关系。

核心设计原则：
- 以**时间批量（batch）**为基本处理单位
- 通过目录层级关联各阶段数据
- 程序保持串行，支持人工命令行执行
- 兼容现有前处理（vex2difx、difxcalc）与后处理（difx2fits、difx2mark4）
- 便于后期扩展并行与流式处理

关键设计决策（v1.1 敲定）：
- **D1**：UVW 由 fxcorr-x 读 .calc/.im 用延迟模型求值（与现 mpifxcorr 一致，UVW 仅在写 SWIN 头时求值，主计算路径不涉及）
- **D2**：可见度输出 SWIN 格式（复用 mpifxcorr 的 visibility.cpp 写盘，difx2fits 零改造）
- **D3**：fxcorr-f 按 recorded band 落盘复数频谱，偏振是 band 属性（`recordedbandpols`），偏振组合（RR/LL/RL/LR）由 fxcorr-x 按 .input 的 BASELINE TABLE 选取

---

## 2. 顶层目录结构

```
project/                          # 项目根目录（可自定义）
├── config/                       # 配置与模型文件
├── raw/                          # 原始基带数据
├── fengine/                      # fxcorr-f 输出（频域谱）
├── vis/                          # fxcorr-x 输出（SWIN 可见度）
├── product/                      # 最终科学产品（FITS / Mark4）
├── meta/                         # 全局索引与日志
└── work/                         # 临时工作区（可选，不进 git）
```

---

## 3. 系统中所有数据类型总表

| 编号 | 数据类型 | 典型文件/目录名 | 产生阶段 | 主要格式 | 典型数据量 | 说明 |
|------|----------|-----------------|----------|----------|------------|------|
| D1 | 观测描述文件 | `*.vex` | 观测调度 | 文本 | KB | 原始观测计划 |
| D2 | vex2difx 控制文件 | `*.v2d` | 用户编写 | 文本 | KB | 控制 vex2difx 行为 |
| D3 | 相关器主配置 | `*.input` | 前处理 | 文本 | KB~百KB | 主配置文件 |
| D4 | 几何模型输入 | `*.calc` | 前处理 | 文本 | KB~百KB | 延迟模型主文件（含站坐标、源、scan 表、IM FILENAME） |
| D5 | 标记文件 | `*.flag` | 前处理 | 文本 | KB | 可选，数据标记 |
| D6 | 延迟模型 | `*.im` | 前处理 | 文本/二进制 | MB 级 | 延迟/uvw 多项式（被 .calc 引用） |
| D7 | 原始基带数据 | `raw/<station>/` | 观测记录 | VDIF/Mark5B/Mark6 等 | **TB 级** | 各台站原始采样数据 |
| D8 | 频域谱数据 | `fengine/<batch_id>/<station>/band_XX.sp` | fxcorr-f | 二进制（.sp） | 较大 | Station-based 结果：延迟对齐、条纹旋转、小数采样校正、FFT 后的复数频谱 |
| D9 | 批量元数据 | `batch.json` | fxcorr-f / fxcorr-x | JSON | KB | 每个批量的描述信息 |
| D10 | 可见度数据 | `vis/<experiment>.difx/DIFX_*.s*.b*` | fxcorr-x | SWIN 二进制 | MB~GB | Baseline-based 结果，difx2fits/difx2mark4 直读 |
| D11 | FITS 科学产品 | `*.FITS` | 后处理 | FITS-IDI | MB~GB | 天文标准格式 |
| D12 | Mark4 科学产品 | Mark4 文件集 | 后处理 | Mark4 | MB~GB | 测地学常用格式 |
| D13 | 全局索引/日志 | `meta/` | 运行过程 | 文本/JSON | KB~MB | 批量索引、运行日志等 |

---

## 4. 各模块输入输出对应表

| 模块 | 类型 | 主要输入 | 主要输出 | 备注 |
|------|------|----------|----------|------|
| **vex2difx** | 前处理1 | D1（.vex）、D2（.v2d） | D3（.input）、D4（.calc）、D5（.flag） | 纯配置生成，不碰原始数据 |
| **difxcalc / calcif2** | 前处理2 | D4（.calc） | D6（.im） | 生成几何延迟模型 |
| **fxcorr-f** | 核心（Station-based） | D3（.input）、D4（.calc）、D6（.im）、D7（raw） | D8（频域谱+自相关+pcal）、D9（batch.json） | 按台站、按批量处理 |
| **fxcorr-x** | 核心（Baseline-based） | D3（.input）、D4（.calc）、D6（.im）、D8（fengine）、D9 | D10（SWIN 可见度）、D9（batch.json） | 按批量处理多台站数据；UVW 由模型求值 |
| **difx2fits** | 后处理1 | D3、D4、D6、D10、D5（可选） | D11（.FITS） | 生成 FITS-IDI，SWIN 零改造直读 |
| **difx2mark4** | 后处理2 | D3、D4、D6、D10、D1 等 | D12（Mark4） | 生成 Mark4 格式 |

---

## 5. 各阶段目录与文件详细规范

### 5.1 配置与模型数据（config/）

```
config/
├── experiment.vex
├── experiment.v2d
├── experiment.input
├── experiment.calc
├── experiment.im
└── experiment.flag          # 可选
```

| 文件 | 编号 | 说明 |
|------|------|------|
| `*.vex` | D1 | 观测描述 |
| `*.v2d` | D2 | vex2difx 控制 |
| `*.input` | D3 | 相关器主配置 |
| `*.calc` | D4 | 几何模型输入（含 IM FILENAME 指向 .im） |
| `*.im` | D6 | 延迟模型 |
| `*.flag` | D5 | 标记文件（可选） |

注意：fxcorr-f **和** fxcorr-x 都读 D3/D4/D6——f 用 .input 解析数据流/频带并用延迟模型做条纹旋转与小数采样校正，x 用 .input 解析基线表/频点并用模型求 UVW（写 SWIN 头）。Configuration（.input 解析）与 Model（.calc/.im）复用 mpifxcorr 现有实现（零 MPI）。

### 5.2 原始基带数据（raw/）

```
raw/
├── STA1/
│   ├── sta1_60512_45000.vdif
│   ├── sta1_60512_45030.vdif
│   └── ...
├── STA2/
│   └── ...
└── STA3/
    └── ...
```

- 编号：D7
- 按台站组织
- 命名建议：`<station>_<batch_id>.<suffix>` 或保持原始记录名
- 数据量：TB 级
- 约束：一个 raw 文件的时间范围须覆盖完整 batch；切批见第 12 节
- 仿真数据（测试替身，由 fxcorr-sim 生成）约束：batch 时间窗须与 subint 网格对齐（fxcorr-sim 读 .input 的 subint 结构保证，见第 12 节）；多节点分布生成时各分片的 VDIF 帧时间戳/帧号须全局连续（程序内校验点）；最小数据集可入仓库（`fxcorr/test/`，附 sha256），不受"运行时数据不进 git"约束

### 5.3 F-Engine 输出（fengine/）

```
fengine/
├── 60512_45000/                      # batch_id
│   ├── batch.json                    # D9（f 版）
│   ├── status.txt                    # 状态：running / done / failed
│   ├── STA1/
│   │   ├── band_00.sp                # D8：recorded band 频谱
│   │   ├── band_01.sp
│   │   ├── ...
│   │   ├── pcal.bin                  # 脉冲校准 tone（每 subint）
│   │   └── autocorr.bin              # 自相关（每 subint，已频率平均）
│   ├── STA2/
│   │   └── ...
│   └── STA3/
│       └── ...
├── 60512_45030/
│   └── ...
└── ...
```

**batch.json 示例**（D9，f 版）：

```json
{
  "batch_id": "60512_45000",
  "start_mjd": 60512.520833333,
  "start_time": "2026-09-08T12:30:00",
  "duration_sec": 30.0,
  "stations": ["STA1", "STA2", "STA3"],
  "config_file": "config/experiment.input",
  "calc_file": "config/experiment.calc",
  "im_file": "config/experiment.im",
  "n_subints": 30,
  "subint_ns": 1000000000,
  "created_at": "2026-09-08T12:35:12Z",
  "status": "done",
  "fxcorr_f_version": "0.1.0"
}
```

**band_XX.sp 二进制格式**（host 字节序，V1 单机；`XX` = recorded band 序号）：

```
Header（定长 256 字节）：
  偏移  类型      字段
  0     char[6]   magic = "FXCSP\0"
  6     u32       version = 1
  10    u32       band_index（recorded band 序号）
  14    char[2]   pol（"R"/"L"，取自 recordedbandpols）
  16    u32       num_channels（recordedbandchannels）
  20    f64       bandwidth_hz（band 带宽）
  28    f64       bandedge_freq_hz（bandlowedgefreq）
  36    u32       lowersideband（0/1）
  40    u32       usecomplex（0/1）
  44    u32       n_subints
  48    u32       subint_ns（= .input 的 subintNS）
  52    u32       blocks_per_send
  56    u32       num_buffered_ffts（每 subint 的 FFT 块数）
  60    u32       flag_words_per_subint（= ceil(blocks_per_send/30)）
  64..255        保留，填 0

每 subint 数据块（顺序存 n_subints 个）：
  i32       scan（scan 序号）
  i32       sec（offsetseconds，相对 scan 起点的秒）
  i32       ns（offsetns）
  u32[flag_words_per_subint]  valid_flags（每 30 位对应一个 FFT 块，位=1 有效）
  f32[blocks_per_send]        weights（每 FFT 块的 dataWeight，0.0~1.0；per-band：各 .sp 文件只存本 band 的权重；槽式更新：写盘时按槽收集后回填，见 fxcorr-f 的 FEngineWriter::flushWeights）
  cf32[blocks_per_send × num_channels]  spectra（subloop 序，每 subloop num_channels 个复数）
```

- cf32 = `struct { float re; float im; }`（与 mpifxcorr 的 `cf32` 定义一致）
- weights 语义：对应 `Mode::getDataWeight(band, subloop)`（Mk5Mode 启用 perbandweights 时按 band 取，否则退化为 dataweight[subloop]）；fxcorr-x 侧 baselineweight 还原为两站对应 band 权重之积（mode.cpp 的 weights 累加语义）
- spectra 语义：已完成解包、延迟对齐（整数+分数采样校正）、条纹旋转、FFT 的结果；**不存共轭副本**，fxcorr-x 侧按需对整段做逐元素共轭（等价于原 `getConjugatedFreqs()`）
- 通道→频率映射约定见第 8 节；无效 subloop（valid=0 或 dataWeight=0）的频谱块为全零
- zoom band 不单独落盘（zoom band 在现实现中是父 band 数组的切片，V1 由 x 侧按 .input 的 zoom 定义从对应 recorded band 切片，或 V1 暂不支持 zoom）

**pcal.bin**（仅当该站配置了 phasecal；`n_tones` 由配置定）：

```
Header：
  char[6]   magic = "FXCPC\0"
  u32       version = 1
  u32       n_subints
  u32       n_tones（最大 tone 数，取各 band 之和）
  u32       n_bands
  每 band：u32 band_index；u32 n_tones_this_band；f64 tone_freq_mhz[n_tones]；char pol[n_tones]
每 subint：cf32[n_tones]（各 band tone 依序排列）
```

**autocorr.bin**（每 subint 按 maxacblocks 批次频率平均后的自相关）：

```
Header：
  char[6]   magic = "FXCAC\0"
  u32       version = 1
  u32       n_subints
  u32       ac_batches（每 subint 的 AC 平均批次记录数 = ceil(blocks_per_send/maxacblocks)）
  u32       n_bands
  每 band：u32 band_index；u32 num_channels
每 subint（ac_batches 条记录，按 fftloop 批序）：
  每 band：cf32[num_channels]（自相关复数谱）+ f32 weight（本批次累积权重）
```

自相关在 f 侧由 `Mode::process` 累积，每 maxacblocks 个 FFT（与 core.cpp:993-1003 同节奏）`averageFrequency()` 平均后落一条记录、随即 `zeroAutocorrelations()`；x 侧把该 subint 的全部记录逐条累加进 SWIN 自相关段（`vectorAdd`，基线号 `257*(telescope_index+1)`，与现 DiFX 约定一致）。maxacblocks 由 .calc 的 AC AVG INTERVAL 与 subint 结构共同决定（公式同 core.cpp:778-783）。

### 5.4 X-Engine 输出（vis/，SWIN）

```
vis/
├── experiment.difx/                  # SWIN 文件集（跨 batch 追加）
│   ├── DIFX_60512_45000.s0000.b0000  # 文件名 = DIFX_<MJD>_<实验开始秒>.s<相位中心>.b<脉冲星bin>
│   ├── DIFX_60512_45000.s0000.b0001
│   └── ...
├── 60512_45000/
│   ├── batch.json                    # D9（x 版）
│   └── status.txt
└── 60512_45030/
    └── ...
```

- **D10 = SWIN 二进制**：每记录 74 字节头（sync 0xFF00FF00、version、baselinenum、dumpmjd、dumpseconds、configindex、sourceindex、freqindex、polpair(2B)、pulsarbin、weight(double)、uvw[3×double]）+ `freqchannels` 个 cf32 可见度。与 mpifxcorr 的 `Visibility::writeSWIN` 逐字节一致，difx2fits / difx2mark4 零改造直读。
- SWIN 文件名用**实验级** MJD+开始秒（.input 的 START MJD/SECONDS），**不含 batch_id**——同一实验的所有 batch 追加写入同一组文件（batch 边界与 intTime 对齐保证不碎片化，见第 12 节）。
- 记录头里 `uvw[3]` 由 fxcorr-x 在写盘时用 Model（.calc + .im）在积分中点求值（`interpolateUVW`）。
- 脉冲校准数据由 f 落盘（5.3 节 pcal.bin）；`PCAL_*.pcal` 文件生成（每实验每站一个，追加式）列入 V2。
- 自相关以基线号 `257*(telescope_index+1)` 写入 `s0000.b0000` 文件。

**batch.json**（D9，x 版）在 f 版基础上增加：

```json
{
  ...
  "baselines": ["STA1-STA2", "STA1-STA3", "STA2-STA3"],
  "integration_sec": 1.0,
  "n_channels": 256,
  "polarizations": ["RR", "LL", "RL", "LR"],
  "difx_dir": "vis/experiment.difx",
  "fxcorr_x_version": "0.1.0"
}
```

### 5.5 最终科学产品（product/）

```
product/
├── experiment_60512_45000.FITS       # D11
├── experiment_60512_45030.FITS
└── mark4/                            # D12（如使用 difx2mark4）
    └── ...
```

### 5.6 全局元数据（meta/）

```
meta/
├── batches.index                     # D13
├── stations.json
├── run.log
└── versions.txt
```

**batches.index 示例**：
```
60512_45000,done,2026-09-08T12:35:12Z
60512_45030,running,2026-09-08T12:36:01Z
60512_45060,pending,
```

---

## 6. 批量标识（batch_id）规范

**推荐格式**（二选一）：

| 格式 | 示例 | 说明 |
|------|------|------|
| MJD + 秒 | `60512_45000` | 推荐机器处理 |
| 紧凑日期时间 | `20260908_123000` | 更人类可读 |

规则：
- 全局唯一
- 可按字符串排序即时间顺序
- 同一批量在 `fengine/` 与 `vis/` 下使用相同 `batch_id`
- batch 时长必须是 `intTime`（.input）的整数倍；batch 起点与 subint 边界对齐（第 12 节）

---

## 7. 时间轴映射（DiFX 内部时间 ↔ batch）

fxcorr-f / fxcorr-x 复用 mpifxcorr 的时间体系，落盘数据自带以下坐标（来自原 `controlbuffer`/`offsets` 语义）：

| 量 | 来源 | 说明 |
|---|---|---|
| scan | .input scan 表序号 | 每 subint 记录 |
| offsetseconds / offsetns | 相对 scan 起点的偏移 | 每 subint 记录（.sp 头中 sec/ns 字段） |
| subintNS | .input（CONFIG 段） | 一个 subint 的时长（ns），一次处理/落盘单位 |
| blockspersend | .input | 一个 subint 内的 FFT 块数 |
| blockns | subintNS / blockspersend | 一个 FFT 块（subloop）的时长（ns） |
| fftchannels | 2×recordedbandchannels（实数采样）；recordedbandchannels（复数采样） | 一个 FFT 块包含的采样数 |

- 一致性恒等式（configuration 校验）：`ffttime = sampletime × numchannels × 2 = subintNS / blockspersend`
- 绝对时间换算：`t = scan_start + offsetsec + offsetns/1e9`；batch 的 `start_mjd` = 第一个 subint 的绝对时间
- SWIN 记录头 `dumpseconds` = 实验开始秒 + 积分段内 subint 数 × subintNS/1e9（与现 DiFX 一致）

---

## 8. 通道 / 频率 / 偏振映射

复用 mpifxcorr 的 freq table 约定（.input 的 FREQ TABLE），fxcorr-f 落盘、fxcorr-x 解释都按此执行：

- **band = 偏振属性**：每个 recorded band 带一个偏振字符（`recordedbandpols[band]`），R/L 是**两个独立 band**、两个独立 FFT 数组、两个独立 .sp 文件。不存在"偏振交织在数组内"。
- **通道→频率**：通道间距 = `bandwidth / numchannels`；band 内第 i 通道频率 = `bandedge + i × bandwidth/numchannels`；频谱 index 0 对应 LO 频率（USB）/ LO−带宽（LSB）/ LO−带宽/2（复数 DSB），随 index 单调递增（LSB 由 x 侧共轭翻转处理，与现实现一致）。
- **freq table 条目**：`bandedgefreq, bandwidth, lowersideband, correlatedagainstupper, numchannels, channelstoaverage, oversamplefactor, decimationfactor`——fxcorr-x 完全按 .input 重读，f 不在 .sp 里重复携带（.sp header 仅保留自描述所需的最小集合）。
- **输出频点**：一个输出 freq 可包含多个输入 freq 的拼接，通道放置由 `choffset = ((fcurr−fref)/bandwidth)×numchannels` 决定；XMAC stride 长度按现算法（最接近 150 的因子）确定——fxcorr-x 复用 configuration.cpp 现成预算逻辑。
- **偏振组合**：RR/LL/RL/LR 由 .input 的 BASELINE TABLE 逐 product 给定（两个 band 号），fxcorr-x 按表取两站对应 band 的频谱相乘；pol 字符只在写 SWIN 头时使用（`polpair` 2 字节字段）。
- **多相位中心 / zoom band / 脉冲星 bin**：V1 仅支持单相位中心、无 zoom、无脉冲星 binning；数据格式已预留扩展（.sp 的 band 序号、SWIN 的 s/b 编号）。

---

## 9. 完整数据流

```
D1 (.vex) + D2 (.v2d)
        ↓
   [vex2difx]
        ↓
D3 (.input) + D4 (.calc) + D5 (.flag)
        ↓
   [difxcalc / calcif2]
        ↓
D6 (.im)
        ↓
D3 + D4 + D6 + D7 (raw)     （D7 为真实观测数据或仿真数据生成器产出）
        ↓
   [fxcorr-f]          ← 按台站、按批量
        ↓
D8 (fengine: band_XX.sp + pcal.bin + autocorr.bin) + D9 (batch.json)
        ↓
D3 + D4 + D6 + D8 + D9
        ↓
   [fxcorr-x]          ← 按批量
        ↓
D10 (vis/<experiment>.difx/ SWIN) + D9
        ↓
   [difx2fits] 或 [difx2mark4]
        ↓
D11 (.FITS) 或 D12 (Mark4)
```

---

## 10. 文件命名汇总

| 数据类型 | 编号 | 推荐命名 | 示例 |
|----------|------|----------|------|
| 批量目录 | - | `<batch_id>` | `60512_45000` |
| F 频谱文件 | D8 | `band_<xx>.sp` | `band_00.sp` |
| F pcal 文件 | D8 | `pcal.bin` | `pcal.bin` |
| F 自相关文件 | D8 | `autocorr.bin` | `autocorr.bin` |
| 可见度文件 | D10 | `DIFX_<MJD>_<sec>.s<XX>.b<XX>` | `DIFX_60512_45000.s0000.b0000` |
| 批量元数据 | D9 | `batch.json` | `batch.json` |
| 状态文件 | - | `status.txt` | `status.txt` |
| FITS 产品 | D11 | `<exp>_<batch_id>.FITS` | `exp_60512_45000.FITS` |

---

## 11. 数据量级关系（典型）

```
D7（原始基带）  ≫  D8（频域谱）  ≫  D10（可见度）  ≈  D11/D12（科学产品）
配置类（D1～D6、D9、D13）体积很小（KB～MB）
```

---

## 12. 切批与重跑约束

- **对齐**：batch 起点必须落在 subint 边界（MJD 秒是 subintNS/1e9 的整数倍），batch 时长 = `intTime` 整数倍。保证 SWIN integration 跨 batch 完整、追加不碎片化。该约束由 fxcorr-sim 前置保证（5.2 节），**fxcorr-f 启动时校验**是最后防线：batch 起点非 subint 边界则报错退出（容差 1µs，吸收 start_mjd 的 f64 表示误差；batch.json 的 start_mjd 建议写精确 repr，如 58948.291666666664）。
- **SWIN 追加**：同一实验所有 batch 写同一 `vis/<experiment>.difx/`；重跑整个实验需清空该目录，重跑单个 batch 需按 subint 范围从对应文件裁掉再追加（V1 不实现单 batch 回滚，重跑 = 全实验重跑）。
- **fengine 覆盖**：重跑某 batch 时，fxcorr-f 覆盖写 `fengine/<batch_id>/` 下文件；fxcorr-x 以 batch.json 的 `status=done` 判定是否需要重跑。
- **work/**：临时文件（如中间缓冲），进程结束后可安全清理，不进 git。

---

## 13. 设计要点小结

- 所有处理以 **batch_id** 为单元，一次只处理一个批量
- 目录即接口，程序通过输入输出目录衔接，无需 MPI
- 每个批量目录自带 `batch.json`，便于追踪与断点续跑
- 原始数据按台站存放，处理后按批量组织
- f 落盘"完全校正"的频谱（含小数采样、条纹旋转、延迟对齐），x 只做 XMAC + 积分 + SWIN 写盘
- 前处理（vex2difx、difxcalc）与后处理（difx2fits、difx2mark4）保持兼容，SWIN 直出使 difx2fits 零改造
- 为后期并行化与流式处理预留清晰扩展空间
