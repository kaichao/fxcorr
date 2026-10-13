# v9 标定资产（阶段 3，判据 8）

**最后更新**：2026-10-13（**A/B 两段完成**——A 段：测试机小样本预标定与 `bench_pair.sh`
固化；B 段：p419 真实规模实录见下节）

v9 两链（v8：fxcorr-f + fxcorr-x；v9：fxcorr-fp + fxcorr-xp）的**工具级性能标定**
（`fxcorr/v9-plan.md` 阶段 3 / §7）。判据是"实测 vs §7 表"：fp/xp 计时分层、
xp 线程数扫描、中间 I/O、两链 A/B。

## `bench_pair.sh`

```
bench_pair.sh <workdir> <batch_id> [outdir]
env: OMP_NUM_THREADS（两链共用，默认 1——xp 的站级 ds 并行与 XMAC 并行共用这个域，
     线程数扫描就扫它）
```

- **破坏性**：每链开跑前清 `fengine` / `vis-parts` / `vis`——只能在专用于标定的
  workdir 上跑（判据对拍请用各自的副本工作流）。
- 站/ds 任务列表从 `raw/<station>/<station>_<batch>[_ds<N>].vdif` 推导；组数从
  `fxcorr-x` 的 `FXCORR_X_GROUPS_ONLY=1` 真值出口推导（注意该输出走 stderr，
  FXLOG 家族，须 `2>&1` 合并）。
- 每任务采集 `/usr/bin/time -f` 的 WALL/峰值 RSS 与 `FXCORR_TIMING=1` 的 TIMING
  分段行，落 `<outdir>/times-{v8,v9}.txt` / `timing-{v8,v9}.txt` /
  `fengine-{v8,v9}.txt`（重复跑累积，取差值用）。

## 现场部署（`build.sh` + `env.sh`）

自 2026-10-13 p419 重建版入仓（与现场同内容）。用法：仓库内容 rsync 到目标机（约定
`$HOME/fxcorr`），两脚本拷源码树根后：

```sh
cd ~/fxcorr && bash build.sh     # 六个库 + f/fp/x/xp/sim → install/bin
source ~/fxcorr/env.sh           # PATH / LD_LIBRARY_PATH
```

内容 = `build.md` 的八坑 + V9 轮第 9/10 坑（vdifio 的 `$(FFTW_CFLAGS)` 上游笔误、
`PKG_CONFIG_PATH` 缺 `$DIFXROOT/lib/pkgconfig`）；`--doonly` 含 `fxcorr-fp,fxcorr-xp`。

## A 段记录（2026-10-13，测试机，mds 副本 4 ds 小样本）

小样本（每任务秒级以下）只作**形状参考**：流程验证、计时分层核对、缩放趋势；
**判据 8 的结论以 B 段真实规模为准**。两链均 `OMP_NUM_THREADS=1`。

| 任务 | WALL | 峰值 RSS | TIMING 要点 |
|---|---|---|---|
| v8 f（单 ds，×4 站 ds） | 0.26–0.28 s | 43 MB | proc≈0.18–0.24（四段总） |
| v8 x（组 0） | 0.18 s | 72 MB | read=0.081、xmac=0.073 |
| v9 fp（单 ds，×4） | 0.01–0.02 s | 6.5 MB | read≈0.0014（**页缓存热**，冷读另计） |
| v9 xp（组 0） | 0.97–1.16 s | 221 MB | station=0.87（= 4×f 的 proc，串行 4 ds）、xmac=0.073 |

- fengine 目录：v8 295.8 MB → v9 17.1 MB（**17.3×**，与阶段 2 的 pcal 样本吻合）。
- **xp 的 OMP 缩放形状**（4 ds、OMP=1/2/4）：total 0.96 → 0.53 → 0.32 s；
  `station` 0.85 → 0.44 → 0.26（随 ds 并行，4 ds 上限 4×）；`xmac` 0.073 → 0.046 →
  0.024（随 (freq,pass) 并行——两段共享 OMP 域，最优线程数取决于两段的平衡，
  B 段扫描要量的就是这个）。
- **A 段抓出并修复一个计时 bug**：xp 的 `zero` lap 原用跨站级段的同一个 timer，
  站级耗时被计入 zero（显示 0.897 s）——改用独立 timer 后 0.00038 s（v8 x 的
  0.00036 量级一致）；修复只动计时、`.part` 逐字节不变。

## B 段实录（2026-10-13，p419 h04r4n03）

**环境**：p419 计算节点 h04r4n03（Slurm job 22388340，30 CPU / 110.6 GiB cgroup；共享盘
ParaStor，/work2 40T）；账号 cstu0030（入口 `ssh p419-n0`，本机别名同）。源码 rsync 自
mac（含 fp/xp），现场一键 `build.sh` 构建到 `~/fxcorr/install`（照 `build.md` 八坑重建
+ 本轮两坑，见 `build.md` 追加节）。工具直接读共享盘（route=0 语义）——**该 workdir 是
route=1 数据**，根记录（`meta/roots/<batch>.json` 记 `/tmp/fxcorr/raw`）与共享盘解析冲突，
按官方出路"remove the record"备份（`.bak-route1`）后删除批 00000001/00000002 的记录再跑
（app 侧 prep 每次会重写，长期无损）。**原始数据**：本目录 `b-p419/`（现场 outdir 的
txt 全量；`times-v8.txt` 已剔除首次失败跑残留的一行）。

