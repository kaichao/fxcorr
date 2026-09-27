# fxcorr 数据规范文档（完整版）

**版本**：1.2
**最后更新**：2026-09-27（**V6 规划，⚠ 全部标"未实施"**：5.9 补**两级 merge（形态 A）**——batch 级只写 `merged.part`、实验级 `merge --experiment` 才写 SWIN 且是唯一写入者；第 6 节任务标识加 `<exp>-merge`；第 4 节模块 I/O、第 9 节数据流图、第 12 节生命周期表同步；**第 12 节的"同实验 batch 串行约定"按多节点部署改写**——SWIN 由形态 A 解决，`PCAL_*`/`SWITCHEDPOWER_*` 的并发语义列为待确认（见该节表格）。路线与验收见 `v6-plan.md`。**分片架构落地**：D16 记录格式定稿（= SWIN 记录流原样，见 5.9）、`fxcorr-x` 的 `ds_group` 分片与 `merge` 子命令已实施、5.2 的 `_ds<N>` 后缀规则明确为"仅多 ds 站"。**第 6 节 batch_id 改为 8 位零填充顺序号**——名字不再承载时间信息，随之取消"batch 时长 ≥ 1 s"的人为约束；位数依据与"band 该不该进 batch"的取舍见 `data-volume.md` §7.6；第 12 节同步。09-26：新增 **D16 分片局部记录**与 5.9 节 `vis-parts/` 目录规范，配套 `data-volume.md` §7 的"计算单元按 ds 分片 + SWIN 合并"方案——合并实现为 **fxcorr-x 的 `merge` 子命令**；第 2 节目录表、第 3 节数据总表、第 4 节模块 I/O、第 9 节数据流、第 10 节命名汇总、第 12 节生命周期表同步）。
**上一版更新**：2026-09-21（**第 11 节的体积公式移入 `data-volume.md` §1.3**，本节只留速查；V5 P6 讨论：第 12 节补六条——**时间层级表**（FFT 块 / Core 短积分 /
Manager 最终积分 ↔ fxcorr 的 `.sp` 块 / subint / intTime）、batch 时长与起点约束（`intTime`
整数倍、下限随配置而变而非固定秒数、**起点须落在积分边界**——并记下当前三层校验都只覆盖
subint 对齐这一缺口）、SWIN 文件粒度（实验级一组文件、非每 batch 一个）、difx2fits 的
触发与输出、SWIN 生命周期、**全链路数据生命周期表**（D15/D7/D8/D14/D10/D11/D12 的最早可删
时点、粒度与再生前提）。09-20：第 1 节明确"计算单元 = batch（f/x 同节点）"（**2026-09-27 按分片修订
为 `(batch, ds 组)`**，见第 1 节），据此把 `sim-common/`
的可见性要求收窄为"同一 batch 读同一份"——本地盘合法，5.2.1 增"各根的可见性要求"（**2026-09-27
按分片修订为 `(batch, ds 组)`**，见第 1 节：一个 batch 的多个 ds 组可分散在多节点）；同日 V5 P5：
目录根变量化——5.2.1 为规则权威；`common/` 改名 `sim-common/`；`work/` 删除。09-19：5.2 补
invalid 位帧的语义）
**适用系统**：fxcorr-f / fxcorr-x 流水线（由 DiFX/mpifxcorr 重构）
**处理模式**：非实时、按时间批量、串行可手工执行

**v1.2 变更**（相对 v1.1）：
- **新增 D16 分片局部记录**与 5.9 节 `vis-parts/` 目录规范——记录格式 2026-09-27 定稿为 **SWIN 记录流原样**（74 字节头 + cf32），`merge` 只做搬运；同节含分组依据、与 `vis/` 的隔离边界、缺片处理、**与全量模式互斥（已落成程序内检查）**
- **batch_id 改为 8 位零填充顺序号**（第 6 节）——名字不再承载时间信息，"batch 时长 ≥ 1 s"随之取消；新增「任务标识（task_id）」规范
- 5.2 的 raw 命名明确 **`_ds<N>` 后缀只在多 datastream 站出现**（单 ds 站的文件名与加多 ds 之前逐字相同）；生成侧已落地
- 第 4 节模块 I/O 加 `fxcorr-x merge`、第 9 节数据流加分片分支、第 2 节目录表加 `vis-parts/`、第 12 节生命周期表加 D16
- 5.7 新增「未来：SQLite 索引」（JSON 权威、sqlite 派生、单写者）

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

- **计算单元 = (batch, ds 组)（2026-09-20 首次明确，2026-09-27 按分片修订）**：一个 batch 是自足的处理单元——该 batch 的 `common` → 全部站的 `station` → 全部站的 f → x。**分片改造（5.9）把它的粒度细化为 `(batch, ds_group)`**：x 的处理单元是"**一个 batch 的**一个 ds 组"，所以**节点承载的单位是 `(batch, ds 组)`**——该组涉及的**全部站的全部 ds**必须在同一节点（互相关要求两站同一 freq 同时在场）。**原来"整个 batch 在同一节点"的说法在分片后过粗**：一个 batch 的多个 ds 组可以分散在多个节点上并行。**f 与 x 同节点**这条不变（同组的 f 与 x 必须同节点）。

  由此：f 只需本节点该 `(batch, ds 组)` 的 raw；x 只需本节点**该组**的 fengine（不是该 batch 全部站的 fengine）——**raw 与 fengine 仍然都不需要跨节点汇聚**，"每节点只持有自己写的部分"成立，只是粒度更细。依据仍是目录本身按 batch 组织（`sim-common/<batch_id>/`、`fengine/<batch_id>/<station>/ds_<N>/`），batch 之间互不依赖。
