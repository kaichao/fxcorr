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

分布部署原则（V2 多节点预留；V1 单机同机无差别，不冲突）：

- **计算本地化**：f/x 都在节点本地计算，充分利用本地 CPU——f 任务（batch×站）调度到该站 raw 数据所在节点；x 任务（batch×站组对）优先调度到输入谱数据所在节点。
- **共享存储主数据流**：配置与元数据（config/、batches/、meta/、product/）及跨 batch 追加的 SWIN（vis/）放共享存储，全部节点一致可见；大批量数据（raw/、fengine/）可放本地存储，目录逻辑集中、物理分布（每节点只持有自己写的部分，见第 2 节存储归属表）。
- **网络/计算/存储权衡**：必要时以**重复计算**（同一数据各节点各算一份，省网络传输、费 CPU）或**网络传输**（拉数到计算节点，省 CPU、费网络）为调节手段，在三者复用上取平衡；实现不应做死"必须本地"或"必须共享"的假设。

---

## 2. 顶层目录结构

```
project/                          # 项目根目录（可自定义）
├── config/                       # 配置与模型文件
├── batches/                      # 批量元数据（batch.json，D9）
├── common/                       # 仿真公共信号（fxcorr-sim common 输出，D15）
├── raw/                          # 原始基带数据
├── fengine/                      # fxcorr-f 输出（频域谱）
├── vis/                          # fxcorr-x 输出（SWIN 可见度）
├── beam/                         # fxcorr-x 相位阵输出（波束频谱，P8）
├── product/                      # 最终科学产品（FITS / Mark4）
├── meta/                         # 全局索引与日志
└── work/                         # 临时工作区（可选，不进 git）
```

多节点部署的存储归属（V2 预留；V1 单机同机，无差别）：

| 目录 | 归属 | 说明 |
|---|---|---|
| `config/` `batches/` `meta/` `product/` | 共享存储 | 配置、元数据、索引、产品；量小，全部节点须一致可见 |
| `common/` | 共享存储 | 仿真公共信号（fxcorr-sim common 一次生成、各站 station 任务只读；≥16× 单站 2bit 数据量，见 11 节）；batch 的全部 station 完成后可删（生命周期同 fengine/，见 12 节） |
| `vis/` | 共享存储 | SWIN 跨 batch 追加、difx2fits 直读；多 x 子集并行时按 subset 子目录分写（第 12 节） |
| `beam/` | 共享存储 | 相位阵波束频谱，按 batch 组织；下游波束消费者直读 |
| `raw/` | 本地存储 | TB 级原始基带；各站数据在各记录节点 |
| `fengine/` | 本地存储 | 各站 f 输出在计算节点；目录逻辑集中、物理分布（每节点只持有自己写的站） |
| `work/` | 本地存储 | 临时文件，进程结束可清理 |

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
| D14 | 波束数据 | `beam/<batch_id>/beam.bin` | fxcorr-x（相位阵） | 二进制（beam.bin） | MB 级 | 相位阵波束加权和频谱，按 acc 窗口记录（P8 2026-09-13；上游无对照格式，fxcorr 自定，见 5.5 节） |
| D15 | 仿真公共信号 | `common/<batch_id>/` | fxcorr-sim common | 二进制（float32 频域 slice）+ JSON | 较大 | 频域公共信号（量化前复基带频谱，权威一份，各站 station 只读切频段；≥16× 单站 2bit 数据量，见 11 节）；格式见 5.8 节（2026-09-14 新增） |

---

## 4. 各模块输入输出对应表

