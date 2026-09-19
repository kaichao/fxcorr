# fxcorr 构建手册

**最后更新**：2026-09-13（构建体系自 V1 定稿后未变）

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
- IPP（Intel Performance Primitives）：**可选**。无 IPP 的环境（测试机）必须带 `--noipp`，否则 genipppc 崩溃；fxcorr 三应用与 fxcorrcommon 不依赖 IPP，上游 datasim 因 subband.{h,cpp} 硬编码 IPP 在 --noipp 下无法构建（由 fxcorr-sim 替代，不装 datasim）

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

## 容器构建（fxcorr/docker/，V2）

V2 起编译与打包全部在容器内完成，测试机（Rocky 9.8）退化为 **docker host**（镜像体系见 `v2-plan.md`）。镜像定义在 `fxcorr/docker/`，每镜像一个子目录（Dockerfile + Makefile + README，模式参考 go-scalebox build/）。

| 子目录 | 镜像 | 基础 | 说明 |
|---|---|---|---|
| `fxcorr-builder/` | fxcorr-builder | debian:13 | 工具链+依赖，构建上下文=仓库根（Makefile `../../..`），镜像内跑 `install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen` 全量编译（后两者依赖 mpifxcorr 安装的 fxcorr.pc，须一并跳过） |
| `fxcorr-base/` | fxcorr-base | debian:13-slim | 运行时依赖 + 从 builder COPY 的 `/usr/local/difx/lib` 与 `/share`（含 difxcalc 星历数据） |
| `fxcorr-f/` `fxcorr-x/` `fxcorr-sim/` | 同名 | fxcorr-base | 各自 COPY 一个 bin |
| `difx-tools/` | difx-tools | fxcorr-base | vex2difx + difxcalc + difx2fits，另补 libgsl28/libgslcblas0 运行时 |

构建顺序（在测试机执行，前置 `make sync`）：

```bash
ssh fxcorr 'cd /root/fxcorr/fxcorr/docker/fxcorr-builder && make build'    # 全量编译，约 30 分钟
ssh fxcorr 'cd /root/fxcorr/fxcorr/docker/fxcorr-base && make build'        # 依赖 builder:latest
ssh fxcorr 'cd /root/fxcorr/fxcorr/docker/fxcorr-f && make build'           # 依赖 base:latest（x/sim/difx-tools 同法）
```

debian:13 与 rocky 9.8 构建差异（已实测解决，详见 v2-plan.md 第 4 节）：

- 容器内不 source setup.bash：`PKG_CONFIG_PATH`、`MPICXX=g++` 必须显式设（install-difx 直接拼接这两个变量，未设崩溃）；补 libgsl-dev、bison、flex。
- difxcalc11 链接报 `relocation R_X86_64_32S`：仅设 `LDFLAGS=-no-pie`（编译参数保持默认——`-fno-pie` 编译会改变 fxcorr-f 运行行为导致数据越界）。
- 运行时包名：`libcfitsio10t64`（trixie t64 命名）、`libfftw3-double3`/`libfftw3-single3` 等。
- 测试机 docker 需 registry mirror（`/etc/docker/daemon.json` 已配 1panel/daocloud/dockerproxy），否则拉 debian:13 超时。

镜像调用：workdir 整体挂载，容器内外绝对路径一致（容器单 batch 对拍 6/6 全等已实测，2026-09-12）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr-f:latest fxcorr-f <batch_id> <station>
docker run --rm -v <workdir>:<workdir> -w <workdir> difx-tools:latest difxcalc <config>.calc
```

## 测试机构建工作流

构建与验证在 Linux 测试机（Rocky 9.8，`ssh fxcorr`，仓库在 `/root/fxcorr`）上做。**宿主集成构建仅用于 V1 回归与对拍**（mpifxcorr 需要宿主 MPI）；V2 常规构建走上面的容器构建节：

```bash
# 本地：同步仓库到测试机（在 fxcorr/ 工作区目录下）
make sync

# 测试机：集成构建
ssh fxcorr 'cd /root/fxcorr && python3 install-difx --noipp --doonly=fxcorrcommon,fxcorr-f,fxcorr-x,fxcorr-sim'
# （install-difx 内部已 source setup.bash 环境；手动独立构建时需 bash -c "source /root/fxcorr/setup.bash && ..."）

# 运行工具需附加库路径
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && fxcorr-f <batch_id> <station>'
```

工具链实测记录：gcc 11.5、automake 1.16.2、fftw/expat 由 dnf 装、无 IPP（须 --noipp）。

## 相关

- 组件注册与模板：根 `CLAUDE.md` 构建体系节（C 应用照 difx2fits、C++ 多依赖应用照 difxfilterbank、C++ 库照 mark6meta）
- 工具用法：`usage.md`；数据布局：`data-spec.md`
