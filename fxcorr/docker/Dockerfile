# fxcorr/fxcorr：串行链路单镜像（两段式）
# stage 1 builder：debian 13 + 构建工具链，全量编译安装到 /usr/local/difx
# stage 2 runtime：debian 13-slim + 运行时依赖 + 从 builder COPY 的 .so / share /
# 六个可执行 + 四个 fxcorr 编排脚本（roots.sh 与三个 wrap_*.sh，见文件末尾）
# 取代 v2 的 6 个镜像（fxcorr-builder/base/f/x/sim/difx-tools），构建不再是链式的。
# 构建上下文 = 仓库根（见 Makefile：../..），COPY 仓库后在 stage 1 内完成编译。

# ---------- stage 1：builder（构建期内容不进运行段） ----------
FROM debian:13 AS builder

# 构建工具链 + 依赖（--noipp 不装 IPP；mpifxcorr 跳过，不装 MPI）
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        libtool \
        pkg-config \
        bison \
        flex \
        gfortran \
        python3 \
        perl \
        libfftw3-dev \
        libexpat1-dev \
        libcfitsio-dev \
        libgsl-dev \
        zlib1g-dev \
        rsync \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# LDFLAGS=-no-pie：仅链接阶段关 PIE（difxcalc11 的 .o 由 gcc 默认编译、f77 链接却
# 按 PIE 处理，报 relocation R_X86_64_32S）。编译参数保持默认——实测 -fno-pie
# 编译会改变 fxcorr-f 运行行为（数据范围越界）。
ENV DIFX_VERSION=2.9.1 \
    DIFXROOT=/usr/local/difx \
    DIFX_PREFIX=/usr/local/difx \
    PKG_CONFIG_PATH=/usr/local/difx/lib/pkgconfig:/usr/local/difx/lib64/pkgconfig \
    MPICXX=g++ \
    LDFLAGS=-no-pie

WORKDIR /src
COPY . /src

# install-difx 常规组件构建；mpifxcorr 不进镜像（免装 MPI）；
# difx2profile/vis2screen 依赖 mpifxcorr 安装的 fxcorr.pc，一并跳过（均不在容器需求清单）
#
# --makeflags="-j4"：install-difx 的 MAKEcommand 默认是裸的 "make "，各组件都单线程
# 编译（该脚本 --help 第 84 行正是为这个用途留的开关，值会追加到每次 make 调用上）。
# **为什么固定 4 而不是 $(nproc)** ——在 8 核机器上实测（2026-09-30，三次串行、同一
# 环境、每次都从干净源码重编，见 fxcorr/docker/README.md）：
#
#     -j1  5m50.7s        -j4  3m27.8s (1.69x)        -j8  3m22.3s (1.74x)
#
# 收益到 4 就基本饱和：-j8 只比 -j4 快 2.7%，user 时间反而多 1m23s。因为能并行的只有
# 各组件内部的 make，而 autoreconf、每组件的 configure、以及 install-difx 逐个遍历
# 三十多个组件这三段本来就是串行的（实测 CPU 利用率 -j4 仅 203%、-j8 仅 252%）。
# 固定 4 的另一层好处：给构建机留出余量，且不受容器里 nproc 口径变化的影响。
RUN python3 install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen \
        --makeflags="-j4"

# ---------- stage 2：runtime ----------
FROM debian:13-slim AS runtime

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

# 运行时依赖：libgcc/libstdc++/fftw/expat/zlib 供 fxcorr 链路；
# gsl/gslcblas/gfortran 供 difxcalc，cfitsio 供 difx2fits（DiFX 生态 .so 由下方 COPY 提供）
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgcc-s1 \
        libstdc++6 \
        libfftw3-single3 \
        libfftw3-double3 \
        libexpat1 \
        zlib1g \
        libgsl28 \
        libgslcblas0 \
        libgfortran5 \
        libcfitsio10t64 \
        python3 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# vex2difx 等读 DIFX_VERSION 环境变量（缺失仅 warning，设上免扰）
