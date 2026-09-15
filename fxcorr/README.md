# fxcorr 脚本与改造概要

本目录（`fxcorr/`）用于 **bash 编排**：在已安装 `fxcorr-f` / `fxcorr-x` / `fxcorr-sim` 的前提下，按 batch 驱动处理。算法实现见 `applications/fxcorr-f`、`applications/fxcorr-x`，仿真数据生成器见 `applications/fxcorr-sim`，共享代码见 `libraries/fxcorrcommon`。

---

## 1. 背景与目标

在 DiFX（本仓库为相关 clone）上，将相关核心从 MPI 一体的 `mpifxcorr`，拆为可串行运行、目录衔接的两个程序，便于流式批处理、脚本编排与容器化；原有前/后处理尽量复用。

**目标：**

- Station-based → `fxcorr-f`
- Baseline-based → `fxcorr-x`
- 去 MPI（计算主路径）
- 按时间批量（batch）处理；V1 单线程，正确性优先
- bash 编排；可容器化

**非近期目标：** 替换 vex2difx / difxcalc / difx2fits；工具内再拆为“一算法一进程”。

---

## 2. 需求摘要

| 编号 | 需求 |
|------|------|
| R1 | 提供独立可执行程序 `fxcorr-f`、`fxcorr-x` |
| R2 | 通过目录传递数据，不依赖 MPI 消息 |
| R3 | 以 batch_id 为处理单位；流式场景下由编排层生成 batch_id |
| R4 | 前处理按实验/配置执行一次，非每个 batch |
| R5 | V1 单线程串行；后期单节点内按模块用 OpenMP / GPU |
| R6 | bash 脚本集成；可挂载数据目录做容器运行 |
| R7 | 与现有 `mpifxcorr` 并存，不强制删除 |

---

## 3. 总体架构

```
vex2difx / difxcalc     （实验级，一次）
  仿真数据生成器        （测试数据替身，串行）
        ↓
  config: .input .im
  raw/<station>/<station>_<batch_id>.vdif
        ↓
  [run_batch.sh：batch.json → fxcorr-f → fxcorr-x]
        ↓
  fengine/<batch_id>/<station>/
        ↓
  vis/<experiment>.difx/（SWIN，跨 batch 追加）
        ↓
  difx2fits 等（按需）
```

- **fxcorr-f**：单站、单批量；解包、模型、通道化等 Station-based。
- **fxcorr-x**：单批量、多站 F 输出；XMAC 与积分。
- **编排**：本目录脚本；不负责算法。

### 3.1 改造基准：mpifxcorr 三层分类

把 mpifxcorr 看成一个整体进行比较，很容易把 MPI、管理、算法混在一起。改造基准按其内容拆成三层：

| 类型 | mpifxcorr 中的内容 | fxcorr-f/x 是否需要 |
|---|---|---|
| **A. 科学算法** | unpack → delay/fringe → FFT → XMAC → accumulation → calibration/output | **必须迁移**（fxcorr-f：unpack→FFT；fxcorr-x：XMAC→积分→SWIN 写盘） |
| **B. 算法辅助功能** | PCAL、autocorr、cross-pol、TCAL、zoom、MPC、pulsar、kurtosis、phased-array 等 | **必须迁移**（V2 算法改进清单 P0-P11，已全部完成，状态见 v2-plan.md 第 5 节） |
| **C. MPI/运行时机制** | core × baseline process grid、MPI send/recv、manager、datastream process、MPI barrier 等 | **不直接迁移**——由 (batch, station) / (batch, 站组对) 任务模型 + 目录接口 + bash 编排替代 |

### 3.2 Station / Baseline 拆分

fxcorr-f/x 的总体设计即 mpifxcorr 按进程角色拆为两侧（C 层机制由任务模型与目录接口承接）：

