# fxcorr-f 目录说明

station-based 相关器前端（F-Engine）：无 MPI 串行程序，逐站处理一个 batch 的本地原始数据，产出 band_XX.sp / pcal.bin / autocorr.bin（格式见 `fxcorr/data-spec.md` 5.3）。对拍目标为 mpifxcorr 的 station-based 段，核心算法（解包/条纹旋转/分数采样/FFT/自相关）零改动复用 fxcorrcommon 的 Mode。

## 调用方式

```
fxcorr-f <batch_id> <station> [workdir] [ds_index]
```

- `workdir` 定位：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认 `.`。
- `ds_index`（默认 0）：站内 datastream 序号（0-based）——多 datastream 站（每记录线程一个 datastream，真实观测常态）每流一个 f 任务；输出 `fengine/<batch_id>/<station>/ds_<ds_index>/`（单流站也是 ds_0/，布局统一）。
- 读 `workdir/batches/<batch_id>.json`（run_batch.sh 预写），取 start_mjd / n_subints / config_file。
- 读 `workdir/<config_file>`（.input，非 MPI 构造），Model 由 .calc 内建（无 .im 依赖）。
- 输出目录 `workdir/fengine/<batch_id>/<station>/ds_<ds_index>/`（自动创建）。
- 原始数据文件路径直接取自 .input 的 DATA TABLE（相对进程 cwd）。
- **DifxMessage 状态发送**（algo-plan P1，difxmonitor 封装）：mpiId = dsindex+1（datastream/core 角色），identifier = .input basename。节奏：Starting → 每 subint 两条 Diagnostic（DataConsumed/InputDatarate）→ Ending → Done；错误路径 Alert + Aborting（fail helper）。RUNNING 不发（归 fxcorr-x）。`FXCORR_STA=1` 时每 autocorr 批次（writeAutocorrelationBatch 前、本批次 zeroAutocorrelations 前）发 DifxMessageSTARecord 到 `DIFX_BINARY_GROUP/PORT`（组装照 core.cpp averageAndSendAutocorrs 1195-1253：data = 实部之和×renorm、最低权重门槛 0.333、时间戳当日秒系；**P9 补平均分支**：minpostavfreqchannels ≥ STADumpChannels 时 STA 前先 averageFrequency、renorm/通道数按平均后修正、写盘跳过重复平均，MTU gate 同上游）。`FXCORR_KURTOSIS=1`（P9）时每 subint 末发 STA_KURTOSIS（照 averageAndSendKurtosis 1378-1434：无 weight gate、无 renorm、整 subint 时间戳）。host 模式组播（DIFX_MESSAGE_GROUP/PORT 未设即静默）；`FXCORR_RUN_MODE=container` 落盘 `meta/difxmsg/<exp>_<batch>_<station>.xml/.sta`（构造时截断，重跑幂等）。

## 文件与 mpifxcorr 对照

