# fxcorr 构建手册

**最后更新**：2026-09-20（V5 P4：容器镜像合并为单镜像）

覆盖本仓库全部组件的两种构建方式：**集成构建**（`install-difx` 编排器，推荐）与**独立构建**（单包 autotools，调试/重编译常用）。

## 构建体系总览

- 仓库**无根 configure.ac**：每个 library / application 是独立 autotools 包，相互之间只通过 pkg-config 发现（`PKG_CONFIG_PATH=$DIFXROOT/lib/pkgconfig`）。
- 顶层编排器是 Python 脚本 `install-difx`：`source setup.bash` 后 `python3 install-difx`，按序构建并安装全部组件到 `$DIFXROOT`（默认 `/usr/local/difx`）。
- 目标组件：`libraries/` 下 14 个共享库（含 fxcorrcommon）+ `applications/` 下独立程序（含 fxcorr-f / fxcorr-x / fxcorr-sim）+ `utilities/` 工具。
- 新增组件须在 install-difx 注册 4 处（components 字典、setNormalComponentsFalse、libtargets/apptargets/utiltargets、--doonly 帮助文本），见根 `CLAUDE.md` 构建体系节。

## 前置依赖

- autotools（autoconf / automake / libtool）、pkg-config
- C/C++ 编译器：gcc/g++（测试机为 gcc 11.5）；MPI 环境用于 mpifxcorr（测试机 openmpi 的 mpicc/mpicxx/mpirun 软链在 /usr/bin，install-difx 的 `CXX=/usr/bin/mpicxx` 依赖此）
- 库：fftw3（含 single 精度 fftw3f）、expat
- IPP（Intel Performance Primitives）：**必须关掉**。无 IPP 的环境（测试机）带 `--noipp`，否则 genipppc 崩溃；**有 IPP 的环境同样要带**——理由见下条。fxcorr 三应用与 fxcorrcommon 不依赖 IPP，上游 datasim 因 subband.{h,cpp} 硬编码 IPP 在 --noipp 下无法构建（由 fxcorr-sim 替代，不装 datasim）
- **为什么有 IPP 也要关**（2026-09-28 查明）：让 `fxcorrcommon` 启用 IPP 不是慢，是**错**——IPP 构建下 `architecture.h` 把 `vectorAlloc_f64` 映射到 `ippsMalloc_64f`，而 `model.cpp` 用它分配的 `Model` 延迟模型数组数值不对，于是 fxcorr-sim 生成的仿真数据延迟偏差约一个样本，表现为**极少数 2bit 量化电平翻转**（t25362 4 站 1.024 s batch：33511 字节 / 131 MB，0.025%），**全程没有任何报错**；而且只在"真实观测参数 + 延迟非零 + 逐字节对拍"三者齐备时暴露。细节见 `libraries/CLAUDE.md`
- **排查提示**：先看 `/usr/local/difx/lib/pkgconfig/ipp.pc` 是否存在——`--noipp` 的作用就是**跳过生成它**（缺了它，`fxcorrcommon` 的 `PKG_CHECK_MODULES(IPP, ipp)` 失败、`HAVE_IPP` 不被定义）。漏带一次 `--noipp` 它就会留下，之后每次 configure 都启用 IPP。`fxcorrcommon` 的 `configure.ac` 已修（选项真正 gate 探测、默认改为禁用），但**始终带 `--noipp`** 仍是第一道保险
- **改了 `fxcorrcommon` 的构建配置后，三个应用都要重编**：否则会以 `undefined symbol: ...Ipp32fc...` 之类的形式失败（旧二进制引用的 IPP 版符号在新库里不存在）

## 方式 A：集成构建（install-difx，推荐）

```bash
source setup.bash                 # 设 DIFXROOT、PKG_CONFIG_PATH、PATH 等
python3 install-difx              # 全量构建安装
python3 install-difx --noipp      # 无 IPP 环境
```

常用选项：

| 选项 | 作用 | 示例 |
|---|---|---|
| `--doonly=` | 只构建列出的组件（逗号分隔） | `--doonly=fxcorrcommon,fxcorr-f,fxcorr-x,fxcorr-sim` |
| `--skip=` | 跳过列出的组件 | `--skip=datasim` |
| `--also=` | 额外构建列出的组件 | `--also=fxcorr-sim` |
| `--pristine` | 清理后重新构建 | `--pristine --doonly=fxcorr-f` |

仅构建 fxcorr 改造相关组件（最小集，依赖库需先装好）：

```bash
source setup.bash
python3 install-difx --noipp --doonly=fxcorrcommon,fxcorr-f,fxcorr-x,fxcorr-sim
```

## 方式 B：独立构建（单包，调试/重编译常用）

每包一套标准 autotools 流程；**生成物不跨机共享**（macOS 与 Linux 的 autotools 版本错配），各机自行 autoreconf：

```bash
cd libraries/<name>               # 或 applications/<name>
autoreconf -fi
./configure --prefix=/usr/local/difx
make -j8
make install
```

注意：

- configure 前先 `source setup.bash`，保证 PKG_CONFIG_PATH 指向 `$DIFXROOT/lib/pkgconfig`，否则依赖库（fxcorrcommon 等）发现失败。
- 包间依赖顺序：先装依赖库再装应用（fxcorr-f/x/sim 依赖 fxcorrcommon、fftw3f；fxcorr-sim 另需 vdifio 的头文件）。
- 重编译单个应用只改该包：`cd applications/fxcorr-f && make -j8 && make install`。

## 容器构建（fxcorr/docker/，V2 建成；V5 P4 起为单镜像）

**V5 P5 起镜像内还带 fxcorr 的四个编排脚本**（`roots.sh` + `wrap_vex2difx.sh` / `wrap_difxcalc.sh` / `wrap_difx2fits.sh`），容器内可直接调用。

