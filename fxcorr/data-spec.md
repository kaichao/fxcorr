# fxcorr 数据规范文档（完整版）

**版本**：1.1
**最后更新**：2026-09-27（**第 6 节 batch_id 改为 8 位零填充顺序号**——名字不再承载时间信息，随之取消"batch 时长 ≥ 1 s"的人为约束；位数依据与"band 该不该进 batch"的取舍见 `data-volume.md` §7.6；第 12 节同步。09-26：新增 **D16 分片局部记录**与 5.9 节 `vis-parts/` 目录规范，配套 `data-volume.md` §7 的"计算单元按 ds 分片 + SWIN 合并"方案——合并实现为 **fxcorr-x 的 `merge` 子命令**；第 2 节目录表、第 3 节数据总表、第 4 节模块 I/O、第 9 节数据流、第 10 节命名汇总、第 12 节生命周期表同步）。
**上一版更新**：2026-09-21（**第 11 节的体积公式移入 `data-volume.md` §1.3**，本节只留速查；V5 P6 讨论：第 12 节补六条——**时间层级表**（FFT 块 / Core 短积分 /
Manager 最终积分 ↔ fxcorr 的 `.sp` 块 / subint / intTime）、batch 时长与起点约束（`intTime`
整数倍、下限随配置而变而非固定秒数、**起点须落在积分边界**——并记下当前三层校验都只覆盖
subint 对齐这一缺口）、SWIN 文件粒度（实验级一组文件、非每 batch 一个）、difx2fits 的
触发与输出、SWIN 生命周期、**全链路数据生命周期表**（D15/D7/D8/D14/D10/D11/D12 的最早可删
时点、粒度与再生前提）。09-20：第 1 节明确"计算单元 = batch（f/x 同节点）"，据此把 `sim-common/`
的可见性要求收窄为"同一 batch 读同一份"——本地盘合法，5.2.1 增"各根的可见性要求"；同日 V5 P5：
目录根变量化——5.2.1 为规则权威；`common/` 改名 `sim-common/`；`work/` 删除。09-19：5.2 补
invalid 位帧的语义）
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

- **计算单元 = batch（2026-09-20 明确）**：一个 batch 是自足的处理单元——该 batch 的 `common` → 全部站的 `station` → 全部站的 f → 一次 x，全在同一节点完成；**f 与 x 同节点，baseline 不跨节点**。依据是目录本身即为 batch 级（`sim-common/<batch_id>/`、`fengine/<batch_id>/<station>/`），batch 之间互不依赖。由此：f 只需本节点该 batch 的 raw；x 只需本节点该 batch 全部站的 fengine——**raw 与 fengine 都不需要跨节点汇聚**，"每节点只持有自己写的部分"正是这个形态。
- **共享存储主数据流**：默认一切都在 `$FXCORR_WORKDIR` 下，即全部节点共享同一份**全局存储**。需要本地化时按 5.2.1 的根变量把 `raw/`（TB 级）与 `fengine/` 指到本地盘，目录逻辑集中、物理分布（每节点只持有自己负责的 batch）。**按上面的 batch 切分，`sim-common/` 同样可以落本地盘**——它要求的是"读同一份"而不是"跨站共享一份"：同一 batch 的公共信号只要被它的全部 station 任务读到即可，同节点必然满足（见 5.2.1 的可见性要求）。配置与元数据（`config/`、`batches/`、`meta/`）没有独立根、恒在 workdir 下；**真正必须全局可见的是实验级的 `vis/` 与 `product/`**——SWIN 跨 batch 追加、difx2fits 一次读整个实验，各节点各写各的会静默分裂（见第 2 节存储归属表与第 12 节）。分片模式（5.9 节）下 **`vis-parts/` 同样必须全局可见**——各节点的 x 分片任务写、`merge` 子命令统一读；它无独立根，恒在 workdir 下。
- **网络/计算/存储权衡**：必要时以**重复计算**（同一数据各节点各算一份，省网络传输、费 CPU）或**网络传输**（拉数到计算节点，省 CPU、费网络）为调节手段，在三者复用上取平衡；实现不应做死"必须本地"或"必须共享"的假设。

---

## 2. 顶层目录结构

```
project/                          # 项目根目录（FXCORR_WORKDIR，可自定义）
├── config/                       # 配置与模型文件
├── batches/                      # 批量元数据（batch.json，D9）
├── sim-common/                   # 仿真公共信号（fxcorr-sim common 输出，D15）
├── raw/                          # 原始基带数据
├── fengine/                      # fxcorr-f 输出（频域谱）
├── vis/                          # fxcorr-x 输出（SWIN 可见度）
├── vis-parts/                    # x 分片的局部记录（D16；合并成 SWIN 后即删）
├── beam/                         # fxcorr-x 相位阵输出（波束频谱，P8）
├── product/                      # 最终科学产品（FITS / Mark4）
└── meta/                         # 全局索引、日志与根记录（meta/roots/）
```

多节点部署的存储归属（**V5 P5 起由根变量决定**，见 5.2.1）。**默认全部在 project/ 下，
即所有节点共享同一份全局存储**；需要把某类数据放到别处（本地盘、另一块共享盘）时，
才给对应的根变量赋值：

| 目录 | 默认位置 | 独立根 | 说明 |
|---|---|---|---|
| `config/` `batches/` `beam/` | `$FXCORR_WORKDIR/<name>` | ❌ | 量小、或按 batch 组织在 workdir 内；恒在 workdir 下（Q17） |
| `meta/` | `$FXCORR_WORKDIR/meta` | ❌ | 同上；索引与根记录必须全局一致可见 |
| `raw/` | `$FXCORR_WORKDIR/raw` | ✅ `FXCORR_RAW_ROOT` | TB 级原始基带，可指向各记录节点的本地盘 |
| `fengine/` | `$FXCORR_WORKDIR/fengine` | ✅ `FXCORR_FENGINE_ROOT` | 各站 f 输出，可指向计算节点本地盘（目录逻辑集中、物理分布） |
| `sim-common/` | `$FXCORR_WORKDIR/sim-common` | ✅ `FXCORR_SIM_COMMON_ROOT` | 全部目录里最大（≥16× 单站 2bit，见 `data-volume.md`）；**本地盘或共享盘均可**——要求是"同一 batch 读到同一份"，batch 切分下该 batch 的站同节点，本地盘即满足；同一 batch 的不同站在不同节点、各自读一份本地副本才是违例 |
| `vis/` | `$FXCORR_WORKDIR/vis` | ✅ `FXCORR_VIS_ROOT` | SWIN 跨 batch 追加、difx2fits 直读（追加保序前提：同实验 batch 串行，见第 12 节） |
| `vis-parts/` | `$FXCORR_WORKDIR/vis-parts` | ❌ | x 分片的局部记录（D16，见 5.9）；**必须与 `vis/` 分离**——difx2fits 只 glob `<outputFile>/DIFX*`，放别处即隔离；量极小，恒在 workdir 下（2026-09-26） |
| `product/` | `$FXCORR_WORKDIR/product` | ✅ `FXCORR_PRODUCT_ROOT` | 可本地生成，再由编排层迁移到全局 |

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
| D8 | 频域谱数据 | `fengine/<batch_id>/<station>/ds_<N>/band_XX.sp` | fxcorr-f | 二进制（.sp） | 较大 | Station-based 结果：延迟对齐、条纹旋转、小数采样校正、FFT 后的复数频谱；N = 站内 datastream 序号 |
| D9 | 批量元数据 | `batch.json` | 编排脚本（make_testdata.sh / run_batch.sh，一次写全） | JSON | KB | 每个批量的描述信息；三工具只读 |
| D10 | 可见度数据 | `vis/<experiment>.difx/DIFX_*.s*.b*` | fxcorr-x | SWIN 二进制 | MB~GB | Baseline-based 结果，difx2fits/difx2mark4 直读 |
| D11 | FITS 科学产品 | `*.FITS` | 后处理 | FITS-IDI | MB~GB | 天文标准格式 |
| D12 | Mark4 科学产品 | Mark4 文件集 | 后处理 | Mark4 | MB~GB | 测地学常用格式 |
| D13 | 全局索引/日志 | `meta/` | 运行过程 | 文本/JSON | KB~MB | 批量索引、运行日志等 |
| D14 | 波束数据 | `beam/<batch_id>/beam.bin` | fxcorr-x（相位阵） | 二进制（beam.bin） | MB 级 | 相位阵波束加权和频谱，按 acc 窗口记录（P8 2026-09-13；上游无对照格式，fxcorr 自定，见 5.5 节） |
| D15 | 仿真公共信号 | `sim-common/<batch_id>/` | fxcorr-sim common | 二进制（float32 频域 slice）+ JSON | 较大 | 频域公共信号（量化前复基带频谱，权威一份，各站 station 只读切频段；≥16× 单站 2bit 数据量，见 `data-volume.md`）；格式见 5.8 节（2026-09-14 新增） |
| D16 | 分片局部记录 | `vis-parts/<batch_id>/...` | fxcorr-x（分片任务） | 二进制（格式待定） | 极小（≈ D10 总量级） | 每个 x 分片任务产出的可见度记录，由 fxcorr-x 的 `merge` 子命令按时间归并写出正式 SWIN（D10）；**写入顺序非自由**，见 5.9 节（2026-09-26 新增） |