- **共享存储主数据流**：默认一切都在 `$FXCORR_WORKDIR` 下，即全部节点共享同一份**全局存储**。需要本地化时按 5.2.1 的根变量把 `raw/`（TB 级）与 `fengine/` 指到本地盘，目录逻辑集中、物理分布（**每节点只持有自己负责的 `(batch, ds 组)`**）。**`sim-common/` 同样可以落本地盘**——它要求的是"**同一 ds 组读同一份**"而不是"跨站共享一份"：该组涉及的站都在同一节点，天然满足（见 5.2.1 的可见性要求与 5.8 的跨节点说明）。配置与元数据（`config/`、`batches/`、`meta/`）没有独立根、恒在 workdir 下；**真正必须全局可见的是实验级的 `vis/` 与 `product/`**——SWIN 跨 batch 追加、difx2fits 一次读整个实验，各节点各写各的会静默分裂（见第 2 节存储归属表与第 12 节）。分片模式（5.9 节）下 **`vis-parts/` 同样必须全局可见**——各节点的 x 分片任务写、`merge` 子命令统一读；它无独立根，**多节点部署时靠 `FXCORR_WORKDIR` 落在共享存储来实现可见性**（见 `data-volume.md` §7.5）。
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

**每个目录的实物形态与逐文件说明见 `fxcorr/workdir-template/`**——那份模板把
上面这些目录全建了出来，每个目录一份 README（有哪些文件、命名长什么样、
谁写谁读、什么时候能删）。本文件只承载**规范**：格式定义、约束、跨目录规则。
查"这个目录长什么样"看模板，查"为什么必须这样"看本文。

多节点部署的存储归属（**V5 P5 起由根变量决定**，见 5.2.1）。**默认全部在 project/ 下，
即所有节点共享同一份全局存储**；需要把某类数据放到别处（本地盘、另一块共享盘）时，
才给对应的根变量赋值：

| 目录 | 默认位置 | 独立根 | 说明 |
|---|---|---|---|
| `config/` `batches/` `beam/` | `$FXCORR_WORKDIR/<name>` | ❌ | 量小、或按 batch 组织在 workdir 内；恒在 workdir 下（Q17） |
| `meta/` | `$FXCORR_WORKDIR/meta` | ❌ | 同上；索引与根记录必须全局一致可见 |
| `raw/` | `$FXCORR_WORKDIR/raw` | ✅ `FXCORR_RAW_ROOT` | TB 级原始基带，可指向各记录节点的本地盘 |
| `fengine/` | `$FXCORR_WORKDIR/fengine` | ✅ `FXCORR_FENGINE_ROOT` | 各站 f 输出，可指向计算节点本地盘（目录逻辑集中、物理分布） |
| `sim-common/` | `$FXCORR_WORKDIR/sim-common` | ✅ `FXCORR_SIM_COMMON_ROOT` | 全部目录里最大（≥16× 单站 2bit，见 `data-volume.md`）；**本地盘或共享盘均可**——要求是"**同一 ds 组**读到同一份"（2026-09-27 按分片修订：原来说是"同一 batch"，但同一 ds 组内的 ds 才读同一频段区段，不同组读不同区段）；同组各站在不同节点、各自读到**不同**副本才是违例。**按覆盖跨度生成时 85% 是没人读的间隙**（5.8 节），实际需要 8.2 GB/s 而非 56.6 GB/s |
| `vis/` | `$FXCORR_WORKDIR/vis` | ✅ `FXCORR_VIS_ROOT` | SWIN 跨 batch 追加、difx2fits 直读（**追加保序前提**：现状是"同实验 batch 串行"；**V6 形态 A 起改为实验级 `merge` 单点写入**，⚠ 未实施——见第 12 节与 5.9 末条） |
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
| D16 | 分片局部记录 | `vis-parts/<batch_id>/...` | fxcorr-x（分片任务） | 二进制（**SWIN 记录流原样**，见 5.9） | 极小（≈ D10 总量级） | 每个 x 分片任务产出的可见度记录，由 fxcorr-x 的 `merge` 子命令按时间归并写出正式 SWIN（D10）；**写入顺序非自由**，见 5.9 节（2026-09-26 新增） |

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
| **fxcorr-x `merge`** | 归并（子命令，**2026-09-27 已实施**） | D16（vis-parts 局部记录） | D10（SWIN 可见度） | 按整数纳秒时间戳归并；SWIN 的**唯一写入者**（`data-volume.md` §7.5）。与分片任务**同二进制、不同调用**，复用 fxcorr-x 已有的 SWIN 写入路径。**⚠ V6 起改为两级**（batch 级只写 `merged.part`、**实验级**才写 SWIN），见 5.9 末条与 `v6-plan.md` S4.1——现状是 batch 级直接写 SWIN |
| **difx2fits** | 后处理1 | D3、D4、D6、D10、D5（可选） | D11（.FITS） | 生成 FITS-IDI，SWIN 零改造直读 |
| **difx2mark4** | 后处理2 | D3、D4、D6、D10、D1 等 | D12（Mark4） | 生成 Mark4 格式 |

---

## 5. 各阶段目录与文件详细规范

### 5.1 配置与模型数据（config/）

> 实物与逐文件说明见 `fxcorr/workdir-template/config/README.md`。

- 编号：D1 `.vex` / D2 `.v2d` / D3 `.input` / D4 `.calc` / D6 `.im`（另有可选的 `.flag`）；
- **实验级**——一个实验一套、整个实验共用（**不是 batch 级**），长期保留；
- 前处理：`.v2d` ──vex2difx──> `.input` + `.calc` ──difxcalc──> `.im`。原程序按 cwd 解析
  内部相对路径，一律经 `fxcorr/wrap_*.sh` 调用（见 5.2.1 末条）；
- **无独立根**，恒在 `$FXCORR_WORKDIR/config`。

### 5.2 原始基带数据（raw/）

> 实物与逐文件说明见 `fxcorr/workdir-template/raw/README.md`。

