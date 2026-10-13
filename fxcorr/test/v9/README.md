# v9 标定资产（阶段 3，判据 8）

**最后更新**：2026-10-13（**新建**——A 段：测试机小样本预标定与 `bench_pair.sh` 固化；
B 段：p419 真实规模标定待做，见末节）

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

## B 段待做（p419 真实规模）

p419-n1 登录节点维护中（2026-10-13，"System is booting up"拒连）；恢复后：

1. **环境**：rsync 源码上 `/public/home/cstu0036/fxcorr/src`（两级 ProxyJump），
   `build.sh` 重建——**注意 build.sh 的 `--doonly` 列表是 V7 时代的**，需加
   `fxcorr-fp` 与 `fxcorr-xp`（install-difx 已注册两者）。
2. **样本**：workdir-b0512ms / workdir-b1024ms 的一个完整批（4 站 × 多 ds 级）。
3. **两链 A/B**：`bench_pair.sh <workdir> <batch>` 各跑一遍（真实规模）。
4. **fp 单任务 WALL** vs f 的 6.64 s（OMP=1）/ 3.05 s（OMP=8）记录值。
5. **xp 线程数扫描**：OMP = 1/2/4/8/16（8 ds/组时 ds 并行的上限与 XMAC 并行的
   竞争关系）。
6. **中间 I/O**：fengine 读写耗时与体积（§7 的 502 MiB → 31.4 MiB 档对账；
   注意冷/热缓存口径）。
7. **汇总 vs `v9-plan.md` §7 表**，不符项归因；结果回填本节与
   `applications/fxcorr-xp/CLAUDE.md`。
