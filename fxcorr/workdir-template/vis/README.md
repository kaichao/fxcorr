# vis/ —— X-Engine 输出（D10，SWIN 可见度）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 5.4；分片方案见 `fxcorr/data-volume.md` §7.5。

| 属性 | |
|---|---|
| **根变量** | `FXCORR_VIS_ROOT`（默认 `$FXCORR_WORKDIR/vis`） |
| **生产者** | `fxcorr-x`（不分片）或**实验级 `merge`**（分片；⚠ V6 形态 A 未实施） |
| **消费者** | `difx2fits` / `difx2mark4`（零改造，直读） |
| **生命周期** | **长期保留** |
| **量级** | GB 级（1 小时 4 站） |
| **共享 / 本地** | **必须全局** |

## 目录结构

```
vis/
└── T25362.difx/                          # 目录名 = .input 的 OUTPUT FILENAME
    ├── DIFX_61037_24611.s0000.b0000      # D10：SWIN 二进制
    ├── DIFX_61037_24611.s0000.b0001
    ├── PCAL_61037_24611_BA               # 脉冲校准文本（f 侧写）
    └── SWITCHEDPOWER_61037_24611_BA      # 开关功率文本
```

**目录名不是约定死的**——由 `.input` 的 `OUTPUT FILENAME` 决定（相对时按
`FXCORR_VIS_ROOT` 拼根）。

SWIN 文件名里的 MJD / 秒是**实验级**的（`.input` 的 `START MJD`/`START SECONDS`），
**不含 batch_id**——同一实验的所有 batch 追加写入同一组文件。`s<相位中心>` /
`b<pulsar bin>` 的后缀在多相位中心或 pulsar binning 时才变化。

## ⚠ 追加顺序不是自由的

`difx2fits` 顺序读记录、按天线检查时间单调（`fitsUV.c:1227` 的 `RecordIsOld`）。
**时间回退的记录被静默丢弃**——只在结尾打印一行
`out-of-time-range records dropped`（`fitsUV.c:1868`），**不报错**。

| 场景 | 结果 |
|---|---|
| 单节点、同实验 batch 按时间序串行 | ✓ 天然满足 |
| 多节点并行（后完成的 batch 先写） | ✗ 回退 → 静默丢数据 |
| 同一 batch 混跑不分片与分片 | ✗ 重复记录 + 回退 |

- 现状防呆：`fxcorr-x` 启动时读目标 SWIN 的记录头，若本 batch 的时间范围内已有
  记录就**报错退出**（`FXCORR_X_SWIN_CONFLICT=allow` 可强制继续）；
- **V6 形态 A** 把 SWIN 的写入收敛到**实验级 `merge` 一次**（`fxcorr/v6-plan.md` S4.1）——
  batch 级只产出 `vis-parts/<batch_id>/merged.part`，不碰 SWIN。**⚠ 未实施**。

## ⚠ 这里还有两个文本文件会被并发写

`PCAL_<mjd>_<sec>_<station>` 与 `SWITCHEDPOWER_*` 由 f/x 以 `ios::app` 追加，
**文件名含实验起点**（不是 batch 起点）——同一实验的全部 batch 写同一批文件。

多节点并行时，行序不再等于时间序（`O_APPEND` 对 `PIPE_BUF` 以内的写是原子的，
短行不会截断交错，但顺序没有保证）。**形态 A 只解决 SWIN，不解决这两个文件**
——实施前须核实 difx2fits 对行序与重复注释头的容忍度（`data-spec` 第 12 节有表）。

## 文件格式

### `DIFX_*`（D10，SWIN 二进制）

每记录 **74 字节头** + `freqchannels` 个 `cf32` 可见度：

| 字段 | 类型 | 说明 |
|---|---|---|
| `sync` | u32 | `0xFF00FF00` |
| `version` | | |
| `baselinenum` | | 基线号；**自相关**用 `257*(telescope_index+1)` |
| `dumpmjd` / `dumpseconds` | int / double | 积分中点时间 |
| `configindex` / `sourceindex` / `freqindex` | | 索引 `.input` 的 CONFIG / SOURCE / FREQ 表 |
| `polpair` | 2 B | 极化对 |
| `pulsarbin` | | |
| `weight` | double | |
| `uvw` | 3 × double | 由 fxcorr-x 在写盘时用 Model（`.calc` + `.im`）在**积分中点**求值 |

与 mpifxcorr 的 `Visibility::writeSWIN` **逐字节一致**——difx2fits / difx2mark4
零改造直读，这是整个设计的前提。**记录长度不在头里**：由 `freqindex` 查
`.input` 的 FREQ 表得到（`nchan / channelsToAverage × 8` 字节）。

### `PCAL_<mjd>_<sec>_<station>`（脉冲校准文本）

由 f 生成，格式与 mpifxcorr **逐字节一致**（可与基准直接 diff 对拍）：

```
# DiFX-derived pulse cal data
# File version = 1
# Start MJD = <mjd>
# Start seconds = <sec>
# Telescope name = <station>
<station> <pcalmjd %17.11f> <intTime/86400 %13.11f> <dsindex> <n_recorded_bands> <max_tones> [<tonefreq %12g> <pol> <re %12.5e> <im %12.5e>]...
```

每 `intTime` 一行；tone 值 = intTime 内 subint 累加 × 可见度层校准缩放，LSB 写原值 /
USB 取负；不足 `max_tones` 补 ` -1 0 0 0`，全零不写行。**追加幂等**（按行时间戳替换），
跨 batch 追加。

`SWITCHEDPOWER_*` 同目录、同追加语义（P6 SwitchedPower 检验资产见
`fxcorr/test/tcal/`）。

## 相关

- `fxcorr/data-spec.md` 5.4：跨目录约束（**SWIN 格式以本节为权威**）
- 分片为什么不能直接写这里：`../vis-parts/README.md`
- 两级 merge 的形态与定序依据：`fxcorr/v6-plan.md` S4.1
