# fxcorr 仓库说明

DiFX 2.9.1 的 clone（分支 fxcorr），正在实施"去 MPI、按 batch 拆分相关器"改造。改造需求与架构见 `fxcorr/README.md`，数据规范见 `fxcorr/data-spec.md`。改造代码前先读对应子目录的 CLAUDE.md。

## 目录地图

| 目录 | 作用 | 指引 |
|---|---|---|
| `mpifxcorr/` | 原 MPI 一体化相关器，拆分的源，保留不删（R7） | `mpifxcorr/CLAUDE.md` |
| `libraries/` | 14 个共享库，独立 autotools 包（含已建成的 fxcorrcommon） | `libraries/CLAUDE.md` |
| `applications/` | 独立程序（vex2difx、difx2fits 等）；fxcorr-f、fxcorr-x、fxcorr-sim 已建成 | 模板见下 |
| `fxcorr/` | 改造工作区：文档 + bash 编排脚本 + test/ 测试资产（含对拍工具 cmp_swin.py） | `fxcorr/CLAUDE.md` |
| `docker/` | 现有 CentOS8 镜像（clone 上游版，未涉及 fxcorr） | — |
| `utilities/` `doc/` `tests/` | 工具 / 文档 / 测试 | — |

## 构建体系（关键：无根 configure.ac）

- 顶层编排器是 **`install-difx`**（Python 脚本）：`source setup.bash` 后运行 `python3 install-difx`。
- `setup.bash` 设置 `DIFXROOT=/usr/local/difx`、`PKG_CONFIG_PATH=$DIFXROOT/lib/pkgconfig` 等。
- 每个库/应用是独立 autotools 包，相互之间只通过 pkg-config 发现，无跨目录统一构建。
- `install-difx` 支持 `--doonly=` / `--skip=` / `--also=` / `--pristine`。

## 新增组件要改的注册点（都在 install-difx）

新增 library / application 时，除新目录自带 `configure.ac` + `Makefile.am` + `<name>.pc.in` 外，必须改 `install-difx` 的 4 处：

1. `components` 字典（约 L211-239）：加 `"组件名" : True`
2. `setNormalComponentsFalse()`（约 L255-273）：加 `components["组件名"] = False`
3. `libtargets`（约 L463-483）/ `apptargets`（约 L509-539）/ `utiltargets`（约 L485-507）：追加条目，格式 `[目录名, difx_version, doreconf, dolibtoolize, doautoheader, dompicxx]`；`difx_version` 目前硬编码为 `''`，即直接在 `libraries/<name>/` 或 `applications/<name>/` 下构建
4. `--doonly` 帮助文本（约 L107-113）：同步组件名列表

## 新组件模板

- **C 应用**：照抄 `applications/difx2fits/` —— `AC_INIT` + `AC_CONFIG_HEADERS([config.h])` + `PKG_CHECK_MODULES(DIFXIO, difxio >= 3.8.0)` + 可选 FFTW（`AC_ARG_WITH` + `AM_CONDITIONAL`）+ `AC_CONFIG_FILES` 列出全部子目录 Makefile。
- **C++ 应用（多依赖）**：参考 `applications/difxfilterbank/` —— 各依赖走 `PKG_CHECK_MODULES`，可选依赖用第 4/5 参数 + `AM_CONDITIONAL`。
- **C++ 库**：照抄 `libraries/mark6meta/` —— 极简 4 手写文件（configure.ac、Makefile.am、mark6meta.pc.in、src/Makefile.am），详见 `libraries/CLAUDE.md`。

## 改造约定

- 改造目标是拆分 mpifxcorr 为 `applications/fxcorr-f`（station-based：解包、模型、通道化）+ `applications/fxcorr-x`（baseline-based：XMAC 与积分）+ `libraries/fxcorrcommon`（共享），另建 `applications/fxcorr-sim`（仿真数据生成器，datasim 的替身），以目录接口传数据，规范见 `fxcorr/data-spec.md`，V1 实施步骤见 `fxcorr/impl-plan.md`。
- 原 `mpifxcorr/` 保留、可并行构建（R7）；算法/接口改动先对照 `fxcorr/data-spec.md`。
- 拆分缝隙与可复用清单见 `mpifxcorr/CLAUDE.md`。

## 测试机相关
- build 过程在测试机运行
- 测试机登录：`ssh fxcoor`
- 同步文件至测试机：在 fxcorr目录下，运行`make sync`
