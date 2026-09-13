# fxcorr-sim：仿真数据生成器生产镜像
# fxcorr-base + 本程序可执行。
# 性能测试场景用它产生大量模拟数据再调后续处理，统一容器化。
# 调用：docker run --rm -v <workdir>:<workdir> fxcorr-sim:latest fxcorr-sim ...

FROM fxcorr-base:latest

ARG VERSION

LABEL maintainer="wukaichao@gmail.com" \
      version="${VERSION}"

COPY --from=fxcorr-builder:latest /usr/local/difx/bin/fxcorr-sim /usr/local/difx/bin/
