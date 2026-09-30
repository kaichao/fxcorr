# fxcorr 改造工作区

**最后更新**：2026-09-30（**V7 P6 完成**——Singularity 容器与裸机的**同条件对照**（同 batch / 同 raw / 同并行参数 / fengine 都落 tmpfs；镜像内脚本与集群源码 md5 全同，不踩"镜像旧"的坑）：**wall overhead ≈0**（容器 1m46.3s vs 裸机 1m49.9s，隔离轮 1m48.2s）、**容器内 `fengine` 落宿主 tmpfs** 经 `df` 与探针证实、**记录集合 1024/1024 按 key 全 match**；但**原判据"SWIN 逐字节相同"被实测推翻**——容器与裸机仅 **24.78%** 元素逐位相同（最大绝对误差 2.38e-06 ≈ 2 ULP）。**根因不是容器运行时，是镜像与宿主的发行版不同**：宿主 Rocky 的 `/lib64/libm.so.6`/`libstdc++`/`libgomp` 一套基础库 vs 镜像 Ubuntu 的 `/lib/x86_64-linux-gnu/` 另一套（FFTW 也不同）——**隔离实验（容器壳 + 宿主二进制 + 宿主库）把误差降到 1.43e-06，证明容器运行时本身零贡献**，而**同环境重复跑 262144/262144 逐位相同**。判据据此订正为**"记录集合相同 + 最大绝对误差在额定容差内"**，并落档生产含义（容器与宿主混用会引入 ULP 级不一致）。顺带清三处文档债：`run_batch_singularity.sh` 的过期注释（镜像里其实有 `run_batch.sh`/`fxinput.py`/`make_testdata.sh`）、§17.2 的过期"待修"标题（落盘校验 P3 已实施并验证）、`:799` 引用的 §17.3 悬空引用（现写成实测）。**此前**（**V7 P4 完成**——目标集群单节点（30 核）上 mpifxcorr 与 fxcorr 的对照：4 站 mpifxcorr wall **13m09s** / CPU **23577 s** / 内存峰值 **13.6 GB**（34 rank），2 站 wall 4m07s / CPU 4476 s / 6.8 GB；两侧 SWIN 按 key 对拍 **2 站 256/256、4 站 1024/1024** 全等；fxcorr 并行版（分片 + `merge --experiment`）**1m31s**，比同配置串行 **快 4.4×**。**顺带修掉两个静默错数据缺陷**：`run_bench.sh` 重排 `FILE` 行时写死空格数——两位数 fileindex 的行值被推到第 22 列，而 mpifxcorr 按**固定第 20 列**取值，取到带前导空格的路径 → 打不开文件、**只算前 10 个 datastream 且退出码仍是 0**；`wrap_vex2difx.sh` 漏规范化 `CORE CONF FILENAME`——该字段由 mpifxcorr 按 **cwd** 解析，workdir 一搬就断，并触发上游 `mpiGetFileContent` 的 manager/从 rank 不对称缺陷（`.calc` 与 `.im` 内容互换）。**此前**（**V7 P3 主体完成**——按 ds 组并行调度（`FXCORR_PARALLEL` / `FXCORR_GROUP_JOBS` / `FXCORR_PURGE_FENGINE`）、`fxcorr-f` 落盘校验、PCAL 按 ds 独立文件；A 矩阵 `3×8×OMP1` 比串行快 **8.4×**（104 s vs 873 s），且**多任务单线程优于少任务多线程**（E 是 135 s、总 CPU 多用 2/3）。**顺带修掉一个真 bug**：`fxcorr-f` 的 OpenMP 归约原本**不逐位可复现**（`autocorr.bin` 在 OMP 1 与 8 下 **85% 元素不同**，而 `.sp` 相同），根因是归约型产物被按"副本段和"相加，改为 `Mode` 的 per-block 累加器 + 按块序归约；**判据订正**：P3 门禁从"逐字节相同"改为"**记录集合相同**"（分片+merge 与全量的记录顺序本就不一一对应，`cmp_swin.py` 默认按 key）。见 `v7-plan.md` §10.2 / §5.1 / §17 / §19。**此前 2026-09-29**（**V7 P3 起步**——动并行调度之前先把编排脚本里的"逻辑"抽出来：`run_batch.sh` 的 118 行内嵌 python 段抽成 **`fxcorr/fxinput.py`**（`.input` 解析、三条前置校验、DATA TABLE 软链、**ds 组划分**；`run_batch.sh` 315 → 211 行，只留编排），抽出前后在真实 workdir 上**逐字等价**（3 个 batch 的 `CFGIN/NGRP/OUTDIR` + 32 行站表完全一致）；给 `fxcorr-x` 加 `FXCORR_X_GROUPS_ONLY=1` 真值出口，新增对照判据 **`test/input/run_consistency.sh`**——python 侧与 C++ 侧的 ds 组划分在同一批 `.input` 上 **20/20 逐组逐成员相同**（t25362 参数配置，4 站 × 8 ds → 4 组），自检按预期报红。**此前**（同日 **V7 已开工**——在**目标集群**（Slurm，`ssh p419-n1` → `ssh -p 50022 <计算节点>`，**另一套机器**）上完成**环境建档**与 **P1 造数**：节点实测 **30 核 / 123.5 GiB / 62 GB tmpfs**，与 `v6-plan.md:234` 假定的"目标节点"**逐项命中**；共享存储 32 并发读 **4.2 GB/s**（刚好接住 4.11 的实时峰值）；造数 4 站 × 8 ds × 20 batch = **84.2 GB**，与 V6 S3 **逐项同值**。构建路径与**八个坑**见 `build.md`「目标集群构建」节。**此前 2026-09-28**（**V6 已收尾**——当日做完 S4.3 的接口侧（`data-spec` 5.7 写死"三个工具完全不碰 sqlite"）、S6.2（`run_bench.sh` 按 RAW 根定位软链，原按 workdir 拼、根一重定向就失效）、S6.4（`fxcorr-x` 的空输入/空产物防呆，挡"日志照报 N integrations written、产物却是空文件"那种静默）；**cf16 结案**并**订正一处倍数错误——8× 是抄错了杠杆 1 的数字，实际是 2×**（`data-volume` §6/§7.4 连带修正，"组合效果"表里那行整体作废）；**新增 `v7-plan.md` 草稿**。移交项见 `v6-plan.md` 末节。**此前**：**V6 主线已完成：S0–S3 与 S4.1**——**S4.1 两级 `merge` 已实施并验证**（当日）：batch 级 `merge <batch>` 只写 `vis-parts/<batch_id>/merged.part`、新增实验级 `merge --experiment` 才写 SWIN 且是唯一写入者；六条判据在测试机全过（与不分片**逐字节相同**、**乱序完成**仍时间单调、缺 batch 报错不写、`FORCE=1` 强制并列明、根不一致报错、workdir 混实验报错），见 `v6-plan.md` 验收表。**S3**：4 站 × 8 ds × **20 个连续 batch** 全链（轻量模式造数 8.6 s / 79 GB，流水线 **4945 s**、FITS 14.5 MB），与 mpifxcorr 同节点对照 wall **0.67×**、CPU **1/20**，数字落 `data-volume.md` §3.1/§3.2。**S1 与 S2 已完成**——**S1**：t25362 真实数据（2 站 16 ds、1.024 s batch）的分片全链对拍，**256/256 按 key 全匹配**、4×64 不漏不重，实测数字落 `data-volume.md` §3.1（f×16 并行 6.1 s / x 17.7 s 单线程 / 分片×4 并行 4.6 s / merge 0.019 s）；**S2**：fxcorr-sim **压力测试轻量模式** `FXSIM_LIGHT=1`——跳过整条物理链、载荷由 `(seed, station, ds)` 派生的确定性伪随机流填充，两模式共用 `VDIFWriter` 故"逐帧头一致"是**结构性保证**；t25362 配置 **0.275 s** 对完整链 **6m22s**（**1389×**），**S3 的造数瓶颈到此解除**。同日修掉 `fxcorrcommon/configure.ac` 的 `PKG_PROG_PKG_CONFIG` 回归（`if ipp_enabled` 包住文件里**首个** `PKG_CHECK_MODULES`，会让默认配置下**所有**后续探测失效，**全新构建必挂**——容器镜像与 `make clean` 后都中招）。**贯穿性回归在新构建上整轮重跑全绿**：`gaps/` 七个、`test/multids/` 11 条、`test/roots/` 三档、单测 71 + 48。**此前** 2026-09-28（**V6 S2.5 已实施：取消 D15，公共信号改为各站本地合成**——`sim-common/` 目录、`FXCORR_SIM_COMMON_ROOT` 根（可重定向根 5→4）、`fxcorr-sim common` 子命令**全部删除**；每个 `station` 任务自己跑一遍生成器、只留自己 band 那段（**算全部、留一部分**，因为公共信号是流式顺序 PRNG，做不到"只算自己那段"）。公共参数只剩 `seed`，随 `batch.json` 走——网格与 batch 的 slice 总数都是 `.input` 的纯函数，各站就地重算必然逐位相同。判据：新路径的 raw 与原落盘路径**逐字节相同（32/32）**。**顺带修掉两处上游缺陷**（都阻塞过验证）：`model.cpp` 的 `clock[j][k]` 分配器不配对（IPP 构建下 `double free`，一行改为 `vectorFree`）；`configure.ac` 的 `--disable-ipp` 不生效且默认启用 → 让 `build.md` 要求的 `--noipp` 形同虚设，**IPP 构建下仿真延迟静默错**（t25362 4 站 1.024 s batch 上是 33511 字节 / 131 MB 的低位偏差，只在延迟非零时出现）。详见 `v6-plan.md` S2.5、`data-spec` 5.8、`libraries/CLAUDE.md`。**此前** 2026-09-27（**V6 已开工**：`v6-plan.md` 的 **S0 步骤 1 完成**——`workdir-template/` 建成并验证（4 站配置的前处理链 `vex2difx`+`difxcalc` 两个变体各跑通，32 datastream / 96 baseline / **4 个 ds 组各 8 ds**）；**频率跨度订正为 7072 MHz**（配置实际值 = `max(freq+bw) − min(freq)`；原记 6816 有误、9752.4 不在配置里），`data-volume` §1.3/§3/§4/§6、`data-spec` 5.8、`v5-plan`、`v6-plan` 的派生数字（sim-common 57.9 GB/batch、56.6 GB/s、203.7 TB/h、129 GB 合计）一并重算。主题是**规模与部署**——**S0 单 batch 端到端实测**（4 站 t25362 参数仿真，两次跑：缩小跨度跑通流程 / 完整跨度拿实测数字；**当前文档里的体量全是推算，S0 后换实测**）、S1 真实规模验证（t25362 分片全链对拍）、S2 压力测试轻量模式（B2）、S3 规模验证（含 mpifxcorr 对照）、S4 编排对接（只做规范与接口：**两级 `merge` 形态 A**、`vis-parts/` 清理与迟到策略、SQLite 索引）、S5 条件触发项、S6 工程债；**多节点部署形态**（计算单元 = **(batch, ds 组)**、`fengine` → tmpfs、`FXCORR_WORKDIR` 必须全局共享）在同文档。容量与两种实现的对比数字见 `data-volume.md` §4.2 / §5.1。**分片架构前三项已全部落地**：① batch_id 8 位顺序号（`make_testdata.sh` 分配 + 重跑幂等、删"每 batch ≥ 1 s"强制提升）；② 多 datastream 生成（fxcorr-sim `ds_index`、噪声种子加 ds 维度（ds=0 的种子与改前相同，单 ds 产物逐字节不变）、输出 `raw/<st>/<st>_<bid>[_ds<N>].vdif`（后缀仅多 ds 站））；③ **x 按 ds 组的分片 + `merge` 归并**（D16 `vis-parts/`、SWIN 唯一写入者、`run_batch.sh` 的 `FXCORR_X_SHARD=1`，含"同 batch 不得混跑两种模式"的程序内检查）。判据见 `test/multids/README.md` 的 11 条**与 mpifxcorr 的多 ds 对拍 16/16**——详见各 `CLAUDE.md` 与 `v5-plan.md` P6 与末节第 6 条。**任务 ID 规范入 data-spec 第 6 节**——`<batch>-<station>-<ds>` / `<batch>-<g>` / `<batch>-merge`，其中 **`ds_group` = 跨站含全极化的"一组 ds"**（实测 `.input` 后修正，t25362 是 4 组、f 任务 16 个）；**全文目录与状态核对**：`sim-common/` 改名、`work/` 删除、补 `vis-parts/`、`beam/` 与 fengine 的 `ds_<N>/` 层；**`batch_id` 改为 8 位零填充顺序号**——名字不再承载时间信息，取消"batch 时长 ≥ 1 s"约束；位数依据、以及"band 该不该进 batch 定义"（**结论：batch 只回答"哪段时间"，频段切分在任务层**）见 `data-volume.md` §7.6。**2026-09-27 分片改造前三项已落地**：batch_id 顺序号（`make_testdata.sh` 分配 + 重跑幂等、删除"每 batch ≥ 1 s"强制提升）、多 datastream 生成（fxcorr-sim `ds_index` + 噪声种子 ds 维度）、**ds 分片 + `merge`**（fxcorr-x `ds_group`、D16 `vis-parts/`、SWIN 唯一写入者；`run_batch.sh` 的 `FXCORR_X_SHARD=1` 走分片路径）。判据见 `test/multids/README.md`；**仅 SQLite 索引未做**（scalebox 阶段），见 `v5-plan.md` 未完成第 6 条。09-26：`data-volume.md` 新增 §7「计算单元的分片」——目标机约束（30 核 / 120 GB SATA SSD / 60 GB tmpfs）与按 ds 分片 + SWIN 合并方案（合并为 **fxcorr-x 的 `merge` 子命令**）；`data-spec.md` 新增 D16 `vis-parts/`（5.9 节）。09-21：V5 P5 目录根变量化完成，验收 11/11）

