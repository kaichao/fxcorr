# fxcorr-x

baseline-based 相关器后端（XMAC 与积分）生产镜像：`fxcorr-base` + `bin/fxcorr-x`。

## 构建

前置：`fxcorr-builder:latest`、`fxcorr-base:latest` 已构建。

```bash
cd fxcorr/docker/fxcorr-x && make build    # 测试机上执行（docker host）
```

## 调用

workdir 整体挂载，容器内外绝对路径一致（data-spec 布局）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr-x:latest fxcorr-x <batch_id>
```