**样本**：workdir-b0512ms 批 00000001（4 站 BA/BX/S6/SX × 8 ds、0.512 s = 100×5.12 ms、
4 组）。**口径**：全程 OMP=1 串行逐任务（线程扫描节除外）；**页缓存命中**（见下）。

### 两链 A/B（每任务/每组的 WALL 与 TIMING 分段）

| | v8（f / x） | v9（fp / xp） |
|---|---|---|
| WALL | f **8.4–8.6 s**（32 任务）；x **24.2–30.4 s**（4 组） | fp **0.07–0.2 s**；xp **68.8–69.9 s** |
| 分解 | f = proc 5.41 + write 2.05 + reduce 0.94 + read 0.02；x = read 7.9–13.9 + xmac 13.1 + wacc 2.1 | fp = init + read 0.03 + write 0.016；xp = **station 52.0–53.1** + xmac 13.8 + wacc 2.2 + read 0.3 |
| 峰值 RSS | f 48–50 MB；x 89 MB | fp **3.5 MB**；xp **400–417 MB**（8 ds/组） |
| 批总量（串行） | 32 f + 4 x ≈ **381 s** | 32 fp + 4 xp ≈ **280 s**（**−26%**） |
| fengine/批 | **33.77 GB** | **2.135 GB**（**15.8×**） |

### 线程扫描（单组 grp0）

- **xp**（OMP 1/2/4/8/16，total）：69.46 → 42.30 → 29.31 → **15.59** → 16.10 s；
  `station` 52.56 → 26.64 → 14.12 → 7.39 → 7.44（**完美 2× 阶梯到 8 = ds 数上限**）；
  `xmac` 13.85/13.81/13.90 → 7.11 → 7.51（**1–4 平坦、8 线程才分**）。
  **拐点 = OMP 8**（8 ds/组）。
- **x 对照**（OMP 1/2/4/8，total）：27.5 → 20.3 → 17.6 → 12.9 s；`read` 11.2 → 5.5 → 3.4
  → 1.4（随线程**线性**，多线程读谱有效）；`xmac` 13.15/13.22/13.20 → 10.68（同"≥8 才分"）。

### 结构事实（对设计账的实测验证）

- **站级代价守恒**：xp `station` 52.0 s ≈ 8 ×（f 的 proc 5.41 + reduce 0.94）= **50.8 s** ✅
  ——同一份 `Mode::process`，从 f 串行版（proc+归约段）搬到 xp 站级串行版的代价几乎不变。
- **xmac 两链一致**：13.1（x，文件读谱）vs 13.8（xp，MemReader 内存直取）。
- **各自最优的单组链路**：v8 = 8×f + x@OMP8 ≈ 81.7 s；v9 = 8×fp + xp@OMP8 ≈ **16.2 s**。

### 中间 I/O 与内存

- **体积**：15.8×（33.77 GB → 2.135 GB）——比率与 §7 的 16× 吻合（余差 = autocorr.bin 消失）。
- **写**：f 2.05 s/任务（32 任务合计 ≈ 66 s）→ fp 0.016 s（**≈130× 减**，写 1× stream.bin）。
- **读**：x 7.9–13.9 s/组 → xp 0.2–0.4 s/组。
- **页缓存口径**：read 恒 ~0.02–0.03 s / 0.6 GB（≈30 GB/s）——含从未跑过的批 00000002 亦然
  （节点 123 GB 内存兜得住，切片/实验轮读过的数据仍在缓存）。**真冷读在本环境测不到**；
  按历史盘速（共享盘单流 214 MB/s）折算，fp 冷读上限 ~1–3 s，**结论不变**（≪ f 的 8.6 s）。
- **SATA/tmpfs 约束解除**（§7 预期证实）；raw 读量 1× 不变（共享盘供给瓶颈不解除，§5.1 保持）。

### vs §7 表

- **比率全部吻合**：15.8× ≈ 16×；"f 段 DSP 消失"实测证实（8.6 → 0.07 s）；x 段含站级后
  ×2.4（结构性的"站级迁入"，见上）。
- **绝对量系统性 ~2×**（f 8.6 vs 6.64；proc 5.4 vs 3.60 s/ds；1055 vs 502 MiB/ds；
  xmac 全批 52.4 vs 21.0 s）——归因 **b0512ms 的通道/带宽配置与 §7 账目所用配置不同**
  （方向、比率一致，不做重跑归因）。
- fp 的 WALL 好于 §7 估算（0.07 vs 估 0.3–0.5 s，页缓存口径）。

### 结论（判据 8）

1. **单组串联**净收益 −26%（95 → 70 s @OMP=1，含 f 段归零与 xp 站级迁入的合账）；
   **各自最优线程**口径 81.7 → 16.2 s。
2. **线程定标**：xp 单进程最优 OMP=8（= 组内 ds 数）；x 侧同样 ≥8 线程才吃到 xmac 并行
   （1–4 线程只并行 read）——**平台并发形态（4 组 × OMP）与核数（30）的配比**留给
   app 侧编排标定，本段给单任务伸缩曲线。
3. **中间 I/O 1/16**：v8 的 16× 谱写出（2 s/任务级）被消除；**介质约束大幅缓解**。
4. 内存：xp 峰值 400–417 MB/组（8 ds）——远低于 v9-plan §7 引用的 2–3 GB 估算。