本目录是 fxcorr 改造的工作区：文档 + bash 编排脚本（**脚本直接放本目录，与 README / data-spec 平级，不再设 scripts/ 子目录**）+ test/ 测试资产 + `workdir-template/` 配置骨架。算法实现见 `applications/fxcorr-f`（已建成，见其 CLAUDE.md）、`applications/fxcorr-x`（已建成，见其 CLAUDE.md），仿真数据生成器见 `applications/fxcorr-sim`（已建成，见其 CLAUDE.md；验证档案见其 VERIFICATION.md），共享代码见 `libraries/fxcorrcommon`（已建成）。

## 文档分工

- **README.md**：需求（R1-R7）、总体架构（含 mpifxcorr 改造基准三层分类与核心算法链，3.1-3.3 节）、实现阶段（V1–V5）、设计要点。改架构/需求时改它。
- **data-spec.md**：数据规范（版本 1.1）—— 目录布局、D1–D16 数据类型、模块 I/O、时间轴与通道/偏振映射、**跨目录约束**、切批约束。**2026-09-27 起第 5 节大幅下沉**：「这个目录里有什么、文件叫什么、二进制字段怎么排」移到 `workdir-template/<目录>/README.md`（第 5 节从 467 行压到 224 行，只留跨目录约束与逐目录指针）。**改数据接口时两处都要改**：格式改 README，跨目录规则改本文。
- **reader-model.md**：fxcorr-f 读取模型与读取缺陷分析——读模型对照（顺序读 vs 定位读）、缺陷根因总表（A 起点 / B 缺口 / C filler / D 有效性）、症状指纹与 `GAPCHECK`/`READPOS` 判据、改造建议。**改 datareader 前先读它**；读取相关的**分析**内容集中在此（data-spec 5.2 只留数据规格约束，`applications/fxcorr-f/CLAUDE.md` 只留目录级实现要点）。
- **v1-plan.md**：V1 实施方案——组件源码清单、core.cpp 切分落点、datareader 改造、install-difx 注册、验收标准。**已冻结**（2026-09-15）。
- **v2-plan.md**：V2 计划——定位（scalebox 编排外置）、镜像体系（fxcorr-builder/base/f/x/sim/difx-tools）、容器构建链、算法改进清单、验收标准。**已冻结**（2026-09-18）。
- **v3-plan.md**：V3 计划——定案决策（时间片 batch 并行、路线 B 串行、P3 实施、P5 不做）、P3 实施步骤与验收标准。**已冻结**（2026-09-19）。
- **v4-plan.md**：V4 计划——读取路径改进路线（阶段 A 资产补齐 / B 修 E4 窗口语义 / C 分层重构 / D 判据固化，每节点一个 commit）、验收线、规划条件评估、真实数据获取策略。**开头是「V4 结论」**（四阶段完成情况、三层判据的代价表、**读模型未解决的 5 条**与后续方向按性价比排序）。**已冻结**（2026-09-19）；末节的后续方向由 v5-plan.md 接手。
- **v5-plan.md**：V5 计划（**已收尾**，2026-09-27；当前版本见下条）——读模型收尾三件：补合成盲区（P1 `run_mixed.sh`，据它修掉 **B7**；P2 `run_pattern.sh`，FILL_PATTERN 按整帧认）、invalid 位定案（P3 `run_invalid.sh`，占槽 + 数据标无效，三件均完成），另有验收与回归总表、未完成清单。后半是 2026-09-19/20 新立的三项：**P4 单镜像**（已完成，验收 5/5）、**P5 目录路径环境变量化 + `sim-common/` 改名**（2026-09-20 已实施，验收 11/11）、**P6 fxcorr-sim 造数能力**（**部分实施**：2026-09-27 完成「多 datastream 生成」＋验收第 7 条；病态数据处方与压力测试轻量模式待做）。**V5 末节"未完成第 6 条"汇总了 09-26/27 定的四项改造**（batch_id 8 位顺序号 / 按 ds 分片 + `merge` 子命令 / 多 ds 生成 / SQLite 索引）——**前三项 2026-09-27 已实施并验证，只剩 SQLite 索引**（scalebox 阶段，编排层职责）。**接手 reader 工作时先读它**；根因与判据仍在 reader-model.md（4.10–4.12），本文件不重复。
- **v6-plan.md**：V6 计划（**已收尾**，2026-09-28）——主题是**规模与部署**：S0 单 batch 端到端实测、S1 真实规模验证（t25362 分片全链对拍）、S2 压测轻量模式（B2）、S2.5 取消 D15（公共信号改各站本地合成）、S3 规模验证（4 站 × 8 ds × 20 连续 batch，含 mpifxcorr 对照）、S4 编排对接（**两级 `merge` 形态 A** 已实施、`vis-parts/` 清理与迟到策略的规范、SQLite 索引的接口侧）、S5 条件触发项、S6 工程债；另有**多节点部署形态**一节（计算单元 = **(batch, ds 组)**、数据放置表、两种实现的资源对比）。**末节「未完成 / 移交 v7」列了交出去的三项**——多节点部署只交付规范（v6 主题没兑现的那一半）、S5.3 真实数据四项、S5.4 `PCAL_*` 并发写。**做规模 / 部署 / 编排相关工作前先读它**；容量公式与对比数字在 data-volume.md。
- **v7-plan.md**：V7 计划（**定稿**，2026-09-29 定——目标集群实测确认后）——主题是**在目标集群上把 CPU 的多核算力用起来，并第一次跨节点验证**。阶段：**P1** 仿真数据生成（轻量模式预生成到 `/work2`、**fxcorr 与 mpifxcorr 共用同一份**）、**P2** 串行 baseline（单线程 `run_batch.sh`，拿并行化的分母）、**P3** **多核优化**（任务级 × 线程级矩阵，**V7 核心**，顺带出**算力 / 容量 / 存储三个标定**）、**P4** mpifxcorr 单节点对照（`run_bench.sh`，**2 站与 4 站两条配置**，补 V6 漏记的**内存峰值**）、**P5** 多节点（若节点许可）；另有一项**贯穿**：**阶段脚本 + Slurm 封装 + 自检判据**（接口对齐 scalebox app——`v6-plan.md:98` 早已写明"它是 scalebox app 的直接输入"）。**P4 排在 P3 之后是刻意的**：V6 吃过"拿逐站串行的 `run_batch.sh` 去比"的亏。**P1–P4 一律用现成脚本、不引入 scalebox**。目标环境：Slurm 集群（**2653 × 32 核 / 123.5 GiB**，**核数与 v6 假定的"目标节点"对上**）、共享存储 `/work2/cstu0036/fxcorr`。**它把 `v6-plan.md` 末节移交的三项接了手**（多节点 → P5）。
- **algo-plan.md**：V2 算法改进需求与设计——每项动机分类（功能未迁移/串行环境新变化）、要解决的问题、预期效果、设计要点、优先级（P0-P5）。改改进范围或设计时改它。
- **fxcorr-sim-arch.md**：fxcorr-sim 分布式架构——单二进制三入口（common/station/默认串行）、频域公共信号模型与数据量依据、一致性规则、datasim 特性差距、P0-P4 阶段。改 fxcorr-sim 架构或公共信号模型时改它。
- **usage.md**：三工具（fxcorr-sim / fxcorr-f / fxcorr-x）命令行手册——参数、环境变量、输入输出、程序内校验、示例。改工具命令行接口时改它。
- **build.md**：构建手册——集成构建（install-difx）与独立构建（单包 autotools）两条路径、依赖、测试机工作流，以及 **V7 目标集群（Slurm）的构建**（含**七个实测坑**与一键 `build.sh` / `env.sh`）。改构建体系时改它。
- **data-volume.md**：**数据量与容量分析**（2026-09-21 建，活文档）——时间/频率两个维度的算法级粒度、流水线各步的**落盘数据总表**、配置参数表、场景对照、瓶颈排序、优化杠杆矩阵、未验证项，以及 **§7 计算单元的分片**（2026-09-26 新增：目标机约束、按 datastream 分片、`vis-parts/` 与合并方案——合并实现为 fxcorr-x 的 `merge` 子命令）。**体积公式的权威位置已从 `data-spec.md` 11 节移入本文档 §1.3**（2026-09-21）；做部署/容量/切批/分片决策时看它。

