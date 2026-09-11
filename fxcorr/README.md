# fxcorr 脚本与改造概要

本目录（`fxcorr/`）用于 **bash 编排**：在已安装 `fxcorr-f` / `fxcorr-x` 的前提下，按 batch 驱动处理。算法实现见 `applications/fxcorr-f`、`applications/fxcorr-x`，共享代码见 `libraries/fxcorrcommon`。

---

## 1. 背景与目标

在 DiFX（本仓库为相关 clone）上，将相关核心从 MPI 一体的 `mpifxcorr`，拆为可串行运行、目录衔接的两个程序，便于流式批处理、脚本编排与容器化；原有前/后处理尽量复用。

**目标：**

- Station-based → `fxcorr-f`
- Baseline-based → `fxcorr-x`
- 去 MPI（计算主路径）
- 按时间批量（batch）处理；V1 单线程，正确性优先
- bash 编排；可容器化

**非目标（近期）：** 替换 vex2difx / difxcalc / difx2fits；工具内再拆为“一算法一进程”。

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
        ↓
  config: .input .im
  raw/<station>/        （外部持续写入）
        ↓
  [fxcorr：切批 + run_batch]
        ↓
  fxcorr-f × stations   →  fengine/<batch_id>/<station>/
        ↓
  fxcorr-x              →  vis/<batch_id>/
        ↓
  difx2fits 等（按需）
```

- **fxcorr-f**：单站、单批量；解包、模型、通道化等 Station-based。
- **fxcorr-x**：单批量、多站 F 输出；XMAC 与积分。
- **编排**：本目录脚本；不负责算法。

---

## 4. 代码与目录落位

| 组件 | 路径 |
|------|------|
| fxcorr-f | `applications/fxcorr-f` |
| fxcorr-x | `applications/fxcorr-x` |
| 共享库 | `libraries/fxcorrcommon` |
| bash 集成 | `fxcorr/`（本目录） |
| 原 MPI 核心 | `mpifxcorr/`（保留） |

运行时数据（通常不进 git）：`config/`、`raw/`、`fengine/`、`vis/`、`product/`、`meta/`。

---

## 5. 处理与 batch 约定

- **batch_id**：在调用 `fxcorr-f` 之前由切批/编排生成（如起始时间编码）。
- **每 batch**：只跑 f（各站）+ x；不跑 vex2difx / difxcalc。
- **预处理**：配置或观测范围变化时再跑。

流式场景下：外部程序持续写入 `raw/` → 监视脚本定期生成 batch_id → 调用 `run_batch.sh`。

---

## 6. 本目录脚本（规划）

| 脚本 | 作用 |
|------|------|
| `run_batch.sh` | 对单个 batch_id 依次调用各站 `fxcorr-f`，再调用 `fxcorr-x` |
| `watch_and_dispatch.sh` | 长驻或轮询：发现齐套时间窗后生成 batch_id 并调用 `run_batch.sh` |

示例（接口以实际实现为准）：

```bash
# 实验开始时一次
# vex2difx config/experiment.v2d
# difxcalc config/experiment.calc

./fxcorr/run_batch.sh 60512_45000 STA1,STA2,STA3
```

---

## 7. 实现阶段（简）

| 阶段 | 内容 |
|------|------|
| V1 | 两程序串行 + 目录接口 + bash 跑通单 batch |
| V2 | 脚本监视 raw、多 batch；可选容器镜像 |
| V3 | 模块级 OpenMP；按需 GPU |
| 可选 | 输出与 difx2fits 更好衔接 |

---

## 8. 设计要点

**应用两个、库一段、脚本编排；实验级配置一次，批次级只跑 F/X；先正确后并行。**
```
