# fxcorr/fxcorr

**最后更新**：2026-09-20（V5 P4：六个镜像合并为一个，此后为单镜像）

串行链路单镜像：`debian:13-slim` 运行时 + 从构建段 COPY 的 DiFX 生态 `.so` 与可执行文件。
一条链路上的全部命令都在 PATH 上，任何一步都用同一个镜像跑。

构建用两段式 Dockerfile（同一文件两个 stage）：stage 1 `builder` 装工具链、编译整个仓库并
安装到 `/usr/local/difx`，stage 2 `runtime` 只装运行时依赖并 COPY 需要的产物——构建期内容
（工具链、源码、`install-difx`）不进运行段。

## 镜像内容

| 命令 | 在哪一段链路 |
|---|---|
| `vex2difx` | `make_testdata.sh` 前处理：VEX → `.input` |
| `difxcalc` | 前处理：`.calc` → `.im` |
| `fxcorr-sim` | 造仿真 VDIF（datasim 的替身） |
| `fxcorr-f` | `run_batch.sh` 逐站 F 引擎 |
| `fxcorr-x` | `run_batch.sh` 单 batch X 引擎 |
| `difx2fits` | 后处理：SWIN → FITS |
| `wrap_vex2difx.sh` / `wrap_difxcalc.sh` | 前处理封装（V5 P5）：在 config/ 内调用原程序 + 把产物里的绝对路径规范化回相对 |
| `wrap_difx2fits.sh` | 后处理封装（V5 P5）：SWIN → FITS，产物落 `FXCORR_PRODUCT_ROOT`；实验级 |
| `roots.sh` | 上面三个脚本 source 的目录根解析（与库内 `FxcorrPath` 同规则） |

`mpifxcorr` 不进镜像（MPI 环境不进容器，`run_bench.sh` 对拍仍宿主直跑）。

## 构建

```bash
cd fxcorr/docker && make build    # 测试机上执行（docker host）
```

构建上下文是仓库根（`../..`），含 `install-difx` 与全部源码；根 `.dockerignore` 排除
`.git/`、`difx-data/`、运行时数据目录与**编译产物**（`*.o` 等——宿主遗留的会被 `COPY . /src`
带进去按时间戳复用，必须排掉）。代码变更后需重建，全量重编译约 13 分钟（8 核实测）。

产物：`fxcorr/fxcorr:latest` 与 `fxcorr/fxcorr:2.9.1`（**330MB** 实测）。

验证库依赖完整：

```bash
docker run --rm fxcorr/fxcorr:latest bash -c 'for f in /usr/local/difx/lib/*.so*; do ldd "$f" | grep -q "not found" && echo "MISSING in $f"; done; echo done'
```

## 调用

workdir 整体挂载，容器内外绝对路径一致（data-spec 布局）：

```bash
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest vex2difx <config>.v2d
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest difxcalc <config>.calc
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest fxcorr-f <batch_id> <station>
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest fxcorr-x <batch_id>
docker run --rm -v <workdir>:<workdir> -w <workdir> fxcorr/fxcorr:latest difx2fits <config>.input
```

实际使用不必手写：`FXCORR_RUN_MODE=container` 时 `run_batch.sh` / `make_testdata.sh` 的
`fxc` 封装自动加这层前缀。`difxcalc` 的星历等运行时数据在 `/usr/local/difx/share/difxcalc/`。

## 与 v2 镜像体系的差异

v2（`v2-plan.md` 第 2 节，已冻结）按进程角色切分为 6 个镜像：builder 出产物、base 出运行时
底座、f/x/sim/difx-tools 各一个生产镜像。落到"串行跑通一条链"这个实际用法上，代价是构建
顺序耦合（5 个镜像靠 `COPY --from=fxcorr-builder:latest`，必须按序 build）、`fxc` 的映射表
只为把命令路由回它所在的镜像、以及同一份 213MB 底座重复四遍。V5 的 P4 据此合并为单镜像。

**有意改动 v2 结论的两处**（v2-plan 是冻结文档，不回头改，记在这里）：

1. 镜像数 6 → 1，v2 验收第 1 条"六镜像构建成功"由单镜像一次构建取代；
2. 镜像名由裸名（`fxcorr-f` 等）改为带命名空间前缀的 `fxcorr/fxcorr`。

合并后体积 330MB（实测；事前按 v2 第 7 节推算为 ≈320MB），`strip` 仍不做。
