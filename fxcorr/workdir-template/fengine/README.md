# fengine/ —— F-Engine 输出（D8，频域谱）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 第 5.3 节（含 `.sp` 的二进制布局）。

| 属性 | |
|---|---|
| **根变量** | `FXCORR_FENGINE_ROOT`（默认 `$FXCORR_WORKDIR/fengine`） |
| **生产者** | `fxcorr-f` |
| **消费者** | `fxcorr-x` |
| **生命周期** | **该组的 x 分片跑完后可删**（组级 purge，2026-10-09 定；下限 = 该 batch 的 x 跑完） |
| **量级** | **= 16 × raw**（同一批数据从 2bit 展开成 cf32，见 `fxcorr/data-volume.md` §4.1） |
| **共享 / 本地** | **本地**；**多节点部署必须落 tmpfs**（`/dev/shm`） |

## 目录结构

```
fengine/
└── 00000001/                   # batch_id
    ├── BA/
    │   └── ds_0/               # 站内 datastream 序号
    │       ├── band_00.sp      # 每个 recorded band 一个频谱文件
    │       ├── band_01.sp
    │       ├── ...
    │       ├── pcal.bin        # 脉冲校准 tone（每 subint）
    │       └── autocorr.bin    # 自相关（每 subint，已频率平均）
    └── S6/
        └── ds_0/
```

**单 datastream 站同样是 `ds_0/`**——布局统一，没有平铺特例。
f 任务 = `(batch_id, station, ds_index)`；`ds_index` 是站内序号，与
`../raw/README.md` 里 `_ds<N>` 的口径一致。

## 三层编号各回答一个问题

| 层 | 含义 | 依据 |
|---|---|---|
| `<batch_id>` | **哪段时间** | `batches/<batch_id>.json` |
| `<station>/ds_<N>` | **哪个站、哪路记录** | `.input` 的 DATASTREAM 表 |
| `band_<xx>.sp` | **哪个 recorded band** | `.input` 的 FREQ 表；`xx` 是**该 ds 内**的序号 |

`band_<xx>` 不是全局 band 号——同一个频段在不同 ds 里可能都是 `band_00.sp`
（这正是"按 ds 分片"能成立的原因：一个 ds 是一路独立数据流，含它自己那组
band 的全部极化）。

## 为什么它必须落 tmpfs

产生率是 **GB/s 级**（t25362 参数 4 站 1 小时 = 65.8 GB/s 量级），
落 SATA SSD 差两个数量级。落 tmpfs 后走内存带宽，该约束消失——
目标机 60 GB tmpfs 正好装得下分片后单组的量。**p419 实测（2026-10-09，mini 配置）**：
SATA SSD 写 **199 MB/s**、tmpfs 写 **2.3 GB/s**——fengine 写需求（30 并发 f）≈ **3.6 GB/s**，
对 SATA 仍超载约 18×、对 tmpfs 约 1.6×（写带宽并发上限 ≈19 个 f）。

**同节点的并发组数有上限**：多组并存会顶到 tmpfs 容量（**组 = 一个频段 X/Y 对 × 全站**，
`data-volume.md` §1.4——mini 4 站 = 8 ds/组、4 组/批），所以要逐组串行、或限制并行组数、
或靠 **f/x 流水线化 + 组级 purge**（x 消费完该组即删）压峰值——**核算公式：在制量 =
在制组数 × 组体量**（§1.4）。

## 文件格式

三种文件都是 **host 字节序**（V1 单机）；`XX` 是**该 ds 内**的序号。

### `band_XX.sp`（D8）

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
  f32[blocks_per_send]        weights（每 FFT 块的 dataWeight，0.0~1.0）
  cf32[blocks_per_send × num_channels]  spectra（subloop 序，每 subloop num_channels 个复数）
```

语义要点：

- `cf32` = `struct { float re; float im; }`（与 mpifxcorr 的 `cf32` 定义一致）；
- **weights 是 per-band 的**：各 `.sp` 只存本 band 的权重；槽式更新——写盘时按槽收集后
  回填（`fxcorr-f` 的 `FEngineWriter::flushWeights`）。fxcorr-x 侧把 baselineweight
  还原为**两站对应 band 权重之积**（`mode.cpp` 的 weights 累加语义）；
- **spectra 已完成**解包、延迟对齐（整数 + 分数采样校正）、条纹旋转、FFT；
  **不存共轭副本**——fxcorr-x 按需对整段做逐元素共轭（等价于原 `getConjugatedFreqs()`）；
- 无效 subloop（`valid=0` 或 `dataWeight=0`）的频谱块为**全零**；
- **zoom band 不单独落盘**：zoom 频谱是父 band 频谱数组的指针切片（`mode.cpp:184-195`），
  fxcorr-x 按 `.input` 的 zoom 定义（`zoomfreqchanneloffset` + zoom freq 的 nchan）
  对父 `band_XX.sp` 做通道切片视图读取——`.sp` 的文件与布局不变。

### `pcal.bin`（仅当该站配置了 phasecal）

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

### `autocorr.bin`

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
  crosspol 段（仅 crosspol=1）：每 band：cf32[num_channels] + f32 weight
```

- 自相关在 f 侧由 `Mode::process` 累积，每 `maxacblocks` 个 FFT 平均后落一条记录、
  随即 `zeroAutocorrelations()`；x 侧把该 subint 的全部记录逐条累加进 SWIN 自相关段
  （`vectorAdd`，基线号 `257*(telescope_index+1)`，与现 DiFX 约定一致）；
- `maxacblocks` 由 `.calc` 的 `AC AVG INTERVAL` 与 subint 结构共同决定；
- **zoom band** 的自相关谱是父 band 平均后数组的切片（偏移已除 `channelstoaverage`），
  weight 从父 recorded band 取；
- **交叉极化段**（`crosspol=1`）接在平行段后，交叉谱 = 同一 FFT 块内
  `R×conj(L)` 与 `L×conj(R)` 的累加；`version 1` 的旧产物按无 crosspol 段处理。

## 相关

- `fxcorr/data-spec.md` 5.3：跨目录约束与改版要求——**文件格式以本节为权威**，
  改布局时递增 `.sp` Header 的 `version`
- 体积公式与分片：`fxcorr/data-volume.md` §1.3、§5.1、§7
- 部署形态（tmpfs、同节点组数上限）：`fxcorr/v6-plan.md`「多节点部署形态」