---

## 4. 各模块输入输出对应表

| 模块 | 类型 | 主要输入 | 主要输出 | 备注 |
|------|------|----------|----------|------|
| **vex2difx** | 前处理1 | D1（.vex）、D2（.v2d） | D3（.input）、D4（.calc）、D5（.flag） | 纯配置生成，不碰原始数据 |
| **difxcalc / calcif2** | 前处理2 | D4（.calc） | D6（.im） | 生成几何延迟模型 |
| **fxcorr-sim common** | 数据生成1 | D3（.input）、D9（batch.json） | D15（sim-common/ 公共信号） | 每个 batch 一次；频域公共信号按全站 band 布局推导 specRes 网格（5.8 节） |
| **fxcorr-sim station** | 数据生成2 | D3、D9、D15 | D7（raw/ 单站 VDIF） | 每站一任务，多节点并行；只读公共信号，不改写 |
| **fxcorr-f** | 核心（Station-based） | D3（.input）、D4（.calc）、D6（.im）、D7（raw）、D9（batch.json，只读） | D8（频域谱+自相关+pcal） | 按台站、按批量处理 |
| **fxcorr-x** | 核心（Baseline-based） | D3（.input）、D4（.calc）、D6（.im）、D8（fengine）、D9（batch.json，只读） | D10（SWIN 可见度）；相位阵配置时改出 D14（beam.bin，无 SWIN） | 按批量处理多台站数据；UVW 由模型求值。**分片模式下改出 D16**（不再直接写 SWIN，见 5.9） |
| **fxcorr-x `merge`** | 归并（子命令，**方案，待实施**） | D16（vis-parts 局部记录） | D10（SWIN 可见度） | 按整数纳秒时间戳归并；SWIN 的**唯一写入者**（`data-volume.md` §7.5）。与分片任务**同二进制、不同调用**，复用 fxcorr-x 已有的 SWIN 写入路径 |
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
│   ├── sta1_60512_45000_ds0.vdif     # 多 datastream 站：每 ds 一个文件
│   ├── sta1_60512_45000_ds1.vdif
│   ├── ...
│   └── sta1_60512_45030_ds0.vdif     # 下一个 batch
├── STA2/
│   └── ...
└── STA3/
    └── ...
