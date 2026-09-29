# fxcorr-x 目录说明

**最后更新**：2026-09-28（**V6 S4.1 已实施并验证**：`merge` 拆成**两级**——batch 级只写 `vis-parts/<batch_id>/merged.part`、新增实验级 `merge --experiment` 才写 SWIN 且是唯一写入者；判据 A–F 在测试机全过，见调用方式与"分片实现要点"末条。**分片模式与 `merge` 子命令于 2026-09-27 实施**：`ds_group` 分片、D16 `vis-parts/`；2026-09-17 主体）

baseline-based 相关器后端（X-Engine）：无 MPI 串行程序，读 fxcorr-f 的 .sp / autocorr.bin 产物，做 XMAC 与长期积分，可见度直出 SWIN（`vis/<experiment>.difx/`）。算法照 mpifxcorr 的 `Core::processdata()` 切分移植，写盘复用 fxcorrcommon 的 Visibility（零改造）。

## 调用方式

```
fxcorr-x <batch_id> [workdir]              # 不分片：整 batch 全 ds，直写 SWIN
fxcorr-x <batch_id> [workdir] <ds_group>   # 分片模式：只算一个 ds 组，写 vis-parts/
fxcorr-x merge <batch_id> [workdir]        # batch 级归并 → vis-parts/<batch_id>/merged.part
fxcorr-x merge --experiment [workdir]      # 实验级归并 → SWIN（唯一写入者）
```

> **`merge` 的两级形态（2026-09-28 实施，取代 2026-09-27 的单级行为）**：单级形态是 `merge
> <batch_id>` **直接写该 batch 的 SWIN**；多节点并行 batch 时那会让 SWIN 时间回退、被 difx2fits
> **静默丢弃**，所以写入收敛到实验级一次。现在 batch 级只写 `merged.part`（接入 `run_batch.sh` 的
> `FXCORR_X_SHARD=1`，逐组跑完后自动调）、**实验级 `merge --experiment` 才写 SWIN**。四条定则
> （实验的认定与 config 来源、缺 batch 的判据取 `batches/*.json` 而非 `meta/batches.index`、按首
> 记录时间定序 + 逐 batch 流式写出、对每个候选 batch 比 `VIS` 根）见 `data-spec` 5.9 末条；
> `usage.md` 的 fxcorr-x 节是接口侧摘要，`v6-plan.md` S4.1 是路线与判据。

> **分片模式（2026-09-27 定并实施）**：目标计算节点装不下一个 batch 的 `fengine`，故 x 要能按 **ds 组**分片——每组 = **跨站、含全部极化**的若干 datastream，覆盖同一频段组（**互相关**的要求：两站同一 freq 必须同时在场，且要算全极化组合）。**分组成员从 `.input` 的 BASELINE TABLE 推导，不是按 ds 序号**——每条 baseline 条目绑定一对具体的 ds、只出一个极化产品；实测映射见 `data-volume.md` §7.3，机理见 `data-spec` 第 8 节。分片任务写 `vis-parts/<batch_id>/ds<G>.part`（D16）而**不写 SWIN**，由 `merge` 子命令按时间归并写出。规范见 `data-spec` 5.9 与第 6 节「任务标识」；改造清单见 `v5-plan.md` 末节第 6 条。