### 文档状态（防误读）

每个文档的**头部**都写着自己的「最后更新」与性质，本表是索引。性质只有两类：

- **活文档**——描述当前系统，代码/接口变了**必须**同步；
- **冻结文档**——记录某一阶段的定案与实施，写完不再改，只在头部声明截止日期。**冻结不等于过时**：
  它是理解当前代码"为什么这么写"的依据。

| 文档 | 性质 | 最后更新 | 一句话 |
|---|---|---|---|
| `README.md` | 活 | 2026-09-27 | 需求与架构（R1–R7 的出处） |
| `data-spec.md` | 活 | 2026-09-27 | **数据规范**：目录布局、D1–D16、模块 I/O、时间轴与通道/偏振映射、**跨目录约束**、切批约束（体积公式已于 09-21 移入 data-volume.md §1.3；**09-27 第 5 节下沉**——逐目录说明与二进制格式移入 `workdir-template/<目录>/README.md`，本节只留跨目录约束与指针，全文 845 → 608 行） |
| `usage.md` | 活 | 2026-09-28 | 三工具命令行手册（09-28 补两级 merge 的接口约定，已去 ⚠） |
| `build.md` | 活 | 2026-09-29 | 构建手册（集成/独立两条路径 + 容器构建；V5 P4 起为单镜像；09-28 补测试机分工与三个实测坑；**09-29 补目标集群（Slurm）构建与七个坑**） |
| `reader-model.md` | 活 | 2026-09-19 | 读取路径的**单一权威分析**；改 datareader 先读它 |
| `algo-plan.md` | 活 | 2026-09-27 | 算法改进的设计与实施记录（P0–P12）；09-27 按 V6 多节点部署修订 P2 节"同实验 batch 串行"前提 |
| `fxcorr-sim-arch.md` | 活 | 2026-09-27 | fxcorr-sim 架构与公共信号模型 |
| `data-volume.md` | 活 | 2026-09-27 | **数据量与容量分析**：体积公式（权威，§1.3）、算法级粒度、落盘数据表、瓶颈与优化杠杆、**§7 计算单元的分片方案**（§7.6 = 频段维度该不该进 batch，含编号位数依据） |
| `v6-plan.md` | 活（**S0–S3 与 S4.1 已完成，余 S4.2 编排侧 / S4.3 / S5 / S6**） | 2026-09-28 | **当前版本的路线**（V6：规模与部署——S1 真实规模验证、S2 压测轻量模式、S3 规模验证含 mpifxcorr 对照、S4 编排对接（两级 merge 形态 A）、S5 条件触发、S6 工程债；含多节点部署形态） |
| `v5-plan.md` | 活（**已收尾**） | 2026-09-27 | V5 的路线与定案（P1/P2/P3 读模型收尾三件；P4 单镜像；P5 目录根变量化；P6 多 datastream 生成；末节"未完成第 6 条"的 batch_id 顺序号 / 多 ds 生成 / 分片 + `merge` 三项已完成）——**未完成项已移交 v6-plan.md** |
| `v4-plan.md` | 冻结 | 2026-09-19 | V4 定案与实施（三层判据、B2）；末节的三条后续方向由 V5 接手 |
| `v3-plan.md` | 冻结 | 2026-09-19 | V3 定案与实施（P3 OpenMP、P12 病态数据） |
| `v2-plan.md` | 冻结 | 2026-09-18 | V2 定案与实施（镜像体系、P0–P11） |
| `v1-plan.md` | 冻结 | 2026-09-15 | V1 定案与实施 |