- 编号：D7；生产者：记录节点（真实观测）/ `fxcorr-sim station`（仿真）；消费者：`fxcorr-f`（按 `.input` 的 DATA TABLE 读）；
- 组织：`raw/<station>/<station>_<batch_id>[_ds<N>].vdif`——**多 datastream 站每 ds 一个文件**，`_ds<N>` 后缀只在多 ds 站出现（单 ds 站为 `<station>_<batch_id>.vdif`，与加多 ds 支持之前逐字相同）；
- **`N` 的口径（三处必须一致）**：站内 datastream 序号（0-based，按 `.input` DATASTREAM 表里该站出现的次序）——`raw/` 文件名、`fengine/<batch_id>/<station>/ds_<N>/` 的目录名、`fxcorr-f` 的 `ds_index` 参数。**不一致就会读错文件**；
- **文件起点语义**：**不要求**等于 batch 起点（真实观测中各记录系统的帧计数器不同相，文件可以从某一秒的中途开始，起点之前的部分判为无效）；文件中间缺帧（"直接缺帧"与"filler 帧占位"两种形态）同样受支持；**标了 VDIF invalid 位的帧按"在时间轴上在位、数据不可用"处理**——它照常占一个时间槽（帧号参与连续性判断），只是对应的块在 f 侧被标无效，这与"占字节不占时间轴"的 filler 是两回事（实测与定案见 `fxcorr/reader-model.md` 4.12）；
- **VDIF 时间参考**：帧头秒字段是**当日秒**（对 86400 取模、不带日期），帧号是秒内序号（对帧率取模、每秒回绕；t25362 为 16000 fps）。文件起点由首帧的 (秒, 帧号) 唯一确定，**不保证落在整秒边界**——这也是"文件起点 ≠ batch 起点"的成因；
- 数据量 TB 级；**一个 raw 文件的时间范围须覆盖完整 batch**（切批约束见第 12 节）；
- **读取路径的完整分析见 `fxcorr/reader-model.md`**（读模型对照、缺陷根因与症状指纹、`GAPCHECK`/`READPOS` 判据）；f 侧目录级实现要点见 `applications/fxcorr-f/CLAUDE.md`；
- 仿真数据约束：batch 时间窗须与 subint 网格对齐（fxcorr-sim 读 `.input` 的 subint 结构保证，见第 12 节）；多节点分布生成时各分片的 VDIF 帧时间戳/帧号须全局连续（程序内校验点）；最小数据集可入仓库（`fxcorr/test/`，附 sha256），不受"运行时数据不进 git"约束。

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

  **各根的可见性要求（2026-09-20 收窄；2026-09-27 按分片修订粒度）**：按第 1 节的 `(batch, ds 组)`
  切分部署时，`RAW` / `FENGINE` / `SIM_COMMON` 三个 **batch 级根只需对该 ds 组所在节点可见**。
  `SIM_COMMON` 因此**可以是本地盘**——公共信号的约束是"**同一 ds 组读同一份**"，而不是"全站共享
  一份"：station 任务与它的 common 都在同一节点时天然成立（同一 ds 组内的所有 ds 覆盖同一频段组，
  读的是 common 的同一区段；不同 ds 组读不同区段、互不重叠，各节点各自生成或各存一份都不影响
  相干——它只与 `(seed, batch)` 有关）。**必须全局可见的只有实验级的 `VIS` / `PRODUCT`**：SWIN 跨
  batch 追加同一组文件、difx2fits 一次读整个实验，分裂了没有任何报错。分片模式下 **`VIS_PARTS`
  同理必须全局可见**（在 workdir 下，见 5.9 与第 2 节）。

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

> 实物、逐文件说明与**三种文件的完整格式**（`band_XX.sp` / `pcal.bin` / `autocorr.bin`）
> 见 `fxcorr/workdir-template/fengine/README.md`。
> **`batch.json`（D9）见 `fxcorr/workdir-template/batches/README.md`**——它原先写在本节，2026-09-27 移回它自己的目录。

- 编号：D8；生产者：`fxcorr-f`；消费者：`fxcorr-x`；
- 布局：`fengine/<batch_id>/<station>/ds_<N>/`——**单 datastream 站同样是 `ds_0/`**（无平铺特例）。f 任务 = `(batch_id, station, ds_index)`，`ds_index` 与 `raw/` 文件名的 `_ds<N>` 同口径（见 5.2）；
- 三层编号各回答一个问题：`<batch_id>` **哪段时间**（`batches/<batch_id>.json`）、`<station>/ds_<N>` **哪个站哪路记录**（`.input` 的 DATASTREAM 表）、`band_<xx>.sp` **哪个 recorded band**（`.input` 的 FREQ 表，`xx` 是**该 ds 内**的序号而非全局 band 号）；
- 生命周期：该 batch 的 x 跑完后可删；
- **多节点部署必须落 tmpfs**（`/dev/shm`）：产生率 GB/s 级，SATA SSD 差两个数量级；同节点的并发组数受 tmpfs 容量约束（见 `data-volume.md` §5.1 与 `v6-plan.md`）；
- **改这三种文件的布局时必须递增其 Header 的 `version`**，并同步 README 与本节。

### 5.4 X-Engine 输出（vis/，SWIN）

> 实物、逐文件说明与 **SWIN 74 字节记录头**见 `fxcorr/workdir-template/vis/README.md`。

- 编号：D10；生产者：`fxcorr-x`（不分片）或**实验级 `merge`**（分片，⚠ V6 形态 A）；
  消费者：`difx2fits` / `difx2mark4`（零改造直读，这是整个设计的前提）；
- 目录名 = `.input` 的 `OUTPUT FILENAME`（相对时拼 `FXCORR_VIS_ROOT`）；
  SWIN 文件名用**实验级** MJD + 开始秒，**不含 batch_id**——同一实验的所有 batch
  追加写入同一组文件（batch 边界与 intTime 对齐保证不碎片化，见第 12 节）；
- **追加顺序不是自由的**：`difx2fits` 按天线检查时间单调、**静默丢弃**回退记录
  （只打一行计数）。单节点串行 batch 天然满足；多节点并行失效——约束与场景见
  `vis/README.md`，方案见 5.9 末条与第 12 节；
- `PCAL_<mjd>_<sec>_<station>` / `SWITCHEDPOWER_*` 两个**文本**文件也落在本目录
  （由 f 生成，格式与 mpifxcorr 逐字节一致，可与基准 diff 对拍）；它们的并发语义
  见第 12 节的待确认表；
- 生命周期：**长期保留**。

### 5.5 X-Engine 波束输出（beam/，P8 2026-09-13）

> 实物与 **`beam.bin` 完整格式**见 `fxcorr/workdir-template/beam/README.md`。

- 编号：D14；生产者：`fxcorr-x`（**相位阵模式**）；消费者：下游分析；**无独立根**；
- **与互相关模式互斥**：`.input` 的 CONFIG 段有 `PHASED ARRAY TRUE` +
  `PHASED ARRAY CONFIG FILE` 时，x 不做互相关、不写 SWIN，改为按 `DWeight`
  加权求和波束（累加不归一化）；