- `workdir` 定位：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认 `.`。
- **`FXCORR_X_GROUPS_ONLY=1`**（2026-09-29 加，V7 P3 前）：分片模式下**只打印全部 ds 组划分就退出**，不读任何数据、不写盘。存在的理由是 `/fxcorr/fxinput.py` 有一份同规则的组划分实现（编排脚本按它算分片边界），两处之间没有编译器兜底，而漂移的症状是"每片少算或多算 ds、产物看起来完全正常"——判据 `fxcorr/test/input/run_consistency.sh` 拿这里当真值对拍，所以它必须能在**没有 raw/fengine** 的机器上跑。**刻意放在组号范围检查之前**：传任意组号（含越界值如 `0`）都能拿到全部组。诊断上也有用：想只问"这个 batch 分几组、组里是谁"，不必先造 fengine。输出与运行期那行 `fxcorr-x: shard mode, ds group G of N = {...}` 同前缀（差一个 ` -> path`），一条 sed 同时读得懂。
- 读 `workdir/batches/<batch_id>.json`（run_batch.sh 预写），取 start_mjd / n_subints / config_file / difx_dir。
- 数据源 `workdir/fengine/<batch_id>/<station>/ds_<N>/`（band_XX.sp + autocorr.bin），station 列表即 .input 的全部 datastream；N = 站内 datastream 序号（按 .input datastream 序累计，多 datastream 站每记录线程一个 f 任务，见 fxcorr-f 的 ds_index）。
- 输出目录由 **.input 的 OUTPUT FILENAME** 决定（SWIN 写盘沿用 config 语义，difx2fits 零改造），batch.json 的 difx_dir 仅为元数据。
- **DifxMessage 状态发送**（algo-plan P1，difxmonitor 封装）：mpiId = 0（manager 角色），identifier = .input basename。节奏：Starting → 每积分写盘一条 Running（Integrator::sendRunning，writedata 后、increment 前——increment 清零 floatresults，时序同上游 fxmanager loopwrite；weight 照抄 visibility.cpp:1100-1146，f32 截断点一致，对拍逐位一致）→ Ending → Done；错误路径 Alert + Aborting（fail helper）。host 模式组播（DIFX_MESSAGE_GROUP/PORT 未设即静默）；`FXCORR_RUN_MODE=container` 落盘 `meta/difxmsg/<exp>_<batch>.xml`（构造时截断，重跑幂等）。

## 分片实现要点（2026-09-27 实施，改 baseline/datastream 循环前必读）

分片的单位是 **ds 组**（跨站、含全部极化的一批 datastream，覆盖同一频段组），不是单个 ds。要点：