```

- 编号：D7
- 按台站组织
- 命名建议：`<station>_<batch_id>.<suffix>` 或保持原始记录名。**多 datastream 站必须每 ds 一个文件、文件名含 ds 编号**（如 `<station>_<batch_id>_ds<N>.vdif`）——同站不同 ds 若同名会互相覆盖；每个 ds 的文件按 `.input` DATA TABLE 的对应行软链。**（fxcorr-sim 目前只生成第一个 ds；多 ds 生成与"mpifxcorr 与 fxcorr 各取所需"的对拍方案见 `v5-plan.md` P6「多 datastream 生成」，2026-09-27）**
- **`_ds<N>` 的 N 是站内序号**（0-based，按 `.input` DATASTREAM 表里该站出现的次序）——**与 `fengine/<batch_id>/<station>/ds_<N>/` 的 N 同一口径**（fxcorr-x 定位 `ds_N/` 时算的就是这个站内序号），也与 `fxcorr-f` 的 `ds_index` 参数同一口径。**三处必须一致**，否则 f 会读错文件。
- 数据量：TB 级
- 约束：一个 raw 文件的时间范围须覆盖完整 batch；切批见第 12 节
- **文件起点语义（约束）**：file-per-batch 布局下 raw 文件的时间起点**不要求**等于 batch 起点（真实观测中各记录系统的帧计数器不同相，文件可以从某一秒的中途开始）；VDIF 文件的时间起点由**首帧时间戳**确定，起点之前（batch 头或 subint 区间）无数据的部分判为无效。batch.json 的 start_mjd 是 batch 起点的精确表示。文件中间缺帧（记录中断，含"直接缺帧"与"filler 帧占位"两种形态）同样受支持。**帧头标了 VDIF invalid 位的帧按"在时间轴上在位、数据不可用"处理**——它照常占一个时间槽（帧号参与连续性判断），只是对应的块在 f 侧被标无效；这与"占字节不占时间轴"的 filler 是两回事，实测与定案见 `fxcorr/reader-model.md` 4.12。
- **VDIF 的时间参考**：帧头秒字段是**当日秒**（对 86400 取模、不带日期），帧号是秒内序号（对帧率取模，每秒回绕；t25362 为 16000 fps）。文件的时间起点由首帧的 (秒, 帧号) 唯一确定，**不保证落在整秒边界**——这也是"文件起点 ≠ batch 起点"的成因。
- **读取路径的完整分析见 `fxcorr/reader-model.md`**：读模型对照（顺序读 vs 定位读）、缺陷根因与症状指纹总表（A 起点 / B 缺口 / C filler / D 有效性）、`GAPCHECK`/`READPOS` 诊断判据、改造建议；f 侧目录级实现要点见 `applications/fxcorr-f/CLAUDE.md`。

- 仿真数据（测试替身，由 fxcorr-sim 生成）约束：batch 时间窗须与 subint 网格对齐（fxcorr-sim 读 .input 的 subint 结构保证，见第 12 节）；多节点分布生成时各分片的 VDIF 帧时间戳/帧号须全局连续（程序内校验点）；最小数据集可入仓库（`fxcorr/test/`，附 sha256），不受"运行时数据不进 git"约束

#### 5.2.1 仿真数据生成器（fxcorr-sim）规范

fxcorr-sim 是 datasim 的替身（datasim 因上游 IPP 依赖无法构建），单二进制三入口（架构见 fxcorr-sim-arch.md）：`common` 生成共享公共信号（D15，5.8 节）、`station` 读公共信号生成单站 VDIF、无子命令本机串行。其配置输入与参数语义：

- **目录根定位（V5 P5 起，规则唯一权威在本节）**：三工具（fxcorr-sim/f/x）与编排脚本
  （make_testdata.sh / run_batch.sh / run_bench.sh / wrap_vex2difx.sh / wrap_difxcalc.sh /
  wrap_difx2fits.sh）共用同一套解析——C++ 侧实现在
  `libraries/fxcorrcommon/src/fxcorrpath.cpp`，bash 侧在 `fxcorr/roots.sh`，两处必须同规则。

  **三档回退**：① `FXCORR_<X>_ROOT` 已设置 → 用它的值（相对则按 cwd 绝对化）；② 否则 →
  `<workdir>/<规范目录名>`。`workdir` 本身按"位置参数 > `FXCORR_WORKDIR` > 默认 `.`"解析
  并绝对化。五个可重定向的根与各自接管的路径：

  | 变量 | 接管 |
  |---|---|
  | `FXCORR_RAW_ROOT` | `.input` DATA TABLE 的 `FILE d/d:`（相对时）；DATA TABLE 软链的落点 |
  | `FXCORR_SIM_COMMON_ROOT` | fxcorr-sim 的公共信号目录 |
  | `FXCORR_FENGINE_ROOT` | f 写 / x 读的 `.sp` 目录 |
  | `FXCORR_VIS_ROOT` | `.input` 的 `OUTPUT FILENAME`（相对时）；PCAL / SWITCHEDPOWER 文本 |
  | `FXCORR_PRODUCT_ROOT` | difx2fits 的产物（由 wrap_difx2fits.sh 使用） |

  `config/` `batches/` `meta/` `beam/` **没有独立根**，恒在 `$FXCORR_WORKDIR/` 下。
  **不设任何根 = 全部在 workdir 下**（默认全共享），设了才换位置。

  **各根的可见性要求（2026-09-20 收窄）**：按第 1 节的 batch 切分部署时，`RAW` / `FENGINE` /
  `SIM_COMMON` 三个 **batch 级根只需对该 batch 所在节点可见**。`SIM_COMMON` 因此**可以是本地盘**
  ——公共信号的约束是"同一 batch 读同一份"，而不是"全站共享一份"：station 任务与它的
  common 都在同一节点时天然成立（跨 batch 的多节点各自生成各的 common 互不影响，它只与
  `(seed, batch)` 有关）。**必须全局可见的只有实验级的 `VIS` / `PRODUCT`**：SWIN 跨 batch 追加
  同一组文件、difx2fits 一次读整个实验，分裂了没有任何报错。

  **两类路径的基准**——只对**相对**路径拼根，**绝对路径一律原样**（真实观测的 `FILE` 行
  就是绝对路径）：

  | 路径 | 相对谁 |
  |---|---|
  | DATA TABLE 的 `FILE d/d:` | `FXCORR_RAW_ROOT` |
  | `.input` 的 `CALC FILENAME`；`.calc` 的 `IM` / `FLAG FILENAME` | **`.input` 所在目录**（配置是一套，整个 config 目录可搬走） |
  | `.input` 的 `OUTPUT FILENAME` | `FXCORR_VIS_ROOT`——**写 `test.difx`，不要写 `vis/test.difx`**，否则会拼成 `$VIS_ROOT/vis/test.difx` |

  **根记录与一致性检查**：编排脚本开跑前把生效的根写 `meta/roots/<batch_id>.json`；三个
  程序启动时与该文件比对**自己真的会用到**的根，不一致即报错退出（只比用到的——否则会被与
  自己无关的根误伤）。脚本侧另有一道：`vis`/`product` 跨 batch 追加，与同实验已有 batch 的
  记录不一致即报错。

  **其余环境变量**：`FXCORR_PRINT_ROOTS=1` 打印各根解析结果与来源；`FXCORR_RUN_MODE` /
  `FXCORR_LOGLEVEL` / `FXCORR_STA` / `FXCORR_KURTOSIS` 见 usage.md。

  **原 difx 程序的调用约定**：vex2difx / difxcalc / difx2fits / mpifxcorr **不认识根变量**，
  且按 cwd 解析 `.input`/`.calc` 内的相对路径——一律由 bash 封装脚本做"当前规范 → 原程序所需
  形式"的转换（vex2difx/difxcalc 另把产物里的绝对路径规范化回相对），不要在新编排代码里
  直接调用它们。两个具体例子：`wrap_difx2fits.sh` 把 VIS 根下的 `.difx` 软链到 config/ 旁再调
  difx2fits；`run_bench.sh`（对拍基准）用 sed 把 `.input` 的 `OUTPUT FILENAME` 改指 `bench/`
  ——**它是唯一一处 SWIN 落点不走 `FXCORR_VIS_ROOT` 的地方**，因为它跑的是 mpifxcorr。
- **配置来源**：读 `$FXCORR_WORKDIR/batches/<batch_id>.json`（D9）取 start_mjd / n_subints / config_file；band 结构、采样率、PHASE CAL tone 网格全部来自 `$FXCORR_WORKDIR/<config_file>`（.input，非 MPI 构造），与 fxcorr-f 同一解析语义——这是分批次对齐要求的硬理由。common 的 specRes/numSamps 网格由**全站** band 布局推导（5.8 节）。
- **任务粒度**：common 任务 = (batch_id)（每 batch 一次）；station 任务 = (batch_id, station)（与 fxcorr-f 同构，多节点并行）；一致性靠共享存储单份 common，不靠多节点重复生成。
- **legacy 模式**：station 子命令带 tone_mhz 位置参数时走旧时域合成路径（tone/噪声/pcal/FXSIM_DELAY/FXSIM_FLUX/FXSIM_SEFD/FXSIM_ADAPTIVE），供字节对拍回归；tone 参数 0 个 = 无 tone、1 个 = 所有 band 同频率、nbands 个 = 逐 band。新路径下 tone 由 P2 谱线机制（common 端频域注入）提供。
- **噪声**：`FXSIM_NOISE`（高斯噪声 σ，默认 0.02；0 关闭），`FXSIM_SEED`（公共种子，默认固定）。公共信号只与 (seed, batch) 有关、与站无关；站噪声种子 = f(seed, station) 派生。两站噪声全关时输出逐位一致（跨站相干校验）。
- **PHASE CAL 注入**：`.input` 的 `PHASE CAL INT (MHZ)` > 0 时按 Configuration 的 tone 网格自动注入（幅度 0.7，避开 2bit 量化器电平陷阱，见 applications/fxcorr-sim/CLAUDE.md），频率/计数与 fxcorr-f 提取端完全一致，构成注入-提取闭环（legacy 与新路径均实现；新路径 P4 双语义：.input PHASE CAL 网格 tone（0.7 幅度、batch 起点连续相位，两路径共用网格读取）＋ FXSIM_PCAL 梳齿（datasim `-p` 语义：k·interval MHz、1/500 幅度、帧边 taper））。
- **输出**：`$FXCORR_RAW_ROOT/<station>/<station>_<batch_id>.vdif`（2bit VDIF；多 band 时帧内
  样本 band 交织；帧时间戳/帧号按 batch 起点换算逐帧自增）。未设该根时即
  `$FXCORR_WORKDIR/raw/`。

### 5.3 F-Engine 输出（fengine/）

```
fengine/
├── 60512_45000/                      # batch_id
│   ├── STA1/
│   │   ├── ds_0/                     # 站内 datastream 序号（多 datastream 站
│   │   │   │                         #   每记录线程一个，f 任务带 ds_index）
│   │   │   ├── band_00.sp            # D8：recorded band 频谱
│   │   │   ├── band_01.sp
│   │   │   ├── ...
│   │   │   ├── pcal.bin              # 脉冲校准 tone（每 subint）
│   │   │   └── autocorr.bin          # 自相关（每 subint，已频率平均）
│   │   └── ds_1/
│   │       └── ...
│   ├── STA2/
│   │   └── ds_0/
│   │       └── ...
│   └── STA3/
│       └── ds_0/
│           └── ...
├── 60512_45030/
│   └── ...
└── ...
```

单 datastream 站同样是 `ds_0/`（布局统一，无平铺特例）。f 任务 = (batch_id, station, ds_index)；`ds_index` 是站内 datastream 序号（0-based，按 .input datastream 序），fxcorr-x 按同一规则回读。

**batch.json 示例**（D9，全字段单文件）：

```json
{
  "batch_id": "60512_45000",
  "start_mjd": 60512.520833333,
  "start_time": "2026-09-08T12:30:00",
  "duration_sec": 30.0,
  "stations": ["STA1", "STA2", "STA3"],
  "baselines": ["STA1-STA2", "STA1-STA3", "STA2-STA3"],
  "config_file": "config/experiment.input",
  "calc_file": "config/experiment.calc",
  "im_file": "config/experiment.im",
  "n_subints": 30,
  "subint_ns": 1000000000,
  "integration_sec": 1.0,
  "n_channels": 256,
  "polarizations": ["RR", "LL", "RL", "LR"],
  "difx_dir": "experiment.difx",
  "created_at": "2026-09-08T12:35:12Z",
  "status": "done",
  "fxcorr_f_version": "0.1.0",
  "fxcorr_x_version": "0.1.0"
}
```

字段说明：

- 时间与结构：`batch_id` / `start_mjd` / `start_time` / `duration_sec` / `n_subints` / `subint_ns`——fxcorr-sim 与 fxcorr-f 读（start_mjd 建议写精确 repr，如 58948.291666666664）。
- `config_file` / `calc_file` / `im_file`：三工具读 .input（config_file）；calc/im 为元数据。
- `stations`：全部参与站（x 任务 = 全站全基线，任务集推导见第 6 节）。
- `baselines` / `integration_sec` / `n_channels` / `polarizations` / `difx_dir`：可见度输出元数据，均可由 .input 提前推导（baselines = 全部基线组合，polarizations 由 BASELINE TABLE 定）。**`difx_dir` 是纯记录字段**（V5 P5 Q10 起）：写的是 `.input` 的 `OUTPUT FILENAME` 原样，**没有任何程序读它**——SWIN 的实际落点是 `FXCORR_VIS_ROOT` + `OUTPUT FILENAME`（data-spec 5.2.1），**不要拿 `difx_dir` 当"这批数据落在哪"的权威**，那件事只有 `meta/roots/<batch_id>.json` 知道。
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
  14    char      pol（1 字节，"R"/"L"，取自 recordedbandpols）
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
├── batches.index                     # D13（当前唯一落盘索引，run_batch.sh 追加）
└── difxmsg/                          # DifxMessage 落盘日志（container 模式，algo-plan P1）
    ├── <exp>_<batch>.xml              # fxcorr-x：每条完整 XML（Starting/Running/Ending/Done/Alert）
    ├── <exp>_<batch>_<station>.xml    # fxcorr-f：同上 + 每 subint 两条 Diagnostic
    └── <exp>_<batch>_<station>.sta    # fxcorr-f：DifxMessageSTARecord 二进制追加（FXCORR_STA=1）
```

