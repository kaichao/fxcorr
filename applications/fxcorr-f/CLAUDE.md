# fxcorr-f 目录说明

station-based 相关器前端（F-Engine）：无 MPI 串行程序，逐站处理一个 batch 的本地原始数据，产出 band_XX.sp / pcal.bin / autocorr.bin（格式见 `fxcorr/data-spec.md` 5.3）。对拍目标为 mpifxcorr 的 station-based 段，核心算法（解包/条纹旋转/分数采样/FFT/自相关）零改动复用 fxcorrcommon 的 Mode。

## 调用方式

```
fxcorr-f <batch_id> <station> [workdir]
```

- `workdir` 定位：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认 `.`。
- 读 `workdir/batches/<batch_id>.json`（run_batch.sh 预写），取 start_mjd / n_subints / config_file。
- 读 `workdir/<config_file>`（.input，非 MPI 构造），Model 由 .calc 内建（无 .im 依赖）。
- 输出目录 `workdir/fengine/<batch_id>/<station>/`（自动创建）。
- 原始数据文件路径直接取自 .input 的 DATA TABLE（相对进程 cwd）。
- **DifxMessage 状态发送**（algo-plan P1，difxmonitor 封装）：mpiId = dsindex+1（datastream/core 角色），identifier = .input basename。节奏：Starting → 每 subint 两条 Diagnostic（DataConsumed/InputDatarate）→ Ending → Done；错误路径 Alert + Aborting（fail helper）。RUNNING 不发（归 fxcorr-x）。`FXCORR_STA=1` 时每 autocorr 批次（writeAutocorrelationBatch 前、本批次 zeroAutocorrelations 前）发 DifxMessageSTARecord 到 `DIFX_BINARY_GROUP/PORT`（组装照 core.cpp averageAndSendAutocorrs 1195-1253：data = 实部之和×renorm、最低权重门槛 0.333、时间戳当日秒系）。host 模式组播（DIFX_MESSAGE_GROUP/PORT 未设即静默）；`FXCORR_RUN_MODE=container` 落盘 `meta/difxmsg/<exp>_<batch>_<station>.xml/.sta`（构造时截断，重跑幂等）。

## 文件与 mpifxcorr 对照

| 本目录 | 作用 | 对照源 |
|---|---|---|
| main.cpp | 参数/batch.json 解析、subint 循环驱动、ac 批次触发、**SwitchedPower 切块喂入（P6）** | 骨架参照 core.cpp:694-801（zeroAutocorrelations→setValidFlags→setData→setOffsets→resetpcal→process 循环）+ :993-1003（acblockcount/maxacblocks 批次）；对齐校验为新增（data-spec 12 节）；switched power 喂入照 vdiffile.cpp:945-964（段粒度 + increment） |
| datareader.{h,cpp} | 粗延迟 + VDIF 帧对齐定位 + 顺序读文件 | datastream.cpp:381-394（延迟/第一个 offsetns）、:516-573（采样→字节偏移）、vdiffile.cpp:417-444（帧对齐、framesin×framebytes）；sendbytes = getDataBytes（configuration.cpp，已帧对齐含 guard）；**getLastFileOffset（P6）** 暴露上次读入的文件偏移供喂入重叠剔除 |
| fenginewriter.{h,cpp} | .sp / pcal.bin / autocorr.bin 写盘 | 布局照 data-spec 5.3；写入序照 core.cpp:1145-1153（finalisepcal→getPcal）、:993-1003（autocorr 批次节奏）、:1260-1265（averageFrequency→getAutocorrelation）；**autocorr.bin 覆盖 total bands（P4a 2026-09-13）**：头/写循环用 getDNumTotalBands、per-band nchan = getDTotalFreqIndex 的 nchan/chanstoavg，zoom 段 weight 从父 recorded band 取（getWeight(false, l)，映射同 core.cpp:1324-1339）；**crosspol 段（P7 2026-09-13）**：header v2 + crosspol 标志（writeAutoCorrs && maxproducts>2），批次记录平行段后接 crosspol 段（getAutocorrelation(true, j)/getWeight(true, j)，zoom 的 crosspol weight 从父 band 取 getWeight(true, l)，照 core.cpp:1288-1301/1342-1369） |

## 关键实现要点（易错，改前必读）

