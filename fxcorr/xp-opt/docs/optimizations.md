# xpp 优化项与效果（相对单线程 xp）

**口径**：单任务 = (batch `00000001`, ds 组 0)，4 站 1.024 s、8 ds；xpp 分片写
`.part`；参照 = `fxcorr-xp OMP=1` 同一组：**wall 50.13 s**（station 40.75、
xmac 6.76、wacc 1.52）。xpp 全部 run 用 cpuset 绑单 NUMA 节点。
`.part` md5 全程为 `56039479bdd341ef3072118c82019748`。

**环境覆盖**：

| 环境 | 状态 | 说明 |
|---|---|---|
| `ssh difx`（gjnode5，2×96 物理核、2 NUMA） | **已完成** | 本文全部数据来自该机 |
| `ssh p419-n0`（h04r4n03，Hygon 30 核） | **已完成** | 宿主二进制 + `env.sh`；数据 `workdir-b1024ms`，fengine 由 fp 生成 |

两套环境的数据**分开记录、不混用**。p419-n0 结果（g=0..3、2 轮均值，s）：

| 实现 | P | wall | station | read | xmac | wacc | 相对 xp1 |
|---|---|---|---|---|---|---|---|
| xp | 1 | 139.81 | 104.12 | 2.46 | 27.73 | 4.44 | 1.00× |
| xpp | 1 | 119.78 | 91.07 | 0.49 | 26.80 | 0.58 | 1.17× |
| xpp | 2 | 61.59 | 46.05 | 0.29 | 14.59 | 0.32 | 2.27× |
| xpp | 4 | 32.55 | 23.43 | 0.29 | 8.32 | 0.18 | 4.30× |
| xpp | 8 | 18.57 | 12.10 | 0.29 | 5.69 | 0.16 | 7.53× |

p419 的 xmac 扩展好于 difx（27.7→5.7 s，4.9×）；xp 的 `read` 偏高是页缓存
冷/热顺序造成的，其余阶段可直接比。

**正式矩阵（2026-11-15，difx，含 wacc）**：镜像 `sha256:cdccaed4…`；4 站 1.024 s、
batch `00000001`、ds 组 0–3；xp OMP=1 参照，xpp P=1/2/4/8；每档 2 轮。
各组 `.part` md5：g0 `56039479…`、g1 `16ea34aa…`、g2 `3b601e13…`、
g3 `3ed2d79d…`，xp 与 xpp 全部相同。结果目录 `results/20261115/mt-*`，
汇总 `results/summary.tsv`。

## 一览

| # | 优化项 | 状态 | 单任务效果（g=0） |
|---|---|---|---|
| 1 | **线程绑核**（cpuset 单节点 + close） | 采用 | 同 OMP=4 的 xmac：不绑 31–37 s → 16.9 s；`close` 18.9 vs `spread` 34.6 s |
| 2 | **xmac 循环交换** | 采用 | xmac P=1 6.76→5.85 s（−13%）；P=2 21.51→17.27 s（全批口径 −20%） |
| 3 | **station 紧凑调度** | 采用 | P=16 wall 24.85→9.71 s；P=8 station 6.47→5.18 s |
| 4 | **xmac 按 (baseline, pol) 任务并行** | 采用 | xmac P=8 4.90→2.40 s（2.0×）；P=4 5.77→3.37 s（1.7×） |
| 4b | **wacc 预计算描述符** | 采用 | wacc P=1 1.35→0.22 s；P=8 0.39→0.17 s（正式均值）；`.part` md5 不变 |
| 5 | **ac/权重直累加 + pcal 单独停放** | 采用 | P=1 station 39.0→31.8 s（−19%）；P=8 station 5.18→4.03 s（正式）；内存 396→90 MiB |
| 6 | AVX2（`FXCORR_XPP_SIMD`） | 试过，已回滚 | 无收益：P=1 反而慢 19%，P=8 +4%（噪声）；两种实现一致 |
| 6b | process 旋转+共轭+自相关融合 | 试过，已回滚 | 标量融合比库内向量化慢：P=1 process 27.9→31.7 s，P=8 station 4.12→4.71 s |
| 6c | FFTW 直写目标数组（去掉 in/out memcpy） | 试过，已回滚 | 位级一致但更慢：P=1 process 27.9→28.2 s，P=8 station 4.12→4.19 s |
| 7 | band 并行 | 未采用 | 劣化 4.5×（413 s vs 串行 91.9 s，2 站全批） |
| 8 | ds×block 并行 | 未采用 | 未完成逐字节验证，默认关闭 |
| 9 | 单进程 P>16 | 不做 | 多进程 × 多线程组合归计算框架 |

## 累计效果（正式矩阵，4 组均值）

