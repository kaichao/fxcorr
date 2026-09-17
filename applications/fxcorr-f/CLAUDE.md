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
- **真实观测数据三条（2026-09-16/17 t25362 实测，改 datareader 前必读）**：① **文件起点不一定落在 batch 起点**——各站记录系统的帧计数器不同相，文件可以从某一秒的中途开始（BA 首帧帧号 1269 = 起点晚 79.312 ms，S6 恰在秒边界）。`KIND_VDIF` 分支必须像 Mk5B/LBA 路径那样读首帧的 (sec, frame number) 算出 `anchorbytes`（早于 batch 起点为正、晚于为负；负值让起点前无数据的 subint 直接判无效，不要钳到 0）；漏算则整个文件按线性时间读偏，症状是「该站相关的**所有**记录（含无缺口积分）可见度差 ~1%、weight 差 0.4%，另一站完全一致」——极易被误判成缺口问题。**取整必须用 `floor(x+0.5)` 而不是 `(long long)(x+0.5)`**（四条格式路径的 `anchorbytes` 现已全部统一为 `floor`；Mk5B/MK5STREAM/LBA 三条原先靠 `batchstartabsns < filefirstabsns` 前置检查保证该值非负，故未暴露，改 `floor` 属防御）：文件晚于 batch 起点时该值为负，C++ 向零截断等于向上取整，锚点会整偏一帧（16 kfps 下 62.5 µs）；而 62.5 µs 比 5 MHz pcal 周期的整数倍恰好差 100 ns，于是 PCAL 里**序号为奇数的 tone 全部相位翻转**（t25362 的 BA 实测：每个 ds、每个积分都是"隔一个反号"），这类符号模式极易被误读成共轭或 sideband 问题。② **缺口不能用线性时间换算**——文件中间缺帧时字节流比时间轴短，`locate()` 的线性换算会读到缺口之后的错位数据且全部标为有效。处理链：`checkFrameContinuity` 扫帧号（VDIF word1 低 24 位，按 framesPerSecond 回绕）发现缺口，`readSubint` 从读取位置减去累计的 `gapshiftbytes`；`shiftFrameGaps` 把缺口之后的帧**整帧后移** Δ 帧（`mark5_unpack_with_offset` 按采样偏移走缓冲、假定位置 i 就是第 i 帧），空洞记入 `gapinvalid`，`fillValidFlags` 按 `f*payloadbytes/blockbytes + lastcount` 把对应块清掉（与它的 `(i-lastcount)*blockbytes < validbytes` 同一坐标系）。**三个坐标/去重陷阱**：(a) 缺口只在 **buffer 内部**找——跨 buffer 的帧号步进是"读取位置按 subint 长度推进、帧号按帧长推进"的正常漂移（合成无缺口数据上实测 ±4 帧），当成缺口会让 `gapshiftbytes` 虚增、读取位置倒退，反把同一缺口越检越多；(b) 累加必须**按缺口在文件中的位置去重**（用**修正后**的读取位置算 `lastfileoffset + i*framebytes`；用帧号不行——它每秒回绕、跨秒不可比；用未修正位置也不行——同一缺口在不同 buffer 里相差整数个 subint），否则一个缺口被相邻 subint 重复计入，实测 145 帧虚增到 536 帧；(c) `gapinvalid` 必须**每个 subint 重建**。**无缺口时这两条路径都不动作**（S6 产物逐字节不变），`gapinvalid` 每个 subint 重建（曾漏清导致缺口位置的块在所有后续 subint 里被清掉、weight 全局塌陷）。③ **filler 帧：占字节、不占时间**（t25362 BA thread 2 实测 1162 帧／8 段；其余 7 个 BA ds 与 S6 全部为 0，可与外部帧号扫描逐一对齐）——记录系统在数据中断处写入**帧头全零**的帧（word0/1/2 全零，恰好也满足 invalid 位=0、framelen=0）。判据是段前帧号 4053、段后 4064（只缺 10 帧）而**段本身有 81 帧**：帧号跳跃只反映真实丢失，与 filler 帧数无关，所以它们**占文件字节、不占时间轴**。**只按帧号步进判缺口会把每个 filler 段边界当成巨量缺失**（4053→0 按模 fps 算成缺 11946 帧，0→4064 再算缺 4063 帧，单个段边界就虚增约一个整秒 16000 帧）：t25362 实测 `missing frames 64066`（真值 143，虚增 448 倍），读取位置前移累积到 514 MB／4 秒，症状是「**该 ds 参与的所有 frq** 从第一个 filler 段起可见度错位，其他 ds 完全正常」。修复链：`vdifIsFiller`（invalid 位或帧头全零）识别后**从帧号链里跳过**（真实缺口因此恰在段边界处正确显现），filler 帧数按整帧累加到 `fillershiftbytes`，`readSubint` 改为 `fileoffset += fillershiftbytes - gapshiftbytes`；`shiftFrameGaps` 改成"按帧号重排"——丢弃 filler、缺口留空，末尾未填充的槽位也记 `gapinvalid`（那里的数据不在本 buffer）。**三个陷阱**：(a) **方向不能反**——缺口让文件**短**于时间轴（位置减），filler 让文件**长**于时间轴（位置加）；(b) 位置前移会**跳过**一段从未扫描的文件（长度恰等于刚发现的 filler 字节），其中的帧必须补扫，否则漏计（实测只计到 626／1162）；(c) 但跳过的区段**不全是 filler**（filler 段之后还跟着数据帧），按"距离＝全 filler"推断会超调、使下一次前移更多，正反馈失控（实测虚增到 44650），必须**实扫该区段**逐帧判定（`countFillerRange`，段长几百 KB、只在发现 filler 段时触发）。**无 filler 的文件路径完全不变**（S6 逐 ds 0 gap 0 filler）。
- **数据质量信号**：f 侧每个缺口打印一行 `GAPCHECK buffer ... frameno A -> B (step, missing)`，退出前打印 `GAPCHECK summary`（buffers/frames/discontinuities/missing frames/**filler frames**/boundaries）。缺口多的数据（真实记录中断）这行就是体检结论，排查对拍差异前先看它——**`filler frames` 应与外部帧号扫描的 filler 数逐一致**（t25362 BA：8 个 ds 分别 0/0/1162/0/0/0/0/0）。
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