| 本目录 | 作用 | 对照源 |
|---|---|---|
| main.cpp | 参数/batch.json 解析、subint 循环驱动、ac 批次触发、**SwitchedPower 切块喂入（P6）**、**STA 发送与 kurtosis（P1/P9）**、**P3 OpenMP 块并行（2026-09-15）**：Mode 副本构造 + 每 subint 一个并行区（连续段分块 process + 主线程块序归约 + worker 槽拷贝/副本清零与写盘并行）+ 线程数设置 | 骨架参照 core.cpp:694-801（zeroAutocorrelations→setValidFlags→setData→setOffsets→resetpcal→process 循环）+ :993-1003（acblockcount/maxacblocks 批次）；对齐校验为新增（data-spec 12 节）；switched power 喂入照 vdiffile.cpp:945-964（段粒度 + increment）；sendSTA 照 :1195-1253（P9 补 :1181-1187 平均分支与 :1249-1254 renorm 修正、:1191 MTU gate）、sendKurtosis 照 :1378-1434（P9） |
| datareader.{h,cpp} | 多格式读取：粗延迟 + 帧对齐/字节率定位 + 顺序读文件（**P10 五路径**：KIND_VDIF / KIND_MUXEDVDIF / KIND_MARK5B / KIND_MK5STREAM / KIND_LBA）；**P11 delay 重对齐（2026-09-14）**：修正起点早于数据起点（segment 0）时按 FFT 块跳块 + tosubtract 补偿（含上游 quirk：仅补偿压回时扣+再跳一块重算）+ 2 字节/整数 ns 对齐，跳块数存 lastcount 供 fillValidFlags 置前 count 块 invalid；−nsinc 整 subint 丢弃早退 | datastream.cpp:381-394（延迟/第一个 offsetns）、:516-573（采样→字节偏移）、:463-470（−nsinc 早退）、:538-568（跳块/tosubtract）、:757-761（bytesbetweenintegerns 累加）、vdiffile.cpp:417-444（帧对齐、framesin×framebytes）；sendbytes = getDataBytes（configuration.cpp，已帧对齐含 guard）；**getLastFileOffset（P6）** 暴露上次读入的文件偏移供喂入重叠剔除；P10 各路径对照源见 algo-plan.md P10 上游路径映射表、P11 等价映射见 P11 设计/实施记录 |
| fenginewriter.{h,cpp} | .sp / pcal.bin / autocorr.bin 写盘 | 布局照 data-spec 5.3；写入序照 core.cpp:1145-1153（finalisepcal→getPcal）、:993-1003（autocorr 批次节奏）、:1260-1265（averageFrequency→getAutocorrelation）；**autocorr.bin 覆盖 total bands（P4a 2026-09-13）**：头/写循环用 getDNumTotalBands、per-band nchan = getDTotalFreqIndex 的 nchan/chanstoavg，zoom 段 weight 从父 recorded band 取（getWeight(false, l)，映射同 core.cpp:1324-1339）；**crosspol 段（P7 2026-09-13）**：header v2 + crosspol 标志（writeAutoCorrs && maxproducts>2），批次记录平行段后接 crosspol 段（getAutocorrelation(true, j)/getWeight(true, j)，zoom 的 crosspol weight 从父 band 取 getWeight(true, l)，照 core.cpp:1288-1301/1342-1369） |

## 关键实现要点（易错，改前必读）