V2 起编译与打包全部在容器内完成，测试机（Rocky 9.8）退化为 **docker host**。镜像定义在 `fxcorr/docker/`：**一份两段式 `Dockerfile` + `Makefile` + `README.md`**（V5 P4 把 v2 的 6 个镜像子目录合并为一个，v2 体系见 `v2-plan.md`）。

| stage | 基础 | 内容 |
|---|---|---|
| stage 1 `builder` | debian:13 | 工具链+依赖，构建上下文=仓库根（Makefile `../..`），镜像内跑 `install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen` 全量编译（后两者依赖 mpifxcorr 安装的 fxcorr.pc，须一并跳过）；构建期内容不进运行段 |
| stage 2 `runtime` | debian:13-slim | 运行时依赖 + 从 builder COPY 的 `/usr/local/difx/lib/*.so*`、`/share`（difxcalc 星历）与 `bin/` 下六个可执行：vex2difx、difxcalc、fxcorr-sim、fxcorr-f、fxcorr-x、difx2fits |

构建（在测试机执行，前置 `make sync`）：

```bash
ssh fxcorr 'cd /root/fxcorr/fxcorr/docker && make build'    # 一次出镜像，全量编译约 13 分钟（8 核实测）
```

产物：`fxcorr/fxcorr:latest` 与 `fxcorr/fxcorr:2.9.1`。

debian:13 与 rocky 9.8 构建差异（已实测解决，详见 v2-plan.md 第 4 节）：

- 容器内不 source setup.bash：`PKG_CONFIG_PATH`、`MPICXX=g++` 必须显式设（install-difx 直接拼接这两个变量，未设崩溃）；补 libgsl-dev、bison、flex。
- difxcalc11 链接报 `relocation R_X86_64_32S`：仅设 `LDFLAGS=-no-pie`（编译参数保持默认——`-fno-pie` 编译会改变 fxcorr-f 运行行为导致数据越界）。
- 运行时包名：`libcfitsio10t64`（trixie t64 命名）、`libfftw3-double3`/`libfftw3-single3` 等。
- 测试机 docker 需 registry mirror（`/etc/docker/daemon.json` 已配 1panel/daocloud/dockerproxy），否则拉 debian:13 超时。

镜像调用：workdir 整体挂载，容器内外绝对路径一致（容器单 batch 对拍 6/6 全等已实测，2026-09-12；P4 单镜像后待重跑）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest fxcorr-f <batch_id> <station>
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest difxcalc <config>.calc
```

实际不必手写：`FXCORR_RUN_MODE=container` 时 `run_batch.sh` / `make_testdata.sh` 的 `fxc`
封装自动加这层前缀（P4 起不再按工具选镜像）。

## 测试机构建工作流

构建与验证在 Linux 测试机上做。**宿主集成构建仅用于 V1 回归与对拍**（mpifxcorr 需要宿主 MPI）；V2 常规构建走上面的容器构建节。

| 机器 | 登录 | 仓库 | 用途 |
|---|---|---|---|
| 测试机（Rocky 9.8） | `ssh fxcorr` | `/root/fxcorr` | 小规模回归（`gaps/`、`test/multids/`、`roots/`）|
| 大机器 | `ssh difx` | `/home/scalebox/fxcorr` | 真实数据与规模验证（V6 的 S0/S1/S3）|

**`make sync` 一次同步两台**（仓库根 `Makefile` 的 `HOSTS = fxcorr difx`）——但**同步不等于重编**：

```bash
# 本地：同步仓库到两台机器（在仓库根执行）
make sync

# 测试机：集成构建（必须先 source setup.bash，见下）
ssh fxcorr 'cd /root/fxcorr && source setup.bash && python3 install-difx --noipp --doonly=fxcorrcommon,fxcorr-f,fxcorr-x,fxcorr-sim'

# 大机器：构建，再补一次安装（安装段要 root，见下）
ssh difx 'cd /home/scalebox/fxcorr && source setup.bash && python3 install-difx --noipp --doonly=fxcorr-sim'
ssh difx 'cd /home/scalebox/fxcorr/applications/fxcorr-sim && sudo -n make install'

# 运行工具需附加库路径
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && fxcorr-f <batch_id> <station>'
```

**三个实测坑**（2026-09-28）：

- **`install-difx` 不会自己 source `setup.bash`**：非交互 `ssh` 下不 source 它，第一步就报 `RuntimeError: DIFXROOT must be defined`（本文件此前写的"内部已 source"是错的）。
- **两台机器要各自重编**：`make sync` 只搬源码，`/usr/local/difx/bin` 里的二进制不动。只重编一台、在另一台上跑，**旧二进制会静默给出旧行为**——2026-09-28 就因此在 `difx` 上把完整链的 6m22s 当成了轻量模式的结果（那里的旧二进制不认 `FXSIM_LIGHT`）。**判据是"这台机器上的二进制是不是刚编的"**，不是"源码同步过没有"。
- **`difx` 上安装要 `sudo -n make install`**：`make` 不需要，只有装到 `/usr/local/difx/bin`（属 root）需要。`install-difx` 在这里会以 `Permission denied` 中断——**构建已完成，补一次安装即可**；但 `--doonly` 列多个组件时它会在第一个组件的安装段就停住，后面的组件不会被构建，**一次一个组件**。

工具链实测记录：gcc 11.5、automake 1.16.2、fftw/expat 由 dnf 装、无 IPP（须 --noipp）。

## 相关

- 组件注册与模板：根 `CLAUDE.md` 构建体系节（C 应用照 difx2fits、C++ 多依赖应用照 difxfilterbank、C++ 库照 mark6meta）
- 工具用法：`usage.md`；数据布局：`data-spec.md`