| 模块 | 类型 | 主要输入 | 主要输出 | 备注 |
|------|------|----------|----------|------|
| **vex2difx** | 前处理1 | D1（.vex）、D2（.v2d） | D3（.input）、D4（.calc）、D5（.flag） | 纯配置生成，不碰原始数据 |
| **difxcalc / calcif2** | 前处理2 | D4（.calc） | D6（.im） | 生成几何延迟模型 |
| **fxcorr-sim common** | 数据生成1 | D3（.input）、D9（batch.json） | D15（common/ 公共信号） | 每个 batch 一次；频域公共信号按全站 band 布局推导 specRes 网格（5.8 节） |
| **fxcorr-sim station** | 数据生成2 | D3、D9、D15 | D7（raw/ 单站 VDIF） | 每站一任务，多节点并行；只读公共信号，不改写 |
| **fxcorr-f** | 核心（Station-based） | D3（.input）、D4（.calc）、D6（.im）、D7（raw） | D8（频域谱+自相关+pcal）、D9（batch.json） | 按台站、按批量处理 |
| **fxcorr-x** | 核心（Baseline-based） | D3（.input）、D4（.calc）、D6（.im）、D8（fengine）、D9 | D10（SWIN 可见度）、D9（batch.json）；相位阵配置时改出 D14（beam.bin，无 SWIN） | 按批量处理多台站数据；UVW 由模型求值 |
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
- **文件起点语义**：file-per-batch 布局下 raw 文件的时间起点 = batch 起点（fxcorr-f 的字节偏移按 batch 起点定位，非 scan 起点；batch 起点 = scan 起点时两者一致）。batch.json 的 start_mjd 是该起点的精确表示。
- 仿真数据（测试替身，由 fxcorr-sim 生成）约束：batch 时间窗须与 subint 网格对齐（fxcorr-sim 读 .input 的 subint 结构保证，见第 12 节）；多节点分布生成时各分片的 VDIF 帧时间戳/帧号须全局连续（程序内校验点）；最小数据集可入仓库（`fxcorr/test/`，附 sha256），不受"运行时数据不进 git"约束

#### 5.2.1 仿真数据生成器（fxcorr-sim）规范

fxcorr-sim 是 datasim 的替身（datasim 因上游 IPP 依赖无法构建），单二进制三入口（架构见 fxcorr-sim-arch.md）：`common` 生成共享公共信号（D15，5.8 节）、`station` 读公共信号生成单站 VDIF、无子命令本机串行。其配置输入与参数语义：

- **配置目录（workdir）定位**：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认 `.`。workdir 内所有相对路径（`batches/<batch_id>.json`、`.input` 的 config_file、`common/`、`raw/` 输出）均相对它解释。三工具（fxcorr-sim/f/x）与编排脚本（make_testdata.sh / run_batch.sh / run_bench.sh）统一此语义。
- **配置来源**：读 `workdir/batches/<batch_id>.json`（D9）取 start_mjd / n_subints / config_file；band 结构、采样率、PHASE CAL tone 网格全部来自 `workdir/<config_file>`（.input，非 MPI 构造），与 fxcorr-f 同一解析语义——这是分批次对齐要求的硬理由。common 的 specRes/numSamps 网格由**全站** band 布局推导（5.8 节）。
- **任务粒度**：common 任务 = (batch_id)（每 batch 一次）；station 任务 = (batch_id, station)（与 fxcorr-f 同构，多节点并行）；一致性靠共享存储单份 common，不靠多节点重复生成。
- **legacy 模式**：station 子命令带 tone_mhz 位置参数时走旧时域合成路径（tone/噪声/pcal/FXSIM_DELAY/FXSIM_FLUX/FXSIM_SEFD/FXSIM_ADAPTIVE），供字节对拍回归；tone 参数 0 个 = 无 tone、1 个 = 所有 band 同频率、nbands 个 = 逐 band。新路径下 tone 由 P2 谱线机制（common 端频域注入）提供。
- **噪声**：`FXSIM_NOISE`（高斯噪声 σ，默认 0.02；0 关闭），`FXSIM_SEED`（公共种子，默认固定）。公共信号只与 (seed, batch) 有关、与站无关；站噪声种子 = f(seed, station) 派生。两站噪声全关时输出逐位一致（跨站相干校验）。
- **PHASE CAL 注入**：`.input` 的 `PHASE CAL INT (MHZ)` > 0 时按 Configuration 的 tone 网格自动注入（幅度 0.7，避开 2bit 量化器电平陷阱，见 applications/fxcorr-sim/CLAUDE.md），频率/计数与 fxcorr-f 提取端完全一致，构成注入-提取闭环（legacy 已实现，新路径 P2）。
- **输出**：`workdir/raw/<station>/<station>_<batch_id>.vdif`（2bit VDIF；多 band 时帧内样本 band 交织；帧时间戳/帧号按 batch 起点换算逐帧自增）。

### 5.3 F-Engine 输出（fengine/）

```
fengine/
├── 60512_45000/                      # batch_id
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

**batch.json 示例**（D9，全字段单文件）：

```json
{
  "batch_id": "60512_45000",
  "start_mjd": 60512.520833333,
  "start_time": "2026-09-08T12:30:00",
  "duration_sec": 30.0,
  "stations": ["STA1", "STA2", "STA3"],
  "station_groups": [["STA1", "STA2"], ["STA3"]],
  "baselines": ["STA1-STA2", "STA1-STA3", "STA2-STA3"],
  "config_file": "config/experiment.input",
  "calc_file": "config/experiment.calc",
  "im_file": "config/experiment.im",
  "n_subints": 30,
  "subint_ns": 1000000000,
  "integration_sec": 1.0,
  "n_channels": 256,
  "polarizations": ["RR", "LL", "RL", "LR"],
  "difx_dir": "vis/experiment.difx",
  "created_at": "2026-09-08T12:35:12Z",
  "status": "done",
  "fxcorr_f_version": "0.1.0",
  "fxcorr_x_version": "0.1.0"
}
```

字段说明：

- 时间与结构：`batch_id` / `start_mjd` / `start_time` / `duration_sec` / `n_subints` / `subint_ns`——fxcorr-sim 与 fxcorr-f 读（start_mjd 建议写精确 repr，如 58948.291666666664）。
- `config_file` / `calc_file` / `im_file`：三工具读 .input（config_file）；calc/im 为元数据。
- `stations`：全部参与站。`station_groups`（可选）：空间切分组，缺省 = 全部站一组 = 全基线；任务集推导见第 6 节。
- `baselines` / `integration_sec` / `n_channels` / `polarizations` / `difx_dir`：可见度输出元数据，均可由 .input 提前推导（baselines = 全部基线组合，polarizations 由 BASELINE TABLE 定）；fxcorr-x 读 `difx_dir` 作元数据，实际输出目录以 .input 的 OUTPUT FILENAME 为准。
- `status`：running / done / failed，编排脚本更新。

**batch.json 位置语义**（D9，谁写谁读）：

- 位于 `batches/<batch_id>.json`（共享存储），**单文件全字段**：由编排脚本（make_testdata.sh / run_batch.sh）或调度器**一次写全**（x 阶段字段由 .input 提前推导，无需分阶段追加）；fxcorr-sim / fxcorr-f / fxcorr-x 各取所需，均**只读、不回写**；status 字段由编排脚本更新（无独立 status.txt）。
- 各工具命令行用法见 `fxcorr/usage.md`（本规范不覆盖命令行接口）。

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
- **zoom band 不单独落盘**（2026-09-13 P4a 实现）：zoom 频谱是父 band 频谱数组的指针切片（mode.cpp:184-195），fxcorr-x 按 .input 的 zoom 定义（zoomfreqchanneloffset + zoom freq 的 nchan）对父 band_XX.sp 做通道切片视图读取（SpReader 切片构造参数），.sp 文件与布局不变

**pcal.bin**（仅当该站配置了 phasecal；`n_tones` 由配置定）：

```
Header：
  char[6]   magic = "FXCPC\0"
  u32       version = 1
  u32       n_subints
  u32       n_tones（最大 tone 数，取各 band 之和）
  u32       n_bands
  每 band：u32 band_index；u32 n_tones_this_band；
           每 tone：f64 tone_freq_mhz + char pol（交错，共 9B/tone）
每 subint：cf32[n_tones]（各 band tone 依序排列）
```

**autocorr.bin**（每 subint 按 maxacblocks 批次频率平均后的自相关）：

```
Header：
  char[6]   magic = "FXCAC\0"
  u32       version = 2
  u32       n_subints
  u32       ac_batches（每 subint 的 AC 平均批次记录数 = ceil(blocks_per_send/maxacblocks)）
  u32       n_bands（total bands = recorded + zoom）
  u32       crosspol（1 = 记录含交叉极化段；0 = 仅平行段。= WRITE AUTOCORRS && maxproducts>2）
  每 band：u32 band_index（datastream-total 序）；u32 num_channels（各 band 各自 nchan/chanstoavg）
