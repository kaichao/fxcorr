# difx-tools：前/后处理单节点工具合集生产镜像
# fxcorr-base + vex2difx、difxcalc（difxcalc11 安装名）、difx2fits
# （difx2mark4 为默认关闭组件，按需再加）
# 调用：docker run --rm -v <workdir>:<workdir> difx-tools:latest <vex2difx|difxcalc|difx2fits> ...

FROM fxcorr-base:latest

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

# difxcalc 链接 gsl 与 gfortran 运行时，difx2fits 用 cfitsio，补库
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgsl28 \
        libgslcblas0 \
        libgfortran5 \
        libcfitsio10t64 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# difxcalc 星历等运行时数据（仅本镜像需要）
COPY --from=fxcorr-builder:latest /usr/local/difx/share /usr/local/difx/share

# vex2difx 等读 DIFX_VERSION 环境变量（缺失仅 warning，设上免扰）
ENV DIFX_VERSION=2.9.1

COPY --from=fxcorr-builder:latest /usr/local/difx/bin/vex2difx /usr/local/difx/bin/
COPY --from=fxcorr-builder:latest /usr/local/difx/bin/difxcalc /usr/local/difx/bin/
COPY --from=fxcorr-builder:latest /usr/local/difx/bin/difx2fits /usr/local/difx/bin/