（stations.json / run.log / versions.txt 为早期预留命名，未实现，已删。）

difxmsg/ 仅 container 模式（`FXCORR_RUN_MODE=container`）产生：组播受限的容器内降级为落盘，由编排层（scalebox）读取转发；文件内容与 host 模式组播包逐字节一致。每进程一个文件（文件名含 batch_id/station），进程启动时截断重写——重跑 batch 幂等，无并发追加竞态。

**当前状态：暂不启用（2026-09-17 定）**。host 模式默认不设 `DIFX_MESSAGE_GROUP`/`DIFX_MESSAGE_PORT`，`DifxMonitor` 的每次发送都是静默 no-op（其头注释即此设计语义），故 `meta/difxmsg/` 不会产生，也不部署 `difx_monitor`/`guiServer` 等消费端。**运维侧的全局视图由 scalebox 编排层承担**：task 级状态码管理与出错信息查看，经标准输出、标准错误、工具定制输出与上下文日志几种形式记录。batch 生命周期只有十秒量级（t25362 一个 batch 11.264 s），单 batch 的实时状态本就不是运维需要的粒度，**聚合视图属编排层职责**。后期如需接入 DiFX 监控生态，再评估 host 组播（需部署组播网络 + 消费端）或 container 落盘（零组播，内容与组播包逐字节一致，见上）。

**注意 `Alert` 是另一条独立通道**：fxcorrcommon 里上游代码（`configuration.cpp` 等）直接调 `cinfo`/`cverbose`/`cdebug`，经 `difxMessageSendDifxAlert` 落地——该函数在 `difxMessagePort < 0` 时**退化为打印** stdout/stderr，而 `difxMessagePort` 初值 -1、只有 `difxMessageInit()` 会设置它；**f/x 都不调用该函数，所以这条兜底打印是唯一路径，这些消息永远不会组播**。它不随本决策改变，由 `FXCORR_LOGLEVEL` 控制详略（见 usage.md 与 `applications/fxcorr-f/CLAUDE.md`）。

**batches.index 示例**：
```
60512_45000,done,2026-09-08T12:35:12Z
60512_45030,running,2026-09-08T12:36:01Z
60512_45060,pending,
```

**未来：SQLite 索引（2026-09-26 定，未实施）**——新的编排框架（scalebox）计划把 batch 元数据
另存一份 SQLite 表，以支持结构化查询（按状态 / 时间 / 实验 / band 检索）。

- **权威仍是 `batches/*.json`**（D9），sqlite 是**派生索引**、可由 JSON 随时重建。这条不能反——
  `test/` 下十余个回归流程都靠**手改 `batch.json`**（改 `config_file` 指不同输入）跑，若 sqlite
  成了权威，手改就失效。
- **单一写者**：只有编排层主路由写（单线程），计算节点只读或完全不碰。**不能多进程写**——
  sqlite 的 WAL 依赖共享内存、在网络文件系统（NFS/Lustre）上不可用，rollback journal 模式
  性能差且仍有损坏风险；共享存储上多写者是经典事故。
- **实现依赖**：用 python3 标准库的 `sqlite3`（编排脚本本就用 python3），**不引入 libsqlite3
  到 C++ 工具**——零新增系统依赖，容器镜像不用改。

### 5.8 仿真公共信号（sim-common/）

```
sim-common/
└── <batch_id>/                      # fxcorr-sim common 输出（D15）
    ├── meta.json                    # 格式版本与网格参数
    └── data_XX.bin                  # 每 0.5s 块一个文件（XX 从 00 顺序编号）
```

- 编号：D15；产生者：`fxcorr-sim common <batch_id>`（每个 batch 一次）；消费者：`fxcorr-sim station`（各站任务只读，不改写）。格式单独定版本，**改文件格式必须先同步本节并递增 version**。
- **信号语义**（datasim gencplx 移植）：量化前频域公共信号——覆盖全站 `[minStartFreq, maxStartFreq+maxBW]` 的复基带频谱时间流，每 `stime = 1/specRes` µs 一个 `numSamps` 点复频谱 slice（STDEV=1 高斯复噪声，实虚独立）；各站 station 端按自己 band 的 (startIdx, blksize) 切频段、加站噪声、逆 DFT 出复基带（切出的频段逐位相同 = 跨站相干来源）。可选谱线（FXSIM_LINE）：gencplx 后逐 slice 乘高斯滤波器（√amp·exp(−π²δ²/2rms²)，δ = 网格点距，re=im 同乘）。
- **网格参数**（由全站 band 布局推导，datasim getSpecRes 移植）：`specRes` = 全站 band 频率差/带宽的 GCD（0.5 MHz 起、二分至 1/2^10，找不到报错）；`numSamps = (maxStartFreq+maxBW − minStartFreq)/specRes`（覆盖全站 band 的实际跨度——band 间有间隙（如 200/205 MHz）时间隙网格点照常生成，仅不被任何站读取；datasim 的 `maxChanFreq = band0带宽×band数` 假设 band 连续、间隙布局会静默越界读公共信号，不照抄）；`minStartFreq` = 全站最低 band 频率。可选 specRes 缩放（FXSIM_SPECRES，正整数）：网格 ÷N 后一致性检查照跑（datasim --specres 语义）。band 频率须落在网格（`(freq−minStartFreq)/specRes` 整数，common 端校验）。
- **meta.json 字段**：