每 subint（ac_batches 条记录，按 fftloop 批序）：
  平行段：每 band：cf32[num_channels]（自相关复数谱）+ f32 weight（本批次累积权重）
  crosspol 段（仅 crosspol=1）：每 band：cf32[num_channels]（交叉极化自相关）+ f32 weight
```

自相关在 f 侧由 `Mode::process` 累积，每 maxacblocks 个 FFT（与 core.cpp:993-1003 同节奏）`averageFrequency()` 平均后落一条记录、随即 `zeroAutocorrelations()`；x 侧把该 subint 的全部记录逐条累加进 SWIN 自相关段（`vectorAdd`，基线号 `257*(telescope_index+1)`，与现 DiFX 约定一致）。maxacblocks 由 .calc 的 AC AVG INTERVAL 与 subint 结构共同决定（公式同 core.cpp:778-783）。**zoom band（2026-09-13 P4a）**：自相关谱为父 band 平均后数组的切片（mode.cpp:382-385，偏移已除 channelstoaverage），zoom 段的 weight 从父 recorded band 取（Mode 的 weights 只有 recorded 维；映射逻辑同 core.cpp:1324-1339）。**交叉极化段（2026-09-13 P7）**：crosspol=1 时记录在平行段后接同构的 crosspol 段（顺序同 core.cpp:1273-1301/1342-1369 的 results 布局串联），交叉谱 = 同一 FFT 块内 R×conj(L) 与 L×conj(R) 的累加（Mode 内 autocorrelations[1]，calccrosspolautocorrs 由 WRITE AUTOCORRS 开启）、weight 累加同平行（perbandweights 时为两 band 权重乘积）；x 侧按 header 的 crosspol 标志读段并累加进结果区 crosspol 偏移（平行段 walk 结束处接续），SWIN 写盘由 Visibility 的 autocorrwidth=2 路径处理（polpair = 平行 [p,p] / 交叉 [p,opposite(p)]）。version 1（旧产物，无 crosspol 字段）按无 crosspol 段处理。

### 5.4 X-Engine 输出（vis/，SWIN）

```
vis/
└── experiment.difx/                  # SWIN 文件集（跨 batch 追加）
    ├── DIFX_60512_45000.s0000.b0000  # 文件名 = DIFX_<MJD>_<实验开始秒>.s<相位中心>.b<脉冲星bin>
    ├── DIFX_60512_45000.s0000.b0001
    └── ...
```

（batch.json 已集中到 `batches/`，见 5.3 节）

- **D10 = SWIN 二进制**：每记录 74 字节头（sync 0xFF00FF00、version、baselinenum、dumpmjd、dumpseconds、configindex、sourceindex、freqindex、polpair(2B)、pulsarbin、weight(double)、uvw[3×double]）+ `freqchannels` 个 cf32 可见度。与 mpifxcorr 的 `Visibility::writeSWIN` 逐字节一致，difx2fits / difx2mark4 零改造直读。
- SWIN 文件名用**实验级** MJD+开始秒（.input 的 START MJD/SECONDS），**不含 batch_id**——同一实验的所有 batch 追加写入同一组文件（batch 边界与 intTime 对齐保证不碎片化，见第 12 节）。
- 记录头里 `uvw[3]` 由 fxcorr-x 在写盘时用 Model（.calc + .im）在积分中点求值（`interpolateUVW`）。
- 脉冲校准数据由 f 落盘（5.3 节 pcal.bin）；实验级文本文件 `PCAL_<mjd>_<sec>_<station>` 由 f 生成（V2 P0，2026-09-13 完成），格式与 mpifxcorr 逐字节一致、可与基准直接 diff 对拍：
  ```
  # DiFX-derived pulse cal data
  # File version = 1
  # Start MJD = <mjd>
  # Start seconds = <sec>
  # Telescope name = <station>
  <station> <pcalmjd %17.11f> <intTime/86400 %13.11f> <dsindex> <n_recorded_bands> <max_tones> [<tonefreq %12g> <pol> <re %12.5e> <im %12.5e>]...
  ```
  每 intTime 一行（tone 值 = intTime 内 subint 累加 × 可见度层校准缩放，LSB 写原值 / USB im 取负，不足 max_tones 补 ` -1 0 0 0`，全零不写行）；追加幂等（按行时间戳替换），跨 batch 追加。
- 自相关以基线号 `257*(telescope_index+1)` 写入 `s0000.b0000` 文件。

batch.json（D9）已并入 `batches/<batch_id>.json` 单文件全字段，见 5.3 节。

### 5.5 X-Engine 波束输出（beam/，P8 2026-09-13）

相位阵配置（.input CONFIG 段 `PHASED ARRAY TRUE` + `PHASED ARRAY CONFIG FILE`）时，fxcorr-x 不做互相关、不写 SWIN，改为波束形成：各站频谱按相位阵配置文件的 DWeight 加权求和（`Σ_ds DWeight[freq][ds] × spectrum_ds`，累加不归一化），按 `ACC TIME (NS)` 窗口落盘。上游 mpifxcorr 的相位阵输出端（padomain/paoutputformat/DIFX/VDIF/TIMESERIES）是无消费者的死代码，**本格式为 fxcorr 自定**（设计见 algo-plan.md P8 节；检验资产见 `fxcorr/test/phasearr/`）。

```
beam/
└── 60512_45000/                      # batch_id
    └── beam.bin                      # D14：每 acc 窗口一条记录（追加写，重跑覆盖幂等）