- **process(i, subloop) 的槽复用**：subloop 是缓冲槽索引（0..NUM BUFFERED FFTS-1），i 是 subint 内 FFT 全局序号。必须双循环 `fftloop × numBufferedFFTs`（core.cpp:786-801 同构），每 fftloop 结束立即写盘该批槽（writeSpectra），不能攒到 subint 尾——槽被后续 fftloop 覆盖。
- **Mode 零改造**：`Configuration::getMode(configindex, dsindex)` 工厂现成（configuration.cpp:857）；getFreqs/getPcal/getAutocorrelation/getWeight/getDataWeight 全为 public。
- **对齐校验**：batch 起点必须落在 subint 边界（容差 1µs，吸收 start_mjd 的 f64 表示误差），非对齐报错退出。batch.json 的 start_mjd 用精确 repr（如 58948.291666666664）。
- **valid flags**：datareader 按读入字节数置位（每 FFT 块 blockbytes 判界，对照 datastream.cpp:600-604）；无效 subint（bytes=0 → INVALID_SUBINT）由 Mode::process 内部全零处理（mode.cpp:655-667），无需特判。
- **weights 是 per-band 且槽式**：`getDataWeight(band, slot)` 的 slot 是缓冲槽、仅在 process 后有效——writeSpectra 时按槽收集进 weightaccum，subint 尾 flushWeights() fseek 回填 subint 头的占位区（.sp 布局不变）。**不能用全局 FFT 序在 process 前写**（曾导致越界读与垃圾权重）。
- **autocorr 按 maxacblocks 批次落盘**：每批次（满 maxacblocks，core.cpp:993-1003 同节奏）writeAutocorrelationBatch（averageFrequency + 落盘 + 权重）后立即 `mode->zeroAutocorrelations()`；autocorr.bin 每 subint 存 ceil(blockspersend/maxacblocks) 条记录（header 的 ac_batches 字段）。**不是每 subint 一条**——与 mpifxcorr 节奏不符则 fxcorr-x 对拍必挂。
- **SwitchedPower 喂入三条易错点（P6，2026-09-13 实测教训）**：① **块粒度必须照上游**：readbytes = (databufferfactor/numdatasegments)×maxbytes（test 配置 256/64 = 4×maxbytes ≈ 4 subint 量），每满一块喂一次（switchedpowerincrement = datarate<512Mbps 时 1）——feed 内 phase 从块起点帧头时间算起，切分点不同则半周期统计分组不同、sigma 必炸；② **subint 读入含帧对齐 guard 重叠**（sendbytes = 133 帧 vs subint 131.07 帧），直接拼块导致块内帧号不连续 → mark5access validate fail → 整帧 blank、统计全 0——喂入必须按 `reader.getLastFileOffset()` 跳过与上 subint 重叠的字节（上游喂的是连续 vdifmux 流，无重叠）；③ **块起点时间从帧头读**（mark5_stream_get_frame_time），字节连续 + 帧对齐即与上游同源，无需外部传时间；interval 恒 1s、TCAL FREQUENCY=0 时整个类不创建。生成器帧头两个 bug（legacy 位、字布局）见 `fxcorr/test/tcal/README.md`。

## V1 边界

- 输入格式：本地 **VDIF**（VDIF/VDIFL），datasim 风格帧；其他格式（Mark5B 等）直接报错退出。
- **单 mux thread**（nummuxthreads=1）、无 fanout、帧粒度 1；vdifmux/corner-turner 不做（V2）。
- **单 scan**、文件序号 = scan 序号（每个数据文件一个 scan，从 scan 起点开始连续记录）。
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
- 对拍（fxcorr-x 完成后）：SWIN 逐记录比较通过（数据完整段 6/6 全等，见 impl-plan 2.3 实施记录）。

**switched power（P6）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/tcal/README.md`（2026-09-13 验证过：switched power 前 2 个完整整秒窗与 mpifxcorr 逐位全等、SWIN 回归 6/6、位序对拍 BYTE-IDENTICAL、无 tcal 回归 6/6）。

**交叉极化自相关（P7）检验步骤**：完整可复现命令与验收判据见 `fxcorr/test/crosspol/README.md`（2026-09-13 验证过：dual-pol autocorr.bin v2 crosspol=1、SWIN 自相关 4 pol 落位正确；单 pol + WRITE AUTOCORRS 与 mpifxcorr 对拍 6/6、无 WRITE AUTOCORRS 回归 2/2、位序 BYTE-IDENTICAL）。