| 字段 | 说明 |
|---|---|
| `version` | 格式版本（初版 1；随布局变更递增） |
| `dtype` | 数据元素类型（初版 `float32` 复数对；留 int16 降级口，见 `data-volume.md`） |
| `spec_res_mhz` / `numsamps` / `min_start_freq_mhz` | 网格参数（上文） |
| `block_bytes` / `slices_per_block` | 每块字节数与 slice 数（`slices_per_block = 0.5s/stime`） |
| `nblocks` | 块文件数（batch 末块按 batch 时长截断） |
| `seed` | 公共信号种子（与站无关；站噪声种子 = f(seed, station) 由 station 端派生） |
| `batch_id` / `start_mjd` | 归属批量与时间起点 |
| `line_freq_mhz` / `line_amp` / `line_rms` | 谱线参数（FXSIM_LINE，2026-09-14 P2 新增可选字段，version 仍为 1 向后兼容；全 0 = 无谱线；rms 单位 = 网格点数） |
| `status` | `running` / `done`（先写数据再置 done，station 以 done 为就绪判据） |

- **数据文件布局**：`data_XX.bin` 内按 slice 顺序平铺——每 slice `numSamps` 个复数（re,im 各 float32 小端交替），slice 内频点序 = 网格升序（minStartFreq 起）；块 XX 覆盖 batch 第 `XX×0.5s` 起的 0.5s。
- **完成可见性**：块文件先写 `<name>.tmp` 再 `rename`；全部块落盘后 meta.json 写 `status=done`。station 端发现 meta 缺失或 status≠done 即报错退出。
- **生命周期**：batch 的全部 station 任务完成后 `sim-common/<batch_id>/` 可删（编排层清理，同 fengine/ 12 节语义）；重跑 batch 时 common 覆盖生成（status 回 running→done）。
- **数据量**：float32 复基带 = 8 字节 × 覆盖带宽 × 时长，恒为单站 2bit VDIF 的 16 倍（`data-volume.md` §1.3）；异带多站时覆盖跨度放大（可达全站总量 20 倍），共享存储容量须按此评估。

### 5.9 分片局部记录（vis-parts/，D16，2026-09-26 新增）

```
vis-parts/
└── <batch_id>/
    ├── ds0.part                      # 一个 x 分片任务的产出（一个 ds 组，跨站）
    ├── ds1.part
    └── ...
```

- **编号**：D16；产生者：fxcorr-x 的分片任务（一任务一份，粒度 `(batch_id, ds_group)`——`ds_group` = **跨站、含全部极化**的一组 datastream，见第 6 节「任务标识」与 `data-volume.md` §7.3）；消费者：**fxcorr-x 的 `merge` 子命令**（同一二进制的另一次调用）——读全部 `.part`，按时间归并，写出正式 SWIN 到 `vis/`。命令行形态见 `usage.md` 的 fxcorr-x 节。
- **分组依据（硬约束，2026-09-27 实测后定）**：`ds_group` 的成员必须从 **`.input` 的 BASELINE TABLE 推导**——即"覆盖同一频段组的那些 baseline 条目所涉及的 ds 集合"，**不是按 ds 序号猜**。每条 baseline 条目绑定一对具体的 ds、只出一个极化产品（极化展开进 baseline 编号，机理见第 8 节），所以只含单极化的分片会丢掉该频段的 RL/LR/LL。t25362 实测：每组 4 个 ds（两站 × 两极化），共 4 组。**同一 batch 内数据齐备**（f 本就覆盖该时段全部站的全部 ds），所以只要分组正确，该频段声明的 baseline 都能算出。
- **为什么需要这一步**：SWIN 的追加顺序**不是自由的**。difx2fits 顺序读记录（`DifxVisRecordgetnext` 是唯一读取原语），并按天线检查时间单调（`fitsUV.c:1227` 的 `RecordIsOld`）；时间回退的记录被**静默丢弃**，只在结尾打印一行 `out-of-time-range records dropped`（`fitsUV.c:1868`），不报错。分片任务各自追加同一组文件必然时间回退，所以**写入必须集中在唯一一处**——`merge` 与分片任务是不同的进程调用，分片任务不再写 SWIN，唯一写入者仍然成立。分析与方案见 `data-volume.md` §7.5。
- **与 `vis/` 的边界（硬约束）**：difx2fits 只 glob `OUTPUT FILENAME` 目录下**以 `DIFX` 开头**的文件（`fitsUV.c:82-98`，glob 模式为 `<job.outputFile>/DIFX*`）。因此 `.part` **既不放进 `vis/` 目录、也不使用 `DIFX` 前缀**——两重隔离，杜绝被误当正式 SWIN 读入。`ds<G>.part` 这个命名是 fxcorr 自定的：**DiFX 生态里不存在"可见度分片"这个概念**，没有既有约定要遵守，只需避免与 `DIFX*` 撞名。
- **记录内容**：每条记录须自足到"能重建一条 D10 记录"——SWIN 记录头的全部字段（baseline 号、时间、config/source/freq 索引、极化对、pulsar bin、weight、UVW）+ 复频谱数据。（具体格式待定，定稿时递增 version 并回写本节。）
- **归并 key**：用**整数纳秒时间戳**（由 scan 起点与 `offsetns` 推算），不是浮点 `sec`——浮点相等比较不可靠。
- **生命周期**：实验的合并写出 SWIN 后即删（编排层清理，同 `fengine/` 第 12 节语义）；重跑分片时覆盖写自己的 `.part`。
- **等待策略**：全量（整个实验的分片到齐后一次合并）或增量（按积分窗口滚动）；分片迟到时的行为必须在编排层显式定义（等 / 超时跳过留空洞 / 告警），见第 12 节。
- **缺片处理（硬约束 + 逃生口）**：`merge` 启动时先核对本 batch 的 ds 组是否集齐，**缺则报错退出、不写任何东西**（stderr 列出缺哪几组）。设 `FXCORR_X_MERGE_FORCE=1` 可强制写出已到齐的部分——缺失组对应的频段在该 batch 的时间段内**没有记录**，difx2fits 不会因此报错，只是静默缺段（只在 `merge` 的 stderr 日志里留痕）。默认严格是刻意的：分片缺失通常意味着任务失败或未调度，静默缺段比报错危险得多。
- **与"全 band 模式"互斥（硬约束，2026-09-27）**：同一 batch 的 fxcorr-x 只能选一种模式——**不分片**（一次处理全部 ds，直写 SWIN）或**分片**（按 `ds_group` 多次写 `.part`，再由 `merge` 写 SWIN）。**两者不能对同一 batch 混跑**：分片任务不写 SWIN，而 `merge` 会为该 batch 的时间范围追加记录，混跑会产生重复记录、破坏 SWIN 的时间单调（§7.5）。**f 侧不受影响**——f 天然是每 ds 一个任务，两种模式下产出**完全相同**的 `fengine/`；选哪种只决定 x 的一次跑还是多次跑。

---

## 6. 批量标识（batch_id）规范

**格式（2026-09-27 修订）**：**8 位零填充十进制顺序号**——`00000001`、`00000042`。