**接手 reader 工作**：先读 `v5-plan.md`（**当前版本**：三件收尾事项的定案与验收）与 `v4-plan.md` 开头的「V4 结论」→ 再读 `reader-model.md` 的缺陷总表（4.1–4.4）、4.10–4.12 的三条定案与小节、诊断契约（6.6）。
**更新文档时**：只同步活文档；冻结文档仅在更正**事实性错误**时动，且同时更新其头部日期。

**本目录之外的文档**（同样在头部声明性质与日期）：

- `test/` 下 13 份 `README.md`——`reader/`、`gaps/` 是**活文档**；其余 11 份（`zoom/ pcal/ mpc/ pulsar/ tcal/ crosspol/ phasearr/ sta/ p10/ p11/ p3/`）是**验证记录**，各对应一个 P 节点、写完不再更新，**重跑回归时以脚本与 `reader/check_reader.py` 为准**（不要把 README 里的旧命令当当前用法）；
- `docker/README.md`（1 份，V5 P4 起为单镜像 `fxcorr/fxcorr`）——**活文档**，描述镜像内容、构建与调用，随镜像变化更新；
- `applications/{fxcorr-f,fxcorr-x,fxcorr-sim}/CLAUDE.md`、`applications/fxcorr-sim/VERIFICATION.md`、`libraries/CLAUDE.md`、仓库根 `CLAUDE.md`——**活文档**，随各自代码/验证更新；
- `mpifxcorr/CLAUDE.md`——描述**冻结的上游代码**，不随改造更新，无需日期。

## 核心约定（源自 data-spec.md）

- **batch_id（2026-09-27 修订）**：**8 位零填充顺序号**（`00000001`）——定宽、单调递增、`workdir` 内全局唯一，由切批规划步骤单点分配；**名字不承载时间信息**（起点/时长/band 全在 batch.json 里）。旧的时间编码格式（如 `60512_45000`）**仍然可用**——三工具不校验格式，只当不透明字符串拼路径。规范见 data-spec 第 6 节。
- **目录**（权威见 data-spec 第 2 节，2026-09-27 核对）：`config/`（.vex/.v2d/.input/.calc/.im/.flag）、`batches/`（`<batch_id>.json`，D9 批量元数据，共享存储）、`raw/<station>/<station>_<batch_id>[_ds<N>].vdif`（TB 级原始基带；**多 datastream 站每 ds 一个文件，`_ds<N>` 后缀只在多 ds 站出现**，见 data-spec 5.2）、`fengine/<batch_id>/<station>/ds_<N>/`（band_XX.sp 复数频谱 + pcal.bin + autocorr.bin，**二进制布局见 `workdir-template/fengine/README.md`**）、`vis/<experiment>.difx/`（SWIN 文件集，跨 batch 追加）、`vis-parts/<batch_id>/ds<G>.part`（D16 分片局部记录，**2026-09-27 已实施**，见 data-spec 5.9；2026-09-28 起同目录另有 batch 级归并产物 `merged.part`，实验级 merge 的输入）、`beam/<batch_id>/beam.bin`（D14 相位阵）、`product/`、`meta/`。均不进 git（**`work/` 已随 V5 P5 删除**）。多节点存储归属（共享/本地）与计算本地化原则见 data-spec 第 1/2 节。
- **batch.json**（`batches/<batch_id>.json`，D9 全字段单文件）：batch_id / start_mjd / start_time / duration_sec / stations / baselines / config_file / calc_file / im_file / n_subints / subint_ns / integration_sec / n_channels / polarizations / difx_dir / status / 版本字段。编排脚本一次写全，三工具只读。
- **数据流**：vex2difx + difxcalc（实验级一次）→ [仿真分支：fxcorr-sim station × 各站每 ds（D7 VDIF；**公共信号在该任务内本地合成，不落盘**，见 data-spec 5.8）] → fxcorr-f × 各站每 ds（D3+D4+D6+D7 → D8+D9）→ fxcorr-x（D3+D4+D6+D8+D9 → D10+D9）→ difx2fits / difx2mark4（按需）。**分片模式（`run_batch.sh` 的 `FXCORR_X_SHARD=1`）**：x 按 ds 组分片 → D16 `vis-parts/` → `fxcorr-x merge` 归并写出 D10（见 data-spec 5.9 与 `data-volume.md` §7.5）。**2026-09-28 起 merge 分两级**（已实施，V6 S4.1）：batch 级 → `vis-parts/<batch_id>/merged.part`，实验级 `fxcorr-x merge --experiment` → D10（**唯一 SWIN 写入者**）；由此推出一条部署硬约束——**`FXCORR_WORKDIR`（含 `config/` `batches/` `meta/` `vis-parts/`）必须落在全局共享存储上**，本地根只有 `RAW`/`FENGINE`（`SIM_COMMON` 已随 V6 S2.5 取消，公共信号不再落盘）。fxcorr-sim 架构（两入口/公共信号模型）见 fxcorr-sim-arch.md。
- **三个敲定决策**：UVW 由 fxcorr-x 读 .calc/.im 求值（D1）；可见度直出 SWIN、difx2fits 零改造（D2）；偏振是 band 属性、偏振组合在 x 侧按 BASELINE TABLE 选（D3）。V1 边界与实施步骤见 v1-plan.md。
- **SWIN 输出目录**：由 .input 的 OUTPUT FILENAME 决定（Visibility 写盘语义，difx2fits 零改造的前提），batch.json 的 difx_dir 仅为元数据。

