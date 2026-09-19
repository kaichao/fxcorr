# fxcorr V3 计划

> **冻结文档**——本文记录 V3 阶段的定案与实施（P3 模块级 OpenMP、P12 真实数据病态处理），
> **截止 2026-09-19，此后未再同步**。当前状态见 `v4-plan.md`（版本路线与 V4 结论）、
> `fxcorr/CLAUDE.md`（工作区现状）；P3 的完整实施记录在 `algo-plan.md` 的 P3 节。

V3 范围经 2026-09-15 讨论定案。V2 已完成（P0-P11 全绿，见 v2-plan.md）；V3 有一个实现项 **P3 进程内多线程（模块级 OpenMP）**，另在 V3 期间收尾了 **P12「真实观测的病态数据」**（2026-09-16 由真实数据暴露、2026-09-18 修完，见下节）。

## 定案决策（2026-09-15）

1. **并行维度 = 时间**："时间片"就是 batch——batch 即并行/调度单元，多 batch 并发由 scalebox 编排承担（编排外置，不在本项目）；无空间维度切分（原 V2 P2 多 x 子集/基线切分取消，见 algo-plan P2 节）。
2. **路线 B：同实验 batch 串行**——同一实验的 batch 按时间序串行处理，实验级共享文件（PCAL_* 读改写、SWITCHEDPOWER_* 追加、SWIN 跨 batch 追加）不存在并发写点，本项目零改动；同实验多 batch 并行（积压追赶）属 scalebox 编排层职责。
3. **P3 实施**：f 侧 OpenMP FFT 批并行 + x 侧基线循环并行（设计见 algo-plan P3 节）；线程数 OMP_NUM_THREADS，未设 = 串行（默认行为与 V2 一致）。
4. **P5 不做**：网络输入/数据流化留到 scalebox 阶段（algo-plan P5 节）。
5. **GPU**：按需，默认不做。

## 实施步骤

1. ✅ 文档定案修正（2026-09-15）：data-spec/CLAUDE/usage/README/algo-plan/v2-plan/v1-plan 清理 station_groups 与旧空间切分表述（P2 取消、P5 不做、路线 B、任务粒度）
2. ✅ P3 前置：性能剖析（测试机）——f/x 热点确认与单线程基线耗时（test/p3/README.md；f = 条纹旋转 > FFT > 写盘，x = 读盘与 XMAC 对半）
3. ✅ P3 设计定稿（algo-plan P3 节）：f = Mode 副本 + 连续段分块 + 块序归约（fxcorrcommon 仅加归约接口）；x = 基线循环并行 + scratch 线程私有化；OMP_NUM_THREADS 未设 = 串行
4. ✅ x 侧实现（2026-09-15）：xmacBatch/uvshiftAndAverage/accumulateWeights 基线循环并行 + 6 块 scratch [nthreads] 副本；串行回归 580/580、4 线程 580/580 逐位全等；4/8 线程 1.76×/2.09×（4MHz 配置读盘主导）
5. ✅ f 侧实现（2026-09-15）：Mode 副本 + subint 级并行区 + 块序归约；串行与 4 线程全链路 580/580 逐位全等（4MHz 配置写盘 ~40% 串行、加速有限，放大场景为准）
6. ✅ 验证与文档收尾（2026-09-15）：放大场景（8 站 28 基线 + nChan 32768、61s batch）对拍 4/8 线程全链路 SWIN 2088/2088 逐位全等、默认串行回归 580/580；加速比 f 2.2×（4 线程见顶，写盘 I/O 主导）、x 2.9×（8 线程，读盘 I/O 主导）——正确性全达标，进一步提速需异步 I/O（P3 范围外，algo-plan P3 实施记录）；usage.md/CLAUDE.md/algo-plan/test-p3-README 已同步

## P12：真实观测的病态数据（2026-09-16 ~ 09-18 收尾）

首个真实 VGOS 观测 t25362 暴露的读取路径缺陷：文件起点偏移（BA 晚 79.3 ms）、记录中断（每 ds 7–8 处、合计 144–148 帧／12 秒）、filler 占位帧（ds_2 有 1162 帧）。**非上游审查发现项**——P0-P11 的对拍全用 fxcorr-sim 理想数据，这三类形态从未被覆盖。**分析与诊断判据见 `fxcorr/reader-model.md`，动机与实施记录见 `algo-plan.md` P12 节。**