```

**beam.bin 二进制格式**（host 字节序，V1 单机）：

```
Header（定长 256 字节）：
  偏移  类型      字段
  0     char[6]   magic = "FXCBM\0"
  6     u32       version = 1
  10    u32       n_subints
  14    u32       n_accs（每 subint 的 acc 窗口数 = subintns/ACC TIME，Configuration 校验整除）
  18    u32       acc_ns（= ACC TIME (NS)）
  22    u32       n_segs（输出段数 = Σ_freq 该 freq 的 pol 数）
  26..255        保留，填 0

段表（n_segs 条，每段 9 字节）：u32 freq_index + char pol + u32 nchan
  （段序 = freq table 序 × 各 freq 的 pol 列表序；nchan = 该 freq 的 NUM CHANNELS）

每 subint（顺序存 n_subints 个）：
  每 acc 窗口（n_accs 条记录）：
    i32       scan（scan 序号）
    i32       sec（窗口起点，相对 scan 起点的秒 = subint sec + acc×acc_ns 进位）
    i32       ns（窗口起点 ns）
    每段：cf32[nchan]（该窗口内所有 FFT 块的加权和频谱）
```

语义要点：

- 波束 = Σ_站 DWeight×频谱，**累加不归一化**、**不查 valid flags**（无效 FFT 块的频谱在 f 侧落盘即全零，直接加权自然等价，与上游 core.cpp:818-865 一致）；权重 ≥0 由 Configuration 校验。
- acc 窗口 = 整数个 FFT 块（Configuration 校验 accffts 为 `NUM BUFFERED FFTS` 的整数倍），窗口起点时间戳 = subint 起点 + acc 序号×acc_ns（scan 相对系，同 .sp）。
- 无 weight 段（上游无此概念）；DWeight 不落 header（.input 可查，落盘易与配置漂移）。
- 每 (freq, pol) 的贡献站映射：recorded band 优先（freq==该 freq 且 pol==papol），否则 zoom band——照上游 core.cpp:822-851。
- 通道数语义修正：上游 core.cpp:821 误用 `getFNumChannels(configindex)`（参数应为 freq 序号），fxcorr 用 `getFNumChannels(freqindex)`。
- 相位阵 batch 不写 SWIN（fxcorr-x 早退分支）；f 侧产物（.sp/autocorr.bin）照常；与 pulsar/多相位中心互斥（上游 if/else 语义）。

### 5.6 最终科学产品（product/）

```
product/
├── experiment_60512_45000.FITS       # D11
├── experiment_60512_45030.FITS
└── mark4/                            # D12（如使用 difx2mark4）
    └── ...
```

### 5.7 全局元数据（meta/）

```
meta/
├── batches.index                     # D13
├── stations.json
├── run.log
├── versions.txt
└── difxmsg/                          # DifxMessage 落盘日志（container 模式，algo-plan P1）
    ├── <exp>_<batch>.xml              # fxcorr-x：每条完整 XML（Starting/Running/Ending/Done/Alert）
    ├── <exp>_<batch>_<station>.xml    # fxcorr-f：同上 + 每 subint 两条 Diagnostic
    └── <exp>_<batch>_<station>.sta    # fxcorr-f：DifxMessageSTARecord 二进制追加（FXCORR_STA=1）
