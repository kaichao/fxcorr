# fxcorr-f

**最后更新**：2026-09-12（V2 镜像体系建成，此后未再变）

station-based 相关器前端（解包、模型、通道化）生产镜像：`fxcorr-base` + `bin/fxcorr-f`。

## 构建

前置：`fxcorr-builder:latest`、`fxcorr-base:latest` 已构建。

```bash
cd fxcorr/docker/fxcorr-f && make build    # 测试机上执行（docker host）
```

## 调用

workdir 整体挂载，容器内外绝对路径一致（data-spec 布局）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr-f:latest fxcorr-f <batch_id> <station>
```
