# difx-tools

**最后更新**：2026-09-12（V2 镜像体系建成，此后未再变）

前/后处理单节点工具合集生产镜像：`fxcorr-base` + `vex2difx`、`difxcalc`（difxcalc11 的安装名）、`difx2fits`。

## 构建

前置：`fxcorr-builder:latest`、`fxcorr-base:latest` 已构建。

```bash
cd fxcorr/docker/difx-tools && make build    # 测试机上执行（docker host）
```

## 调用

workdir 整体挂载，容器内外绝对路径一致（data-spec 布局）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> difx-tools:latest vex2difx <config>.v2d
docker run --rm -v <workdir>:<workdir> -w <workdir> difx-tools:latest difxcalc <config>.calc
docker run --rm -v <workdir>:<workdir> -w <workdir> difx-tools:latest difx2fits <config>.input
```

difxcalc 的星历等运行时数据在 `/usr/local/difx/share/difxcalc/`（随 fxcorr-base 带入）。