- **ds 组用并查集从 BASELINE TABLE 推导**（`deriveDsGroups`）：每条 baseline 绑定一对具体的 ds，连通分量就是一组；合并时小的根胜出，于是代表元 = 组内最小 ds 序 = 组序。**不能按 ds 序号或 freq 条目配对**——同频段的 X/Y 是两条不同的 freq 条目，而 t25362 的第 2 条 baseline 是 (ds1,ds8) 而非 (ds1,ds9)。
- **`threadcrosscorrs` 的布局回放必须保持全局**，屏蔽只决定"算不算"。`xmacBatch` 的 `fxpasses` 预计算与主循环按 `resultindex += ...` 复现数组偏移，而 `uvshiftAndAverageBaselineFreq` 用 `getThreadResultBaselineOffset` 这类**固定函数**取偏移——两者必须一致。若屏蔽时连 `resultindex` 的推进也跳过，后续 baseline 全部错位，症状是**分片的基线记录全 0**（autocorr 正常，它不走这条路）。实现上是"组外的 baseline 只推进 `resultindex` 再 `continue`"。
- **`localFreqIndex()` 只用于"是否相乘"**（`xmacBatch` 的主循环与 `accumulateWeights`）；任何**布局回放**处都要用未屏蔽的 `config->getBLocalFreqIndex`。
- **autocorr 段要按 ds 过滤**（`Visibility::setActiveDatastreams`）：这些记录无条件写（不像基线记录要 `weight > 0`），不过滤则每个分片都会写全部 ds 的自相关，merge 后重复。
- **`.part` 就是 SWIN 记录流原样**：`Visibility::setOutputPath()` 把 `flushBuffersToDisk` 重定向到一个显式文件；单相位中心 + 无 pulsar binning（分片模式报错挡掉这两种）保证只有一个缓冲区。
- **`merge` 的两级（2026-09-28 实施，`main.cpp` 的 `doMerge` / `doMergeExperiment`）**：**归并算法两级共用**——按整数纳秒分组 + `stable_sort`（键 `(key, autocorr, baseline, freqindex, pulsarbin)`，**刻意不含极化对**——头里只有 polpair 没有 k，字典序不等于写盘顺序，靠 stable_sort 保原序）；autocorr 判据是"非零且能被 257 整除"，只在撞号时影响周期内顺序，不影响集合。**batch 级**读 `ds*.part`、写 `vis-parts/<batch_id>/merged.part`（`ios::trunc`，重跑覆盖）；**实验级 `--experiment` 是 SWIN 的唯一写入者**，多节点并行 batch 时"每 batch 完成即写 SWIN"会让时间回退、被 difx2fits 静默丢弃。实验级的四条定则（细则见 `data-spec` 5.9 末条）：① **实验的认定**——扫 `batches/*.json`，取**已有 `merged.part`** 的当候选，用候选的 `config_file` 构造配置，候选间不一致或候选为空都报错；② **缺 batch 的应有集**取 `batches/*.json` 中 `config_file` 匹配的**全部** batch（**`status` 不参与判据**，只进报错信息）；③ 按**首记录时间**定序后**逐 batch 流式写出**（不做全局排序——各 batch 时间不重叠，峰值 = 单 batch）；④ 对每个**候选** batch 逐个比 `VIS` 根。`readPart` 两级共用（`merged.part` 就是同一份记录流）；`FXCORR_X_SWIN_CONFLICT` 随写出移交实验级，查的是**本次写出的全部 batch 的总时间范围**。
- **互斥检查（`swinHasBatchRange`）**：三种模式启动时读目标 SWIN 的记录头，本 batch 时间范围内已有记录即报错（`FXCORR_X_SWIN_CONFLICT=allow` 绕过）。只读 74 字节头、数据 `seekg` 跳过。
- **空输入 / 空产物防呆（2026-09-28 实施，`main.cpp` 的 `countValidBlocks` + batch 末尾两处判定）**：挡的是最坏的一种静默——f 与 x 都退 0、日志照报 "N integrations written"，产物却是空文件。① **`validblocks == 0`**：整个 batch 全部 `.sp` 视图、全部 subint 的**每一个 FFT 块**都是无效的。`FEngineWriter::writeSubintHeader` 把每块的有效性写进 `.sp` 的 valid flags（"one bit per FFT block"），`SpReader::flags()` 早就有访问器而**此前全仓库没有一处调用**——x 只靠 `Visibility::writedata` 的 weight 门控间接判断，于是"raw 里根本没有这段数据"与"有数据但权重算出来是 0"分不开。② 全量模式下 `integrationswritten > 0` 却在目标 SWIN 的**本 batch 时间范围内数出 0 条记录**（复用 `swinHasBatchRange`）：数据有效但每条基线的 weight 都为 0 时走这条。分片模式写 `.part`、由 batch 级 merge 收，故只做①。两者都由 **`FXCORR_X_ALLOW_EMPTY=1`（或 `allow`）** 绕过。**判据刻意取"全无效"而非"部分无效"**——部分无效是常态（gap / filler / 真实观测丢帧），`test/gaps/` 的七个用例正是覆盖那一侧的。**实测**：raw 指错 batch 时原本静默写 0 条记录，现在报 "read no valid data at all"；同一份数据在 raw 正确时两者都放行，且产物与改动前**逐字节相同**（v6mg 三 batch、probe 128 ms 用例）。
- **`.part` 的第一次写截断、后续追加**（`Visibility::wroteoutputpath`）：`writeSWIN` 每积分周期调一次，同一次运行内是追加；而重跑分片必须覆盖自己的 `.part`（data-spec 5.9），所以由第一次写负责截断。
- **`readPart` 必须读 `.input`**：记录长度不在头里，由 `freqindex` 查 FREQ 表得到。sync word 与长度不符即报错退出（宁可停，也不要写出看着正常实则残缺的 SWIN）。

**判据**（`fxcorr/test/multids/README.md` 有可复现步骤）：单组分片与不分片逐字节相同；多组分片 + merge 与不分片逐字节相同；缺片默认不写、`FXCORR_X_MERGE_FORCE=1` 强制并告警。

## 文件与 mpifxcorr 对照

