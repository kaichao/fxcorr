# fxcorr-builder：fxcorr 构建环境镜像
# 内容：debian 13 + DiFX 构建工具链与依赖库，全量编译安装到 /usr/local/difx
# 下层镜像（fxcorr-base 等）从这里 COPY 构建产物。
# 构建上下文为仓库根（见 Makefile：../../..），含 install-difx 与全部源码。

FROM debian:13

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

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

# install-difx 常规组件构建；mpifxcorr 不进任何镜像（免装 MPI）；
# difx2profile/vis2screen 依赖 mpifxcorr 安装的 fxcorr.pc，一并跳过（均不在容器需求清单）
RUN python3 install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen
