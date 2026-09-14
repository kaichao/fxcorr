# fxcorr-sim 分布式架构（单二进制 `fxcorr-sim`）

2026-09-14 定稿（前身：tmp/fxcorr-sim-distributed-architecture.md 草稿）。本文定 fxcorr-sim 的分布式形态与公共信号模型；调用方式/实现要点/测试记录见 `applications/fxcorr-sim/CLAUDE.md`，命令行手册见 `usage.md`，common/ 产物格式见 `data-spec.md`。

## 1. 定位与目标

用一个可执行文件 `fxcorr-sim` 生成对接 fxcorr 流水线的多站基带数据，支持单机一键与多节点分布式：

- 输出：`raw/<station>/<station>_<batch_id>.vdif`（file-per-batch，不变）
- 配置与时间：`batches/<batch_id>.json`、`config/`（`.input`、按需 `.im`）
- 中间：`common/<batch_id>/`（公共信号，权威一份）
- V1：单线程；加速靠多任务/多节点跑站级，不用 OpenMP

分布式由编排层（V2 scalebox）驱动 `common` / `station` 两个入口，程序内不引入 MPI。datasim（MPI 一体式）与本设计的关系：公共信号 Bcast 换成共享存储单份落盘、全站一次生成换成逐站任务、MPI 并行换成 (batch, station) 任务分片——信号合成算法（频域公共信号 + 分层注入）照 datasim 移植。

## 2. 单一入口，三种用法

```text
fxcorr-sim common  <batch_id> [workdir]
fxcorr-sim station <batch_id> <station> [workdir] [tone_mhz ...]
fxcorr-sim         <batch_id> [workdir]
```

| 调用 | 行为 |
|------|------|
| **`common`** | 只生成公共信号 → `common/<batch_id>/`（meta.json + 数据文件） |
| **`station`** | 只读公共信号，生成**一个**站 → `raw/<station>/`；tone_mhz 位置参数 = **legacy 模式**触发器（旧时域合成路径，字节对拍回归专用） |
| **无子命令（默认）** | **本机串行**：先 `common`，再对 `.input` 全部站依次 `station`（一键多站；tone 参数不允许出现在此入口） |

实现上两个模块：`run_common()`、`run_station()`；默认模式 = 二者顺序调用。旧 4 参调用 `fxcorr-sim <batch> <station> [workdir]` 废止（改 station 子命令），编排脚本同步迁移。

## 3. 数据流

### 3.1 分布式（推荐）

```
共享存储 workdir/
  config/  batches/<batch_id>.json

[任务 1 — 仅一次]
  fxcorr-sim common --workdir W --batch B
        │
        ▼
  common/B/  + 完成标记（meta.json status=done）
        │
        ├─────────────────┬──────────────────┐
        ▼                 ▼                  ▼
fxcorr-sim station    station             station
  --station STA1        STA2                STAk
        │                 │                  │
        ▼                 ▼                  ▼
  raw/STA1/...       raw/STA2/...       raw/STAk/...
```

### 3.2 单机默认

```
fxcorr-sim --workdir W --batch B
  → 内部: run_common(); for s in stations: run_station(s);
```

## 4. 一致性与部署

| 规则 | 说明 |
|------|------|
| 权威公共数据 | 仅**一次**成功的 `common` 写入共享存储；`common/<batch_id>/` 为共享资产 |
| 站级 | 只读 `common/<batch_id>/`，不改写公共文件 |
| 完成可见性 | 数据文件先写临时名再 `rename`；meta.json 的 `status=done` 写全后 station 才可启动 |
| 禁止 | 多节点同时跑**默认多站**（会重复 common、竞争写站数据）；common 由编排保证单实例 |
| 站噪声种子 | `f(global_seed, station_id)`，与公共种子分离 |
| 消费后清理 | batch 的全部 station 完成后 common/<batch_id>/ 可删（同 fengine/ 生命周期管理，data-spec 12 节） |

**优先：** 单任务 `common` + 多任务 `station`。**退路：** 单机默认串行。**不优先：** 每节点各自重算 common（见第 8 节成本分析；同架构节点确定性复现在科学上等价，但失去"权威单份"的单源一致性，且重复计算无收益）。

选"权威单份"而非"确定性复现"的核由：单源读取使跨站一致性与节点架构解耦——确定性复现的逐位一致只在同架构节点成立（fftw 不同 SIMD 路径可能有末位浮点差异），共享存储单份没有此限制。

## 5. 信号模型

**Common（`run_common`）**

- 输入：batch 时间窗、`.input` 全站 band 布局、通量或谱模型（P2）、全局种子
- 输出：**频域**公共信号 S（已拍板）：每 `stime = 1/specRes` µs 一个 `numSamps` 复频谱 slice，float32 复数对（datasim gencplx 语义，STDEV=1）；`specRes` = 全站 band 频率差/带宽的 GCD 网格（0.5 MHz 起、二分至 1/2^10，datasim getSpecRes 移植）、`numSamps = maxChanFreq/specRes`（全站 band 覆盖跨度）；覆盖 `[minStartFreq, maxStartFreq+maxBW]`
- 确定性：S 只与 (seed, batch) 有关，与站无关；seed = FXSIM_SEED（默认固定），编排统一写
- 不含站噪声、不含站相关随机数、不做量化

