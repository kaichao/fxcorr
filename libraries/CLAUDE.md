# libraries 目录说明

14 个共享库，每个都是独立 autotools + libtool 包，通过 pkg-config 相互发现（无统一根构建）。fxcorrcommon 已建成。

## 库一览

| 目录 | 版本 | 作用 |
|---|---|---|
| difxio | 3.8.1 | 解析/生成 DiFX 配置与模型文件（.input/.im/.calc/.flag/.threads），核心结构 DifxInput |
| fxcorrcommon | 0.1 | 去 MPI 相关核心（station-based 算法 + 配置/模型 + SWIN 写盘），mpifxcorr libfxcorr.a 的共享库版，见下节 |
| difxmessage | 2.9.0 | 组播状态/告警/参数消息收发；内含 mark5ipc 子库（Mark5 锁） |
| mark5access | 1.7 | 原始基带解包：mark5_stream 两段式 API，支持 Mark4/Mark5B/VLBA/VDIF/CODIF 等 |
| vdifio | 1.6 | VDIF 帧头、vdifmux 多线程合流、corner-turner、vdifreader |
| codifio | 0.9 | CODIF 帧头解析（mark5access/vdifio 可选消费） |
| dirlist | 0.2 | Mark5/Mark6 介质目录清单 |
| mark6meta | 0.4 | Mark6 .meta 元数据读写 |
| mark6sg | 2.0.4 | Mark6 scatter-gather 类文件访问（含 jsmn JSON） |
| beamformer | 0.1.0 | 波束合成（Armadillo+BLAS+LAPACK） |
| python | 1.4 | DiFX Python 包（difxdb/difxfile/difxutil 等） |
| rpfits / vex / perl | — | RPFITS 格式、VEX 解析（bison/flex）、Perl 模块；走独立构建分支 |

## 关键库接口速查（fxcorr-f / fxcorr-x 依赖）

- **difxio**：头 `difxio.h`（顶层聚合）→ `difxio/` 子目录。`loadDifxInput()` / `loadDifxCalc()` 读配置；`evaluateDifxInputDelayRate()` / `evaluateDifxInputUVW()` 算延迟/UVW（fxcorr-x 必需）；`DifxParameters` 是 .input 底层解析器。**注意：mpifxcorr 目前不用 difxio**（自带 configuration.cpp 解析）。
- **difxmessage**：唯一公开头 `difxmessage.h` 装在 `$(includedir)` 根。`difxMessageInit(mpiId, identifier)` / `difxMessageReceiveOpen` / `difxMessageSendDifxStatus*`；组播地址由环境变量 `DIFX_MESSAGE_GROUP` / `DIFX_MESSAGE_PORT` 给定（setup.bash 默认 224.2.2.1:50201）；expat 为强制依赖。
- **mark5access**：`mark5_stream` 两段式构造（source + format），`new_mark5_format_mark5b/vdif/vlba/...`；解包入口 `mark5_unpack(ms, packed, unpacked, nsamp)` / `mark5_stream_decode`；`mark5_stream_open()` 自动探测格式；`USE_CODIFIO` / `USE_MARK6SG` 条件编译。
- **vdifio**：头 `vdifio.h` 装 `$(includedir)` 根。帧头读写（createVDIFHeader / getVDIFFrameMJD / setVDIF...）、`vdifmux()` 合流、`getCornerTurner()`、`vdifreader*` 多线程读取。

## 新库样板（照抄 mark6meta，最小 4 手写文件）

```
libraries/<name>/
├── configure.ac        # AC_INIT + LIBRARY_VERSION + LT_INIT + AC_PROG_CXX
│                       # + PKG_CHECK_MODULES(依赖) + AC_CONFIG_FILES([Makefile <name>.pc src/Makefile])
├── Makefile.am         # SUBDIRS = src；pkgconfigdir = $(libdir)/pkgconfig；pkgconfig_DATA = <name>.pc
├── <name>.pc.in        # Name/Description/Requires/Version/Libs: -L${libdir} -l<name>/Cflags: -I${includedir}
└── src/Makefile.am     # lib_LTLIBRARIES = lib<name>.la；-version-info $(LIBRARY_VERSION)
```

要点：`LIBRARY_VERSION` 必须 `AC_SUBST`；`AC_CONFIG_FILES` 漏列子目录 Makefile 会报 "cannot find Makefile"；若用 AX_OPENMP 需在 `m4/` 放 `openmp.m4`（参考 vdifio/m4/）。

## fxcorrcommon（已建成）

- 源 = mpifxcorr 的 `libfxcorr_a_SOURCES` 11 文件（configuration/pcal/mathutil/sysutil/mode/mk5mode/polyco/visibility/model/datamuxer/alert）+ 公共头 architecture.h/mpifxcorr.h/fraction.h（去 MPI 改动见 `mpifxcorr/CLAUDE.md` 与 fxcorr/impl-plan 2.1）。
- 依赖（configure.ac 探测）：difxmessage >= 2.9.0、mark5access >= 1.7、vdifio >= 1.6、codifio >= 0.2（CODIF_HEADER_BYTES 必需）；IPP 可选否则 fftw3+fftw3f；mark6sg/mark5ipc/dirlist 零引用不探测。
- 头装 `$(includedir)/fxcorrcommon/` 子目录（避免与 mpifxcorr 同名头冲突）；**pcal.h 引用 fraction.h，fraction.h 必须进安装头清单**（曾误归 internal 导致 fxcorr-f 编译失败）。
- 实用工厂：`Configuration::getMode(configindex, dsindex)`（各 Mode 子类创建，fxcorr-f 直接用，无需暴露 clock offsets 等内部 getter）。
- `fxcorr.pc` 名字被 mpifxcorr 占用（"不含 MPI 的对象库"），本库用 `fxcorrcommon.pc`。