## 本目录脚本（规格见 v1-plan.md 2.4）

| 脚本 | 作用 | 状态 |
|---|---|---|
| `make_testdata.sh` | 构建 data-spec 布局的标准测试数据（前处理 + 仿真 VDIF + batch.json），支持多 batch | 已实现 |
| `run_bench.sh` | difx 原命令基准：mpifxcorr 固化流程出基准 SWIN 供 cmp_swin.py 对拍 | 已实现 |
| `run_batch.sh` | fxcorr 流水线：前置校验对齐 → 根记录与实验级一致性检查 → DATA TABLE 软链重指本 batch → 逐站 fxcorr-f → fxcorr-x → 更新 status/meta/batches.index；**`FXCORR_X_SHARD=1` 走分片路径**（逐 ds 组跑 + 一次 **batch 级** merge → `vis-parts/<bid>/merged.part`，**不写 SWIN**——SWIN 由实验级 `merge --experiment` 单点写出，那是实验级操作、不进本脚本） | 已实现 |
| `fxinput.py` | **`.input` 解析与 batch 预处理**（V7 P3 前从 `run_batch.sh` 的内嵌 python 段抽出，2026-09-29）：解析 `.input`、三条前置校验、DATA TABLE 软链重指、**ds 组划分**。两个子命令——`prepare <workdir> <batch> <rawroot> <visroot>`（全套，`run_batch.sh` 用）与 `groups <workdir> <batch>`（只打印组划分，无副作用，供对照测试）。**组划分与 `fxcorr-x` 的 C++ 侧 `deriveDsGroups` 是同一条规则的两处实现**（`data-spec` 第 8 节），改一处必须改两处；对照判据 `test/input/run_consistency.sh` | 已实现 |
| `roots.sh` | **目录根解析**（V5 P5）：四个根的三档回退、`mkdir -p` 各根、写 `meta/roots/<batch_id>.json`、实验级根一致性检查。被下面四个脚本 source；与库内 `FxcorrPath` 同规则 | 已实现 |
| `wrap_vex2difx.sh` | vex2difx 封装：在 config/ 内调用（它的 vex= 与产物都相对 cwd）+ 把产物里的绝对路径规范化回相对 | 已实现 |
| `wrap_difxcalc.sh` | difxcalc 封装：同上（规范化 `.calc` 的 IM/FLAG FILENAME） | 已实现 |
| `wrap_difx2fits.sh` | difx2fits 封装：组装三件套到 PRODUCT 根 + 相对路径绝对化，跑完清理（只留 FITS）。**实验级操作**，由编排层在该实验全部 batch 跑完后调一次。**`.input` 的 `CALC FILENAME` 必须指向 PRODUCT 下那份已绝对化的 `.calc`**（2026-09-28 修）：指向 config 原件时它的 `IM FILENAME` 还是相对名，difx2fits 按 cwd（PRODUCT）解析找不到 `.im`，而缺 `.im` 时它的 `fitsMC.c` **不查 NULL** 直接索引 `scan->im[antId]` → **段错误**（`fitsML.c` 有 `if(scan->im)` 保护，所以 ML 那遍只打印一行 "No IM info available"、MC 那遍才崩）。修前 FITS 写到 51 KB 就断，修后 5.4 MB 完整 | 已实现 |

`watch_and_dispatch.sh` 已砍（V1 静态数据集无轮询场景）；流式监视与多节点调度 V2 由 scalebox 承担，容器化同列 V2（scalebox Module 需容器镜像）。

调用示例：

```bash
./fxcorr/make_testdata.sh [workdir] [tone_mhz ...]   # -n N 连续 N 个 batch；-p P station 本地并行；--nodes "host:st1,st2" ssh 分发；FXSIM_NOISE/SEED/ADAPTIVE/SPECRES/LINE/FLUX/SEFD（DELAY 由程序默认开）、BATCH_NSUBINTS 环境变量
./fxcorr/run_bench.sh [workdir]            # 出基准 SWIN 到 bench/（对拍基准，NP 覆盖 mpirun 进程数）
./fxcorr/run_batch.sh 00000001         # fxcorr 流水线（batch.json 须已由 make_testdata.sh 写好）
```

**容器模式**（V2 建成；**V5 P4 起为单镜像，验收 5/5 已过**）：`FXCORR_RUN_MODE=container` 时两脚本内 `fxc` 封装加 `docker run --rm -v $WORKDIR:$WORKDIR -w $(pwd) fxcorr/fxcorr:latest` 前缀——链路上六个命令同在 `fxcorr/fxcorr`（原先按工具→镜像映射，P4 合并后映射取消），FXSIM_NOISE/SEED 与**四个根**透传（Q12：容器内程序仍要知道用哪个根；多根挂载由外部编排平台按"各根按宿主同路径可见"保证，脚本不实现）。P5 起四个编排脚本（`roots.sh` + 三个 `wrap_*.sh`）也打进镜像，可在容器内直接调用。默认 host = 宿主直跑。镜像定义与构建见 `docker/README.md`。

