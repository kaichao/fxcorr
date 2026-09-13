# fxcorr V2 计划（第一版）

V1 已完成（验收 4/4）。本文定义 V2 的范围、镜像体系与任务，2026-09-12 定稿第一版，后续继续完善。

---

## 1. V2 定位

- **scalebox 编排、分片参数化**：放到其他仓库（license 隔离等原因），本仓库不做。分片是调度层概念，本仓库只保证 (batch_id, station) 参数化接口（V1 已具备）。
- **本仓库 V2 主线**：
  1. **容器化封装**：对已有工具做模块镜像，供 scalebox 容器再封装；
  2. **模块镜像的集成测试**：容器内跑通单 batch、对拍、性能测试数据链路；
  3. **V1 未解决的高级问题（算法改进）**：清单见第 5 节，优先级后续细化。

## 2. 镜像体系

| 镜像 | 基础 | 内容 | 说明 |
|---|---|---|---|
| `fxcorr-builder` | debian:13 | 构建工具链（build-essential、autotools、pkg-config 等）+ 依赖库 + 全量编译安装到 /usr/local/difx | 构建只在它里面做一次，产物由下层 COPY |
| `fxcorr-base` | debian:13-slim | 运行时依赖（libstdc++6 等）+ /usr/local/difx 的 DiFX 生态 .so | 各生产镜像公共底座 |
| `fxcorr-f` | fxcorr-base | fxcorr-f 可执行 | 独立生产镜像 |
| `fxcorr-x` | fxcorr-base | fxcorr-x 可执行 | 独立生产镜像 |
| `fxcorr-sim` | fxcorr-base | fxcorr-sim 可执行 | 独立生产镜像；性能测试场景用它产生大量模拟数据再调后续处理，统一容器化 |
| `difx-tools` | fxcorr-base | 前/后处理单节点工具：vex2difx、difxcalc11（difxcalc）、difx2fits（difx2mark4 按需） | 共用镜像 |

- **mpifxcorr 不进任何镜像**（MPI 环境不进容器）；run_bench.sh 对拍保留宿主直跑（V1 已通）。
- 镜像定义放 **`fxcorr/docker/`**，与上游 `docker/`（EOL CentOS8，不沿用）隔离。
- 构建与生产同为 debian 13 系，glibc 同版本，无跨发行版问题（原 rocky 9.8 仅保留为 docker host）。
- 镜像命名用裸名（`fxcorr-builder`、`fxcorr-base`、`fxcorr-f`、`fxcorr-x`、`fxcorr-sim`、`difx-tools`），tag `2.9.1`/`latest`；push 到 registry 时再定前缀。
- `fxcorr/docker/` 下每镜像一个子目录（Dockerfile + Makefile + README，模式参考 go-scalebox build/）：builder 的构建上下文 = 仓库根（Makefile 里 `../../..`，根 `.dockerignore` 排除 .git 与运行时数据目录），其余镜像上下文 = 各自子目录。

## 3. 构建链与运行方式

- 测试机（Rocky 9.8，docker 29.8.0 已装）退化为 **docker host**；编译、打包全部在容器内完成。
- 构建顺序：`fxcorr-builder` → `fxcorr-base`（COPY builder 的库）→ 各生产镜像（COPY 各自可执行）。
- 运行时依赖分层：生产容器自带 glibc（debian:13-slim），DiFX 生态 .so（fxcorrcommon、difxio、codifio、mark5access、vdifio、fftw、expat 等）从 builder 拷贝，保证与构建环境逐字节一致。
- 集成脚本（make_testdata.sh / run_batch.sh 等）**编排逻辑不变**，执行方式通过前缀变量切换：宿主直跑（V1 回归）或 `docker run --rm -v $WORKDIR:$WORKDIR <镜像>`。workdir 整体挂载，容器内外绝对路径一致，data-spec 布局天然适配。

## 4. 早期任务与风险

- **debian:13 构建全链验证**：install-difx --noipp 全绿是在 rocky 9.8 上验证的，构建环境换 debian:13 后需重验。已踩坑并解决（2026-09-12，构建验证进行中）：
  - `PKG_CONFIG_PATH` 必须显式设置（install-difx L584 直接拼接该变量，未设即 TypeError 崩溃；setup.bash 在容器内不 source）；
  - `MPICXX=g++` 必设（install-difx 对 dompicxx=True 组件直接拼接 MPICXX，difx2profile/vis2screen 等上游组件需它；fxcorr-f/x/sim 注册已改 dompicxx=False 用 g++）；
  - debian 下 difxcalc11 的 .o 由 gcc 默认编译、f77 链接却按 PIE 处理，报 `relocation R_X86_64_32S`——builder ENV 仅设 `LDFLAGS=-no-pie`。**编译参数必须保持默认**：实测 `-fno-pie` 编译会改变 fxcorr-f 运行行为（容器跑批数据范围越界，宿主直跑正常）；容器 vs 宿主对拍 6/6 全等已在修正后验证；
  - 依赖补丁：difxcalc11 需 libgsl-dev，vex2difx 需 bison/flex（包名差异：fftw3-dev / libexpat1-dev / libcfitsio-dev / zlib1g-dev）；
  - 运行时包名（fxcorr-base/difx-tools 用）：libfftw3-single3 / libfftw3-3 / libexpat1 / **libcfitsio10t64**（trixie t64 命名）/ zlib1g / libgsl28 + libgslcblas0（仅 difx-tools）。
  - 测试机网络：直连 registry-1.docker.io 超时，`/etc/docker/daemon.json` 已配 registry-mirrors（1panel/daocloud/dockerproxy）。
