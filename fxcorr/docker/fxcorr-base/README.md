# fxcorr-base

**最后更新**：2026-09-12（V2 镜像体系建成，此后未再变）

fxcorr 生产镜像公共底座：debian 13-slim + 运行时依赖（libstdc++6、fftw、expat、cfitsio、zlib）+ 从 `fxcorr-builder:latest` COPY 的 `/usr/local/difx`（DiFX 生态 .so 与工具）。

fxcorr-f / fxcorr-x / fxcorr-sim / difx-tools 均从此镜像派生，各自只加可执行文件。

## 构建

前置：`fxcorr-builder:latest` 已构建。

```bash
cd fxcorr/docker/fxcorr-base && make build    # 测试机上执行（docker host）
```

产物：`fxcorr-base:latest`。

验证库依赖完整：

```bash
docker run --rm fxcorr-base:latest bash -c 'for f in /usr/local/difx/lib/*.so*; do ldd "$f" | grep -q "not found" && echo "MISSING in $f"; done; echo done'
```