| 项 | 规定 |
|---|---|
| 形态 | `NNNNNNNN`，固定 8 位、零填充。**定宽是必需的**：不定宽时 `100` 会排在 `42` 前面，字符串排序就不再等于编号顺序 |
| 作用域 | 同一 `workdir` 内**全局唯一、单调递增**（跨实验也不重复）。容量 10⁸ 个 batch——1.024 s 的 batch 可连续覆盖 **1185 天**，0.512 s 的覆盖 592 天，切到 5.12 ms（1 个 subint）也有 5.9 天。**位数为何取 8、以及"band 该不该进 batch"的取舍，见 `data-volume.md` §7.6** |
| 时间语义 | **batch_id 不承载任何时间信息**。起点、时长、band 等全部在 `batches/<batch_id>.json`（D9）里，字段见 5.3 |
| 分配 | 由**切批规划步骤单点分配**（现为 `make_testdata.sh`，未来是 scalebox）：取 `batches/` 下已有编号的最大值 +1。**计算节点不生成 batch_id** |
| 排序 | 字符串排序 = 编号顺序 = **时间顺序**（由分配时的单调性保证，与 batch 的**处理**顺序无关——乱序补跑不影响） |
| 一致性 | 同一 batch 在 `fengine/`、`sim-common/`、`vis-parts/` 下使用同一 `batch_id` |
| 格式校验 | **三工具不做格式校验**——`batch_id` 一律当不透明字符串用于路径拼接（`batches/<id>.json`、`fengine/<id>/…`）。因此**旧的时间编码格式（`60512_45000` / `20260908_123000`）仍然可用**，历史测试资产无需迁移 |

**为什么改**：旧格式把起点时间编进名字，代价是**同一秒内只能有一个 batch**——亚秒级 batch 直接做不到（`make_testdata.sh` 曾为此把 `BATCH_NSUBINTS` 强制抬高到"每 batch ≥ 1 s"）。顺序号切断这个人为耦合：batch 时长只由数据正确性（积分边界，第 12 节）与资源约束（`data-volume.md` §7）决定，与编号无关。**旧规范里"batch 时长 ≥ 1 s"这一条随之取消**。

**不受影响的约束**：batch 时长必须是 `intTime` 整数倍、起点须落在**积分**边界（第 12 节）——这些是数据正确性要求，与编号格式无关。

**空间维度不进 batch_id**：f 任务 = (batch_id, station, ds_index)（多 datastream 站每记录线程一个，见 5.3），x 任务 = (batch_id)（全站全基线）——**分片模式下细化为 (batch_id, ds_group)**，见 5.9；任务集由调度器从 batch.json 的 `stations` 与 .input 的 datastream 表推导（x 任务取全部基线）。**任务级标识（`<batch>-<station>-<ds>` / `<batch>-<g>` / `<batch>-merge`）见下一节「任务标识（task_id）」**；**为什么频段维度也不该进 batch_id**（即"每个频段组一个 batch"的取舍），见 `data-volume.md` §7.6。

> **关于本文档其余各节的示例**：目录树与命名汇总里的 `60512_45000` 一类是**时间编码格式**，保留只为可读性（一眼能对上时间段），**不表示推荐格式**；`DIFX_<MJD>_<sec>.s<XX>.b<XX>` 里的 MJD+秒更是**实验级**标识，与 batch_id 无关。实际命名以本节为准。

### 任务标识（task_id）规范（2026-09-27 新增）

编排层用**一个字符串**标识每个计算任务，脚本解析成维度值后调用工具。**格式：维度值用 `-` 连接，段数与内容决定任务类型。**

| task_id | 任务 | 展开为 |
|---|---|---|
| `<batch>` | 整 batch 的 x（**不分片**模式） | `fxcorr-x <batch> [workdir]` |
| `<batch>-<g>` | 第 g 个 **ds 组**（**分片**模式，见 5.9） | `fxcorr-x <batch> [workdir] <g>` |
| `<batch>-<station>-<ds>` | 某站某 ds 的 f 任务 | `fxcorr-f <batch> <station> [workdir] <ds>` |
| `<batch>-merge` | 归并本 batch 的全部分片（同实验须按时间序串行） | `fxcorr-x merge <batch> [workdir]` |

**各段含义**：

- `<batch>` = 8 位 batch_id（见上一节）。
- `<station>` = 站名（`.input` 的 TELESCOPE NAME；字母数字，不含 `-`）。
- `<ds>` = **站内** datastream 序号（0-based）——与 `fxcorr-f` 的 `ds_index`、`raw` 的 `_ds<N>` 后缀、`fengine/<bid>/<st>/ds_<N>/` **四处同一口径**，详见 5.2。
- `<g>` = **ds 组序号**（0-based，按频段升序）。**注意它与 `<ds>` 不是同一个维度**：一组 ds 是**跨站、含全部极化**的——互相关要求两站同一 freq 同时在场，且要算全极化组合（RR/LL/RL/LR）。t25362 是 **4 组**，每组 4 个 ds（2 站 × 2 极化）；实测的 ds→(频段, 极化) 映射与分组依据见 `data-volume.md` §7.3。

**解析建议**：按**段数**分派，不要按位置硬编码——1 段 = 整 batch 的 x；2 段且第二段为 `merge` = 归并，2 段且为数字 = x 分片；3 段 = f 分片。

**f 与 x 的任务数不同**（这是"任务 ID 不能只有一种形状"的根因）：f 任务 = 站数 × 每站 ds 数（t25362 是 16），x 分片任务 = 频段组数（t25362 是 4）。**编排层要维护任务依赖**——一个 x 分片任务只等"该 batch 中属于该 ds 组的那些 f 任务"完成，不是等全部 f 任务。

**这是编排层约定，不改变三工具的命令行接口**——`fxcorr-f` 的 `ds_index` 与 `fxcorr-x` 待加的 `ds_group` 已能表达全部维度。任务 ID 的价值在编排侧：任务表、日志、重试、去重都以一个字符串为键（对齐 scalebox 的 task 模型）。

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
- **偏振组合**：RR/LL/RL/LR 由 .input 的 BASELINE TABLE **逐条目给定**，fxcorr-x 按表取两站对应 band 的频谱相乘；pol 字符只在写 SWIN 头时使用（`polpair` 2 字节字段）。**关键结构（2026-09-27 实测 t25362）**：每条 baseline 条目**只出 1 个 product**（`POL PRODUCTS n/k: 1`），该 product 的极化 = **A 侧 band 的极化 × B 侧 band 的极化**——**极化维度被展开进了 baseline 编号**，四种组合就是四条独立条目。t25362 实测：`BASELINE ENTRIES: 16` = 4 个频段组 × 4 种组合，每条绑定一对具体的 `D/STREAM A INDEX` / `D/STREAM B INDEX`：

  | baseline | A 侧 ds | B 侧 ds | 组合 | 频段 |
  |---|---|---|---|---|
  | 0 | 0（BA, X） | 8（S6, X） | RR | f0-7 |
  | 1 | 0 | 9（S6, Y） | RL | f0-7 |
  | 2 | 1（BA, Y） | 8 | LR | f0-7 |
  | 3 | 1 | 9 | LL | f0-7 |
  | 4–15 | … | … | 同样 4 种 | f8-15 / 16-23 / 24-31 |

  **两条推论**：① **不同 baseline 之间完全独立计算**——RR/RL/LR/LL 各自算各自的输入对，没有跨极化的合并步骤；② **但一条 baseline 需要它声明的两个 ds 同时在场**——所以按 ds 分片时**分组的依据是 BASELINE TABLE**（"覆盖同一频段组的那些条目所涉及的 ds 集合"），**不是 ds 序号**：上表第 2 条是 `(1, 8)` 而非 `(1, 9)`，按序号配对就会配错。分片若只含单极化，该频段的 RL/LR/LL 会全部算不出来——见 5.9 与 `data-volume.md` §7.3。