make_testdata.sh 实现要点（实测）：config 资产缺才复制 fxcorr/test/ 的 test.vex/test.v2d；**单 batch 用 difxcalc 原产物 test.input（0.524288s subint，6/6 对拍同配置），仅 `-n N` 多 batch sed 出 128ms SUBINT 变体**（test-sim.input，多 batch 连续切分起点须帧边界；128ms 帧对齐会触发 mpifxcorr vdifmux 帧号 bit7 错读，多 batch 不与 mpifxcorr 对拍）；batch.json 全字段一次写全（start_mjd 精确 repr）；**`polarizations` 逐条 baseline 推导**（A 侧 band 极化 × B 侧 band 极化，与 `configuration.cpp` 的 `polpairs` 同规则、字符透传不改写——2026-09-27 补，原先硬写单元素且做了 R→X 映射，与规范不符；实测：test 配置 `["RR"]`、t25362 `["XX","XY","YX","YY"]`）；**batch_id 由本脚本按 data-spec 第 6 节分配**（8 位顺序号 = `batches/` 已有编号最大值 +1；已有 batch 的 `(start_mjd, n_subints, subint_ns)` 与本次一致则复用编号——重跑幂等，2026-09-27 已实施。原先"每 batch ≥ 1 s"的强制提升已删除，`-n 3` 现在就是 0.512 s/batch 的亚秒切分）；**多 datastream 站按 (station, ds) 展开**：每个 ds 一个 `fxcorr-sim station ... <ds>` 任务、一个文件（`_ds<N>` 后缀仅多 ds 站，与 fxcorr-sim 的 `stationOutPath` 同规则的两处实现），软链逐 ds 链到 DATA TABLE 的对应行（2026-09-27）。软链指向最后 batch。**对拍 mpifxcorr 须 `FXSIM_NOISE=0`**（带噪 2bit 数据触发 vdifmux 读端错乱，见 memory）。**并行分发（P1，实测 p1reg）**：`-p P` xargs 本地并行、`--nodes "host:st1,st2"` ssh 远程（全路径 + `LD_LIBRARY_PATH=$DIFXROOT/lib`、BatchMode/accept-new、FXSIM_* 透传）；远程/本地产物 BYTE-IDENTICAL、失败传播非零退出；`--nodes` 与 container 模式互斥（`-p` 不互斥）。容器模式下 station 任务由 `stationcmd()` 输出 `docker run … fxcorr/fxcorr fxcorr-sim` 前缀（P4 验收时补，原先只有 common 走了容器）；详见 v1-plan 2.4 实施记录。

run_bench.sh 实现要点（实测）：batch 定位走 DATA TABLE 软链 target 的 batch_id（fallback batches/ 最新 json）；EXECUTE TIME 截断 = `floor(initsec + (N−1)×intTime) + 1`（mpifxcorr 停写判定按积分起点、整秒字段，N = batch 时长/intTime 不整除即报错）；OUTPUT FILENAME sed 指 `bench/<exp>.difx`；mpirun 在 workdir 内跑（DATA TABLE 相对路径）、root 加 --allow-run-as-root、`LD_LIBRARY_PATH=$DIFXROOT/lib`；mpifxcorr 拒绝覆盖已有 SWIN（脚本先 rm -rf）。batch 时长 < 1s 无整秒解（mpifxcorr 多写 weight-0 积分），默认配置 2.097s 无碍。

run_batch.sh 实现要点：前置校验、`.input` 解析、DATA TABLE 软链、ds 组划分**都在 `fxinput.py` 里**（2026-09-29 抽出；前置校验三条为 batch 起点 subint 边界 1µs 容差、时长 intTime 整数倍、intTime 为 subint 整数倍，语义同 fxcorr-f/x 程序内校验）；**DATA TABLE 软链每次重指本 batch 的 VDIF**（make_testdata.sh 多 batch 时软链停在最后 batch，跑其他 batch 必须重做；**多 ds 站的文件名带 `_ds<N>` 后缀**——与 fxcorr-sim 的 `stationOutPath`、`make_testdata.sh` 的 `vdifrel` 同一规则、**三处必须同改**；2026-09-28 因这里缺后缀，多 batch 下软链**从不重指**、f 按本 batch 的时间轴静默读**另一个 batch 的数据**，SWIN 写下 0 条记录——单 batch 时 make_testdata.sh 建的软链本来就是对的，所以 `test/multids/` 判据 11 抓不到；raw 数据不存在即报错，不再静默跳过）；站列表取 batch.json stations（缺失时从 .input DATASTREAM 解析）；status 流转 running→done/failed 写回 batch.json（json.dump 保字段序）；done 时追加 `meta/batches.index` 一行 `<batch_id>,done,<UTC 时间戳>`。**V5 P5 加两道根相关**：开跑前 `fxcorr_check_roots`（同实验 `vis`/`product` 与已有 batch 记录不一致即报错退出）与 `fxcorr_write_roots`（写 `meta/roots/<batch_id>.json`，程序启动时会比对自己用到的根）；`mkdir -p` 四个根（Q19：根由编排层建，程序遇根不存在只报错）。

## workdir-template（配置骨架，2026-09-27 加）

一个可用的 fxcorr workdir 的**实物骨架**：十个目录全建出来（`config/` 有配置实物，其余各一份 README），**不含任何数据**。三个用途：**V6 S0 的起点**、**S3 规模验证的基准**、**`data-spec` 布局的可执行样本**——此前仓库里没有一份能看到的 workdir 实物，全在测试机上。它**不属于 `test/`**：那里是"一个特性一个目录、写完不再更新"的验证记录，而这份配置要被 S0/S3/scalebox 持续使用。

**与 `data-spec` 的分工（2026-09-27 定）**：`data-spec` 只留规范（格式定义、约束、跨目录规则），每个目录的**实物形态与逐文件说明**下沉到 `workdir-template/<dir>/README.md`；`data-spec` 第 2 节与 9 个 `5.x` 节各留一行指针。查"这个目录长什么样"看模板，查"为什么必须这样"看 `data-spec`。

| 资产 | 内容 |
|---|---|
| `config/t25362-base.{vex,v2d}` | t25362 真实观测原配置（2 站，逐字取自 `ssh difx`），生成器的输入 |
| `config/t25362-4st.{vex,v2d}` | 4 站（BA/BX/S6/SX，BX/SX 由 BA/S6 **复制改名、坐标照抄**）、跨度 **7072 MHz** —— S0 第②次跑、S3 基准 |
| `config/t25362-4st-mini.{vex,v2d}` | 同 4 站、跨度压到 **1024 MHz**（唯一频率重排到连续网格）—— S0 第①次跑，即 `data-volume` §6 杠杆 1 |
| `config/data/filelist_*.t[1-8]` | 32 个 filelist（4 站 × 8 ds）；路径写**相对名**，`.input` 的 DATA TABLE 照抄它，`make_testdata.sh` 据此在 raw 根下建软链 |
| `gen_vex.py` | 生成器：base → 4 站 → mini；`--install <workdir> <variant>` 装进 workdir 并改名 `test.*`（`make_testdata.sh`/`wrap_*.sh` 认死这个前缀） |

**每个目录一份 README**（`batches/ raw/ fengine/ vis/ vis-parts/ beam/ product/ meta/ config/`，外加模板根一份总表），统一结构：**属性表**（根变量 / 生产者 / 消费者 / 生命周期 / 量级 / 共享还是本地）→ **目录树**（真实示例名）→ **文件格式**（二进制字段表，**2026-09-27 起以 README 为权威**）→ 该目录特有的坑。`data-spec.md` 第 5 节对应的 9 处各留一行指针。

**实测记录见 `workdir-template/README.md`**：两个变体的前处理链均跑通，32 datastream / 96 baseline / 4 个 ds 组（每组 4 站 × 2 极化）。**生成 4 站配置时踩到的两个坑**记在那里：`$SITE`/`$ANTENNA` 必须用新名字（同名 site 被两站共用会让 vex2difx 按 site 索引时把两站当成同一个）；`difxcalc` 对新站名报缺海潮负荷系数（只是警告）。