- 上游 mpifxcorr 的相位阵输出端（padomain/paoutputformat/…）是**无消费者的死代码**，
  所以本格式**为 fxcorr 自定**（设计见 `algo-plan.md` P8 节）。

### 5.6 最终科学产品（product/）

> 实物与逐文件说明见 `fxcorr/workdir-template/product/README.md`。

- 编号：D11（FITS）/ D12（Mark4）；生产者：`fxcorr/wrap_difx2fits.sh`；消费者：下游分析；
- **实验级操作**——difx2fits 一次读整个 `<exp>.difx/`，**不进 `fxcorr/run_batch.sh`**，
  由编排层在该实验**全部 batch 跑完后调一次**；
- **产物名由 difx2fits 按传入的 `<base>` 自定**（`<base>` 或 `<base>.0.bin*.source*.FITS`），
  fxcorr 不控制它；`<exp>_<batch_id>.FITS`（第 10 节）只是**推荐命名**；
- 根：`FXCORR_PRODUCT_ROOT`；生命周期：**长期保留**。

### 5.7 全局元数据（meta/）

> 实物与**文件格式示例**（`batches.index` / `roots/<batch_id>.json`）见 `fxcorr/workdir-template/meta/README.md`。

- 内容：`batches.index`（D13，batch 状态流水，append-only，由 `fxcorr/run_batch.sh` 追加）、`roots/<batch_id>.json`（该 batch 开跑时的根快照）、`difxmsg/`（container 模式的 DifxMessage 落盘，**当前暂不启用**）；
- **无独立根**，恒在 `$FXCORR_WORKDIR/meta`，**必须全局一致可见**；
- 合法状态只有 `running` / `done` / `failed`；**它是"哪些 batch 已完成"的判据来源**（实验级 `merge` 靠它核对，见 5.9）；
- **根记录与一致性检查**：脚本开跑前写 `meta/roots/<batch_id>.json`，三个程序启动时与它比对**自己真的会用到**的根、不一致即报错退出；脚本侧另有一道实验级检查（`vis`/`product` 跨 batch 追加，与同实验已有 batch 记录不一致即报错）。规则细节见 5.2.1；
- `Alert` 是**另一条独立通道**：fxcorrcommon 里上游代码直接调 `cinfo`/`cverbose`/`cdebug`，经 `difxMessageSendDifxAlert` 落地——该函数在 `difxMessagePort < 0` 时**退化为打印** stdout/stderr，而 f/x 都不调用 `difxMessageInit()`，所以这些消息永远不会组播。它由 `FXCORR_LOGLEVEL` 控制详略。

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

> 实物、**信号语义、网格参数与 `meta.json` 字段**见 `fxcorr/workdir-template/sim-common/README.md`。

- 编号：D15；生产者：`fxcorr-sim common <batch_id>`（每 batch 一次）；消费者：`fxcorr-sim station`（各站只读，不改写）；**只有仿真才有这个目录**；
- **改文件格式必须先递增 `meta.json` 的 `version`**，并同步 README 与本节；
- **可见性**：要求是"**同一 ds 组读到同一份**"（不是"全站共享一份"）——不同 ds 组读的是不同频段区段、互不重叠，各节点各自生成或各存一份都不影响相干（公共信号只与 `(seed, batch)` 有关）。见 5.2.1 的根可见性表；
- **生命周期**：batch 的全部 station 任务完成后 `sim-common/<batch_id>/` 可删（编排层清理）；重跑 batch 时 common 覆盖生成（`status` 回 `running` → `done`）；
- **任务粒度**：common = `(batch_id)`；station = `(batch_id, station)`（与 fxcorr-f 同构，多节点并行）。一致性靠共享存储单份 common，**不靠多节点重复生成**；
- **legacy 模式**：station 子命令带 `tone_mhz` 位置参数时走旧时域合成路径（供字节对拍回归）；新路径的 tone 由 P2 谱线机制（common 端频域注入）提供；
- **噪声**：`FXSIM_NOISE`（高斯噪声 σ，默认 0.02；0 关闭）、`FXSIM_SEED`（公共种子，默认固定）。**公共信号只与 (seed, batch) 有关、与站无关**；站噪声种子 = `f(seed, station)` 派生。两站噪声全关时输出逐位一致（跨站相干校验的判据）。

- **数据量（2026-09-27 补口径）**：float32 复基带 = `8 字节 × 覆盖带宽 × 时长`——注意这里的带宽是**覆盖跨度**（`minStartFreq` 到 `maxStartFreq+maxBW`），**不是实际被读的带宽**。t25362 参数下两者差得很远：跨度 7072 MHz → **56.6 GB/s**，而实际被任何站读取的只有 `32 band × 32 MHz = 1024 MHz` → **8.2 GB/s**——**85% 是 band 间间隙，生成了但没人读**（上一条已说明）。所以：

  | 口径 | 4 站 t25362 | 说明 |
  |---|---|---|
  | 现状（覆盖跨度） | **56.6 GB/s** | 间隙照常生成 |
  | 按频段组分片后 | **8.2 GB/s**（降 6.9×） | 只覆盖实际带宽，组间间隙一并省掉 |
  | 轻量模式 | **0** | 载荷由 `f(seed, station, band)` 直接生成，不用公共信号（`v6-plan.md` S2） |

  **这条 6.9× 的差不是优化，是当前实现的浪费**——按频段组分片生成（`v6-plan.md` S5.2）落地前，任何大跨度仿真都在白写 85% 的数据。

- **跨节点可见性（2026-09-27 补）**：计算单元细化到 `(batch, ds 组)` 后（第 1 节），**每个节点只需要它那组的频段切片**——同一 ds 组内的所有 ds 覆盖同一频段组，所以它们读的是**同一区段**；不同组读不同区段，互不重叠。由此：

  | 形态 | 各节点要什么 | 是否需要拷贝整份 |
  |---|---|---|
  | 现状（common 是全跨度一整份） | 按 `startIdx` seek 读自己那一段 | **不需要拷贝**，但要求存放 common 的位置对该节点可读；**生成阶段仍是 56.6 GB/s 的写入浪费** |
  | 按频段组分片后（S5.2） | 只需要自己那一份（≈ 8.2/N组 GB/s） | **不需要，且可以本地生成**——同组的全部站任务都在同一节点，种子取 `f(seed, batch, 频段组)`（与站无关）即可保证"各节点各生成一份"逐位相同 |

  **所以不需要"把整份 common 拷到多个本地节点"**——现状下各节点 seek 读自己那段即可，分片后更是每组一份、各节点自给。**真正要跨节点共享的只有实验级的 `vis/` 与 `product/`**（第 1 节与第 2 节存储归属表）。