| 本目录 | 作用 | 对照源 |
|---|---|---|
| main.cpp | batch.json 解析、V1 边界检查、逐 subint 驱动（xcblockcount/maxxcblocks 批次、尾批、时间推进） | core.cpp:657-784（批控制）、:985-991 / :1055-1060（uvshift 触发）；fxmanager.cpp:650-698 的单 Visibility 串行替代 |
| spreader.{h,cpp} | .sp 读取：256 字节头校验 + 按 subint fseek 读头/flags/weights/spectra（线性 FFT 序）；**zoom 切片视图（P4a 2026-09-13）**：构造参数 (channeloffset, nchanoverride) 时每 FFT 块只读父 .sp 的切片段，spectra()/numChannels() 语义不变 | 布局见 data-spec 5.3；对应 fxcorr-f 的 FEngineWriter |
| xmac.{h,cpp} | XMAC 批循环 + baselineweight 累加 + uvshiftAndAverage（含 pulsar binning 与多相位中心）；**P3 OpenMP（2026-09-15）**：xmacBatch 的 used (freq, xmac-pass) 对预收集 + 基线循环并行（per-(f,x) baseoffset 表替代顺序累加 resultindex，threadcrosscorrs 布局不变）、uvshiftAndAverage 的 (freq, baseline) collapse(2) 并行、accumulateWeights 基线循环最外层并行（循环交换后每基线累加序列不变）；conjbuf/pulsarscratchspace/chanfreqs/rotator/rotated/argument 按 nthreads 私有副本（ompcompat.h 无 OpenMP 时退化串行） | core.cpp:867-982（删 phased array）、:1005-1052、:1431-1938（单线程、无锁）；**脉冲星（P4c）**：bins 计算 + pulsar/scrunch 分支照 :803-812/:914-975，scrunch 折叠照 :1438-1475，bin 展开照 :1626-1631/:1781-1789，bweight bin 循环照 :1080-1087，工作区分配照 :1940-2064；**多相位中心（P4b）**：差分延迟 + rotator 生成 + 旋转 + 结果区步进 + decorr 段照 :1636-1725/:1746-1815/:1877-1923，工作区分配照 :416-433，shiftdecorr 写段照 :1090-1105；vis2 逐块 `vectorConj_cf32` 后 `vectorAddProduct_cf32`（等价 getConjugatedFreqs）；zoom band 由 config 表驱动零改动（band index = ds total 序，readers 已含 zoom 视图） |
| integrate.{h,cpp} | 单 Visibility（numvis=1）：addData 满 intTime → writedata → increment；autocorr.bin 逐批次累加进 results 自相关段/acweight 段 | fxmanager.cpp:168-185（构造+polnames）、:114-133（todiskbuffer 预算）；core.cpp:1260-1370（autocorr 累加，**P4a 2026-09-13 起覆盖 total bands**：nbands 校验 getDNumTotalBands、freqindex 用 getDTotalFreqIndex、acbuf 取最大 nchan，zoom 的 weight 已是 f 侧换算好的父 band 值）；**P7 2026-09-13**：header 支持 version 1/2（v2 多 u32 crosspol 标志），crosspol=1 时每条批次记录平行段后接 crosspol 段，读段 lambda 两次调用、resultindex/weightindex 从平行 walk 结束处**连续**接续（core.cpp:1288-1301/1342-1369 的 results 串联布局，非独立 offset 区） |
| beamengine.{h,cpp} | **相位阵波束形成（P8 2026-09-13）**：per (freq,papol) 段表（recorded band 优先、zoom 兜底）+ per subint per acc 窗口 Σ_ds DWeight×频谱落盘 beam/<batch_id>/beam.bin | core.cpp:818-865（f 侧波束加权，Mode 换成 .sp 频谱）；输出端上游死代码无对照，beam.bin 布局 fxcorr 自定（data-spec 5.5）；通道数用 getFNumChannels(f)（上游 :821 误传 configindex） |

## 关键实现要点（易错，改前必读）

