# fxcorr-fp 目录说明

**最后更新**：2026-10-13（**阶段 3 性能标定**——p419 真实规模：fp **0.07–0.2 s/任务**
（RSS 3.5 MB；页缓存口径），对照 f 的 8.4–8.6 s；全表见 `fxcorr/test/v9/README.md`。
**新建**——v9 链的 datastream 侧；设计见 `fxcorr/v9-plan.md`，阶段 0 判据 1/2/7 已实测通过）

v9 链的站级前端（**f′ / f-passthrough**）：无 MPI 串行程序，逐站读一个 batch 的本地原始
数据，做**读帧 + 帧对齐 + 坏帧/缺口簿记**，把 `DataReader::readSubint` 的产物原样写成
**`stream.bin`**（1× 数据流，格式见 `fxcorr/data-spec.md` 5.3.1）——**零 DSP**：不解包、
无 FFT、不构造 `Mode`。站级处理栈（unpack / pcal 提取 / FFT / 条纹旋转 / frac sample /
autocorr / kurtosis / STA）全部在 v9 链的 **fxcorr-xp** 侧。角色对照 mpifxcorr 的
datastream（读帧、整数对齐、搬 2-bit 原始样本）。

## 调用方式

```
fxcorr-fp <batch_id> <station> [workdir] [ds_index]
```

- 参数语义、workdir 定位（位置参数 > `FXCORR_WORKDIR` > `.`）、`FXCORR_REAL_PATH` /
  命名规则两态数据定位（v8）、batch.json / `.input` 加载、单 scan 校验、ds 组划分、
  输出目录 `<FENGINE_ROOT>/<batch>/<g>/<station>/ds_<N>/`（组层布局，data-spec 5.3）
  ——**全部与 fxcorr-f 相同**（本工具从 fxcorr-f 裁剪而来）。
- 输出：**`stream.bin`**（每 subint 一条定长 record）；**不再有** `band_XX.sp` /
  `autocorr.bin` / `pcal.bin`（谱与自相关随站级栈移到 fxcorr-xp，pcal.bin 与 PCAL
  文本由 xp 写）。
- **SWITCHEDPOWER 仍由本工具写**（原始帧字节统计，不解包；`vis/<exp>.difx/
  SWITCHEDPOWER_*`），喂入的块粒度、guard 重叠剔除与 `lastReadContiguous()` 判据
  与 fxcorr-f 逐行相同。
- **`FXCORR_STA` / `FXCORR_KURTOSIS` 置位时报一行警告后忽略**（这两类记录由
  fxcorr-xp 产出）——不静默，便于编排层发现开关挂错工具。
- **DifxMessage**：mpiId = dsindex+1（datastream 角色）、identifier = .input basename；
  节奏 Starting → 每 subint 两条 Diagnostic（DataConsumed / InputDatarate）→ Ending →
  Done；错误路径 Alert + Aborting——与 fxcorr-f 相同。
- `FXCORR_LOGLEVEL` 同 fxcorr-f（log.h 与 fxcorr-f 相同，改一处必须同步另一处）。

## 文件与来源对照

| 本目录 | 作用 | 来源 |
|---|---|---|
| main.cpp | 参数 / batch.json / 根解析、subint 循环（readSubint → SWP 喂入 → fillValidFlags → 写 record）、DifxMessage、三档时间戳（采集点 init/read/swp/write/fin/del，TSV 的 prog 列 = `fxcorr-fp`、线程列恒 0） | 从 fxcorr-f/main.cpp 裁剪：删除 Mode 构造与副本、FFT/归约/槽副本、FEngineWriter、PcalTextWriter、STA/kurtosis、maxacblocks/intTime 逻辑；保留读驱动、SWP 喂入、诊断与时间戳机制 |
| streamwriter.{h,cpp} | `stream.bin` 写盘：header（92 B）+ 每 subint 定长 record；写盘判错三步（`fflush`→`ferror`→`fclose`，v7-plan 17.2） | 新写；格式见 `fxcorr/v9-plan.md` 4 与 data-spec 5.3.1 |
| datareader.{h,cpp} | 与 fxcorr-f **逐字节相同的拷贝**（五路径读取、P11 delay 重对齐、P12/C 类缺口/filler 修正、READPOS/GAPCHECK 诊断家族——字段冻结契约） | fxcorr-f/src 拷贝（2026-10-13）；**改一处必须同步另一处**，回归用 `test/gaps/run_v9_compare.sh` |
| corrections.h / frametimeline.h / log.h | 与 fxcorr-f 相同（层 2 `Ledger` / 层 1 `walkFrameChain`+`placeFrames` 纯函数；日志级别） | fxcorr-f/src 拷贝 |
| （无 ompcompat.h） | 本工具**完全串行**：无并行区、不链 OpenMP（configure.ac 无 AC_OPENMP） | — |

