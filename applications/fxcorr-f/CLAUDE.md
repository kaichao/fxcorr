# fxcorr-f 目录说明

station-based 相关器前端（F-Engine）：无 MPI 串行程序，逐站处理一个 batch 的本地原始数据，产出 band_XX.sp / pcal.bin / autocorr.bin（格式见 `fxcorr/data-spec.md` 5.3）。对拍目标为 mpifxcorr 的 station-based 段，核心算法（解包/条纹旋转/分数采样/FFT/自相关）零改动复用 fxcorrcommon 的 Mode。

## 调用方式

```
fxcorr-f <batch_id> <station> [workdir]
```

- 读 `workdir/fengine/<batch_id>/batch.json`（run_batch.sh 预写），取 start_mjd / n_subints / config_file。
- 读 `workdir/<config_file>`（.input，非 MPI 构造），Model 由 .calc 内建（无 .im 依赖）。
- 输出目录 `workdir/fengine/<batch_id>/<station>/`（自动创建）。
- 原始数据文件路径直接取自 .input 的 DATA TABLE（相对进程 cwd）。

## 文件与 mpifxcorr 对照

| 本目录 | 作用 | 对照源 |
|---|---|---|
| main.cpp | 参数/batch.json 解析、subint 循环驱动、ac 批次触发 | 骨架参照 core.cpp:694-801（zeroAutocorrelations→setValidFlags→setData→setOffsets→resetpcal→process 循环）+ :993-1003（acblockcount/maxacblocks 批次）；对齐校验为新增（data-spec 12 节） |
| datareader.{h,cpp} | 粗延迟 + VDIF 帧对齐定位 + 顺序读文件 | datastream.cpp:381-394（延迟/第一个 offsetns）、:516-573（采样→字节偏移）、vdiffile.cpp:417-444（帧对齐、framesin×framebytes）；sendbytes = getDataBytes（configuration.cpp，已帧对齐含 guard） |
| fenginewriter.{h,cpp} | .sp / pcal.bin / autocorr.bin 写盘 | 布局照 data-spec 5.3；写入序照 core.cpp:1145-1153（finalisepcal→getPcal）、:993-1003（autocorr 批次节奏）、:1260-1265（averageFrequency→getAutocorrelation） |

## 关键实现要点（易错，改前必读）

- **process(i, subloop) 的槽复用**：subloop 是缓冲槽索引（0..NUM BUFFERED FFTS-1），i 是 subint 内 FFT 全局序号。必须双循环 `fftloop × numBufferedFFTs`（core.cpp:786-801 同构），每 fftloop 结束立即写盘该批槽（writeSpectra），不能攒到 subint 尾——槽被后续 fftloop 覆盖。
- **Mode 零改造**：`Configuration::getMode(configindex, dsindex)` 工厂现成（configuration.cpp:857）；getFreqs/getPcal/getAutocorrelation/getWeight/getDataWeight 全为 public。
- **对齐校验**：batch 起点必须落在 subint 边界（容差 1µs，吸收 start_mjd 的 f64 表示误差），非对齐报错退出。batch.json 的 start_mjd 用精确 repr（如 58948.291666666664）。
- **valid flags**：datareader 按读入字节数置位（每 FFT 块 blockbytes 判界，对照 datastream.cpp:600-604）；无效 subint（bytes=0 → INVALID_SUBINT）由 Mode::process 内部全零处理（mode.cpp:655-667），无需特判。
- **weights 是 per-band 且槽式**：`getDataWeight(band, slot)` 的 slot 是缓冲槽、仅在 process 后有效——writeSpectra 时按槽收集进 weightaccum，subint 尾 flushWeights() fseek 回填 subint 头的占位区（.sp 布局不变）。**不能用全局 FFT 序在 process 前写**（曾导致越界读与垃圾权重）。
- **autocorr 按 maxacblocks 批次落盘**：每批次（满 maxacblocks，core.cpp:993-1003 同节奏）writeAutocorrelationBatch（averageFrequency + 落盘 + 权重）后立即 `mode->zeroAutocorrelations()`；autocorr.bin 每 subint 存 ceil(blockspersend/maxacblocks) 条记录（header 的 ac_batches 字段）。**不是每 subint 一条**——与 mpifxcorr 节奏不符则 fxcorr-x 对拍必挂。

## V1 边界

- 输入格式：本地 **VDIF**（VDIF/VDIFL），datasim 风格帧；其他格式（Mark5B 等）直接报错退出。
- **单 mux thread**（nummuxthreads=1）、无 fanout、帧粒度 1；vdifmux/corner-turner 不做（V2）。
- **单 scan**、文件序号 = scan 序号（每个数据文件一个 scan，从 scan 起点开始连续记录）。
- zoom band 不落盘；多相位中心/脉冲星 binning 不涉及（f 侧本就没有）。

## 构建与注册

- 模板照 difxfilterbank：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f——fftw3f 头被 fxcorrcommon 的 architecture.h GENERIC 分支 include）/ Makefile.am / src/Makefile.am。
- install-difx 注册：components 字典、setNormalComponentsFalse、apptargets（dompicxx=True）、--doonly 帮助文本，共 4 处。

## 测试

- 资产在 `fxcorr/test/`：test.vex（上游 tests/Synthetic/test-usb.vex 原版）、test.v2d（2 站 T1/T2）、gen_test_vdif.py（生成 2bit 单 band VDIF；**低位先打包**，对齐 mark5access format_vdif.c lut2bit 位序——datasim 因上游 IPP 依赖无法 --noipp 构建，此脚本是替代品）、cmp_swin.py（SWIN 逐记录比较）。
- 测试机工作目录 `/root/fxcortest/`：config/（vex2difx + difxcalc 产物）+ TEST1.vdif/TEST2-usb.vdif + fengine/<batch_id>/。跑法见 memory（test-machine.md）。
- 已验证：两站 4 秒数据（8Ms/s 2bit，tone 1.5/1.0MHz）7 subint 跑通；autocorr 峰在 1.5/1.0MHz 通道；.sp header 字段与 data-spec 一致；对齐校验正负测试通过。
- 对拍（fxcorr-x 完成后）：SWIN 逐记录比较通过（数据完整段 6/6 全等，见 impl-plan 2.3 实施记录）。
