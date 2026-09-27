# sim-common/ —— 仿真公共信号（D15）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 5.8；体积核算见 `fxcorr/data-volume.md` §4、§6。

| 属性 | |
|---|---|
| **根变量** | `FXCORR_SIM_COMMON_ROOT`（默认 `$FXCORR_WORKDIR/sim-common`） |
| **生产者** | `fxcorr-sim common`（每 batch 一次） |
| **消费者** | `fxcorr-sim station`（该 batch 的每个站） |
| **生命周期** | 该 batch 的**全部 station 任务**完成后可删 |
| **量级** | **全部目录里最大**；只由覆盖跨度决定，**与站数无关** |
| **共享 / 本地** | **本地即可**——要求是"**同一 ds 组**读到同一份" |

## 目录结构

```
sim-common/
└── 00000001/                 # batch_id
    ├── meta.json             # 格式版本与网格参数
    ├── data_00.bin           # 每 0.5 s 一个块文件（00 起顺序编号）
    ├── data_01.bin
    └── ...
```

## 只有仿真才有这个目录

真实观测没有公共信号。它是 fxcorr-sim 的中间产物：各站从**同一份**公共信号
出发，各自叠加站噪声与几何延迟，才能得到跨站相干的仿真数据（真实数据天然相干，
不需要这个）。

**所以它不可再生也不可对照**——清掉它，该 batch 的仿真数据就再也造不出来
（除非重跑 `common`）。

## 文件格式

### 信号语义

**量化前的频域公共信号**——覆盖全站 `[minStartFreq, maxStartFreq+maxBW]` 的复基带
频谱时间流，每 `stime = 1/specRes` µs 一个 `numSamps` 点复频谱 **slice**（STDEV=1
高斯复噪声，实虚独立）。各站 station 端按自己 band 的 `(startIdx, blksize)` 切频段、
加站噪声、逆 DFT 出复基带——**切出的频段逐位相同**，这就是跨站相干的来源。

可选谱线（`FXSIM_LINE`）：gencplx 之后逐 slice 乘高斯滤波器
（`√amp·exp(−π²δ²/2rms²)`，`δ` = 网格点距，re/im 同乘）。相位校准 tone 由 P4 的
梳齿机制注入。

### 网格参数（由**全站** band 布局推导）

| 参数 | 定义 |
|---|---|
| `specRes` | 全站 band 频率差与带宽的 **GCD**（0.5 MHz 起、二分至 1/2^10，找不到即报错） |
| `numSamps` | `(maxStartFreq + maxBW − minStartFreq) / specRes` |
| `minStartFreq` | 全站最低 band 频率 |

**band 之间有空隙时间隙照常生成**（如 200 / 205 MHz 两个 band 之间），只是不被任何站
读取——这正是下一节"量级"的由来。band 频率须落在网格上（`(freq − minStartFreq)/specRes`
为整数，common 端校验）。`FXSIM_SPECRES=N` 把网格 ÷N（一致性检查照跑）。

### `meta.json`

| 字段 | 说明 |
|---|---|
| `version` | 格式版本（初版 1；随布局变更递增） |
| `dtype` | 数据元素类型（初版 `float32` 复数对；留 int16 降级口） |
| `spec_res_mhz` / `numsamps` / `min_start_freq_mhz` | 网格参数（上文） |
| `block_bytes` / `slices_per_block` | 每块字节数与 slice 数（`slices_per_block = 0.5 s / stime`） |
| `nblocks` | 块文件数（batch 末块按 batch 时长截断） |
| `seed` | 公共信号种子（**与站无关**；站噪声种子 = `f(seed, station)`，station 端派生） |
| `batch_id` / `start_mjd` | 归属批量与时间起点 |
| `line_freq_mhz` / `line_amp` / `line_rms` | 谱线参数（全 0 = 无谱线） |
| `status` | `running` / `done` |

### `data_XX.bin`

按 slice 顺序平铺：每 slice `numSamps` 个复数（`re`, `im` 各 float32 **小端**交替），
slice 内频点序 = 网格升序（`minStartFreq` 起）。块 `XX` 覆盖 batch 第 `XX × 0.5 s`
起的 0.5 s。

**完成可见性**：块文件先写 `<name>.tmp` 再 `rename`；全部块落盘后 `meta.json` 写
`status=done`。station 端发现 meta 缺失或 `status≠done` 即报错退出——**`done` 是就绪判据**。

## 可见性要求：同一 ds 组，同一份

多节点部署下，**同一个 ds 组涉及的那些站必须读到同一份**公共信号；不同 ds 组
读的是不同频段区段，各自拿到不同的副本**不算违例**。

（这条在分片之前是"同一 batch 同一份"，按分片修订后收窄到 ds 组——
`data-spec` 第 2 节的存储归属表。）

## 量级由"覆盖跨度"决定，不由实际带宽

```
体积 = 8 B/复样本 × 覆盖跨度 × 时长
覆盖跨度 = max(freq + bw) − min(freq)      ← fxcorr-sim 的 deriveGrid
```

**不是实际被读的带宽**——band 之间的间隙照样生成网格点，只是没有任何站去读它。

以 4 站 t25362 参数为例（`fxcorr/data-volume.md` §4）：

| | 跨度 | 速率 | 1 小时 | 1.024 s batch | 利用率 |
|---|---|---|---|---|---|
| 现状（覆盖跨度） | 7072 MHz | 56.6 GB/s | 203.7 TB | **57.9 GB** | **14.5%** |
| 按频段组分片生成 | 1024 MHz | 8.2 GB/s | 29.5 TB | 8.4 GB | 100% |
| 轻量模式（不生成） | — | 0 | 0 | 0 | — |

**85% 是白生成的**——这是这个目录最大的特征，也是它成为部署第一约束的原因。

> **数值权威在 `fxcorr/data-volume.md` §4**，上表是导读副本；S0 实测后整体替换时以那边为准。
> 相关的跨目录结论（占比、瓶颈排序、总盘）也在那里——这里只讲"这个目录由什么决定"。

## 压缩手段（按代价递增）

| 手段 | 效果 | 代价 | 状态 |
|---|---|---|---|
| 缩小观测的频段跨度 | 与跨度成正比（6.9×） | **零**——改观测配置 | `../config/` 的 mini 变体 |
| 按频段组分片生成 | 只覆盖实际带宽（6.9×） | 中——改 D15 落盘布局 | ⚠ 未实施（`fxcorr/v6-plan.md` S5.2） |
| 轻量模式完全不生成 | 归零 | 中——载荷改伪随机，不能用于正确性对拍 | ⚠ 未实施（`fxcorr/v6-plan.md` S2） |

## 相关

- 规范：`fxcorr/data-spec.md` 5.8
- 生成器架构：`fxcorr/fxcorr-sim-arch.md`
- 体积公式与杠杆：`fxcorr/data-volume.md` §1.3、§4、§6