## 关键实现要点（易错，改前必读）

- **record 语义 = f 的 setData/setOffsets 输入**：`status/validbytes/scan/sec/ns/
  offsetsec/offsetns/flags/data` 与 fxcorr-f 喂给 `Mode` 的调用参数一一对应
  （`sec/ns` 是 readSubint 的输出、原样含 `INVALID_SUBINT`；`offsetsec/offsetns` 是
  循环里算的）。invalid subint（readSubint 返回 0）写 status=1、data 区全 0；
  xp 端据此原样重放。**逐字节判据见测试节**。
- **定长 record**：data 区固定 sendbytes 字节，validbytes 之后的尾部补 0——内容是
  确定的（同一输入两次跑产物相同），xp 只按 validbytes 重放，不读填充。
- **validbytes 不是常数**：muxed VDIF / Mark5B / 窗口不完整路径下 readSubint 的
  返回值可小于 sendbytes——所以 record 必须携带它（v9-plan §4 的格式修订点）。
- **写盘判错**：`StreamWriter::finalise()` 返回 false 即 `fail()` 退出——full
  filesystem 必须在这里报（fengine 落盘量已降到 1×，但语义不变）。
- **读路径的病态数据**：与 fxcorr-f 同一份 datareader → `fxcorr/reader-model.md`
  的全部行为（缺口/filler/invalid 位/起点偏移）照旧；诊断输出（`GAPCHECK`/`READPOS`
  家族）与 fxcorr-f 逐行为冻结契约（`check_reader.py`、`test/gaps/` 脚本解析它们）。
- **建目录**：`mkdir -p` 到 `ds_<N>` 全路径（含批与组层）——与 fxcorr-f 相同，
  `run_batch.sh` 的批一级 mkdir 之外兜底。

## 构建与注册

- `configure.ac` / `Makefile.am` / `src/Makefile.am` 照 fxcorr-f 模板
  （PKG_CHECK_MODULES: fxcorrcommon + fftw3f + mark5access；**无 AC_OPENMP**）。
- install-difx 注册 4 处（components 字典、setNormalComponentsFalse、apptargets、
  --doonly 帮助文本）——2026-10-13 已加。
- 测试机上裸构建：`autoreconf -i && ./configure --prefix=/usr/local/difx && make
  && make install`。镜像（`fxcorr/docker/Dockerfile`）的 COPY 行**已加**（2026-10-13，
  随 v9 接入重建：`fxcorr/fxcorr:latest` = `e41dfca36e4f`，容器内 `fxcorr-fp` 可用）。

## 测试（2026-10-13，v9 阶段 0）

- **判据 1（流内容 = f 的 setData 序列）**：同一 workdir / batch / 数据上，f（临时
  dump 补丁、用后撤回、不提交）的 `setData` 输入序列与 fp 的 stream.bin **逐字节
  一致**（4,273,424 B；数据含缺口与 filler——病态路径一并覆盖）。
- **判据 2（尺寸 1×）**：`stream.bin` = 92 + nsubints×(28 + 4×flagwords + sendbytes)
  精确成立（test 配置：sendbytes 1,068,256、4 条 record）。
- **判据 7（gaps 七个用例全跑）**：
  - 七个 `fxcorr/test/gaps/run_*.sh` 在 fxcorr-f 上**全 PASS**（同日顺带修掉
    `Mode::blockPcalCapable` 在无 phasecal 配置上的空指针崩溃，见
    `libraries/CLAUDE.md`；七个脚本的 `.sp` 路径已随组布局补 `*/` 组层）；
  - **`fxcorr/test/gaps/run_v9_compare.sh`**（新资产）：15 个场景（七用例默认 spec
    + invalid 位置位）上 fp 与 f 的 `GAPCHECK`/`READPOS` 诊断**逐行一致**、无效块
    逐 subint 一致（`sp_valid.py` vs 新的 `stream_valid.py`）。