- **P3 OpenMP 三条（2026-09-15，改并行相关代码前必读）**：① **逐位一致靠"基线写区独立 + 序列不变"**——并行化只重排基线间执行顺序，单基线的运算序列（conj→mul→累加、uvshift 平均）与串行完全一致；resultindex 顺序累加已改为 per-(f,x) baseoffset 表（xmacBatch 预收集段照原步进逻辑重放，含 localfreqindex<0 不占位的 quirk），threadcrosscorrs 布局不变。② **scratch 必须按线程私有**——conjbuf/pulsarscratchspace（xmacBatch 内每 (j,p) 复用）与 chanfreqs/argument/rotator/rotated（MPC 的 uvshiftAndAverageBaselineFreq 内复用）都是成员单缓冲，已改 [nthreads] 副本并加 omp_get_thread_num() 取用；新加并行循环时先查复用缓冲。③ **线程数语义**——main 开头 OMP_NUM_THREADS 未设 = omp_set_num_threads(1)（默认串行，V2 回归不破）；XmacEngine 构造取 omp_get_max_threads() 分配 scratch 副本，因此线程数设置必须在 XmacEngine 构造之前（main 已保证）。shifterrorcount（MPC 错误计数）用 `#pragma omp atomic`。
- **results 三层结构**：subintresults（coreresultlength，每 subint 清零）→ XMAC/uvshift/自相关累加 → `Visibility::addData` 加进长积分。offset 体系（threadresult*/coreresult*）全部沿用 configuration 预算，**resultindex 累加顺序必须与 populateResultLengths 一致**（f→x→baseline→pol）。
- **uvshiftAndAverage 简化版**：nsoffset/nswidth 参数保留但单相位中心下不用（rotator/decorr 全跳过）；频谱平均段照 core.cpp:1829-1853（virtualplacement/bin 平均，非 d260 老版）。
- **自相关批次节奏**：autocorr.bin 每 subint 存 ac_batches=ceil(bps/maxacblocks) 条记录（f 侧每批次 averageFrequency+zero），x 侧必须**全部读入逐条累加**，与 mpifxcorr 的 averageAndSendAutocorrs 节奏一致——否则 weight 与数值都对不上。
- **zoom 两条易错点（P4a，2026-09-13 实测教训）**：① x 侧**每 subint 的时间戳校验循环必须遍历全部 band 视图**（readers[ds].size()，含 zoom 视图）——只读 recorded 时 zoom 视图的 specbuf 恒 0、互相关全零而自相关正常（f 侧 autocorr.bin 独立落盘），首轮对拍 zoom 段 max rel 1.0 即此因；② BASELINE 段的 `D/STREAM A/B BAND` 行 key 序号是 **pol product 序号**不是 freq 序号（configuration.cpp:1016-1019 按 k 取行），自造 .input 时写错会导致 zoom band 静默解析为 0 且无报错。
- **脉冲星三条易错点（P4c，algo-plan P4c 关键点）**：① **两套时间系勿混**：polyco 的 setTime/getBins 用**当日秒系**（main.cpp 的 sec = startseconds + scanstartsec + expectedsec + expectedns/1e9），bins 的 offsetmins 是 subint 内 FFT 序号×blockns/6e10（i 从 0 起，无 startblock）；rotator/delay 用 scan 相对系（offsetsec，见 P4b）——同一次 uvshift 内两种系并存；② **bweight 的 freqchannels 因子**：pulsar 分支 bweight = w1×w2/freqchannels（core.cpp:924/945），non-pulsar 分支不除——pulsar 时权重在 XMAC 内更新、accumulateWeights 早退；③ **scrunch 负权重语义**：负 binweight 的 bin 参与数据折叠但不进 baselineweight（`binweights[destbin] > 0.0` 才累加）；另：pulsaraccumspace 源槽恒 0（单 pulsar 星历），scrunch 折叠在 uvshiftAndAverage 开头、之后 corebinloop=1 走单 bin 路径。
- **scrunch accumspace 清零（P4c 实测坑）**：pulsaraccumspace 必须在 uvshiftAndAverage **尾部**清零（core.cpp:1565-1602，threadcrosscorrs 清零之后）——漏移植会导致 accumspace 跨 uvshift 窗口累积，可见度按积分序放大（首积分 1.5×、次积分 3.5×，模式 (2n-0.5)×），对拍必炸。
- **多相位中心三条易错点（P4b，algo-plan P4b 关键点）**：① **时间基准**：calculateDelayInterpolator 的 offsettime = offsetsec + nsoffset/1e9，offsetsec 是 subint 起点的 **scan 相对秒**（main.cpp 由 expectedsec/expectedns 合成，勿用当日秒系）；② **单源退化必须逐位一致**：延迟/rotator/decorr/写段全部由 numphasecentres>1 条件包裹（照 core.cpp:1636/1699/1887），单源路径与 P4b 前实现完全相同；③ **decorr 段结果区布局**：floatresults 的 shiftdecorr 段在 bweight 段之后、每 (freq,baseline) 连续 numphasecentres 个 f32，offset 用 getCoreResultBShiftDecorrOffset 勿手算。
- **weight 语义链**：.sp weights = 每 FFT 块 dataWeight（槽式回填，见 fxcorr-f CLAUDE.md）→ baselineweight = Σ weight1×weight2 → floatresults（bweightoffset×2 处）；autocorr weight = 各批次 getWeight 累加 → acweightoffset×2 处。writedata 内部除以 fftsperintegration 归一。
- **日志级别（2026-09-17）**：`FXCORR_LOGLEVEL`（`error`/`warn`/`info`/`verbose`/`debug`，默认 `info`）控制全部诊断输出。`src/log.h` 提供宏 `FXLOG(level)` 与 `fxApplyAlertLevel()`，与 fxcorr-f 的同名文件**同源同内容，改一处必须同步另一处**（同 `ompcompat.h`）。main.cpp 在 `Configuration` 构造前调用一次 `fxApplyAlertLevel()`——否则 fxcorrcommon 上游代码经 `Alert` 流（`cinfo`/`cdebug` 等）打印的约 85 行配置加载输出不受级别控制（f/x 都不调 `difxMessageInit`，`difxMessageSendDifxAlert` 恒走 `difxMessagePort < 0` 的打印兜底分支，详见 fxcorr-f 同名条目）。x 侧自身输出除两处 per-batch 完成小结走 `FXLOG(FXLOG_INFO)`（相位阵 beam、常规 SWIN 各一条）外全是错误信息，恒显。只影响输出，不影响产物。
- **V1 启动边界检查**（main.cpp）：单 scan、单相位中心、intTime 为 subintNS 整数倍（相位阵时跳过——不写 SWIN 无积分网格需求）、无 pulsar；batch 起点 subint 边界校验（容差 1µs，同 fxcorr-f）。**maxproducts>2 已放行（P7 2026-09-13）**：crosspol 由 autocorr.bin header 的 crosspol 标志驱动（f 侧写段、x 侧读段、Visibility 的 autocorrwidth=2 写盘全链零改造就位）。**相位阵已支持（P8 2026-09-13）**：phasedArrayOn 时走 main.cpp 的**早退分支**（readers 校验后 BeamEngine + per subint 读谱加权落盘，不构造 XmacEngine/Integrator、不写 SWIN），旧路径逐字节不动——相位阵改动一律放早退分支或 beamengine，**不要动 XMAC 路径**（曾因 if/else 包裹重构引发回归堆损坏，教训见 fxcorr/test/phasearr/README.md）。
- **executeseconds 语义**（Visibility::writedata 停写判定）：executeseconds 以 **scan 起点**为基准（mpifxcorr EXECUTE TIME 语义），batch 起点偏移 initsec 时须 `executeseconds = batch时长 + initsec + 1`，否则 batch 起点非 scan 起点的 batch 全部静默不写盘（2026-09-12 修复，batch 起点 scan 起点时退化为原语义、对拍回归 6/6）。
- **mpifxcorr mux 滞后**：对拍时 mpifxcorr 数据后段会有确定性的 invalid 边界 subint（vdifmux 流式管线滞后，两次运行可复现），fxcorr 无此滞后——对拍 batch 取数据完整覆盖段（见 v1-plan 2.3 实施记录）。

