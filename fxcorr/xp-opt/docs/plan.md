# 单任务优化路线（2026-11-15 重写）

## 目标与口径

- **单任务** = 一个 (batch, ds 组)：一个 xpp 进程处理一个 ds 组，写 `.part`。
- 指标：`T_xp(OMP=1, 同组) / T_xpp(P, 同组)`；P ≤ 16；绑核固定单 NUMA 节点。
- 正确性：`.part` md5 与 xp 逐字节一致（SWIN 由 merge 写出后再核对）。
- 多进程 × 多线程的组合与任务调度归**计算框架**；本目录只保证单任务在给定 P 下
  尽可能快。

## 已确认的结构事实（4 站 1.024 s）

| 阶段 | 并行来源 | 单任务可并行度 | 备注 |
|---|---|---|---|
| station | 每个 datastream | 组内 ds 数（4 站 = 8） | P=8 实测约 7.5× |
| xmac | 已用频率 × xmac stride | **1**（每组一个频段） | 旧实现单任务内串行；Step 2 改为 (baseline, pol) 任务 |
| wacc | 每条 baseline | 6（4 站） | 待扩到 (baseline, freq) |

## 阶段

1. **Step 1 标定**（完成）：xp OMP=1 与 xpp P=1/2/4/8/16 的单组基线。
2. **Step 2 单任务并行**（进行中）：xmac 按 (baseline, pol) 任务并行；station
   紧凑调度。P=8 的 wall 已从 12.7 s 降到 8.6 s（相对 xp1 5.80×）。
3. **Step 3 xmac 内核**（完成）：诊断确认任务数 192、串行前导≈0、频率
   3.0 GHz，扩展不足来自访存；AVX2 两种实现（target 克隆、整文件 -mavx2）
   在整程序里都无收益（P=1 反而更慢），**已回滚**。保留 `spectra()`/
   `numChannels()` 外提。下一步转向减少访存冗余（任务粒度/数据复用）。
4. **Step 4 station 内部**（完成）：process 占 station 85%；FFT/unpack/pcal
   22%，其余是每 (block, band) 的小循环。**Step 5**：库新增
   `Mode::enableBlockPcal`（只停放 pcal），xpp 串行路径改为 ac/权重直累加、
   pcal 仍按块序归约；`.part`/`pcal.bin`/PCAL 文本与 xp 逐字节一致，
   station P=8 5.25→4.12 s、内存 396→90 MiB。
5. **Step 5 参数表**（已完成）：`docs/parameters.md`——difx/p419 两套环境、
   4 站 1.024 s 的 P 推荐与分阶段预期；其他站数/批长待补。

## 判据与纪律

- 每个变体：`.part` md5 必须等于 xp（4 站 1.024 s g0：
  `56039479bdd341ef3072118c82019748`）。
- xmac 单次波动约 ±5–10%，比较用 2 轮均值。
- 正式结果用 `--formal`；一次性探针留仓库外临时区。

## 环境覆盖

- `ssh difx`：单任务优化与数据已完成（本文 Step 1–5）。
- `ssh p419-n0`：**已完成**（h04r4n03，Hygon 30 核；宿主二进制 + `env.sh`，
  fengine 由 fp 现场生成）。P=8 相对 xp1 为 7.53×，分阶段表见
  `docs/log.md` 与 `results/README.md`。

## 当前状态

- 4 站 1.024 s、g=0..3 正式矩阵（2 轮均值，含 wacc 优化）：xp1 48.71 s；
  xpp P=1/2/4/8 为 38.73/22.15/12.25/7.05 s，相对 xp1
  1.26×/2.20×/3.98×/6.91×。P=8 阶段：station 4.04、xmac 2.32、wacc 0.17 s。
- 下一步：station 的 process 仍占 85%（per-band 拷贝/旋转/自相关的融合）；
  其次 xmac 的访存复用与 wacc；p419-n0 的复测待补。