## test/ 测试资产

**目录组织**（2026-09-13 定）：根目录放共享/通用件（base 配置、造数、通用对拍工具，各特性检验都依赖）；每个特性一个独立子目录（`zoom/`、`pcal/`，后续 P4b/P4c/P6-P11 各建），放该特性的专属脚本/配置，**检验步骤与验证记录写在各特性目录的 `README.md`**。新特性资产一律进特性目录，不再平放根目录。

| 文件 | 作用 |
|---|---|
| `test.vex` | 上游 `tests/Synthetic/test-usb.vex` 原版（2 站 T1/T2、单 band 4MHz USB、2bit、2020y100d07h00m00s） |
| `test.v2d` | 配套 vex2difx 配置（antennas=T1,T2，tInt=1，nChan=4096） |
| `gen_test_vdif.py` | 生成 2bit 单 band VDIF 测试数据（datasim 因上游 IPP 依赖无法 --noipp 构建，此脚本替代；**低位先打包**对齐 mark5access 位序；fxcorr-sim 的位序逐字节对拍参照，对拍已验证 BYTE-IDENTICAL；帧号公式已修为 `n % fps`（原 `n % 8000000 // 32000` 恒为 0，对拍时发现）） |
| `test2b.vex` / `test2b.v2d` | 2 band 测试配置（test.vex 加 205MHz 第 2 band），fxcorr-sim 多 band 验证资产；**2026-09-13 修正 $TRACKS 帧长 8032→16032**（2 band VDIF 帧实长，原值让 vex2difx 生成错误 DATA FRAME SIZE、mpifxcorr vdifmux 帧长错乱） |
| `cmp_swin.py` | SWIN 逐记录比较（74 字节头 + 可见度复数），通用对拍工具（v1-plan 验收标准 2） |
| `pcal/test-pcal.vex` / `pcal/test-pcal.v2d` | 带 phasecal 的测试配置（PHASE_CAL_DETECT tone 列表 `2:3:4:5`、phaseCalInt=1 → .input 4 tones 201-204MHz），PCAL_*.pcal 对拍验证资产（algo-plan P0） |
| `pcal/README.md` | P0 检验资产用法 + 验证方法与结果记录 |
| `zoom/gen_test_zoom.py` | 从无 zoom 的 .input 生成 test-zoom.input + EXECUTE TIME 截断变体（P4a zoom 检验资产） |
| `zoom/cmp_swin_zoom.py` | 按 SWIN 头 freqindex 分拆多 nchan 记录的逐记录比较（zoom 对拍用，同文件多 freq 各不同 nchan） |
| `zoom/README.md` | P4a 检验步骤（完整可复现命令）+ 验收判据 + 验证记录 |
| `mpc/test-mpc.v2d` / `mpc/README.md` | P4b 多相位中心检验资产（addPhaseCentre=TEST2 走 vex2difx 原生链路）+ 检验步骤/验收判据/验证记录 |
| `pulsar/gen_test_pulsar.py` / `pulsar/README.md` | P4c 脉冲星 binning 检验资产（.input 变体 + pulsar config + 自造 tempo polyco，--scrunch/--negative-weight 变体）+ 检验步骤/验收判据/验证记录 |
| `tcal/test-tcal.v2d` / `tcal/README.md` | P6 SwitchedPower 检验资产（两站 tcalFreq=80 → .input TCAL FREQUENCY）+ 检验步骤/验收判据/验证记录（含生成器帧头两个 bug 的记录：legacy 位、vdifio/mark5access 字布局） |
| `crosspol/test-pols.vex` / `crosspol/test-pols.v2d` / `crosspol/README.md` | P7 交叉极化自相关检验资产（RCP+LCP 同 200MHz dual-pol → .input POL PRODUCTS 4 + WRITE AUTOCORRS）+ 检验步骤/验收判据/验证记录（含 test2b $TRACKS 帧长修复与 fxcorr-sim nbands 语义修复两个坑） |
| `phasearr/gen_test_phasearr.py` / `phasearr/cmp_beam.py` / `phasearr/README.md` | P8 相位阵检验资产（.input 变体 + 相位阵配置文件生成，SUBINT 改 numbufferedffts 整数倍过上游 accffts 校验；beam.bin 与手算加权和逐位核对）+ 检验步骤/验收判据/验证记录（含 getinputkeyval 第 20 列坑与早退分支决策记录；上游 mpifxcorr 相位阵死代码无法对拍） |
| `sta/sta_ctrl.c` / `sta/gen_test_sta.py` / `sta/cmp_sta.py` / `sta/README.md` | P9 STA/kurtosis 检验资产（sta_ctrl：difxmessage 控制消息发送 + BINARY_STA 组播抓包；CHANS TO AVG 4 变体覆盖 STA 频域平均分支；原始 record 流逐位对拍）+ 检验步骤/验收判据/验证记录（含 P1 cf32 stride bug 修复与基准控制消息时序坑） |
| `p10/gen_test_mk5b.py` / `gen_test_lba.py` / `gen_test_ivdif.py` / `gen_test_p10.py` / `verify_lba.py` / `p10/README.md` | P10 输入格式检验资产（Mk5B 10016 帧生成器、LBA 16 字节 ASCII 头+2bit 低位先生成器、fanout 多线程 VDIF 生成器、.input 变体、LBA 自洽验证脚本）+ 检验步骤/验收判据/验证记录（含 LBA 位序、VDIF word3 布局、EDV4 三坑等 bug 记录） |
| `p11/gen_test_p11.py` / `p11/README.md` | P11 reader 语义检验资产（TEST2 对跖点 → 几何 delay 11.2ms 的 test-delay.vex/v2d 生成器，触发 delay 重对齐跳块语义）+ 检验步骤/验收判据/验证记录（含 vex2difx 从 cwd 找 vex 的坑） |
| `test/roots/run_consistency.sh` | **根解析一致性判据**（V5 P5 遗留第 4 条）：三档回退下把 `roots.sh`（脚本侧）与 `FxcorrPath`（程序侧）的四个根逐项比对 + 自检报红。**规则再变时先扩它**——两处实现没有编译器兜底，它只覆盖"根的值"，不覆盖"哪类路径归哪个根" |
| `gaps/README.md` | 缺口/filler 处理检验资产（fxcorr-sim 的 `FXSIM_GAPS` 造记录中断：缺口与 filler 两种形式在**同一位置**、**帧号范围相同**，与 t25362 的真实中断同构）+ 检验步骤/验收判据/验证记录（含 **filler 必须推进帧号**的语义坑、make_testdata.sh 要求 workdir 已存在的坑）。七个脚本：`run_boundary.sh`（缺口跨 subint 边界，B5 判据）、`run_filler.sh`（filler ≫ 缺口的过渡区，判据 = 两种形式逐 subint 无效块一致）、`run_window.sh`（**窗口长度**缺口：filler 段之后还有数据时，段后数据落在读取窗口之外，判据 = `reader/check_reader.py` 的 E4 = 0；2026-09-19 加，修复前红 69 帧）、`run_startoffset.sh`（**文件起点晚于 batch 起点**（A 类）：生成器 `FXSIM_STARTOFFSET` + check_reader 的 E5 绝对时间锚，含"判据必须能报红"的自检；2026-09-19 加）、`run_mixed.sh`（**缺口与 filler 同段并存、多组相邻**：照 t25362 的实测段序造两个场景——两组相邻（组间只隔 2 帧数据、同窗口内两次修正叠加）与长 filler 跨 subint，各配纯缺口等价形式；*M2 抓出并修掉了 B7*，见 `reader-model.md` 4.10；2026-09-19 加）、`run_pattern.sh`（**占位帧的字节形态**：同一段时间造三次，只换占位帧为全零头 / 整帧 `FILL_PATTERN` / 帧首 `FILL_PATTERN`，判据 = 三者 `GAPCHECK summary` 逐字段相同 + 逐 subint 无效块一致 + E1–E4 全绿；定案"按整帧认"的实测依据是上游对三者 SWIN 逐字节相同，见 `reader-model.md` 4.11；2026-09-19 加）、`run_invalid.sh`（**invalid 位帧**：帧在位、数据不可用——用后处理把一段帧的 word0 最高位置 1，判据 = 读取位置与对照逐行相同 + 无效块增加 + 真值零 finding；定案"占槽 + 数据标无效"，见 `reader-model.md` 4.12；2026-09-19 加）、`scan_filler.py` / `sp_valid.py` / `dump_weight.py`（帧头扫描 / .sp 无效块 / SWIN 权重对比三个诊断工具）。**七个 `run_*.sh` 的验收判据 2026-09-19 起全部收敛到 `reader/check_reader.py`**（A3：绝对判据——文件真值而非"与另一种形式比"，且各带"判据能报红"的自检；旧相对判据保留作交叉核对） |
| `multids/gen_multids_input.py`、`gen_splitbands_input.py` / `multids/README.md` | **多 datastream 与分片检验资产**（2026-09-27 加）：前者把单 ds 的 .input 展开成"每站多 ds"（DATASTREAM 表复制 + 极化交替、BASELINE 表取笛卡尔积、DATA TABLE 加 `_ds<N>`），后者把"单 ds 多 band"拆成"每 band 一个 ds"以造出**多个 ds 组**；含 `.input` 解析器的四个坑与 11 条验收判据（含分片+merge 逐字节等于不分片） |
| `input/run_consistency.sh` / `input/README.md` | **ds 组划分的两处实现对照判据**（2026-09-29 加，V7 P3 前）：`fxcorr/fxinput.py`（编排侧，算分片边界与并行调度粒度）vs `fxcorr-x` 的 `deriveDsGroups`（计算侧，真打 ds 掩码）。真值出口是 `FXCORR_X_GROUPS_ONLY=1`（**不需要 raw/fengine**）；判据 = 逐组逐成员相同 + 自检报红。**比 `roots/` 那一对更险**——根错了程序会报不一致，组划分错了没有任何症状。**尚未接入 `multids/` 的多 ds / 拆带样本**，只有一个拓扑 |
| `reader/README.md` | **reader 对账的独立真值层**（2026-09-19 加）：`file_truth.py`（扫 VDIF → 台账 JSON：数据段帧号↔文件偏移、filler 段、缺口；缺口与 filler 在真值里统一为"槽未被占用"）+ `check_reader.py`（五条断言对账 fxcorr-f 的 `READPOS`/`GAPCHECK holes`：E1 定位 / E2 数据 / E3 落点（区分**多标**与**漏标**）/ E4 覆盖（**净损失**：文件里有、却从未进入任何读取窗口的帧）/ E5 锚点（读窗口起点的**绝对**时间 vs batch 起点 + 序号×跨度——E1 只看相邻差、E2/E3 是自洽性判据，`anchorbytes` 整体偏时都抓不到，A 类靠 E5 现形；基准不同源、READPOS 不完整、文件含中断时跳过）。判据是绝对的——文件真值而非"与另一种形式比"；`gaps/` 六个脚本都已接入本工具（2026-09-19，A3），`cmp_swin.py` 在病态数据上的失效背景见其 README 与 `reader-model.md` 4.6/4.7。目录内另有 `test_timeline.cpp` 与 `test_corrections.cpp`——**分层重构两层纯函数的单测**（`applications/fxcorr-f/src/frametimeline.h` 的帧时间轴层、`corrections.h` 的修正量层），零依赖直接 `g++ -I applications/fxcorr-f/src` 编译，61 + 48 项断言（C1/C2，2026-09-19；timeline 那 61 项含 FILL_PATTERN 两种位置的识别与一条假阳性检验） |
| `reader/run_t25362.sh` | **真实数据回归**（阶段 D 固化，2026-09-19）：本地驱动 `ssh difx` 跑 t25362 的 BA ds_2（有 filler）与 ds_0（对照），判据 = `GAPCHECK summary` 逐字段对基线 + `check_reader.py` 零 finding；`--no-run` 用现成日志重复对账（自检手段）。用法与自检记录见 `reader/README.md` |
| `gen_vex_v2d.sh` | vex/v2d 生成脚本（datasim scripts/genv2dvex.sh 的 fxcorr 版；test.vex/test.v2d 模板参数化：站坐标/源/时间/频率/帧长/EOP 环境变量或 obs_info 文件覆盖，`CALC=1` 续跑 vex2difx→difxcalc 链；已全链验证：生成→.input/.calc/.im→sim→f→x→SWIN，详见 applications/fxcorr-sim/CLAUDE.md） |
| `make_testdata.sh` | 数据构建脚本（已实现，见上方脚本表） |
| `testdata-min/` | 最小数据集（规划）：对拍最小子集 + sha256 入仓库，待 2 秒配置对拍实测干净后定 |