### 5.9 分片局部记录（vis-parts/，D16，2026-09-26 新增）
> 实物与逐文件说明见 `fxcorr/workdir-template/vis-parts/README.md`；本节只留规范。

- **编号**：D16；产生者：fxcorr-x 的分片任务（一任务一份，粒度 `(batch_id, ds_group)`——`ds_group` = **跨站、含全部极化**的一组 datastream，见第 6 节「任务标识」与 `data-volume.md` §7.3）；消费者：**fxcorr-x 的 `merge` 子命令**（同一二进制的另一次调用）——读全部 `.part`，按时间归并，写出正式 SWIN 到 `vis/`。命令行形态见 `usage.md` 的 fxcorr-x 节。
- **分组依据（硬约束，2026-09-27 实测后定）**：`ds_group` 的成员必须从 **`.input` 的 BASELINE TABLE 推导**——即"覆盖同一频段组的那些 baseline 条目所涉及的 ds 集合"，**不是按 ds 序号猜**。每条 baseline 条目绑定一对具体的 ds、只出一个极化产品（极化展开进 baseline 编号，机理见第 8 节），所以只含单极化的分片会丢掉该频段的 RL/LR/LL。t25362 实测：每组 4 个 ds（两站 × 两极化），共 4 组。**同一 batch 内数据齐备**（f 本就覆盖该时段全部站的全部 ds），所以只要分组正确，该频段声明的 baseline 都能算出。
- **为什么需要这一步**：SWIN 的追加顺序**不是自由的**。difx2fits 顺序读记录（`DifxVisRecordgetnext` 是唯一读取原语），并按天线检查时间单调（`fitsUV.c:1227` 的 `RecordIsOld`）；时间回退的记录被**静默丢弃**，只在结尾打印一行 `out-of-time-range records dropped`（`fitsUV.c:1868`），不报错。分片任务各自追加同一组文件必然时间回退，所以**写入必须集中在唯一一处**——`merge` 与分片任务是不同的进程调用，分片任务不再写 SWIN，唯一写入者仍然成立。分析与方案见 `data-volume.md` §7.5。
- **与 `vis/` 的边界（硬约束）**：difx2fits 只 glob `OUTPUT FILENAME` 目录下**以 `DIFX` 开头**的文件（`fitsUV.c:82-98`，glob 模式为 `<job.outputFile>/DIFX*`）。因此 `.part` **既不放进 `vis/` 目录、也不使用 `DIFX` 前缀**——两重隔离，杜绝被误当正式 SWIN 读入。`ds<G>.part` 这个命名是 fxcorr 自定的：**DiFX 生态里不存在"可见度分片"这个概念**，没有既有约定要遵守，只需避免与 `DIFX*` 撞名。
- **记录内容与格式（2026-09-27 定稿）**：`.part` **就是 SWIN 记录流原样**——74 字节记录头（`visibility.cpp` 的 `appendSWINHeaderBuffered`：sync word / 版本 / baseline 号 / MJD / 秒（double）/ config 索引 / source 索引 / freq 索引 / 极化对两字节 / pulsar bin / weight（double）/ UVW 三分量）+ `nchan/chansToAverage` 个 `cf32` 复频谱，与 D10 逐字节同构。这样 `merge` 只做搬运、不需要理解语义。
- **记录长度不在头里**：由 `freqindex` 查 `.input` 的 FREQ 表得到（`nchan / channelsToAverage × 8` 字节），所以 `merge` 必须读 `.input`——它本来就要读（输出文件名、根解析同源）。**分片模式下每条记录必属单相位中心、无 pulsar binning**（程序对这二者直接报错退出），所以 `flushBuffersToDisk` 的多文件分支不会出现，一个分片就是一个文件。
- **归并 key**：用**整数纳秒时间戳**（由 scan 起点与 `offsetns` 推算），不是浮点 `sec`——浮点相等比较不可靠。
- **生命周期**：实验的合并写出 SWIN 后即删（编排层清理，同 `fengine/` 第 12 节语义）；重跑分片时覆盖写自己的 `.part`。
- **等待策略（2026-09-27 定，未实施）**：**全量**——实验级 merge 等全部 batch 的 `merged.part` 到齐后一次合并；缺 batch 时**默认报错退出、不写**，`FXCORR_X_MERGE_FORCE=1` 的语义沿用（强制写出已到齐的部分 + stderr 列明缺了哪些）。**增量合并明确不做**：滚动窗口必须先定义"分片迟到时等 / 超时跳过 / 告警"，没有真实需求驱动时引入只会变成"看起来在跑、实际卡住"；将来首个结果的可见延迟成为真实痛点时再立条目（`v6-plan.md` S4.2）。
- **缺片处理（硬约束 + 逃生口）**：`merge` 启动时先核对本 batch 的 ds 组是否集齐，**缺则报错退出、不写任何东西**（stderr 列出缺哪几组）。设 `FXCORR_X_MERGE_FORCE=1` 可强制写出已到齐的部分——缺失组对应的频段在该 batch 的时间段内**没有记录**，difx2fits 不会因此报错，只是静默缺段（只在 `merge` 的 stderr 日志里留痕）。默认严格是刻意的：分片缺失通常意味着任务失败或未调度，静默缺段比报错危险得多。
- **与"全 band 模式"互斥（硬约束，2026-09-27）**：同一 batch 的 fxcorr-x 只能选一种模式——**不分片**（一次处理全部 ds，直写 SWIN）或**分片**（按 `ds_group` 多次写 `.part`，再由 `merge` 写 SWIN）。**两者不能对同一 batch 混跑**：分片任务不写 SWIN，而 `merge` 会为该 batch 的时间范围追加记录，混跑会产生重复记录、破坏 SWIN 的时间单调（§7.5）。**f 侧不受影响**——f 天然是每 ds 一个任务，两种模式下产出**完全相同**的 `fengine/`；选哪种只决定 x 的一次跑还是多次跑。
- **互斥已落成程序内检查**（2026-09-27）：三种模式启动时都读目标 SWIN 的**记录头**（数据用 `seekg` 跳过，GB 级文件也是秒级），若本 batch 的时间范围内已有记录则报错退出，提示"这个 batch 已经被全量或 merge 写过"；`FXCORR_X_SWIN_CONFLICT=allow` 可强制继续。分片任务自身不写 SWIN，所以**重跑分片不会被拦**。
- **`.part` 的重复运行是覆盖写**：同一次运行里 `writeSWIN` 每个积分周期写一次（追加），但**一次运行的第一次写是截断**——重跑分片得到的是新内容，不会把上一次的翻倍。
- **两级 merge（2026-09-27 定，⚠ 未实施——V6 的 S4.1，见 `v6-plan.md`）**：多节点并行时"每个 batch 跑完立刻 merge 写 SWIN"会让**时间回退**——节点 B 先完成先写、节点 A 后写，而 difx2fits 对回退记录**静默丢弃**（见上"为什么需要这一步"）。所以 SWIN 的写入要收敛到**实验级一次**：

  | 级 | 调用 | 读 | 写 |
  |---|---|---|---|
  | **batch 级** | `fxcorr-x merge <batch_id> [workdir]` | `vis-parts/<batch_id>/ds*.part` | **`vis-parts/<batch_id>/merged.part`**（不碰 SWIN） |
  | **实验级** | `fxcorr-x merge --experiment [workdir]` | 全部 batch 的 `merged.part` | **D10 SWIN——它才是唯一写入者** |

  实验级按各 `merged.part` 的**首记录时间**定序，**不是** `batch_id` 数值序（第 6 节只保证编号由单点分配，未规定"编号 = 时间序"）；两者不一致时报错而非静默选取。`FXCORR_X_SWIN_CONFLICT`（上面那条互斥检查）随之**移交实验级**。`run_batch.sh` 的分片路径改为"逐组 + batch 级 merge"，**实验级 merge 不进 `run_batch.sh`**——它与 `wrap_difx2fits.sh` 同为实验级操作，由编排层在实验全部 batch `done` 后调一次。

  **为什么保留 batch 级那一层**（而不是让实验级直接读全部分片）：24 h 观测按 t25362 参数是 84,375 batch × 4 组 = **337,500 个 `.part`**；batch 内归并把文件数收敛 4 倍，且这一层归并**本来就必须做**，放在 batch 完成时做还能只重跑失败的 batch。`.part` 的 glob 是 `ds*.part`，与 `merged.part` 不冲突——命名即隔离。

  **在形态 A 落地前**，现状是 batch 级 merge 直接写 SWIN + "同实验按时间序串行"（`usage.md` 原则 2）——单节点串行 batch 时正确，多节点并行 batch 时静默错数据。

