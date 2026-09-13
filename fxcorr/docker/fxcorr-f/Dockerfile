# fxcorr-f：station-based 相关器前端（解包、模型、通道化）生产镜像
# fxcorr-base + 本程序可执行。
# 调用：docker run --rm -v <workdir>:<workdir> fxcorr-f:latest fxcorr-f <batch_id> <station>

FROM fxcorr-base:latest

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

COPY --from=fxcorr-builder:latest /usr/local/difx/bin/fxcorr-f /usr/local/difx/bin/
