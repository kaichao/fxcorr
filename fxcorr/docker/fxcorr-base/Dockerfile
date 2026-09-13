# fxcorr-base：fxcorr 生产镜像公共底座
# 内容：debian 13-slim + 运行时依赖 + 从 fxcorr-builder 拷贝的 /usr/local/difx
# fxcorr-f / fxcorr-x / fxcorr-sim / difx-tools 都从这里派生。
# 构建上下文为本目录（内容全部来自基础镜像与 fxcorr-builder，无需仓库文件）。

FROM debian:13-slim

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

# 运行时依赖（DiFX 生态 .so 由下方 COPY 提供）
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgcc-s1 \
        libstdc++6 \
        libfftw3-single3 \
        libfftw3-double3 \
        libexpat1 \
        zlib1g \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# 只带 DiFX 生态 .so（不含 .a/.la 静态库）；share/（difxcalc 星历）与
# cfitsio 运行时仅 difx-tools 需要，移入该镜像；bin/ 由各生产镜像按需 COPY
COPY --from=fxcorr-builder:latest /usr/local/difx/lib/*.so* /usr/local/difx/lib/

ENV DIFXROOT=/usr/local/difx \
    PATH=/usr/local/difx/bin:${PATH} \
    LD_LIBRARY_PATH=/usr/local/difx/lib