---

## 6. 批量标识（batch_id）规范

**格式（2026-09-27 修订）**：**8 位零填充十进制顺序号**——`00000001`、`00000042`。

| 项 | 规定 |
|---|---|
| 形态 | `NNNNNNNN`，固定 8 位、零填充。**定宽是必需的**：不定宽时 `100` 会排在 `42` 前面，字符串排序就不再等于编号顺序 |
| 作用域 | 同一 `workdir` 内**全局唯一、单调递增**（跨实验也不重复）。容量 10⁸ 个 batch——1.024 s 的 batch 可连续覆盖 **1185 天**，0.512 s 的覆盖 592 天，切到 5.12 ms（1 个 subint）也有 5.9 天。**位数为何取 8、以及"band 该不该进 batch"的取舍，见 `data-volume.md` §7.6** |
| 时间语义 | **batch_id 不承载任何时间信息**。起点、时长、band 等全部在 `batches/<batch_id>.json`（D9）里，字段见 5.3 |
| 分配 | 由**切批规划步骤单点分配**（现为 `make_testdata.sh`，未来是 scalebox）：取 `batches/` 下已有编号的最大值 +1。**计算节点不生成 batch_id**。`make_testdata.sh` 另做一层复用以保证重跑幂等：已有 batch 的 `(start_mjd, n_subints, subint_ns)` 与本次规划一致时沿用它的编号（否则每次重跑都分配新号、把整批数据重新生成一遍）。三个分量缺一不可——单 batch 用 `test.input`（0.524288 s subint）、`-n` 多 batch 用 `test-sim.input`（128 ms），起止时刻可能相同而粒度与时长不同 |
| 排序 | 字符串排序 = 编号顺序 = **时间顺序**（由分配时的单调性保证，与 batch 的**处理**顺序无关——乱序补跑不影响） |
| 一致性 | 同一 batch 在 `fengine/`、`sim-common/`、`vis-parts/` 下使用同一 `batch_id` |
| 格式校验 | **三工具不做格式校验**——`batch_id` 一律当不透明字符串用于路径拼接（`batches/<id>.json`、`fengine/<id>/…`）。因此**旧的时间编码格式（`60512_45000` / `20260908_123000`）仍然可用**，历史测试资产无需迁移 |

**为什么改**：旧格式把起点时间编进名字，代价是**同一秒内只能有一个 batch**——亚秒级 batch 直接做不到（`make_testdata.sh` 曾为此把 `BATCH_NSUBINTS` 强制抬高到"每 batch ≥ 1 s"）。顺序号切断这个人为耦合：batch 时长只由数据正确性（积分边界，第 12 节）与资源约束（`data-volume.md` §7）决定，与编号无关。**旧规范里"batch 时长 ≥ 1 s"这一条随之取消**。

**不受影响的约束**：batch 时长必须是 `intTime` 整数倍、起点须落在**积分**边界（第 12 节）——这些是数据正确性要求，与编号格式无关。

