# fxcorr V3 计划

V3 范围经 2026-09-15 讨论定案。V2 已完成（P0-P11 全绿，见 v2-plan.md；另有 **P12「真实观测的病态数据」**2026-09-16 起由真实数据暴露、部分完成，见 algo-plan P12 节）；V3 只有一个实现项：**P3 进程内多线程（模块级 OpenMP）**。

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

## 验收标准

1. 多线程（OMP_NUM_THREADS=N）SWIN 与单线程结果 cmp_swin.py 全等（并行不改变单基线浮点运算顺序，逐位一致）
2. 未设 OMP_NUM_THREADS 时行为与 V2 完全一致（回归 6/6）
3. 加速比测试（测试机 8 核，fxcorr-sim 大数据量场景）：f/x 各自提速且不劣化
4. 文档同步（usage.md 环境变量、各 CLAUDE.md）
