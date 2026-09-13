# fxcorr-builder

fxcorr 构建环境镜像：debian 13 + DiFX 构建工具链与依赖库，构建时全量编译安装到 `/usr/local/difx`（`install-difx --noipp --nodoc --skip=mpifxcorr`）。

- 构建上下文 = 仓库根（Makefile 中 `../../..`），COPY 仓库后在镜像内完成编译；产物由下层镜像 COPY，不在运行时使用。
- mpifxcorr 不进任何镜像，故跳过（免装 MPI）；IPP 无许可，走 `--noipp`。
- 代码变更后需重建本镜像（全量重编译，约 30 分钟量级）。

## 构建

```bash
cd fxcorr/docker/fxcorr-builder && make build    # 测试机上执行（docker host）
```

产物：`fxcorr-builder:latest`。