**空间维度不进 batch_id**：f 任务 = (batch_id, station, ds_index)（多 datastream 站每记录线程一个，见 5.3），x 任务 = (batch_id)（全站全基线）——**分片模式下细化为 (batch_id, ds_group)**，见 5.9；任务集由调度器从 batch.json 的 `stations` 与 .input 的 datastream 表推导（x 任务取全部基线）。**任务级标识（`<batch>-<station>-<ds>` / `<batch>-<g>` / `<batch>-merge`）见下一节「任务标识（task_id）」**；**为什么频段维度也不该进 batch_id**（即"每个频段组一个 batch"的取舍），见 `data-volume.md` §7.6。

> **关于本文档其余各节的示例（2026-09-27 更新）**：目录树与命名汇总里的 batch_id 示例**已全部换成 8 位顺序号**（`00000001`），与推荐格式一致——此前它们写的是时间编码格式（`60512_45000`）并声称"保留只为可读性"，但**示例就是读者会照着写的东西**，现统一。**唯一的例外是 `DIFX_<MJD>_<sec>.s<XX>.b<XX>`**——那里的 MJD+秒是**实验级**标识（SWIN 按实验组织、跨 batch 追加），与 batch_id 无关，不要把它当成"batch_id 也能写成那样"的先例。

### 任务标识（task_id）规范（2026-09-27 新增）

编排层用**一个字符串**标识每个计算任务，脚本解析成维度值后调用工具。**格式：维度值用 `-` 连接，段数与内容决定任务类型。**

| task_id | 任务 | 展开为 |
|---|---|---|
| `<batch>` | 整 batch 的 x（**不分片**模式） | `fxcorr-x <batch> [workdir]` |
| `<batch>-<g>` | 第 g 个 **ds 组**（**分片**模式，见 5.9） | `fxcorr-x <batch> [workdir] <g>` |
| `<batch>-<station>-<ds>` | 某站某 ds 的 f 任务 | `fxcorr-f <batch> <station> [workdir] <ds>` |
| `<batch>-merge` | **batch 级**归并：本 batch 全部分片 → `merged.part`（⚠ 未实施，现状是直接写 SWIN） | `fxcorr-x merge <batch> [workdir]` |
| `<exp>-merge` | **实验级**归并：全部 batch 的 `merged.part` → D10 SWIN（⚠ 未实施；形态 A 下**它是 SWIN 的唯一写入者**） | `fxcorr-x merge --experiment [workdir]` |

后两行是 **V6 的两级 merge（形态 A）**，见 5.9 末条与 `v6-plan.md` S4.1；`<exp>` = 实验标识
（`OUTPUT FILENAME` 的 `.difx` 目录名），**不是** batch_id——实验级任务跨全部 batch。

**各段含义**：

- `<batch>` = 8 位 batch_id（见上一节）。
- `<station>` = 站名（`.input` 的 TELESCOPE NAME；字母数字，不含 `-`）。
- `<ds>` = **站内** datastream 序号（0-based）——与 `fxcorr-f` 的 `ds_index`、`raw` 的 `_ds<N>` 后缀、`fengine/<bid>/<st>/ds_<N>/` **四处同一口径**，详见 5.2。
- `<g>` = **ds 组序号**（0-based，按频段升序）。**注意它与 `<ds>` 不是同一个维度**：一组 ds 是**跨站、含全部极化**的——互相关要求两站同一 freq 同时在场，且要算全极化组合（RR/LL/RL/LR）。t25362 是 **4 组**，每组 4 个 ds（2 站 × 2 极化）；实测的 ds→(频段, 极化) 映射与分组依据见 `data-volume.md` §7.3。

**解析建议**：按**段数**分派，不要按位置硬编码——1 段 = 整 batch 的 x；2 段且第二段为 `merge` = 归并，2 段且为数字 = x 分片；3 段 = f 分片。

**f 与 x 的任务数不同**（这是"任务 ID 不能只有一种形状"的根因）：f 任务 = 站数 × 每站 ds 数（t25362 是 16），x 分片任务 = 频段组数（t25362 是 4）。**编排层要维护任务依赖**——一个 x 分片任务只等"该 batch 中属于该 ds 组的那些 f 任务"完成，不是等全部 f 任务。

**这是编排层约定，不改变三工具的命令行接口**——`fxcorr-f` 的 `ds_index`、`fxcorr-x` 的 `ds_group` 与 `merge`（含形态 A 的 `--experiment`）已能表达全部维度。任务 ID 的价值在编排侧：任务表、日志、重试、去重都以一个字符串为键（对齐 scalebox 的 task 模型）。

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

分片模式（**2026-09-27 已实施**，见 `data-volume.md` §7 与 5.9 节）：fxcorr-x 按 datastream 切分后，分片任务**不直接写 SWIN**，而是产出 D16 局部记录，再由 `fxcorr-x merge` 归并写出 D10——SWIN 的追加顺序必须严格按时间，写入者只能有一个。**V6 起 merge 分为两级**（⚠ 未实施，见 5.9 末条与 `v6-plan.md` S4.1）：batch 级归并在**计算节点本地**完成、只写 `merged.part`，**实验级**才写 SWIN：

```
D8 (fengine) + D9
        ↓
   [fxcorr-x × 每 ds 一片]        ← 各片独立、可乱序、可跨节点
        ↓
D16 (vis-parts/<batch_id>/ds<G>.part)
        ↓
   [fxcorr-x merge <batch>]       ← batch 级：按整数纳秒时间戳归并（节点本地）
        ↓
D16 (vis-parts/<batch_id>/merged.part)
        ↓
   [fxcorr-x merge --experiment]  ← 实验级：按 batch 数据时间序写出；**唯一写入者**
        ↓
D10 (vis/<experiment>.difx/ SWIN)
```

