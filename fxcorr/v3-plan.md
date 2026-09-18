# fxcorr V3 计划

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
- ⚠ **对拍未归零，未决**：t25362 仍有 576 条差异（积分 0、4、10 各 192 条，全在 BA）。逐 ds 统计表明积分 4/10 的差异**几乎全部来自 ds_2**（该 ds 在积分 4 有 459 帧、积分 10 有 1135 帧被标无效，其余 ds 只有 73–75 帧）——即 **fxcorr 把 filler 占用的时间槽判为无效，mpifxcorr 的基准不判**。这是 C 类 filler 的语义差异、不是 B5；两条出路（认可 filler 无数据应判无效、改用 L1/L2 判据验收；或查 `mpifxcorr` 的 `VDIFDataStream` 为何不做该判定）留待定。

## 验收标准

1. 多线程（OMP_NUM_THREADS=N）SWIN 与单线程结果 cmp_swin.py 全等（并行不改变单基线浮点运算顺序，逐位一致）
2. 未设 OMP_NUM_THREADS 时行为与 V2 完全一致（回归 6/6）
3. 加速比测试（测试机 8 核，fxcorr-sim 大数据量场景）：f/x 各自提速且不劣化
4. 文档同步（usage.md 环境变量、各 CLAUDE.md）