```

difxmsg/ 仅 container 模式（`FXCORR_RUN_MODE=container`）产生：组播受限的容器内降级为落盘，由编排层（scalebox）读取转发；文件内容与 host 模式组播包逐字节一致。每进程一个文件（文件名含 batch_id/station），进程启动时截断重写——重跑 batch 幂等，无并发追加竞态。

**batches.index 示例**：
```
60512_45000,done,2026-09-08T12:35:12Z
60512_45030,running,2026-09-08T12:36:01Z
60512_45060,pending,
```

### 5.8 仿真公共信号（common/）

```
common/
└── <batch_id>/                      # fxcorr-sim common 输出（D15）
    ├── meta.json                    # 格式版本与网格参数
    └── data_XX.bin                  # 每 0.5s 块一个文件（XX 从 00 顺序编号）
```

- 编号：D15；产生者：`fxcorr-sim common <batch_id>`（每个 batch 一次）；消费者：`fxcorr-sim station`（各站任务只读，不改写）。格式单独定版本，**改文件格式必须先同步本节并递增 version**。
- **信号语义**（datasim gencplx 移植）：量化前频域公共信号——覆盖全站 `[minStartFreq, maxStartFreq+maxBW]` 的复基带频谱时间流，每 `stime = 1/specRes` µs 一个 `numSamps` 点复频谱 slice（STDEV=1 高斯复噪声，实虚独立）；各站 station 端按自己 band 的 (startIdx, blksize) 切频段、加站噪声、逆 DFT 出复基带（切出的频段逐位相同 = 跨站相干来源）。
- **网格参数**（由全站 band 布局推导，datasim getSpecRes 移植）：`specRes` = 全站 band 频率差/带宽的 GCD（0.5 MHz 起、二分至 1/2^10，找不到报错）；`numSamps = maxChanFreq/specRes`（全站 band 覆盖跨度）；`minStartFreq` = 全站最低 band 频率。band 频率须落在网格（`(freq−minStartFreq)/specRes` 整数，common 端校验）。
- **meta.json 字段**：

| 字段 | 说明 |
|---|---|
| `version` | 格式版本（初版 1；随布局变更递增） |
| `dtype` | 数据元素类型（初版 `float32` 复数对；留 int16 降级口，见 11 节） |
| `spec_res_mhz` / `numsamps` / `min_start_freq_mhz` | 网格参数（上文） |
| `block_bytes` / `slices_per_block` | 每块字节数与 slice 数（`slices_per_block = 0.5s/stime`） |
| `nblocks` | 块文件数（batch 末块按 batch 时长截断） |
| `seed` | 公共信号种子（与站无关；站噪声种子 = f(seed, station) 由 station 端派生） |
| `batch_id` / `start_mjd` | 归属批量与时间起点 |
| `status` | `running` / `done`（先写数据再置 done，station 以 done 为就绪判据） |

- **数据文件布局**：`data_XX.bin` 内按 slice 顺序平铺——每 slice `numSamps` 个复数（re,im 各 float32 小端交替），slice 内频点序 = 网格升序（minStartFreq 起）；块 XX 覆盖 batch 第 `XX×0.5s` 起的 0.5s。
- **完成可见性**：块文件先写 `<name>.tmp` 再 `rename`；全部块落盘后 meta.json 写 `status=done`。station 端发现 meta 缺失或 status≠done 即报错退出。
- **生命周期**：batch 的全部 station 任务完成后 `common/<batch_id>/` 可删（编排层清理，同 fengine/ 12 节语义）；重跑 batch 时 common 覆盖生成（status 回 running→done）。
- **数据量**：float32 复基带 = 8 字节 × 覆盖带宽 × 时长，恒为单站 2bit VDIF 的 16 倍（11 节）；异带多站时覆盖跨度放大（可达全站总量 20 倍），共享存储容量须按此评估。

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
- **空间维度不进 batch_id**（batch_id 只回答"哪个时间段"，排序语义不被破坏）：f 任务 = (batch_id, station)，x 任务 = (batch_id, 站组对)；任务集由调度器从 batch.json 的 `stations` / `station_groups` 推导（station_groups 缺省 = 全站一组 = 全基线；组对取三角部分含 (G,G) 组内）

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

仿真数据分支（D7 的仿真来源，fxcorr-sim 两阶段）：

```
D3 (.input) + D9 (batch.json)
        ↓
   [fxcorr-sim common]     ← 每个 batch 一次
        ↓