**在形态 A 落地前**，现状是上图少了实验级那一层——`fxcorr-x merge <batch>` 直接写 SWIN，靠
"同实验 batch 按时间序串行"（第 12 节）保证单调，**多节点并行 batch 时会静默错数据**。

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
| 批量目录 | - | `<batch_id>` | `00000001` |
| F 频谱文件 | D8 | `band_<xx>.sp` | `band_00.sp` |
| F pcal 文件 | D8 | `pcal.bin` | `pcal.bin` |
| F 自相关文件 | D8 | `autocorr.bin` | `autocorr.bin` |
| 可见度文件 | D10 | `DIFX_<MJD>_<sec>.s<XX>.b<XX>` | `DIFX_60512_43200.s0000.b0000` |
| 分片局部记录 | D16 | `vis-parts/<batch_id>/ds<G>.part` | `vis-parts/00000001/ds0.part` |
| 波束文件 | D14 | `beam/<batch_id>/beam.bin` | `beam/00000001/beam.bin` |
| 公共信号 | D15 | `sim-common/<batch_id>/data_XX.bin` + `meta.json` | `sim-common/00000001/data_00.bin` |
| 批量元数据 | D9 | `batches/<batch_id>.json` | `batches/00000001.json` |
| FITS 产品 | D11 | `<exp>_<batch_id>.FITS` | `exp_00000001.FITS` |

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
  | D16 `vis-parts/` | batch × ds 组 | 该实验的 **`merge` 写出 SWIN 之后**——现状是 batch 级 merge 之后；**形态 A（V6，⚠ 未实施）下是实验级 `merge --experiment` 之后** | 重跑该分片的 x，需 D8 在 | 纯中转数据，归并成功即无用；**归并前必须集齐**，缺一片则报错不写 |
  | D11/D12 `product/` | 实验 | **终态保留**（归档到长期存储为止） | 重跑 difx2fits，需 D10 在 | 最终科学产品 |
  | D1–D6/D9/D13 `config/`、`batches/`、`meta/` | 实验 / batch | **终态保留** | 前处理可重跑（vex2difx/difxcalc） | KB~MB；删了就没有"当初怎么跑的"的追溯依据 |

  两点注意：① 表里是**最早**可删时点（下限）——它按"下游跑过一遍"给，而实际删除还应等**该级结果确认**（改权重 / 改 pcal / 修 reader 的 bug 都要回头重跑，此时上游必须还在）；② **可再生的依赖链是逐级的**：raw → fengine → vis → product，删掉任一级就等于把以下各级钉死。仿真链路里 raw 也可再生（fxcorr-sim，真正不可再生的只有真实观测的 raw）。
- **batch duration 选择**：硬约束 = intTime 整数倍 + 起点落在**积分**边界（上文）。**旧的"batch_id 秒唯一（时长 ≥ 1 s）"已取消**（第 6 节，2026-09-27）：顺序号不承载时间，batch 时长不必 ≥ 1 s。之上权衡：① **调度并行度**——batch 是并行/调度单元（scalebox task，编排外置 V2+），batch 数越多多节点并行越好；② **内存**——x 侧 .sp 整 batch 常驻（V1），duration × 站数 × D8 数据率须在节点内存内；③ **重试成本**——失败重跑整个 batch（V1 无 batch 内回滚）；④ **吞吐 vs 延迟**——启动开销（配置读入、目录、SWIN 头）摊销想大，首个结果的可见延迟与 fengine/ 存储峰值想小。**大 duration 不利**：x 侧内存线性涨（OOM 风险）、失败重算面大、并行度下降（batch 少 → 节点闲置）、端到端延迟高、fengine/ 中间数据峰值大（D8 ≈ 16×D7，见 `data-volume.md` §1.3）。
- **站间时间同步**：per-station 串行架构下站间不靠 MPI 屏障同步，同步信息全在数据时间戳（VDIF 帧 epoch/帧号、.sp header 的 scan/sec/ns）+ .im/.calc 的时钟与几何延迟模型；fxcorr-f 的粗延迟（采样级移位）+ 条纹旋转/小数采样即"把两站拉到同一时刻"。站间残余延迟误差 Δτ 的后果按量级分档：**< 1 采样** → 被小数采样校正吸收；**1 采样～亚 subint** → 带宽 smearing 去相关（幅度 ×sinc(Δτ·Δν)，4MHz 带宽 1 采样误差即 -36%）+ 跨频相位斜坡 2πΔf·Δτ；**帧级（4ms）** → 两站乘不同时刻信号，完全去相关、weight 崩；**subint 级** → .sp 时间戳错位，错位相乘、静默错数据。注意：fxcorr-x 按 .sp header 时间对齐、**不校验站间一致性**（站间错位不报错、静默产出低质量数据，与 mpifxcorr 行为一致）；真实观测的站钟漂移靠 .im 时钟多项式补偿，模型不准的残余误差随时间演化。
- **并行模型（V3 定案，V6 修订）**：并行维度只有时间——batch 即并行/调度单元（多 batch 并发由 scalebox 编排承担，编排外置不在本项目）；无空间切分（原 V2 P2 基线子集方案已取消，见 algo-plan P2 节）。**V3 的"同实验 batch 串行约定（路线 B）"在 V6 的多节点部署下不再成立**——数百个节点并行处理同一实验的不同 batch 是常态，实验级共享文件的并发语义必须显式解决：

  | 文件 | 并发语义 |
  |---|---|
  | **SWIN**（D10） | **已由形态 A 解决**（写入收敛到实验级 `merge --experiment` 一次），见 5.9 末条（⚠ 未实施） |
  | **`PCAL_*` / `SWITCHEDPOWER_*`** | **仍待确认**——两者都是 `ios::app` 追加（`visibility.cpp:122`），文件名含**实验起点**（`config->getStartMJD/getStartSeconds`，**不是 batch 起点**），所以同一实验的全部 batch 写同一批文件。POSIX 的 `O_APPEND` 对 `PIPE_BUF`（4096 B）以内的写是原子的，短行**不会截断交错**；但**行序不再等于时间序**，且每次打开都会重写一遍注释头（串行下同样如此）。**V6 实施前要核实 difx2fits 对行序与重复注释头的容忍度**（`v6-plan.md` S4.1 的待确认项） |

---

## 13. 设计要点小结

- 所有处理以 **batch_id** 为单元，一次只处理一个批量
- 目录即接口，程序通过输入输出目录衔接，无需 MPI
- 每个批量目录自带 `batch.json`，便于追踪与断点续跑
- 原始数据按台站存放，处理后按批量组织
- f 落盘"完全校正"的频谱（含小数采样、条纹旋转、延迟对齐），x 只做 XMAC + 积分 + SWIN 写盘
- 前处理（vex2difx、difxcalc）与后处理（difx2fits、difx2mark4）保持兼容，SWIN 直出使 difx2fits 零改造
- 为后期并行化与流式处理预留清晰扩展空间
