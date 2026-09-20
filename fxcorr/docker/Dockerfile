# fxcorr/fxcorr：串行链路单镜像（两段式）
# stage 1 builder：debian 13 + 构建工具链，全量编译安装到 /usr/local/difx
# stage 2 runtime：debian 13-slim + 运行时依赖 + 从 builder COPY 的 .so / share / 六个可执行
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
RUN python3 install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen

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