**Station（`run_station`）**

```
V_i = g_i · S(t − τ_i) · e^{jφ_i} + n_i
```

- 读同一份 S，切自己 band 的频段（startIdx/blksize 由站 band 频率/带宽对 specRes 网格换算）
- 加本站噪声 n_i（SEFD/σ，datasim fabricatedata 语义：×√F → +√SEFD 站噪声 → ÷√(F+SEFD)；FXSIM_FLUX > 0 启用（替代 FXSIM_NOISE 路径），SEFD 单值或按 datastream 序逗号列表，默认 1000；P0 的 FXSIM_NOISE σ 语义保留为 flux ≤ 0 路径）
- Ormsby 频域边缘滤波（0, 1/2, 4/5, 1…1, 4/5, 1/2）→ IDFT 出复基带缓冲
- τ_i 注入（已实现，datasim updatevalues + processdata 语义）：每帧 .im 模型求 delay/rate，fracsamperror 累积超半复样本整样本移位（帧窗口在滚动基带缓冲上移动），频域亚样本校正（e^{j·2π·bandwidth·idx/vpsamps·fracerr}），时域条纹旋转（band 起始频率，fraction_of 小数相位）；FXSIM_DELAY=0 时校正链恒等（字节回归判据）
- 复转实：Hermitian 2N IDFT 取实部 → 2bit 量化打包（复用 quantise2bit + FXSIM_ADAPTIVE）→ VDIF 写盘
- pcal 注入（.input PHASE CAL 网格，station 端，新路径待后续阶段）

### 数据量事实（设计依据，2026-09-14 分析）

公共信号 float32 复基带 = 8 字节/复样本 × 覆盖带宽；单站 2bit VDIF = 0.5 字节/s × 记录带宽。比值恒 **16 倍**（32bit/复样本 ÷ 2bit/实样本，与带宽无关）：

| 场景 | common 数据率 | 单站 VDIF | 说明 |
|---|---|---|---|
| test 配置（1×4MHz，8Ms/s 2bit） | 32 MB/s | 2 MB/s | batch 4.096s → common 131 MB vs 单站 8.4 MB，batch 粒度绝对量可控 |
| 异带多站（8 站各 4MHz 分布 40MHz 跨度） | 320 MB/s | 2 MB/s/站 | common = 全站总量（16 MB/s）的 20 倍，共享存储持续读写需评估 |

- 缓解手段（预留，不实现）：dtype 降 int16（比值 8 倍，96dB 动态范围对噪声信号无碍）；覆盖范围按 batch 实际用到的频段裁剪。**起步 float32，meta.json 带 dtype 字段留降级口。**
- 结论：common 落盘量 ≥16× 单站数据，但 batch 粒度下绝对量可控、消费后可删；与"每节点各自重算 common（省 I/O、每站多算 ~10% 计算，见第 8 节）"相比，权威单份的一致性收益压倒 I/O 代价，定为唯一形态。

## 6. 目录约定

```
workdir/
├── config/                 # .input / .im
├── batches/<batch_id>.json
├── common/<batch_id>/      # common 子命令输出（新产物类型，data-spec 第 2/3/5 节同步）
│   ├── meta.json           # 版本/dtype/specRes/numSamps/minStartFreq/块表/seed/status
│   └── data_XX.bin         # 每 0.5s 块一个文件，float32 频域 slice 顺序
└── raw/<station>/
    └── <station>_<batch_id>.vdif
```

数据文件布局与 meta.json 字段以 `data-spec.md` 5.8 节为准（格式版本随文件格式变更递增；改格式必须先同步 data-spec）。

## 7. CLI 要点

```text
fxcorr-sim common  <batch_id> [workdir]
fxcorr-sim station <batch_id> <station> [workdir] [tone_mhz ...]
fxcorr-sim         <batch_id> [workdir]
```

- 位置参数风格与 fxcorr-f / fxcorr-x 一致；workdir 语义沿用（位置参数 > `FXCORR_WORKDIR` > `.`）
- 站列表（默认模式）：来自 `.input` 全部 datastream（与 run_batch.sh 同语义）
- `station` 模式：公共未就绪（meta.json 缺失或 status≠done）则报错退出
- `station` 模式带 tone_mhz 位置参数 = legacy 模式：完全旧合成路径（tone/pcal/FXSIM_DELAY/FXSIM_FLUX/FXSIM_SEFD/FXSIM_ADAPTIVE 全部原样），供字节对拍回归
- 环境变量：`FXSIM_SEED`（公共种子，两入口共用）、`FXSIM_NOISE`（station 端噪声 σ，flux ≤ 0 路径）、`FXSIM_ADAPTIVE`（station 端量化）、`FXSIM_SPECRES`（specRes 缩放）、`FXSIM_LINE`（谱线 freq,amp,rms，freq 绝对 MHz、rms 网格点）、`FXSIM_FLUX`/`FXSIM_SEFD`（datasim 定标链，flux > 0 启用）、`FXSIM_DELAY`（延迟注入，默认开，0 关）