- **P3 OpenMP 五条（2026-09-15，改并行相关代码前必读）**：① **副本模式**——并行 = 每线程一个 Mode 副本（getMode 工厂重建，工作缓冲独立；共享读：Configuration/Model/interpolator/FFTW plan 并发 execute 安全），subint 级的 setData/setOffsets/setValidFlags/resetpcal/zero* 须对每个副本调用；fxcorrcommon 只加了 mode.h 的归约接口（getFreqsWrite/setDataWeight/addWeight/addPcal/getKurtosisProducts*/addKurtosisProducts），Mode::process 内部零改动。② **块序归约是逐位一致的关键**——连续段分块（t*numffts/nthreads）+ 副本按线程序累加进主 Mode = 串行块序；浮点加法不可交换，任何重排归约顺序（如并行归约、round-robin 分块）都会破坏对拍。③ **归约节奏**——每 fftloop 一次（writeSpectra 与 ac 批次检查都在主线程 t==0 段）：累积归约（串行）+ 槽拷贝（worker 各拷各段，与累积归约并行）+ 写盘/批次（主线程，与 worker 的副本清零并行）；acblockcount/acshiftcount 只由 t==0 读写、区外主线程读（barrier 分隔）。④ **线程数语义**——main 开头 OMP_NUM_THREADS 未设 = omp_set_num_threads(1)（默认串行，V2 回归不破）；副本在 setDumpKurtosis 之后构造（dokurtosis 须先定）。⑤ **并行区粒度**——每 subint 一个并行区（不是每 fftloop，fork/join 会吃光收益），fftloop 间用 barrier；无 OpenMP 编译时 ompcompat.h 退化串行。
- **process(i, subloop) 的槽复用**：subloop 是缓冲槽索引（0..NUM BUFFERED FFTS-1），i 是 subint 内 FFT 全局序号。必须双循环 `fftloop × numBufferedFFTs`（core.cpp:786-801 同构），每 fftloop 结束立即写盘该批槽（writeSpectra），不能攒到 subint 尾——槽被后续 fftloop 覆盖。
- **Mode 零改造**：`Configuration::getMode(configindex, dsindex)` 工厂现成（configuration.cpp:857）；getFreqs/getPcal/getAutocorrelation/getWeight/getDataWeight 全为 public。
- **对齐校验**：batch 起点必须落在 subint 边界（容差 1µs，吸收 start_mjd 的 f64 表示误差），非对齐报错退出。batch.json 的 start_mjd 用精确 repr（如 58948.291666666664）。
- **valid flags**：datareader 按读入字节数置位（每 FFT 块 blockbytes 判界，对照 datastream.cpp:600-604）；无效 subint（bytes=0 → INVALID_SUBINT）由 Mode::process 内部全零处理（mode.cpp:655-667），无需特判。
- **weights 是 per-band 且槽式**：`getDataWeight(band, slot)` 的 slot 是缓冲槽、仅在 process 后有效——writeSpectra 时按槽收集进 weightaccum，subint 尾 flushWeights() fseek 回填 subint 头的占位区（.sp 布局不变）。**不能用全局 FFT 序在 process 前写**（曾导致越界读与垃圾权重）。
- **autocorr 按 maxacblocks 批次落盘**：每批次（满 maxacblocks，core.cpp:993-1003 同节奏）writeAutocorrelationBatch（averageFrequency + 落盘 + 权重）后立即 `mode->zeroAutocorrelations()`；autocorr.bin 每 subint 存 ceil(blockspersend/maxacblocks) 条记录（header 的 ac_batches 字段）。**不是每 subint 一条**——与 mpifxcorr 节奏不符则 fxcorr-x 对拍必挂。
- **STA/kurtosis 三条易错点（P9，2026-09-13 实测教训）**：① **fold 循环的 stride**——上游 acdata 是 `(f32*)getAutocorrelation()`，`acdata[2*k*chans_to_avg]` 的 ×2 是 f32 视角的实部 stride；fxcorr 用 cf32* 指针时必须写 `acdata[k*chans_to_avg].re`（P1 照抄 ×2 导致隔块取样、谱形完全错误，逐位对拍暴露）；② **平均分支顺序**——weight gate 必须在 renorm/通道数修正**之前**（gate 用原始 freqchannels，core.cpp:1218-1220 在 :1249-1254 前），写盘跳过平均靠 datastreamsaveraged 参数显式传（默认 false，误传真会二次平均）；③ **kurtosis 时间戳/归一化与 autocorr STA 不同**——nsoffset/nswidth 是整个 subint（blockspersend×blockns）而非 ac 批次，无 weight gate、无 renorm，nChan = min(STADumpChannels, FNumChannels) 且 data 直接拷贝 sk（折叠已在 calculateAndAverageKurtosis 内做）；zeroKurtosis 只在 dokurtosis 时调用（core.cpp:703-704 条件调用，s1/s2 惰性分配在 zeroKurtosis 内）。
- **读路径的病态数据（真实观测 t25362 暴露，2026-09-16 ~ 18 修完）**：文件起点偏移、缺口、filler 三类共 14 条缺陷——**病根、症状指纹、判据、实施记录全部见 `fxcorr/reader-model.md`，改 datareader 前先读它**。此处只保留六条操作边界：① **无缺口、无 filler 的数据路径必须逐字节不变**（以 `gapspan` 为空为界，`gapshiftAt` 不被调用、`shiftFrameGaps` 的起始槽检查不触发；S6 与全部合成数据对拍依赖此点）；② **方向不能反**——缺口让读取位置**减**、filler 让读取位置**加**（`fileoffset += fillershiftbytes - gapshiftAt(...)×framebytes`）；③ **`gapinvalid` 必须每个 subint 重建**，否则缺口位置的块会被永久清掉、weight 全局塌陷；④ **缺口的账按时间轴位置记**（`gapspan`：缺口后第一帧的**文件位置** + 帧数 + **该缺口之前的 filler 帧数** `fillerbefore`），不要退回"检测到就整笔生效"——那正是 B5（跨 subint 边界时读取位置超前），判据在 `fxcorr/test/gaps/run_boundary.sh`；⑤ **读位置必须先扫后定**——`readSubint` 在读取前循环「算位置 → `scanSkippedStretch` 扫到位 → 重算」到收敛，**不要在读取之后才补扫**：filler 段让读位置前跳、跳过的区段里若藏着缺口，会缩短正要用的那个位置，读到的就是错位数据且被整块判无效（`reader-model.md` 4.7，判据 `fxcorr/test/gaps/run_filler.sh`）。`scanSkippedStretch` 是唯一入口，`checkFrameContinuity` 里那次调用此时为空操作、只为覆盖别的读取路径保留。⑥ **窗口按时间槽填满，不按输入字节**（B2，2026-09-19）——`readSubint` 的第一趟仍直接读进输出缓冲（无中断路径逐字节未变），只有这一段填不满 `slots` 个槽时（filler 占字节不占槽，吃掉了宽度）才改用可增长的 `inbuf` 翻倍多读，并**把扫描范围截断到"填满所需"**（多读的尾部属于下一个 subint，进本 subint 的账会让下一次读位置偏移）。三条随之而来的约束：**`nframes` 与 `slots` 不是一回事**（前者是为填满槽扫过的文件帧数、后者是缓冲区的槽数，只有无中断时相等；`READPOS` 两个都报，`test/reader/check_reader.py` 的 E3 用后者、E4 用前者）；**SwitchedPower 喂入要判 `lastReadContiguous()`**（跨过中断的 subint 缓冲是时间轴上的帧栅格、不是文件字节的一段，跳过不喂）；**`shiftFrameGaps` 只有一份算法**（`dryrun` 与实跑共用：dry-run 报"要填满得消费多少帧"，实跑搬运；分成两份即静默错位）。
- **数据质量信号与日志级别**：f 侧退出前打印 `GAPCHECK summary`（逐 subint 明细为 `READPOS`，每缺口一条 `GAPCHECK buffer/skipped`，每个重建过的 buffer 一条 `GAPCHECK holes`（无效块落点），读取位置异常漂移另有 `GAPCHECK boundary`）——**`READPOS` 的 `gapframes/dst/framens`（另有 `uncorr/passes`，2026-09-18 加）与 `GAPCHECK holes` 是定位 B5 这类"计数正常但位置错"缺陷的判据**，各字段语义与用法见 `fxcorr/reader-model.md` 第 6 节。summary 属 `info` 级（默认可见），两处逐条明细属 `verbose` 级；级别由环境变量 `FXCORR_LOGLEVEL`（`error`/`warn`/`info`/`verbose`/`debug`，默认 `info`）控制，宏 `FXLOG(level)` 定义在 `src/log.h`（只影响输出，不影响产物）。新增诊断输出按同一口径分配级别：错误与汇总常显，逐条明细进 `verbose`。**级别同时管住 fxcorrcommon 里上游代码的输出**（2026-09-17 补齐）：`configuration.cpp` 等直接调 `cinfo`/`cverbose`/`cdebug`（`Alert` 全局对象，不经 `FXLOG`），它们经 `difxMessageSendDifxAlert` 落地——该函数在 `difxMessagePort < 0` 时**退化为打印**（warning 及以上走 stdout、error 及以下走 stderr），而 `difxMessagePort` 初值 -1、只有 `difxMessageInit()` 会设置它，**f/x 都不调用该函数，所以这条兜底打印是唯一路径、这些消息永远不会组播**。两个 `main.cpp` 因此在 `Configuration` 构造前各调一次 `fxApplyAlertLevel()`：按 `FXCORR_LOGLEVEL` 逐档 `Alert::setAlertLevel(DIFX_ALERT_LEVEL_DO_NOT_SEND)`，`cerror`/`csevere`/`cfatal` 恒开。Configuration 加载阶段实测 85 行（64 DEBUG + 21 INFO）随之收放：默认 `info` 留 21 行、`warn` 归零、`debug` 档与改造前逐字节相同（无输出丢失）。**`log.h` 在 f/x 两处各一份，改一处必须同步另一处**（同 `ompcompat.h`）。
- **同一批产物不能并发重跑**：f 侧对同一 `fengine/<batch>/<station>/ds_N/` 是**覆盖写**、x 侧 SWIN 是**追加写**，同一批 f 任务起两份即交错写入同一批 `.sp`（症状隐蔽：进程都正常退出、GAPCHECK 都正常，只有文件大小不是记录长的整数倍）。用 `> log 2>&1 &` 在 ssh 里起后台任务**不可靠**——`nohup`／`setsid` 试了两次都"报告成功但随后看不见进程"，实际已在跑；清理重跑前务必 `ps` 确认无残留、并 `grep -c "GAPCHECK summary"` 确认每个日志只有一份。
- **SwitchedPower 喂入三条易错点（P6，2026-09-13 实测教训）**：① **块粒度必须照上游**：readbytes = (databufferfactor/numdatasegments)×maxbytes（test 配置 256/64 = 4×maxbytes ≈ 4 subint 量），每满一块喂一次（switchedpowerincrement = datarate<512Mbps 时 1）——feed 内 phase 从块起点帧头时间算起，切分点不同则半周期统计分组不同、sigma 必炸；② **subint 读入含帧对齐 guard 重叠**（sendbytes = 133 帧 vs subint 131.07 帧），直接拼块导致块内帧号不连续 → mark5access validate fail → 整帧 blank、统计全 0——喂入必须按 `reader.getLastFileOffset()` 跳过与上 subint 重叠的字节（上游喂的是连续 vdifmux 流，无重叠）；③ **块起点时间从帧头读**（mark5_stream_get_frame_time），字节连续 + 帧对齐即与上游同源，无需外部传时间；interval 恒 1s、TCAL FREQUENCY=0 时整个类不创建。生成器帧头两个 bug（legacy 位、字布局）见 `fxcorr/test/tcal/README.md`。