- builder 已定 **`--skip=mpifxcorr,difx2profile,vis2screen`**（difx2profile/vis2screen 依赖 mpifxcorr 安装的 fxcorr.pc，一并跳过；均不在容器需求清单）。
- difx-tools 清单已定：vex2difx、difxcalc（difxcalc11 安装名）、difx2fits；difx2mark4 按需再加。
- builder 构建命令：`python3 install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen`（--nodoc 免装 doxygen）。

## 5. V1 遗留算法改进清单（优先级已定稿 2026-09-13）

每项的动机分类、要解决的问题、预期效果、设计要点详见 **`algo-plan.md`**。排序依据：先补齐数据链路完整性（P0/P1），再补算力（P2/P3），后补科学功能（P4 按改动量），最后新能力（P5）。

| 优先级 | 改进项 | 动机分类 | 一句话说明 |
|---|---|---|---|
| P0 | `PCAL_*.pcal` 文件生成 | 功能未迁移 | ✅ 2026-09-13（f 按 intTime 聚合 tone 写实验级文本，追加幂等；单 batch 与 mpifxcorr 基准逐字节对拍通过，多 batch 追加/重跑幂等验证通过） |
| P1 | difxmessage 状态/STA 消息 | 功能未迁移 + 环境变化 | ✅ 2026-09-13（fxcorrcommon 增 difxmonitor 封装；f = datastream/core 角色发 Starting/Diagnostic/STA，x = manager 角色发 Starting/Running/Ending/Done；host 组播与 mpifxcorr 基准逐字段对拍通过，container 落盘 meta/difxmsg/ 与组播字节一致、重跑幂等） |
| P2 | 多 x 子集并行 | 串行环境新变化 | 基线切子集多进程并行，SWIN 合并（data-spec 12 节） |
| P3 | 多线程（f/x 进程内并行） | 串行环境新变化 | OpenMP 并行 FFT 批 / 基线循环，与 P2 叠加 |
| P4 | zoom band → 多相位中心 → 脉冲星 binning | 功能未迁移 | x 侧补齐科学功能，按改动量排序 |
| P5 | 网络输入 / 数据流化 | 串行环境新变化（新能力） | 网络流输入，依赖采集环境 |

## 6. 验收标准（第一版）

| # | 标准 | 状态 |
|---|---|---|
| 1 | fxcorr-builder / fxcorr-base / fxcorr-f / fxcorr-x / fxcorr-sim / difx-tools 六镜像在测试机构建成功 | ✅ 2026-09-12 |
| 2 | debian:13 构建全链验证通过（等价 install-difx 常规组件全绿） | ✅ 2026-09-12（--skip=mpifxcorr,difx2profile,vis2screen） |
| 3 | 容器内跑通单 batch：`docker run fxcorr-f/x` 输出 SWIN 与宿主直跑对拍全等（cmp_swin.py） | ✅ 2026-09-12（6/6 记录全等） |
| 4 | 集成脚本容器模式跑通（执行前缀切换） | ✅ 2026-09-13（`FXCORR_RUN_MODE=container` 开关 + fxc 封装，容器 SWIN 与宿主基准对拍全等；宿主默认模式回归通过） |
| 5 | fxcorr-sim 容器模式生成模拟数据 → fxcorr-f/x 容器模式处理全链路跑通（性能测试数据链路） | ✅ 2026-09-13（-n 2 多 batch 全容器链路，vex2difx/difxcalc/fxcorr-sim/fxcorr-f/x 均在容器内跑通） |
| 6 | difx-tools 镜像内 vex2difx / difxcalc / difx2fits 跑通 | ✅ 2026-09-13（difxcalc 需 libgfortran5 运行时，已补入镜像；difx2fits 出 FITS） |

## 7. 待细化

- 镜像体积（2026-09-13 已做一轮优化，现状）：builder 2.05GB（不优化）；base 与 fxcorr-f/x/sim 各 213MB；difx-tools 324MB（多出的 share/difxcalc 星历 28MB、gsl/gfortran/cfitsio 运行时 22MB、三 bin ~19MB）。已做：base 只 COPY `lib/*.so*`（去 .a/.la）、share 与 cfitsio 移到 difx-tools。未做：strip 可执行（difx 工具未 strip，vex2difx 15.9MB，strip 可省但需在 COPY 同层做或改 builder，暂不处理）
- 容器模式执行前缀已定：**单一环境变量开关 `FXCORR_RUN_MODE=container`**，脚本内 `fxc` 封装按工具→镜像映射加 docker run 前缀（workdir 整体挂载、cwd 与宿主直跑一致、FXSIM_NOISE/SEED 透传）