## 8. 计算与并行

| 项 | 设计 |
|----|------|
| common : station 计算比 | common 段（公共噪声生成 + 切频段 + IDFT + normalize）≈ 单站总量的 **10%**（test 配置估算：~200M flops/s vs 站总 ~2G flops/s，大头在站级帧校正链）；specRes 变细（多站频率差 GCD 小或 FXSIM_SPECRES 缩放）时公共段占比上升（specRes 减半 → 占比翻倍），这是"每节点各自重算 common 不优先"的量化依据 |
| 并行 | 多进程/多节点跑 **`station`**（任务粒度 = (batch, station)，与 fxcorr-f 同构）；V1 程序内单线程 |
| OpenMP | V1 不需要；P3 按需再评估 |

## 9. 实现结构（单二进制内）

```
fxcorr-sim
├── main                    # 解析子命令/默认模式、环境变量、legacy 分派
├── run_common()            # 频谱生成 + 分块写盘 + meta.json（rename 完成可见性）
├── run_station()           # 读 common → 频域链 → 量化打包 → VDIF（新路径）
├── SignalGen               # 分层：公共信号层（新）/ 站专属层（噪声、延迟注入）/ 量化打包层（共用）
│                           #   legacy 路径原样保留在站专属层旁
└── VDIFWriter              # 帧封装（不动）
```

公共文件读写独立成 `commonsignal.{h,cpp}`（格式版本化），与 SignalGen 解耦。

## 10. 与现有组件/特性的关系

| 组件/特性 | 关系 |
|------|------|
| 当前 `fxcorr-sim`（tone/噪声/pcal 时域合成） | 演进为本设计；旧路径整体保留为 station 子命令的 legacy 模式（字节对拍回归依赖） |
| tone 位置参数 | legacy 专属；新路径下 tone 由 P2 谱线机制（common 端频域注入）提供，跨站天然相干 |
| FXSIM_FLUX/FXSIM_SEFD | station 端 SEFD 定标（datasim fabricatedata 语义），已实现（flux > 0 启用） |
| FXSIM_DELAY | 新路径：频域链延迟注入（procptr/fracsample/条纹旋转）默认开、`0` 关；legacy 保留纯相位版 |
| FXSIM_ADAPTIVE / pcal / 量化打包 | station 端，沿用 |
| `datasim` | 物理参考；逻辑拆进 `run_common` / `run_station`，IPP→fftw3f |
| `fxcorr-f` / `fxcorr-x` | 只读 `raw/` + config，零感知 |
| 编排脚本（make_testdata.sh / run_batch.sh） | 数据生成步骤改两段式：common 一次 → 逐站 station（P0-5） |

## 11. 阶段

| 阶段 | 内容 |
|------|------|
| P0 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/CLAUDE.md）**：子命令框架 + 默认串行；频域 S 落盘（common）；station 读 S 加噪量化出 VDIF；legacy 模式挂接（字节对拍 BYTE-IDENTICAL）；编排脚本两段式；文档同步；测试机验证（跨站相干两站 FXSIM_NOISE=0 逐位一致、σ=1.0 相关系数 0.444 vs 理论 0.5、新路径全链路 SWIN） |
| P1 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/CLAUDE.md）**：make_testdata.sh `-p P` 本地并行 + `--nodes` ssh 节点映射分发 station 任务；测试机验证（p1reg）——3 batch × 2 站并行生成、远程/本地逐位一致、失败传播非零退出、并行产物全链路 SWIN 12 记录 |
| P2 | **已完成（2026-09-14，验证记录见 applications/fxcorr-sim/CLAUDE.md）**：SEFD/通量定标（station 端，datasim fabricatedata 链）；延迟注入完整链（procptr/fracsample/条纹旋转，默认开）；谱线 FXSIM_LINE + specres FXSIM_SPECRES（common 端） |
| P3 | 按需再考虑进程内并行 |

## 12. 约束

- 一个二进制，逻辑两阶段；分布式靠编排调用 `common` / `station`，程序内不用 MPI
- 默认模式仅限单机（或明确单任务）一键生成
- 一致性靠**共享存储上单份 common**，不靠多节点重复 common
- 公共文件格式单独定版本；改格式必须先同步 `data-spec.md`（新增 common/ 产物节后即为规范来源）
- legacy 模式只做回归，不做新特性

## 13. 一句话

**`fxcorr-sim` 单入口：`common` 写共享公共信号，`station` 按站读公共并写 raw；不加子命令则本机串行 common+多站；集群上 common 一次、station 多节点并行。**