D15 (common/<batch_id>/ 频域公共信号)
        ↓
   [fxcorr-sim station]    ← 按台站、多节点并行（只读 D15）
        ↓
D7 (raw/<station>/<station>_<batch_id>.vdif)
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
| 波束文件 | D14 | `beam/<batch_id>/beam.bin` | `beam/60512_45000/beam.bin` |
| 公共信号 | D15 | `common/<batch_id>/data_XX.bin` + `meta.json` | `common/60512_45000/data_00.bin` |
| 批量元数据 | D9 | `batches/<batch_id>.json` | `batches/60512_45000.json` |
| 状态文件 | - | `status.txt` | `status.txt` |
| FITS 产品 | D11 | `<exp>_<batch_id>.FITS` | `exp_60512_45000.FITS` |

---

## 11. 数据量级关系（典型）

```
D8（频域谱）  ≈  16×D7（原始基带，2bit 记录）  ≫  D10（可见度，单基线）
D15（公共信号） ≈  16×D7（单站 2bit；float32 复基带 vs 2bit 实基带，与带宽无关）
D10 随基线数 O(N²) 增长，多站大阵可能反超 D7；D11/D12（科学产品）≈ D10 量级
配置类（D1～D6、D9、D13）体积很小（KB～MB）
```

注意：D8 **大于** D7——.sp 是全精度复数落盘（每采样 0.5 个 8 字节 cf32 = 4B/采样），而 2bit 原始数据只有 0.25B/采样。mpifxcorr 的 F 数据流式不落盘，落盘体量此前不可见；"频谱域压缩"实际发生在 D8→D10 的积分折叠。

**通用公式**（单站、单 pol；D7/D8/D10 可精确估算，D11/D12 仅量级）：

| 数据 | 公式 | 说明 |
|---|---|---|
| D7 | `fs × b/8 × nband` | fs = 采样率（样本/s，= 2×band 带宽，见 5.2 节帧结构反推）；b = 记录位宽（1/2/8bit） |
| D15 | `覆盖带宽 × 8B × 时长` | float32 复基带（8B/复样本 × 覆盖带宽复采样率），**恒 = 16× 单站 2bit D7**；覆盖带宽 = 全站 band 跨度（同带多站 = 单站带宽，异带多站放大，见 5.8 节）；float16 可降 8×（预留） |
| D8 | `(nchan/fftchannels) × fs × 8B × nband` | cf32 = 8B/复数；fftchannels = 2×nchan（50% 重叠）时 = **4×fs×nband**，与 nchan 无关（nchan 增大 → 每 FFT 块采样同比增大 → 块数反比减少，两者抵消）；pcal.bin / autocorr.bin 体积可忽略 |
| D10 | `(74 + 8×nchan) B × nband/intTime × nbaseline` | 每基线每 band 每积分一条 74B 头 + nchan 个 cf32 记录（SWIN 布局见 5.3）；nbaseline = N(N−1)/2 |
| D11（FITS-IDI） | ≈ 1~2×D10 | difx2fits 把 SWIN 复制进 FITS UV DATA 加表头，同量级、稍大 |
| D12（Mark4） | ≈ D10 | 同量级复制 |

**D7/D8 比例**（50% 重叠 + cf32 前提下，与 nchan、fs 均无关）：`D8/D7 = 32/b`——2bit → 16 倍；1bit → 32 倍；8bit → 4 倍。只有 ≥32bit 浮点记录（不常用）D7 才反超 D8。

**实例**：