- ✅ A 起点（3 条）/ B 缺口（5 条）/ C filler（6 条）共 14 条全部修完；最后的 B5「缺口跨 subint 边界时修正超前」2026-09-18 修好——读取位置按缺口的时间轴位置过滤（`gapspan` / `gapshiftAt`），`shiftFrameGaps` 的起始槽由帧号偏移决定。
- ✅ 验收：`fxcorr/test/gaps/run_boundary.sh` 两个场景（跨边界不盖起点 / 盖住起点）在修前报红、修后转绿；`fxcorr/test/gaps/` 的 T1/T2 计数判据不变（73/73 与 73/0）；无缺口数据产物**逐字节不变**（cmp5 md5 相同，S6 与全部合成数据对拍依赖此点）。
- ✅ 真实数据（`ssh difx`，工作目录 `/data/scalebox/t25362work`）：BA ds_0 的 `READPOS firstfno` 由 4026 回到 **4098**（该 subint 起点应对应的帧号）；无效块落在正确的 subint（835 标缺口 1、2，836 标缺口 3、4，与外部帧号扫描的真实缺口位置吻合）。
- ✅ **过渡缓冲区缺陷已修（2026-09-18）**：t25362 的 576 条差异定位到 fxcorr 在 filler 检测过渡区的两处缺陷——① `gapshiftAt` 把缺口文件偏移换算成时间槽时漏扣缺口之前的 filler（长 filler 段之后的缺口一律晚 `filler_before` 个槽才生效）；② 跳过的区段在读取**之后**才补扫，而段里的缺口会缩短正要用的那个位置。修法：`gapspan` 每项记 `fillerbefore`；补扫提为 `scanSkippedStretch`，`readSubint` 读取前循环「算位置 → 扫到位 → 重算」至收敛。效果：ds_2 的无效块 7170→**1749**（积分 4）、17720→**1734**（积分 10），真值 70/73，ds_0 参照 1129/1142；判据 `fxcorr/test/gaps/run_filler.sh`（长 filler 合成，修前红 1071 对 169、修后绿）。详见 `reader-model.md` 4.6/4.7。
- ⚠ **对拍仍未归零，但方向已翻转、改由基准主导**：ds_2 相关记录 fxcorr 0.993104 对基准 0.975402（积分 4）、0.993146 对 0.980038（积分 10）——基准自身偏低 1.8%/1.3%，而真值只需偏低 0.43%。原因是 mpifxcorr 的 `vdifmux` 把 filler 字节滑过丢弃、输出短于输入，而 `DATA FORMAT: VDIF` 下其块有效性是纯字节数判据（不查 invalid 位），受影响 subint 的尾部块被标无效。**故 t25362 不能用 `cmp_swin.py` 全等验收**。
- ✅ **真值对账工具已补，残留定性完成（2026-09-19）**：`fxcorr/test/reader/`（`file_truth.py` + `check_reader.py`）——从 VDIF 本身算"哪些时间槽有数据"（缺口与 filler 统一为"槽未被占用"），对账 fxcorr 的 `READPOS`/`GAPCHECK holes`，四条断言 E1 定位 / E2 数据 / E3 落点 / E4 覆盖。结论：残留是**真损失 80 帧**（batch 内 0.044%，三处 24/18/38），不是标记口径——**根因是定位读按字节读一段连续区域，本 subint 被 filler 隔开的数据落在窗口之外**（上游 `vdifmux` 顺序读会滑过 filler 继续填，没有这个形态）；ds_0 对照（无 filler）2181 subint 零 finding、E4 = 0，判据不误报。**修复待定**：改 `readSubint` 让读取跨过 filler 填满缓冲区，改前先补合成复现（现有 `FXSIM_GAPS` 场景 E4 为 0，还没覆盖这个形态）。详见 `reader-model.md` 4.7；**后续路线、每节点验收线与规划条件评估见 `v4-plan.md` 阶段 A/B**（该项转 V4 跟踪，本文件不再更新）。

## 验收标准

1. 多线程（OMP_NUM_THREADS=N）SWIN 与单线程结果 cmp_swin.py 全等（并行不改变单基线浮点运算顺序，逐位一致）
2. 未设 OMP_NUM_THREADS 时行为与 V2 完全一致（回归 6/6）
3. 加速比测试（测试机 8 核，fxcorr-sim 大数据量场景）：f/x 各自提速且不劣化
4. 文档同步（usage.md 环境变量、各 CLAUDE.md）
