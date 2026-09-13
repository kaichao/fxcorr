# fxcorr-x：baseline-based 相关器后端（XMAC 与积分）生产镜像
# fxcorr-base + 本程序可执行。
# 调用：docker run --rm -v <workdir>:<workdir> fxcorr-x:latest fxcorr-x <batch_id>

FROM fxcorr-base:latest

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

COPY --from=fxcorr-builder:latest /usr/local/difx/bin/fxcorr-x /usr/local/difx/bin/