## V1 边界

- 输入格式（**P10 已补齐，2026-09-14**）：本地 **VDIF/VDIFL**（单线程）、**INTERLACEDVDIF**（多线程 corner-turn，KIND_MUXEDVDIF）、**MARK5B**（mark5bfix 修复）、**LBA 家族**（LBASTD/LBAVSOP/LBA8BIT/LBA16BIT，ASCII 头+raw）、**MKIV/VLBA/VLBN/KVN5B/CODIF**（mark5access 通用流，KIND_MK5STREAM）；K5VSSP/K5VSSP32 上游不可用仍报错退出；硬件访问（StreamStor/Mark6）不迁移。
- 多 mux thread 仅限 INTERLACEDVDIF 语义（fanout corner-turn）；vdifmux 输出为 mark5access 期望布局（含 EDV4 头）。
- **进程内多线程已支持（P3 2026-09-15）**：`OMP_NUM_THREADS=N` 启用块级并行（Mode 副本 + 块序归约，结果与串行逐位一致，usage.md）；未设 = 串行。构建经 AC_OPENMP（无 OpenMP 编译环境退化串行，ompcompat.h）。
- **单 scan**、文件序号 = scan 序号（每个数据文件一个 scan，从 scan 起点开始连续记录）。
- **delay 重对齐已补（P11 2026-09-14）**：修正起点早于数据起点（segment 0）时跳 FFT 块 + tosubtract 补偿（含上游 quirk 照抄）+ 2 字节/整数 ns 对齐 + 前 count 块 valid flags invalid，语义与上游 calculateControlParams 逐行对照（algo-plan P11 设计/实施记录）；valid flag 跨段续接子句在单文件连续读下自动等价（读窗口无段边界），不显式实现。delay≠0 对拍 6/6、回归 6/6（fxcorr/test/p11）。
- **数据文件起点 = batch 起点**（data-spec 5.2 file-per-batch 布局）：datareader 构造接收 batch 起点的绝对 sec/ns（main.cpp 由 batchstartjob 换算），locate 的字节偏移相对 batch 起点计算（batch 起点 = scan 起点时退化为原语义）。**数据块时间（*sec/*ns）须保持 scan 相对系**（Mode::setData 与 setOffsets 同系）；换算注意 batchstartabsns 是当日秒系、currentscanstartsec 是 scan 相对系（曾混用导致 nearestsample 越界、weight 全 0，2026-09-12 修复并回归对拍 6/6）。
- zoom band 的 .sp 不落盘（x 侧从父 .sp 切片，P4a 2026-09-13）；**autocorr.bin 已含 zoom 段**（total bands，Mode 的 zoom 自相关是父数组切片、getMode 工厂自动传 numzoombands，f 侧零改动）；多相位中心/脉冲星 binning 不涉及（f 侧本就没有）。
- **SwitchedPower 已支持（P6 2026-09-13）**：.input DATASTREAM 段 `TCAL FREQUENCY`（Hz，v2d antenna 级 `tcalFreq`）> 0 时 f 侧统计原始 2bit 高电平状态并写 `SWITCHEDPOWER_<mjd>_<sec>_<dsid>` 文本（SwitchedPower 类在 fxcorrcommon，mark5access 解析帧头）；= 0（默认）路径零改动。
- **交叉极化自相关已支持（P7 2026-09-13）**：autocorr.bin version 2 + header 加 u32 crosspol 标志（= WRITE AUTOCORRS && maxproducts>2，同 core.cpp:1288 条件）；crosspol=1 时每条批次记录 = 平行段（total bands）+ crosspol 段（同构），crosspol 谱/weight 从 Mode 的 autocorrelations[1]/weights[1] 取（getMode 工厂已把 writeautocorrs 传入，Mode 计算零改造），zoom 的 crosspol weight 从父 recorded band 取（getWeight(true, l)，父匹配同平行）。自相关 SWIN 写盘由 fxcorrcommon Visibility 的 autocorrwidth=2 路径零改造处理。

## 构建与注册

- 模板照 difxfilterbank：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f——fftw3f 头被 fxcorrcommon 的 architecture.h GENERIC 分支 include）/ Makefile.am / src/Makefile.am。
- install-difx 注册：components 字典、setNormalComponentsFalse、apptargets（dompicxx=True）、--doonly 帮助文本，共 4 处。

## 测试

- 资产在 `fxcorr/test/`：test.vex（上游 tests/Synthetic/test-usb.vex 原版）、test.v2d（2 站 T1/T2）、gen_test_vdif.py（生成 2bit 单 band VDIF；**低位先打包**，对齐 mark5access format_vdif.c lut2bit 位序——datasim 因上游 IPP 依赖无法 --noipp 构建，此脚本是替代品）、cmp_swin.py（SWIN 逐记录比较）。
- 测试机工作目录 `/root/fxcortest/`：config/（vex2difx + difxcalc 产物）+ TEST1.vdif/TEST2-usb.vdif + fengine/<batch_id>/。跑法见 memory（test-machine.md）。
- 已验证：两站 4 秒数据（8Ms/s 2bit，tone 1.5/1.0MHz）7 subint 跑通；autocorr 峰在 1.5/1.0MHz 通道；.sp header 字段与 data-spec 一致；对齐校验正负测试通过。
- 对拍（fxcorr-x 完成后）：SWIN 逐记录比较通过（数据完整段 6/6 全等，见 v1-plan 2.3 实施记录）。

**switched power（P6）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/tcal/README.md`（2026-09-13 验证过：switched power 前 2 个完整整秒窗与 mpifxcorr 逐位全等、SWIN 回归 6/6、位序对拍 BYTE-IDENTICAL、无 tcal 回归 6/6）。

**交叉极化自相关（P7）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/crosspol/README.md`（2026-09-13 验证过：dual-pol autocorr.bin v2 crosspol=1、SWIN 自相关 4 pol 落位正确；单 pol + WRITE AUTOCORRS 与 mpifxcorr 对拍 6/6、无 WRITE AUTOCORRS 回归 2/2、位序 BYTE-IDENTICAL）。

**Kurtosis STA + STA 频域平均分支（P9）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/sta/README.md`（2026-09-13 验证过：CHANS TO AVG 1 与 4 两轮 STA+kurtosis 对拍 318 条逐位全等、无开关 SWIN 回归 6/6、autocorr.bin 开/关 STA 逐字节一致；含 P1 stride bug 修复与基准控制消息时序的记录）。注意 mpifxcorr 基准的 dump 由 difxmessage 控制消息运行时开启（命令线程在 .input 读完后才 spawn，先发的消息会丢），基准抓包须 0.2s×40 轮连发 + 首 subint 过滤（见 README）。