- **多相位中心 / zoom band / 脉冲星 bin**（均已支持，2026-09-13）：zoom 频谱不单独落盘、x 侧按 zoomfreqchanneloffset 对父 .sp 做切片视图（5.3 节）；多相位中心每源一套 SWIN `.s%04d` 文件（.im 的 NUM PHASE CENTRES 驱动，源序号进 SWIN 头 sourceindex）；脉冲星 binning 每 bin 一套 `.b%04d` 文件（.input PULSAR BINNING + pulsar config，PULSAR BIN 字段进 SWIN 头）。

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

分片模式（**方案，待实施**，见 `data-volume.md` §7 与 5.9 节）：fxcorr-x 按 datastream 切分后，分片任务**不直接写 SWIN**，而是产出 D16 局部记录，再由 `fxcorr-x merge` 归并写出 D10——SWIN 的追加顺序必须严格按时间，写入者只能有一个：

```
D8 (fengine) + D9
        ↓
   [fxcorr-x × 每 ds 一片]      ← 各片独立、可乱序、可跨节点
        ↓
D16 (vis-parts/<batch_id>/ds<G>.part)
        ↓
   [fxcorr-x merge]             ← 按整数纳秒时间戳归并；唯一写入者
        ↓
D10 (vis/<experiment>.difx/ SWIN)
```

仿真数据分支（D7 的仿真来源，fxcorr-sim 两阶段）：