## V1 边界

- 输入仅 fxcorr-f 的 .sp（**zoom band 已支持（P4a 2026-09-13）**：x 侧按 .input 的 zoom 定义对父 .sp 做通道切片视图，无新文件；**多相位中心已支持（P4b）**：.im 的 NUM PHASE CENTRES 驱动，每源一套 .s 文件，写盘零改造；**脉冲星 binning 已支持（P4c）**：.input PULSAR BINNING + pulsar config + polyco，.b 文件与 SCRUNCH 两种模式，写盘零改造；**相位阵已支持（P8）**：phasedArrayOn 时只出 beam/<batch_id>/beam.bin、不写 SWIN）；STA/PCAL 文件生成均不做（V2）。
- 无流式：整 batch 逐 subint 顺序读 .sp（每 subint 各站各 band 一次 fseek），谱数据按 subint 常驻（blockspersend×nchan cf32/站/band）。
- **进程内多线程已支持（P3 2026-09-15）**：`OMP_NUM_THREADS=N` 启用基线循环并行（scratch 按线程私有化，结果与串行逐位一致，usage.md）；未设 = 串行。构建经 AC_OPENMP（无 OpenMP 编译环境退化串行，ompcompat.h）。

## 构建与注册

- 模板照 fxcorr-f：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f）/ Makefile.am / src/Makefile.am。
- install-difx 注册 4 处（components、setNormalComponentsFalse、apptargets dompicxx=True、--doonly 帮助）。