ENV DIFXROOT=/usr/local/difx \
    DIFX_VERSION=2.9.1 \
    PATH=/usr/local/difx/bin:${PATH} \
    LD_LIBRARY_PATH=/usr/local/difx/lib

# DiFX 生态 .so（不含 .a/.la 静态库）；share/ 是 difxcalc 的星历等运行时数据
COPY --from=builder /usr/local/difx/lib/*.so* /usr/local/difx/lib/
COPY --from=builder /usr/local/difx/share /usr/local/difx/share

# 串行链路全部命令：前处理 vex2difx/difxcalc、造数 fxcorr-sim、
# 运行 fxcorr-f/fxcorr-x、后处理 difx2fits
COPY --from=builder /usr/local/difx/bin/vex2difx /usr/local/difx/bin/
COPY --from=builder /usr/local/difx/bin/difxcalc /usr/local/difx/bin/
COPY --from=builder /usr/local/difx/bin/fxcorr-sim /usr/local/difx/bin/
COPY --from=builder /usr/local/difx/bin/fxcorr-f /usr/local/difx/bin/
COPY --from=builder /usr/local/difx/bin/fxcorr-x /usr/local/difx/bin/
COPY --from=builder /usr/local/difx/bin/difx2fits /usr/local/difx/bin/

# fxcorr 的规范适配脚本（V5 P5 Q15/Q16）：原 difx 程序（vex2difx / difxcalc /
# difx2fits）不认识根变量，由这几个脚本转换路径后调用，所以必须与程序同在
# 镜像里。roots.sh 只被它们 source，同样放这里（$SCRIPTDIR 取同目录）。
COPY --from=builder /src/fxcorr/roots.sh /usr/local/difx/bin/
COPY --from=builder /src/fxcorr/wrap_vex2difx.sh /usr/local/difx/bin/
COPY --from=builder /src/fxcorr/wrap_difxcalc.sh /usr/local/difx/bin/
COPY --from=builder /src/fxcorr/wrap_difx2fits.sh /usr/local/difx/bin/

# 两个编排脚本也进镜像（2026-09-29，V7 P6）：此前镜像里只有上面四个适配脚本，
# run_batch.sh / make_testdata.sh 留在宿主，于是容器化调用必须 --bind 源码目录才能跑。
# 它们都需要 python3（校验段要解析 batch.json），所以上面 runtime 段的依赖里加了
# python3——否则会以 "line 106: python3: command not found" 立刻失败（V7 实测）。
# 两者都 source 同目录的 roots.sh，放这里即可（$SCRIPTDIR 取同目录）。
COPY --from=builder /src/fxcorr/make_testdata.sh /usr/local/difx/bin/
COPY --from=builder /src/fxcorr/run_batch.sh /usr/local/difx/bin/

# set_roots.sh（2026-10-08）：根记录改写——由 fxcorr-prep 模块在流水线任务内调用
# （prep 落在有数据面的节点；2026-10-07–10-08 曾短暂由 app-fxcorr 的 router 进程
# 写，因 p419 上 router 落控制面、无数据面而移回数据面）。source 同目录 roots.sh。
COPY --from=builder /src/fxcorr/set_roots.sh /usr/local/difx/bin/

# fxinput.py 是 run_batch.sh 的**运行期依赖**（2026-09-29 起，V7 P3 前）：`.input`
# 解析、前置校验、DATA TABLE 软链、ds 组划分都从 run_batch.sh 的内嵌段搬到了这里，
# 它按 `$SCRIPTDIR/fxinput.py` 找——所以两者必须同目录，漏了这个 COPY 会让容器里
# 每一次跑批都以 "No such file or directory" 立刻失败。三个 wrap_*.sh 与
# roots.sh 不依赖它（它们是独立命令），只有 run_batch.sh 依赖。
COPY --from=builder /src/fxcorr/fxinput.py /usr/local/difx/bin/