| 配置 | wall (s) | station | read | xmac | wacc | 相对 xp1 |
|---|---|---|---|---|---|---|
| xp OMP=1 | 48.71 | 39.74 | 0.29 | 6.54 | 1.50 | 1.00× |
| xpp P=1 | 38.73 | 31.73 | 0.20 | 6.30 | 0.22 | 1.26× |
| xpp P=2 | 22.14 | 16.02 | 0.19 | 5.44 | 0.18 | 2.20× |
| xpp P=4 | 12.25 | 8.03 | 0.20 | 3.53 | 0.18 | 3.98× |
| xpp P=8 | 7.05 | 4.04 | 0.20 | 2.32 | 0.17 | 6.91× |

分阶段（g=0 单组，临时口径）：Step 1 绑核+循环交换 P=8 12.68 s；
Step 2 station 调度 + xmac 任务并行 P=8 8.64 s；Step 5 直累加 + pcal 停放
P=8 约 7.4 s，正式矩阵为 7.30 s。

## 各项说明

### 1. 线程绑核

libgomp 默认不绑核，线程可跨两个 NUMA 节点；同 OMP=4 的 xmac 因落点不同可差
1.8×。`run_xpp.sh` 默认 `--bind close --cpuset auto`（OMP≤96 时 cpuset
0..P-1、mems=0），并记录到 manifest/metrics。`run_xp.sh` 不加绑定，保持参照原样。

### 2. xmac 循环交换

`XmacEngine::xmacBatchPeriod`：pass 放外层、fftloop 放内层，每个 uvshift 周期
只建一次并行区；`resultindex` 每 fftloop 重置，累加顺序不变。单线程下
xmac 6.76→5.85 s；全批 OMP=2 口径 21.51→17.27 s。SWIN/`.part` 逐字节一致。

### 3. station 紧凑调度

shard 模式下原 `schedule(dynamic)` 在 32 个迭代（24 个立即返回）上分配不均，
OMP=16 比 OMP=8 慢 2.4×。改为先构造 active ds 紧凑列表、`schedule(static)`：
P=16 wall 24.85→9.71 s，P=8 station 6.47→5.18 s。

### 4. xmac 按 (baseline, pol) 任务并行

单组的 xmac 并行度只有"已用频率"（本数据 1 个 pass），原实现单任务内是串行的。
改为每个 pass 拆成 (baseline × pol product) 任务，预计算每个任务的
`threadcrosscorrs` 偏移；每个累加器的加法顺序不变。P=8 时 xmac
4.90→2.40 s；P=4 5.77→3.37 s，`.part` 逐字节一致。

### 5. ac/权重直累加 + pcal 单独停放

旧 xpp 串行也启用 per-block 累加器：每 block 清零并写块槽（约 2.05M block ×
8 band），process 33.3 s、归约另 3.84 s。改动：

- fxcorrcommon 新增 `Mode::enableBlockPcal()`（只分配 pcal 的每块停放槽），
  `clearBlockAccumulators` 让 ac 与 pcal 各自按槽数清零；默认路径不变。
- xpp 串行路径不启用 ac/权重块累加器，直接按块序累加到 `autocorrelations`/
  `weights`（等价 f 的 nthreads=1）；pcal 仍按块停放并归约，保持与 xp 相同的
  加法顺序。

效果：P=1 station 39.0→31.8 s（process 33.3→27.9、归约 3.84→2.02），
P=8 station 5.18→4.12 s；峰值内存 396→90 MiB。`.part`、`pcal.bin`（8 个）、
PCAL 文本（8 个）与 xp **逐字节一致**。

更激进的"pcal 也直累加"版本 P=8 wall 6.65 s，但 pcal.bin 与 xp 不一致，已放弃。

### 6. AVX2（未采用）

- 两种实现（`target("avx2")` 克隆、整文件 `-mavx2`）都确认生成 256 位代码；
- 整程序 P=1 xmac 7.23–7.45 s（比 SSE 6.25 s 慢），P=8 2.20–2.37 s
  （对 SSE 2.27–2.31 s 无实质收益）；
- 微基准显示 kernel 本身 8 线程也只有 3.9×（SSE）/5.5×（AVX2），撞内存墙；
- 已回滚 AVX2 路径与开关，保留 `spectra()`/`numChannels()` 循环外提。

### 7/8. 未采用的并行层次

- band 并行：任务粒度是每 block 每 band，2 站 1.024 s 下十万级提交，
  wall 413 s vs 串行 91.9 s，同步成本压过计算；
- ds×block 并行：大粒度版已编译但逐字节验证与性能测试未完成，默认关闭。

详见 `docs/log.md` 的 Step 1–5 与 `docs/archive/`。
