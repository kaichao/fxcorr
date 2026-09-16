# fxcorr-x 目录说明

baseline-based 相关器后端（X-Engine）：无 MPI 串行程序，读 fxcorr-f 的 .sp / autocorr.bin 产物，做 XMAC 与长期积分，可见度直出 SWIN（`vis/<experiment>.difx/`）。算法照 mpifxcorr 的 `Core::processdata()` 切分移植，写盘复用 fxcorrcommon 的 Visibility（零改造）。

## 调用方式

```
fxcorr-x <batch_id> [workdir]
```

- `workdir` 定位：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认 `.`。
- 读 `workdir/batches/<batch_id>.json`（run_batch.sh 预写），取 start_mjd / n_subints / config_file / difx_dir。
- 数据源 `workdir/fengine/<batch_id>/<station>/ds_<N>/`（band_XX.sp + autocorr.bin），station 列表即 .input 的全部 datastream；N = 站内 datastream 序号（按 .input datastream 序累计，多 datastream 站每记录线程一个 f 任务，见 fxcorr-f 的 ds_index）。
- 输出目录由 **.input 的 OUTPUT FILENAME** 决定（SWIN 写盘沿用 config 语义，difx2fits 零改造），batch.json 的 difx_dir 仅为元数据。
- **DifxMessage 状态发送**（algo-plan P1，difxmonitor 封装）：mpiId = 0（manager 角色），identifier = .input basename。节奏：Starting → 每积分写盘一条 Running（Integrator::sendRunning，writedata 后、increment 前——increment 清零 floatresults，时序同上游 fxmanager loopwrite；weight 照抄 visibility.cpp:1100-1146，f32 截断点一致，对拍逐位一致）→ Ending → Done；错误路径 Alert + Aborting（fail helper）。host 模式组播（DIFX_MESSAGE_GROUP/PORT 未设即静默）；`FXCORR_RUN_MODE=container` 落盘 `meta/difxmsg/<exp>_<batch>.xml`（构造时截断，重跑幂等）。

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
