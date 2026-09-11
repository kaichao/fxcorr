# mpifxcorr 目录说明

DiFX 原 MPI 一体化相关器，是 fxcorr-f / fxcorr-x 拆分的源。本目录保留不删（R7），拆分以拷贝 + 改造方式进行。

## 构建

- autotools：`aclocal -I m4; autoconf; autoheader; automake -a -c; ./configure --prefix=$DIFXROOT CXX=mpicxx; make`
- 依赖（configure.ac，全走 pkg-config）：`ipp`（可选，否则 `fftw3`+`fftw3f`）、`difxmessage >= 2.9.0`、`vdifio >= 1.6`、`mark6sg >= 2.0.4`、`mark5access >= 1.7`、可选 `mark5ipc` / `dirlist` / `streamstor`；OpenMP 宏在 `m4/openmp.m4`
- 产物：`mpifxcorr`、`neuteredmpifxcorr`、`libmpifxcorr.a`、**`libfxcorr.a`**（无 MPI 子集）、`fxcorr.pc` / `mpifxcorr.pc`

## 源码结构（src/ 69 文件 ≈3.3 万行）

| 类别 | 文件 | 作用 |
|---|---|---|
| A 主控/MPI | mpifxcorr.cpp、fxmanager.{h,cpp}、architecture.h.in | main() 角色分发、管理节点调度/收数/写盘、MPI tag 与向量/FFT 宏 |
| B 数据流 | datastream.{h,cpp}、mk5.*、nativemk5.*、mark5b*.*、vdif*.*、datamuxer.{h,cpp}、switchedpower.{h,cpp} | 读文件/网络、粗延迟、环形缓冲、pthread 读线程；各记录格式 DataStream 子类；VDIF corner-turn |
| C station-based 算法 | mode.{h,cpp}、mk5mode.{h,cpp}、pcal.{h,cpp}、model.{h,cpp}、polyco.{h,cpp} | 解包/条纹旋转/分数采样/FFT/自相关（Mode::process）、格式解包、脉冲校准、延迟模型、脉冲星权重 |
| D baseline-based | core.{h,cpp}、visibility.{h,cpp} | **同时含两段计算**；长期积分、SWIN/ASCII/PCAL 写盘 |
| E 配置/工具 | configuration.{h,cpp}（最大，3772 行）、alert.{h,cpp}、mathutil.*、sysutil.*、fraction.h、watchdog.*、mark5utils.* | .input 全解析、日志、小工具 |

## 拆分缝隙（最重要）

**不在文件边界，在 `core.cpp` 的 `Core::processdata()`（:657）函数体内：**

- `:786-801` station-based 段：`modes[j]->process(i, fftsubloop)` —— 解包 + 条纹旋转 + 分数采样 + FFT + 自相关 + pcal
- `:814-982` baseline-based 段：相位阵加权或常规 XMAC（`vectorAddProduct_cf32`）、脉冲星分支
- 之后：`uvshiftAndAverage()`（:1431）、自相关平均与 STA 上报、`copyPCalTones()`

**fxcorr-f 落盘的中间产物** = 每 subloop 的 `Mode::getFreqs()/getConjugatedFreqs()`（fftoutputs/conjfftoutputs）+ `getDataWeight` + valid flags + scan/sec/ns + 配置/频率映射；**fxcorr-x 的输入**与此一一对应。文件格式约定见 `fxcorr/data-spec.md`（fengine/ 的 `band_XX.sp`）。

## MPI 耦合分布

- 高密度：datastream.cpp(28)、mpifxcorr.cpp(25)、fxmanager.cpp(19)、core.cpp(19)、mk5.cpp(11)、configuration.cpp(11)
- **零 MPI**：model、mode、visibility、pcal、polyco（+ mathutil、sysutil）—— 天然可下沉 fxcorrcommon

## 可复用先例（改造成本最低点）

- `src/Makefile.am` 已定义 `libfxcorr.a`（"不需要 MPI 的对象"），`libfxcorr_a_SOURCES = configuration pcal mathutil sysutil mode mk5mode polyco visibility model datamuxer alert` —— 即 fxcorrcommon 起步清单。
- `Configuration` 有非 MPI 构造版本（configuration.cpp:99，`enableMpi=false`），已被 difx_monitor / difxfilterbank / datasim 复用。
- `pcal.cpp` 自带独立 `main()`（`-DUNIT_TEST`），可作单模块回归测试样板。
- **mpifxcorr 完全不依赖 difxio**：`.input`/`.calc`/`.im` 解析全部自带。fxcorr 是否改用 difxio（`loadDifxInput` 等）是改造决策点，见 `libraries/CLAUDE.md`。

## 节点分工与数据流

- rank 0 = MANAGER（FxManager：调度、收可见度、长期积分、写盘、监控）；rank 1..numdatastreams = DataStream 节点（只做 IO + 粗延迟）；其余 = Core（Mode::process + XMAC）。
- 流：DataStream 读线程 → `CR_PROCESSDATA`（原始字节）+ `CR_PROCESSCONTROL`（controlbuffer）→ Core → `results` 经 `CR_VALIDVIS` → FxManager → Visibility 写盘（`DIFX_%05d_%06d.s%04d.b%04d`、`PCAL_*.pcal`）。
- 时序：FxManager 按 `nsincrement` 推进，环形缓冲深度 `Core::RECEIVE_RING_LENGTH=4`。

## 改造注意

- `architecture.h.in` 的 MPI tag 定义与向量函数宏（IPP/FFTW/generic 映射）是将来共享头的起点。
- `utils/*.py`（startdifx.py、genmachines.py 等）是 MPI 编排脚本，对应 fxcorr 的 bash 编排（`fxcorr/run_batch.sh`）。
- 拆出代码是否保留 difxmessage 依赖是设计决策：difxmessage 是组播状态消息，本身不是 MPI。
