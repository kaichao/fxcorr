# fxcorr 构建手册

**最后更新**：2026-09-29（V7：新增「目标集群构建（V7，Slurm）」一节——Slurm 集群上实测的**七个坑**与一键脚本 `build.sh`/`env.sh`；此前 2026-09-20 V5 P4 容器镜像合并为单镜像）

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

## 目标集群构建（V7，Slurm）

V7 的部署与多核验证在**另一套 Slurm 集群**上做（与上面两台机器都不同）。节点经 Slurm 申请，
登录节点 `ssh p419-n1` → `ssh -p 50022 <计算节点ip>`；实测节点 **30 核 / 123.5 GiB / 62 GB tmpfs**。

| 项 | 值 |
|---|---|
| 源码 | `/public/home/cstu0036/fxcorr/src`（rsync 自 mac，排除 `.git`，217 MB） |
| **构建脚本** | **`/public/home/cstu0036/fxcorr/build.sh`**——七项修复已固化，一键重建 |
| **运行环境** | **`/public/home/cstu0036/fxcorr/env.sh`**（`source` 后用） |
| 安装前缀 | **`/public/home/cstu0036/fxcorr/install`**——**不用 `/usr/local/difx`**：那是 root 的，且计算节点是临时作业，产物放共享盘下次申请节点还在 |
| MPI | `/opt/hpc/software/mpi/hpcx/v2.7.4/gcc-7.3.1`（`DIFXMPIDIR` 要指向它，`setup.bash` 默认的 `/usr` 不对） |
| 共享存储 | `/public/home/cstu0036/fxcorr`（4 TB）与 `/work2/cstu0036/fxcorr`（250 GB），均为 ParaStor 并行文件系统 |

```bash
# 本地：同步源码（经两级 ProxyJump 直连计算节点）
rsync -az --exclude='.git/' -e 'ssh -o ProxyCommand="ssh -W %h:%p p419-n1" -p 50022' \
      ./ cstu0036@<节点IP>:/public/home/cstu0036/fxcorr/src/

# 计算节点：一键构建
cd /public/home/cstu0036/fxcorr && bash build.sh

# 使用
source /public/home/cstu0036/fxcorr/env.sh && fxcorr-f <batch_id> <station>
```

**八个实测坑**（2026-09-29）。**前七个根因是同一个**：这台机器的 FFTW **只装了单精度、且开发
文件不规范**（`/public/software/mathlib/fftw`），而 DiFX 的构建系统假设 `fftw3` / `fftw3f` 都有
标准开发包（头 + `.so` + `.pc`）；**第八个（GSL）是同一个模式**，在补构建前处理程序时撞到：

| # | 现象 | 处理 |
|---|---|---|
| 1 | `/usr/bin/python3` 是 **2.7.5**（RHEL 7），而 `install-difx` 是 Python 3 脚本 | PATH 前置 miniforge 的 3.12（`/public/software/apps/miniforge3-25.3.1-0/bin`） |
| 2 | 只有 `fftw-libs-double` 运行时（`/usr/lib64/libfftw3.so.3`），无 `.so` 软链与 `.pc` → `PKG_CHECK_MODULES(fftw3)` 失败 | 自建 patch 目录：软链 `libfftw3.so` + 写 `fftw3.pc` |
| 3 | `install-difx` 默认要生成 `ipp.pc`，而 `/opt/intel` 不存在（`ValueError: invalid literal for int(): 'unknown'`） | **`--noipp`** |
| 4 | `ld: cannot find -lfftw3f`——pc 里声明的 `-L` 没进链接命令 | 把 `libfftw3f.so` 也软链进 patch 目录（链接器会在**所有** `-L` 路径里找） |
| 5 | `vdifPhase.c: fftw3.h: No such file`——编译只带 `-I$DIFXROOT/include` | 把 `fftw3.h` 软链进 `$DIFXROOT/include` |
| 6 | **单精度 pc 叫 `fftwf.pc`**（非标准命名），而 DiFX 的 configure 找 `fftw3f.pc` | 补一个 `fftw3f.pc` 指向 patch 目录 |
| 7 | 运行时报 `libfftw3f.so.3.5.7: cannot open shared object file`——**那个库没有标准 SONAME**，链接器把完整文件名记进了 DT_NEEDED | `LD_LIBRARY_PATH` 必须含 `/public/software/mathlib/fftw/lib64` |
| 8 | **补构建前处理程序时**：`difxcalc11` 报 `Package requirements (gsl) were not met` | GSL 也只在 `/public/software/mathlib/gsl/2.7`（系统仅有 `.so.0` 运行时）→ 它的 `lib/pkgconfig` 进 `PKG_CONFIG_PATH`、`lib` 进 `LD_LIBRARY_PATH`。**与坑 2 是同一个模式** |

> **坑 8 不是"额外的"——它说明这台机器上非系统路径的库不止 FFTW 一个**。以后遇到
> `Package requirements (X) were not met`，先去 `/public/software/mathlib/` 找同名目录。

**排查顺序**：先看 `pkg-config --modversion fftw3 fftw3f` 与 `--libs` 的输出——第 2 / 4 / 6 都是
它的输出不对；第 7 是链接产物记的 SONAME，与 configure 无关，只能靠运行时库路径解。

**实测构建命令与结果**（2026-09-29，退出码 0）：

```bash
# ① fxcorr 三程序 + 依赖（落在 install/bin/）
python3 install-difx --noipp \
  --doonly=difxio,codifio,difxmessage,mark5access,vdifio,fxcorrcommon,fxcorr-f,fxcorr-x,fxcorr-sim

# ② 前处理程序（P1 造数要用 make_testdata.sh，它走 vex2difx + difxcalc；dirlist 是
#    vex2difx 的依赖，容易漏）
python3 install-difx --noipp --doonly=dirlist,vex2difx
python3 install-difx --noipp --doonly=difxcalc11      # 需要 GSL，见坑 8
```

**运行期差异**（与构建无关，但同样耗时）：

- **`env.sh` 必须把 miniforge 的 `python3` 前置**：编排脚本（`make_testdata.sh` /
  `run_batch.sh`）与 `gen_vex.py` 都是 Python 3，而 `/usr/bin/python3` 是 **2.7.5**，
  不前置就在语法错误上打转。
- **bash 是 4.2.46**（RHEL 7），测试机是 5.x：`set -u` 下展开**空数组**（`${ARR[*]}`）在
  bash < 4.4 会报 unbound variable。`make_testdata.sh` 撞到过两处（`${TONES[*]}`、`${ENVS[*]}`），
  **已修**（加 `:-`）。写新脚本时记住这条。

**不要改 DiFX 源码来绕这些坑**——七项全是环境事实，补软链与 pc 文件即可；改源码会让本仓库与
上游分叉，而这些差异换一台装齐 FFTW 的机器就不存在。

## 相关

- 组件注册与模板：根 `CLAUDE.md` 构建体系节（C 应用照 difx2fits、C++ 多依赖应用照 difxfilterbank、C++ 库照 mark6meta）
- 工具用法：`usage.md`；数据布局：`data-spec.md`