```
                    mpifxcorr
                       │
          ┌────────────┴────────────┐
          │                         │
      Station side              Baseline side
          │                         │
       F engine                  X engine
          │                         │
      fxcorr-f                  fxcorr-x
          │                         │
        .sp                     SWIN
```

### 3.3 FX 相关器核心算法链

```
FX correlator 核心算法链
raw data
   │
   ├─ format decode
   ├─ delay correction
   ├─ integer sample correction
   ├─ fractional sample correction
   ├─ phase/fringe rotation
   ├─ FFT
   │
   ├───────────────┐
   │               │
 autocorrelation  PCAL / TCAL / kurtosis
   │
   └────── .sp ────┐
                    │
                fxcorr-x
                    │
              baseline selection
                    │
               conjugation
                    │
                   XMAC
                    │
          freq averaging / correction
                    │
        phase centre / pulsar processing
                    │
                integration
                    │
                  SWIN
```

（delay/integer/fractional correction 与 phase/fringe rotation 在 f 侧解包之后、FFT 之前；autocorrelation 与 PCAL/TCAL/kurtosis 是 f 侧旁路输出；phase centre/pulsar 处理在 x 侧 uvshift 段。）

---

## 4. 代码与目录落位

| 组件 | 路径 |
|------|------|
| fxcorr-f | `applications/fxcorr-f` |
| fxcorr-x | `applications/fxcorr-x` |
| fxcorr-sim（仿真数据生成器） | `applications/fxcorr-sim` |
| 共享库 | `libraries/fxcorrcommon` |
| bash 集成 | `fxcorr/`（本目录） |
| 原 MPI 核心 | `mpifxcorr/`（保留） |

运行时数据（通常不进 git）：`config/`、`batches/`（批量元数据，D9）、`raw/`、`fengine/`、`vis/`、`product/`、`meta/`。

---

## 5. 处理与 batch 约定

- **batch_id**：在调用 `fxcorr-f` 之前由切批/编排生成（如起始时间编码）。
- **每 batch**：只跑 f（各站）+ x；不跑 vex2difx / difxcalc。
- **预处理**：配置或观测范围变化时再跑。

流式/多节点调度（V2+）：由 scalebox 编排承担——batch_id 生成与 `run_batch.sh` 的调用改由编排器发出，脚本本身不变。

---

## 6. 本目录脚本（规划）

| 脚本 | 作用 |
|------|------|
| `make_testdata.sh` | 构建 data-spec 布局的标准测试数据（前处理 + 仿真 VDIF + batch.json） |
| `run_bench.sh` | difx 原命令基准：mpifxcorr 固化流程出基准 SWIN 供对拍 |
| `run_batch.sh` | 对单个 batch_id：写 batch.json → 依次调用各站 `fxcorr-f` → 调用 `fxcorr-x` |

`watch_and_dispatch.sh` 已砍（V1 静态数据集无轮询场景）；流式监视与多节点调度 V2 由 scalebox 承担，容器化同列 V2（scalebox Module 需容器镜像）。

示例（接口以实际实现为准）：

```bash
./fxcorr/make_testdata.sh                 # 一次性：前处理 + 仿真数据 + batch.json
./fxcorr/run_bench.sh                     # 对拍基准（difx 原命令）
./fxcorr/run_batch.sh 60512_45000 STA1,STA2,STA3
```

---

## 7. 实现阶段（简）

| 阶段 | 内容 |
|------|------|
| V1 | 两程序串行 + 目录接口 + bash 跑通单 batch |
| V2 | 容器化封装（模块镜像）+ 镜像集成测试 + V1 遗留算法改进；scalebox 编排与分片参数化放其他仓库 |
| V3 | 并行化（多 x 子集进程级 + 模块级 OpenMP，P2/P3）+ 网络输入/数据流化（P5）；按需 GPU |
| 可选 | 输出与 difx2fits 更好衔接 |

---

## 8. 设计要点

**应用三个、库一个、脚本编排；实验级配置一次，批次级只跑 F/X；先正确后并行。**
```