测试流程（测试机 /root/fxcortest/）：`vex2difx test.v2d` → `difxcalc test.calc`（出 .input/.calc/.im）→ 数据生成两种方式：**fxcorr-sim**（读 batch.json 生成 `raw/<station>/<station>_<batch_id>.vdif`——**多 ds 站为 `_ds<N>` 每 ds 一个文件**，各软链到 .input DATA TABLE 对应行）或 `gen_test_vdif.py TEST1.vdif 4 1.5 8`（对拍参照）→ 写 `batches/<batch_id>.json`（start_mjd 用精确 repr，否则 fxcorr-f 对齐校验报错）→ `fxcorr-f <batch_id> T1` / `T2` → `fxcorr-x <batch_id>` → `cmp_swin.py` 与 mpifxcorr 对拍。以上手工步骤已由 `make_testdata.sh` 自动化（tInt=0.25 变体 test-sim.input，SUBINT 128ms）。已验证（2026-09-12，fxcorr-sim 数据）：tone 峰落 1.5/1.0MHz 通道；SWIN 对拍 6/6 记录全等（可见度 <1e-6、weight 逐位一致）；difx2fits 出 FITS；pcal 链路 4 tones 检出；2 band（test2b）全链路跑通（mpifxcorr 读不了 2 band VDIF，2 band 对拍以物理验证为准，详见 applications/fxcorr-sim/CLAUDE.md）。对拍注意 mpifxcorr 的 mux 滞后（v1-plan 2.3 实施记录）；**对拍数据须 `FXSIM_NOISE=0` 生成**（带噪 2bit 数据触发 mpifxcorr vdifmux 读端错乱，0 积分输出）。

## 相关指引

- 拆分缝隙（core.cpp processdata）与可复用清单：`mpifxcorr/CLAUDE.md`
- 依赖库与 fxcorrcommon 样板：`libraries/CLAUDE.md`
- 构建注册（install-difx 4 处）：根 `CLAUDE.md`；构建流程（集成/独立两条路径）：`build.md`
- 工具命令行用法：`usage.md`