## 测试

- 资产与对拍工具在 `fxcorr/test/`：根目录共享件 cmp_swin.py（SWIN 逐记录比较，验收标准 2）；**zoom 检验资产（P4a）** 在 `fxcorr/test/zoom/`：gen_test_zoom.py（从 test.input 生成 test-zoom.input + EXECUTE TIME 截断变体）、cmp_swin_zoom.py（按 SWIN 头 freqindex 分拆多 nchan 对拍）、README.md（检验步骤与验证记录）；**多相位中心检验资产（P4b）** 在 `fxcorr/test/mpc/`：test-mpc.v2d（addPhaseCentre 走 vex2difx 原生链路）、README.md（检验步骤/验收判据/验证记录，含 .im 的 SRC 索引语义）；**脉冲星检验资产（P4c）** 在 `fxcorr/test/pulsar/`：gen_test_pulsar.py（.input 变体 + pulsar config + 自造 tempo polyco）、README.md（检验步骤/验收判据/验证记录）。
- 测试机 /root/fxcortest/：f 侧产物 fengine/58948_25200/ → `fxcorr-x 58948_25200` → config/test.difx/DIFX_*.s0000.b0000；对拍 mpifxcorr 用 EXECUTE TIME 截断到完整积分段（test-mpi2.input，EXECUTE TIME=2）。
- 已验证：2 站 4 秒实验 4 subint（2 积分）SWIN 与 mpifxcorr 逐记录全等（6/6，可见度 <1e-6、weight 逐位一致）；difx2fits 出 FITS 成功；多 batch 第 2 个 batch（起点 = scan 起点 + 1.024s，test-sim 配置 8 subints）4 积分 12 条记录全链路跑通。

**zoom（P4a）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/zoom/README.md`（2026-09-13 验证过，测试机可一键复现）。

**交叉极化自相关（P7）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/crosspol/README.md`（2026-09-13 验证过：dual-pol 全链路 SWIN 72 条含自相关 RR/LL/RL/LR、单 pol + WRITE AUTOCORRS 与 mpifxcorr 对拍 6/6 全等、无 WRITE AUTOCORRS 回归 2/2、位序 BYTE-IDENTICAL）。注意自相关伪基线号 257×(tel+1) 与单 pol 互相关 T1-T1/T2-T2 基线号撞号，对拍解析按记录序/polpair 区分。

**相位阵（P8）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/phasearr/README.md`（2026-09-13 验证过：DWeight 0.5/0.5 与 1.0/0.0 两变体 beam.bin 50×4 窗口与手算加权和逐位全等、时间戳正确、谱峰 chan 1536、不写 SWIN；无相位阵回归对拍 6/6）。注意上游 mpifxcorr 相位阵是死代码无法对拍；SUBINT 须 numbufferedffts 整数倍（上游 accffts 校验）。
