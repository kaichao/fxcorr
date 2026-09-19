# fxcorr-sim

**最后更新**：2026-09-12（V2 镜像体系建成，此后未再变）

仿真数据生成器生产镜像：`fxcorr-base` + `bin/fxcorr-sim`。性能测试用它产生大量模拟 VDIF 数据，再容器化调用下游 fxcorr-f/x。

## 构建

前置：`fxcorr-builder:latest`、`fxcorr-base:latest` 已构建。

```bash
cd fxcorr/docker/fxcorr-sim && make build    # 测试机上执行（docker host）
```

## 调用

workdir 整体挂载，容器内外绝对路径一致（data-spec 布局）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr-sim:latest fxcorr-sim ...
```