| 场景 | D7 | D8 | D10 |
|---|---|---|---|
| test 配置（1×4MHz band、2bit、8Ms/s、nchan=4096、intTime 1.048576s） | 2 MB/s/站 | 32 MB/s/站（16×） | 31 KB/s（1 基线）→ 2.7 GB/天 |
| 大阵（N=20、8×32MHz band、2bit、64Ms/s、nchan=2048、intTime 2s） | 128 MB/s/站 → 11 TB/天 | 2 GB/s/站（16×）→ 177 TB/天/站 | 12.5 MB/s（190 基线）→ 1.1 TB/天 |

**含义**：① fengine/ 中间数据是 D7 的 16 倍，须按生命周期尽快清理（batch 的 x 完成后即可删，见 12 节）；② D10 才是真正的"小"数据：单基线时 ≈ D7 的 1/64，但随基线数平方增长，大阵可反超 D7；③ D15 公共信号与 D8 同量级（16× 单站 2bit D7），但 batch 的全部 station 完成后即可删（早于 x 完成），生命周期最短。

---

## 12. 切批与重跑约束

- **对齐**：batch 起点必须落在 subint 边界（MJD 秒是 subintNS/1e9 的整数倍），batch 时长 = `intTime` 整数倍。保证 SWIN integration 跨 batch 完整、追加不碎片化。该约束由 fxcorr-sim 前置保证（5.2 节），**fxcorr-f 启动时校验**是最后防线：batch 起点非 subint 边界则报错退出（容差 1µs，吸收 start_mjd 的 f64 表示误差；batch.json 的 start_mjd 建议写精确 repr，如 58948.291666666664）。
- **SWIN 追加**：同一实验所有 batch 写同一 `vis/<experiment>.difx/`；重跑整个实验需清空该目录，重跑单个 batch 需按 subint 范围从对应文件裁掉再追加（V1 不实现单 batch 回滚，重跑 = 全实验重跑）。
- **fengine 覆盖**：重跑某 batch 时，fxcorr-f 覆盖写 `fengine/<batch_id>/` 下文件；fxcorr-x 以 batch.json 的 `status=done` 判定是否需要重跑。
- **batch duration 选择**：硬约束 = intTime 整数倍 + 起点 subint 边界（上文）+ batch_id 秒唯一（时长 ≥1s）。之上权衡：① **调度并行度**——batch 是并行/调度单元（V2 scalebox task），batch 数越多多节点并行越好；② **内存**——x 侧 .sp 整 batch 常驻（V1），duration × 站数 × D8 数据率须在节点内存内；③ **重试成本**——失败重跑整个 batch（V1 无 batch 内回滚）；④ **吞吐 vs 延迟**——启动开销（配置读入、目录、SWIN 头）摊销想大，首个结果的可见延迟与 fengine/ 存储峰值想小。**大 duration 不利**：x 侧内存线性涨（OOM 风险）、失败重算面大、并行度下降（batch 少 → 节点闲置）、端到端延迟高、fengine/ 中间数据峰值大（D8 ≈ 16×D7，见 11 节）。
- **站间时间同步**：per-station 串行架构下站间不靠 MPI 屏障同步，同步信息全在数据时间戳（VDIF 帧 epoch/帧号、.sp header 的 scan/sec/ns）+ .im/.calc 的时钟与几何延迟模型；fxcorr-f 的粗延迟（采样级移位）+ 条纹旋转/小数采样即"把两站拉到同一时刻"。站间残余延迟误差 Δτ 的后果按量级分档：**< 1 采样** → 被小数采样校正吸收；**1 采样～亚 subint** → 带宽 smearing 去相关（幅度 ×sinc(Δτ·Δν)，4MHz 带宽 1 采样误差即 -36%）+ 跨频相位斜坡 2πΔf·Δτ；**帧级（4ms）** → 两站乘不同时刻信号，完全去相关、weight 崩；**subint 级** → .sp 时间戳错位，错位相乘、静默错数据。注意：fxcorr-x 按 .sp header 时间对齐、**不校验站间一致性**（站间错位不报错、静默产出低质量数据，与 mpifxcorr 行为一致）；真实观测的站钟漂移靠 .im 时钟多项式补偿，模型不准的残余误差随时间演化。
- **多 x 子集并行（V2）**：x 任务按站组对切分并行时，各子集写独立子目录 `vis/<experiment>.difx/<subset_id>/`（SWIN 文件名规则不变），difx2fits 前合并到同一目录。V1 单子集（全基线一个任务）无此问题。
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
