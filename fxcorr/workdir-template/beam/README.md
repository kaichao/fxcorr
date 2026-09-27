# beam/ —— X-Engine 波束输出（D14）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 5.5（含 `beam.bin` 的完整二进制布局）。

| 属性 | |
|---|---|
| **根变量** | **无**——恒在 `$FXCORR_WORKDIR/beam` |
| **生产者** | `fxcorr-x`（**相位阵模式**，见下） |
| **消费者** | 下游分析（目前无工具直读，格式为 fxcorr 自定） |
| **生命周期** | 按需保留 |
| **量级** | 小（每 acc 窗口一条记录，远小于 SWIN 的基线数） |

## 目录结构

```
beam/
└── 00000001/                 # batch_id
    └── beam.bin              # 每 acc 窗口一条记录（追加写，重跑覆盖幂等）
```

## 相位阵模式与互相关模式互斥

`.input` 的 CONFIG 段有 `PHASED ARRAY TRUE` + `PHASED ARRAY CONFIG FILE` 时，
`fxcorr-x` **不做互相关、不写 SWIN**，改为波束形成：各站频谱按相位阵配置文件的
`DWeight` 加权求和（`Σ_ds DWeight[freq][ds] × spectrum_ds`，**累加不归一化**），
按 `ACC TIME (NS)` 窗口落盘。所以开启相位阵的 batch，`vis/` 下不会有它的 SWIN。

上游 mpifxcorr 的相位阵输出端（padomain / paoutputformat / DIFX / VDIF /
TIMESERIES）是**无消费者的死代码**，所以 `beam.bin` 的格式 **fxcorr 自定**
（设计见 `fxcorr/algo-plan.md` P8 节）。

## 文件格式（`beam.bin`，host 字节序，V1 单机）

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

- 波束 = `Σ_站 DWeight × 频谱`，**累加不归一化**、**不查 valid flags**——无效 FFT 块的
  频谱在 f 侧落盘即全零，直接加权自然等价（与上游 `core.cpp:818-865` 一致）；权重 ≥0
  由 Configuration 校验；
- acc 窗口 = 整数个 FFT 块（Configuration 校验 `accffts` 为 `NUM BUFFERED FFTS` 的整数倍）；
- 窗口起点时间戳 = subint 起点 + acc 序号 × `acc_ns`（scan 相对系，同 `.sp`）。

## 相关

- `fxcorr/data-spec.md` 5.5：跨目录约束（**格式以本节为权威**）
- 检验资产与判据：`fxcorr/test/phasearr/`（P8）