```
D3 (.input) + D9 (batch.json)
        ↓
   [fxcorr-sim common]     ← 每个 batch 一次
        ↓
D15 (sim-common/<batch_id>/ 频域公共信号)
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
| 分片局部记录 | D16 | `vis-parts/<batch_id>/ds<G>.part` | `vis-parts/60512_45000/ds0.part` |
| 波束文件 | D14 | `beam/<batch_id>/beam.bin` | `beam/60512_45000/beam.bin` |
| 公共信号 | D15 | `sim-common/<batch_id>/data_XX.bin` + `meta.json` | `sim-common/60512_45000/data_00.bin` |
| 批量元数据 | D9 | `batches/<batch_id>.json` | `batches/60512_45000.json` |
| FITS 产品 | D11 | `<exp>_<batch_id>.FITS` | `exp_60512_45000.FITS` |

---

## 11. 数据量级关系（典型）

**已移至 `data-volume.md`**（2026-09-21）：体积公式、比值关系、参考实例与容量结论的**权威位置**
现在是 `data-volume.md` 的 §1.3。本节只留一张速查（供本规范内部引用时一眼可见）：

```
D8（频域谱）  ≈  16×D7（原始基带，2bit 记录）  ≫  D10（可见度，单基线）
D15（公共信号） ≈  16×D7（单站 2bit；float32 复基带 vs 2bit 实基带，与带宽无关）
D10 随基线数 O(N²) 增长，多站大阵可能反超 D7；D11/D12（科学产品）≈ D10 量级
配置类（D1～D6、D9、D13）体积很小（KB～MB）
```

要点：**D8 大于 D7**（`.sp` 是 cf32 落盘 = 4 B/采样，2bit 原始只有 0.25 B/采样），"频谱域压缩"
发生在 D8→D10 的积分折叠。

---

## 12. 切批与重跑约束

- **时间层级**（DiFX 内部层级 ↔ fxcorr 术语，2026-09-21 补）：

  | 层级 | 长度 | 构成 | fxcorr 里的名字 |
  |---|---|---|---|
  | FFT 块（第 7 节的 `blockns`，亦称 subloop） | 4 µs | 256 点 FFT；t25362 下 = `subintNS / blockspersend` = 5.12 ms / 1280 | `.sp` 里的一段频谱（一个 FFT 块的输出） |
  | Core 短积分 | 5.12 ms | 1280 个连续 FFT 块 | **subint**——`.sp` 按它落盘，f 按它读、x 按它循环 |
  | Manager 最终积分 | 1.024 s | 200 个短积分 | **intTime**——SWIN 一条记录 = 一个积分 |

  三段数字取自 t25362（32 MHz band ×8、2bit、实采样 64 Ms/s/band、`INT TIME (SEC)` 1.024），彼此自洽：1280 × 4 µs = 5.12 ms、200 × 5.12 ms = 1.024 s。**三层都是配置量而非常量**：换观测（带宽 / 通道数 / `INT TIME`）三层长度全都变；表里给的是 t25362 的实例值。DiFX 的 Core/Manager 分层在 fxcorr 里对应"f 的 subint 处理 / x 的积分"两层，`intTime` 是**积分**的长度而不是 subint 的。

- **对齐**：batch 起点必须落在**积分**边界（MJD 秒是 `subintNS/1e9` 的整数倍，**且**相对 scan 起点是 `intTime` 的整数倍），batch 时长 = `intTime` 整数倍。保证 SWIN integration 跨 batch 完整、追加不碎片化。**只对齐到 subint 是不够的**——起点落在积分中间时，该 batch 跨越两个"半个积分"，SWIN 追加会写出两条不完整的记录。该约束由 fxcorr-sim 前置保证（5.2 节），**fxcorr-f 启动时校验**是最后防线：batch 起点非 subint 边界则报错退出（容差 1µs，吸收 start_mjd 的 f64 表示误差；batch.json 的 start_mjd 建议写精确 repr，如 58948.291666666664）。**subint 不是最小 batch 单位**：约束形式是"batch 时长 = `intTime` 的整数倍"，而 `intTime` 自身是 `subint` 的整数倍——所以 batch 最短 = **1 个 `intTime`**，要更短只能把 `.input` 的 `INT TIME (SEC)` 配小（下限 = 1 个 `subint`）。**这个下限随配置而变，不是一个固定秒数**（t25362 的 1.024 s 只是那份观测的 `intTime`；该配置下 batch = 11 个 intTime = 11.264 s）。边界必须落在**积分**边界而不是更细的 subint 边界——切在积分中间，同一条 SWIN 记录会被两个 batch 各追加一半。**校验现状（2026-09-21 核对）**：`fxcorr-f`（`main.cpp:485-495`）与 `run_batch.sh`（`run_batch.sh:128-137`）校验的是三条——起点在 subint 边界、batch 时长是 `intTime` 整数倍、`intTime` 是 subint 整数倍；**"起点落在积分边界"没有任何一层校验**。`BATCH_NSUBINTS` 不是 `intTime/subint` 的整数倍时就会踩到：test 配置（1 subint = 0.524288 s、`intTime` = 2 subint、默认 4 subint/batch）恰好安全；t25362 配置（200 subint = 1 个 `intTime`）下默认 4 subint/batch 就会让 batch 起点落在积分中间。
- **SWIN 追加**：同一实验所有 batch 写同一 `vis/<experiment>.difx/`；重跑整个实验需清空该目录，重跑单个 batch 需按 subint 范围从对应文件裁掉再追加（V1 不实现单 batch 回滚，重跑 = 全实验重跑）。**粒度不是"每 batch 一个文件"**：文件名 `DIFX_<实验 START MJD>_<实验开始秒>.s<相位中心>.b<脉冲星bin>`（5.4 节）用的是**实验级**时间戳、不含 batch_id，同一实验的全部 batch 用 `ios::app` 追加进同一组文件；文件数 = 相位中心数 × 脉冲星 bin 数（通常 **1 个**），单个文件里排列着**全部基线 × 全部 band × 全部积分**的记录。
- **fengine 覆盖**：重跑某 batch 时，fxcorr-f 覆盖写 `fengine/<batch_id>/` 下文件；fxcorr-x 以 batch.json 的 `status=done` 判定是否需要重跑。
- **后处理（difx2fits）的触发与输出**：**实验级**操作——读整个 `vis/<experiment>.difx/`，输出 `<base>.<n>.bin<bin>.source<src>.FITS`（每个 source 一个文件），每次运行**整份重读重写**、同名覆盖，不是增量追加。两条硬约束：① 该时间窗内全部 batch 的 `status` 为 `done`——缺段不会报错，只会静默少那一段数据；② 没有 fxcorr-x 正在写同一个 vis 目录——追加写与读并行时最后一条记录可能只落一半。粒度按**时间窗**给（按小时 / 按观测段 scan / 按一天的处理量），**不按 batch 计数**：整份重写的成本随累积量增长（间隔 T、总时长 L 时总读取量约 L²/2T 量级），出得越频繁越贵。上游 DiFX 的 `-F/--fits` 是"每个 job 结束出一版"（`mpifxcorr/utils/startdifx.py:783`），我们的 job 就是 batch，粒度太细。**不能放进 `run_batch.sh` 逐 batch 调用**（Q16）；由编排层在该实验（或该时间窗）全部 batch 跑完后调一次。
- **SWIN 的生命周期**：它是 difx2fits 的唯一输入，也是 `vis2screen` / `difx2mark4` 的输入。**可再生但有前提**——重跑 f + x 即可重出，前提是 raw 还在；而 raw（TB 级）通常最先删，raw 一删 SWIN 就成了唯一还能重出 FITS 的东西。删除判据因此是"**FITS 已通过 QC 且不再需要重处理**"：改权重 / 改 pcal / 改 flag 的阶段要反复重跑 difx2fits，此时不能删。体积上 SWIN 比 raw 小两三个数量级（`data-volume.md` §4），保留成本低于 raw——不确定时留 SWIN 比留 raw 划算。
- **全链路数据生命周期**（各目录的**最早**可删时点、粒度与再生前提；执行者是编排层，脚本与本仓库不实现删除逻辑）：

  | 数据 | 粒度 | 最早可删时点 | 再生前提 | 备注 |
  |---|---|---|---|---|
  | D15 `sim-common/` | batch | 该 batch **全部 station 任务完成**（早于 x，`data-volume.md` §1.3） | 重跑 `fxcorr-sim common`（同 batch + 同 seed 逐字节相同），**不依赖 raw** | 全链路最大却最早可删 |
  | D7 `raw/` | station × batch | 该站在该 batch 的 **f 完成** | **真实观测不可再生**；仿真可重跑 `fxcorr-sim station`（需 D15） | 唯一"删了就没了"的数据 |
  | D8 `fengine/` | batch × station | 该 batch 的 **x 完成**（`data-volume.md` §1.3） | 重跑 f，需 D7 在 | = D7 的 16 倍（2bit），中间数据里最大 |
  | D14 `beam/` | batch | 随该 batch 的 x 完成（D14 由 x 产出） | 重跑 x，需 D8 在 | MB 级；仅相位阵实验有 |
  | D10 `vis/` | **实验**（跨 batch 追加） | **FITS 通过 QC 且不再需要重处理** | 重跑 f + x，需 D7 在 | 实验级，**不能按 batch 删** |
  | D16 `vis-parts/` | batch × ds 组 | 该 batch 的 **`merge` 写出之后**（该 batch 时间范围已进 SWIN） | 重跑该分片的 x，需 D8 在 | 纯中转数据，归并成功即无用；**`merge` 前必须集齐**，缺一片则报错不写 |
  | D11/D12 `product/` | 实验 | **终态保留**（归档到长期存储为止） | 重跑 difx2fits，需 D10 在 | 最终科学产品 |
  | D1–D6/D9/D13 `config/`、`batches/`、`meta/` | 实验 / batch | **终态保留** | 前处理可重跑（vex2difx/difxcalc） | KB~MB；删了就没有"当初怎么跑的"的追溯依据 |

  两点注意：① 表里是**最早**可删时点（下限）——它按"下游跑过一遍"给，而实际删除还应等**该级结果确认**（改权重 / 改 pcal / 修 reader 的 bug 都要回头重跑，此时上游必须还在）；② **可再生的依赖链是逐级的**：raw → fengine → vis → product，删掉任一级就等于把以下各级钉死。仿真链路里 raw 也可再生（fxcorr-sim，真正不可再生的只有真实观测的 raw）。
- **batch duration 选择**：硬约束 = intTime 整数倍 + 起点落在**积分**边界（上文）。**旧的"batch_id 秒唯一（时长 ≥ 1 s）"已取消**（第 6 节，2026-09-27）：顺序号不承载时间，batch 时长不必 ≥ 1 s。之上权衡：① **调度并行度**——batch 是并行/调度单元（scalebox task，编排外置 V2+），batch 数越多多节点并行越好；② **内存**——x 侧 .sp 整 batch 常驻（V1），duration × 站数 × D8 数据率须在节点内存内；③ **重试成本**——失败重跑整个 batch（V1 无 batch 内回滚）；④ **吞吐 vs 延迟**——启动开销（配置读入、目录、SWIN 头）摊销想大，首个结果的可见延迟与 fengine/ 存储峰值想小。**大 duration 不利**：x 侧内存线性涨（OOM 风险）、失败重算面大、并行度下降（batch 少 → 节点闲置）、端到端延迟高、fengine/ 中间数据峰值大（D8 ≈ 16×D7，见 `data-volume.md` §1.3）。
- **站间时间同步**：per-station 串行架构下站间不靠 MPI 屏障同步，同步信息全在数据时间戳（VDIF 帧 epoch/帧号、.sp header 的 scan/sec/ns）+ .im/.calc 的时钟与几何延迟模型；fxcorr-f 的粗延迟（采样级移位）+ 条纹旋转/小数采样即"把两站拉到同一时刻"。站间残余延迟误差 Δτ 的后果按量级分档：**< 1 采样** → 被小数采样校正吸收；**1 采样～亚 subint** → 带宽 smearing 去相关（幅度 ×sinc(Δτ·Δν)，4MHz 带宽 1 采样误差即 -36%）+ 跨频相位斜坡 2πΔf·Δτ；**帧级（4ms）** → 两站乘不同时刻信号，完全去相关、weight 崩；**subint 级** → .sp 时间戳错位，错位相乘、静默错数据。注意：fxcorr-x 按 .sp header 时间对齐、**不校验站间一致性**（站间错位不报错、静默产出低质量数据，与 mpifxcorr 行为一致）；真实观测的站钟漂移靠 .im 时钟多项式补偿，模型不准的残余误差随时间演化。
- **并行模型（V3，2026-09-15 定案）**：并行维度只有时间——batch 即并行/调度单元（多 batch 并发由 scalebox 编排承担，编排外置不在本项目）；无空间切分（原 V2 P2 基线子集方案已取消，见 algo-plan P2 节）。**同实验 batch 串行约定（路线 B）**：同一实验的 batch 按时间序串行处理，实验级共享文件（PCAL_* 文本读改写、SWITCHEDPOWER_* 追加、SWIN 跨 batch 追加）不存在并发写点，无需加锁；同实验多 batch 并行（积压追赶场景）属 scalebox 编排层职责，届时再评估这些文件的并发语义。

---

## 13. 设计要点小结

- 所有处理以 **batch_id** 为单元，一次只处理一个批量
- 目录即接口，程序通过输入输出目录衔接，无需 MPI
- 每个批量目录自带 `batch.json`，便于追踪与断点续跑
- 原始数据按台站存放，处理后按批量组织
- f 落盘"完全校正"的频谱（含小数采样、条纹旋转、延迟对齐），x 只做 XMAC + 积分 + SWIN 写盘
- 前处理（vex2difx、difxcalc）与后处理（difx2fits、difx2mark4）保持兼容，SWIN 直出使 difx2fits 零改造
- 为后期并行化与流式处理预留清晰扩展空间
