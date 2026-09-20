# fxcorr V5 计划

> **进行中**（2026-09-19 开）——V5 处理 `v4-plan.md` 末节「后续方向」里条件已具备的两件半：
> **补两个合成盲区**（P1、P2）与 **invalid 位定案**（P3），三件均已完成；"要真实数据"那条
> 的触发条件未变，仍在等。
> **P4（容器镜像合并）、P5（目录路径环境变量化）、P6（fxcorr-sim 造数能力）不来自
> v4 的后续方向**，是 2026-09-19 / 09-20 新立的三项：P4 已完成（2026-09-20，验收 5/5），
> P5、P6 方案已定、待实施。
> **定案、证据与验收写在这里，根因与判据写在 `reader-model.md`**（4.10 B7、4.11 FILL_PATTERN、
> 4.12 invalid 位）——两处分工与 v1–v4 一致：本文件只承载路线与验收。

## V5 范围（2026-09-19 定）

来源是 `v4-plan.md` 末节的三条后续方向，加上补盲区时**当场抓出来的一个缺陷**，再加一条
2026-09-19 新立的容器化收尾：

| # | 事项 | 状态 |
|---|---|---|
| 1 | 带形态清单要真实数据（暴露 FILL_PATTERN / invalid 位的真实样本） | 未做——触发条件未变，手上仍只有 t25362 一份真实观测（零头形态） |
| 2 | 补两个合成盲区：缺口 + filler 同段并存、`FILL_PATTERN` | ✅ 完成（P1、P2） |
| 3 | invalid 位定案 | ✅ 完成（P3：定案"占槽 + 数据标无效"并实施） |
| 4 | **P1 的副产品**：B7——filler 段长于一个 subint 时整段漏读 | ✅ 已修（P1，`reader-model.md` 4.10） |
| 5 | **六个容器镜像合并为一个**（两段式 Dockerfile，装齐串行链路全部命令） | ✅ 完成（P4，2026-09-20，验收 5/5） |
| 6 | **各目录路径环境变量化** + `common/` 改名 `sim-common/` | 计划已定（P5），待实施 |
| 7 | **fxcorr-sim 造数能力**：病态数据处方 + 压力测试数据轻量模式 | 计划已定（P6），待实施 |

前四条的共同前提是 v4 阶段 D 已经就位的三层判据（单测秒级、合成几分钟、真机一次 ssh），
所以那一轮没有新建任何验收框架，只是把新的形态填进现成的判据里。第 5–7 条与读模型无关：
第 5 条是部署收口（判据复用 v2 的容器对拍），第 6 条是路径配置化，第 7 条是造数能力——
后两条的判据见各自小节。

## P1：缺口与 filler 同段并存、多组相邻（2026-09-19 完成）

**依据**：`reader-model.md` 7.5 第 3 项——现有资产把缺口与 filler **分别**测（`run_filler.sh`
单处中断、`run_boundary.sh` 两处纯缺口），没有一组含"多组交错"。

**先抽真实形态**（`scan_filler.py` 扫 t25362 的 BA ds_2，2026-09-19）：8 组中断，每组的形态是
「**filler 段紧跟一个真实缺口**」（81:10、82:10、229:28、180:22、17:2、49:6、16:2、508:63），
**组间只隔 2–25 帧数据**（组 3/4、5/6 之间只有 2 帧）。7.5 原来写的"缺口两端夹 filler"
与实测不符，按实测形态设计场景。

**资产**：`fxcorr/test/gaps/run_mixed.sh`——M1（两组相邻，同窗口内两次 filler 修正 + 两次
缺口修正叠加）、M2（508 帧 filler 跨 4 个 subint），各配"纯缺口等价形式"。
判据 = 相对（两形式逐 subint 无效块一致）+ 绝对（E1–E3 无 finding、E4 = 0）+ 自检报红。

**M1 全绿**（209 == 209，E4 = 0）。**M2 抓出 B7**（见下），修好后同样全绿（259 == 259）。

### P1 副产品：B7（已修）

**现象**（M2 修复前）：被 filler 段跨越的那个 subint **整段消失**——`GAPCHECK summary` 只报
`buffers 3`（batch 有 4 个 subint）、该 subint 的 `.sp` 全 512 块无效、E4 报 82 帧数据从未进入
任何读取窗口。

**根因**（不是位置）：`readWindow` 的 doubling 循环读到文件尾时给 `input` 留下 eofbit/failbit，
循环结束后没有清；`openFile` 只在换文件时清，所以**下一个 subint** 继承了这个状态，`seekg`
之后的 `good()` 判假、`readWindow` 返回 -1、`readSubint` 返回 0——静默丢弃整个 subint。

**修法**：`readSubint` 入口（`openFile` 之后）加一行 `input.clear()`；读停在文件内部时是空操作。

**触发条件**："填满槽必须读到文件尾"，而不是"filler 段长于 subint"本身——两者在 M2 里重合。
t25362 不触发（fps 16000 → 8388 帧/subint，最长 filler 508 帧），低帧率观测会踩到。

详见 `reader-model.md` 4.10。

## P2：FILL_PATTERN 两种位置（2026-09-19 完成）

**依据**：`reader-model.md` 4.8 的形态清单——`FILL_PATTERN`（0x11223344）那两行是 fxcorr 唯一
"完全不认"的形态。清单同时要求**先定案再实现**。

**定案所需的证据**（2026-09-19 实测）：同一段时间造三份数据（生成器 `FXSIM_GAPS` 新增
`:p<N>` 整帧、`:h<N>` 帧首），只换占位帧的字节：

| 形态 | 上游 mpifxcorr 基准 SWIN | fxcorr（认之前） |
|---|---|---|
| `f` 全零头（t25362 的形态） | `90e54869917e4627bee27b127e5ab10c` | 正常 |
| `p` 整帧 pattern | 同上（**逐字节相同**） | `missing 223 / filler 0`、2 buffers |
| `h` 帧首 pattern | 同上（**逐字节相同**） | `missing 277 / filler 0`、3 buffers |

**定案：认，按整帧处理**。上游对两种位置最终都跳掉整帧（帧尾判据直接跳；帧首判据只跳 8 字节，
随后逐字节重新同步，净效果相同——`h` 的 SWIN 与 `f` 相同正说明这点）；fxcorr 是定位读、没有
"半个帧"的状态，所以按整帧处理既与上游等价又自洽。

**实现——三处同一判据**：`frametimeline.h` 的 `vdifIsFiller`、`file_truth.py` 的 `is_filler`、
`test_timeline.cpp`（含假阳性检验：模式在 payload 中部不算 filler）。

**资产**：`fxcorr/test/gaps/run_pattern.sh`——三形态的 `GAPCHECK summary` 逐字段相同
（都报 `missing 28 / filler 229`）+ 逐 subint 无效块一致 + E1–E4 全绿 + 自检。
修复后实测无效块逐 subint 相同（24/98/0/0）。

详见 `reader-model.md` 4.11。

## P3：invalid 位（实测完成）

**依据**：`reader-model.md` 4.8 形态清单的最后一行——mpifxcorr 的 mux flags 里没有
`ENABLEVALIDITY`，它不把 invalid 帧当 filler；fxcorr 当（`vdifIsFiller` 的第一条判据）。
这条判据是 2026-09-17 的 C 类修复按形态清单加的（`git log -S` 到 `05e5781e2`），**当时没有实测依据**。

**语义背景**：VDIF 的 invalid 位描述的是"这一帧在时间轴上存在、但数据无效"——与其它三种占位
形态（占字节、不占时间轴）**不是一回事**。上游的处理印证了这一点：`vdifmux` 把输入帧的
invalid 位收进 mask（`vdifmux.c:866-882`），经 `PROPAGATEVALIDITY` 写进输出 EDV4 头的
`validitymask`（`:888-914`），下游解包时把无效数据置零——**帧号照常推进、槽位照占**。

**实测形态**：无中断数据 + 把第 125..353 帧的 word0 bit31 置 1（帧号与时间轴完全不动，
只声称"数据无效"）。

| | 对照（原样） | invalid 形态 |
|---|---|---|
| `GAPCHECK summary` | `buffers 4 ... missing 0 filler 0` | `buffers 4 ... **missing 229 filler 229**` |
| `READPOS subint 1` | `readoff 0 ... nframes 133 slots 133` | `readoff 0 ... nframes 355 slots 133 fillershift 1839328` |
| `READPOS subint 2/3` | `readoff 1028096 / 2080288`（各自推进） | **两个 subint 读同一个 `readoff 2843328`** |

即：fxcorr 把那 229 帧当 filler 丢弃 → **时间轴被压缩 229 帧** → 后续读位置整体前移、
相邻 subint 读到同一段数据。

**上游的实测对照**（同一份 invalid 数据与重新生成的原样数据各跑一次 `run_bench.sh`，
`cmp_swin.py` 逐记录比较）：

```
/tmp/swin_inv/...: 6 records     /tmp/swin_ctrl/...: 6 records        ← 记录数相同
record 0 (bl 258 frq 0 pol RR sec 25200.524288):
  w: 0.47678470611572266 vs 0.9892578125                             ← 权重打折
  vis[0]: 2403.33837890625 vs 2402.10546875 (rel 5.13e-04)           ← 数值几乎不变
record 3 ... sec 25201.572864:  w: 0.6389076709747314 vs 1.0
```

**记录数与时间戳逐条对应**——上游的时间轴完全正常；差异只在 weight（打折）与可见度
（相对 5e-4）。上游的处置见 `vdifmux.c:905-940`：无效线程的 `threadBuffers[i]` 指向
`src`（不用真实数据），输出头写 `validitymask`、计数进 `nPartialOutput`，下游据此把
对应的块判无效。**核心是"帧在位"**——位置对了，权重与数值才是可讨论的。

**定案：占时间槽 + 该帧对应的块标无效**（数据不参与积分）。理由三条：位置必须对（上游与
标准都是"帧在位"）；数据不该用（invalid 的语义就是"不可信"）；上游的"打折保留"来自它
corner-turn 时把无效线程的指针指向 `src` 这个实现方式，不值得照抄。

**实施（三处，`reader-model.md` 4.12 有完整说明）**：

- `frametimeline.h`：`vdifIsFiller` 只剩全零头与 `FILL_PATTERN`，新增 `vdifIsInvalid`；
  `walkFrameChain` 让 invalid 帧**当数据帧**走链，`placeFrames` 让它**占槽**并记
  `invalidslots`，新增 `SlotPlacement::invalidRanges()` 把洞与无效槽合并成一张区间表；
- `datareader.cpp`：`gapinvalid = p.invalidRanges()`；`checkFrameContinuity` 的 `reorder`
  多一条"缓冲区里有 invalid 帧"——**没有 invalid 帧时那条判断一字未动**；
- `file_truth.py` / `check_reader.py`：真值加 `invalid` 段（占槽），`holes_in_window` 与
  E4 的覆盖面都把它算进去。

**实测（`fxcorr/test/gaps/run_invalid.sh`）**：修之前时间轴压缩 229 帧（subint 2/3 读同一
`readoff`、`missing`/`filler` 各报 229）；修之后 `READPOS` 序列与对照**逐行相同**、GAPCHECK
summary 一致，无效块从 `11/0/0/0` 变成 `24/512/372/0`（那 229 帧的数据被清），真值对账
（`data 296 invalid 229`）零 finding，自检能报红。

**一处与上游的有意分歧**：上游对 invalid 帧的权重是**打折保留**（实测 0.4768 / 0.6389 对
0.9893 / 1.0），fxcorr 是**整段清掉**（更严格）。t25362 没有这个形态，不影响现有对拍；
分歧记在 `reader-model.md` 4.12，将来若有真实样本再评估。

## P4：六个镜像合并为一个（2026-09-19 立，2026-09-20 完成）

**依据**：v2 的镜像体系（`v2-plan.md` 第 2 节）是**按进程角色切分**的——builder 出产物、
base 出运行时底座、f/x/sim/difx-tools 各自一个生产镜像，共 6 个。切分本身对"模块化封装、
按角色挑镜像"是成立的，但落到**串行跑通一条链**这个实际用法上，代价全部显现：

1. **构建顺序耦合**：5 个生产镜像都靠 `COPY --from=fxcorr-builder:latest` 取产物，builder
   必须先 build 且 tag 固定为 `latest`——少一步就 build 不出来，且无法并行；
2. **`fxc` 的映射表是纯负担**：4 条"工具→镜像"映射（fxcorr-f/x/sim → 各自镜像、三个 difx
   工具 → difx-tools）只为把命令路由回它所在的镜像，一条链路要轮流启动 3 个镜像；
3. **体积重复**：base 与 f/x/sim 是同一份 213MB 底座的四个副本（DiFX 生态 .so 逐字节相同），
   差别只在 COPY 的那一个 bin。

**方案：单镜像 + 两段式 Dockerfile**（一个文件两个 stage，取代原来的 6 目录 6 份定义）：

- **stage 1 `builder`**：`FROM debian:13`，装工具链与依赖库，跑
  `python3 install-difx --noipp --nodoc --skip=mpifxcorr,difx2profile,vis2screen`。
  v2 踩过的坑原样保留：`PKG_CONFIG_PATH` 显式设置、`MPICXX=g++`、**只设链接期
  `LDFLAGS=-no-pie`**（编译参数保持默认，`-fno-pie` 会改变 fxcorr-f 运行行为）。
- **stage 2 `runtime`**：`FROM debian:13-slim`，装运行时包（base 的 libgcc-s1/libstdc++6/
  libfftw3-{single,double}3/libexpat1/zlib1g + difx-tools 的 libgsl28/libgslcblas0/
  libgfortran5/libcfitsio10t64），再 `COPY --from=builder` 三块：`lib/*.so*`（不含 .a/.la）、
  `share/`（difxcalc 星历）、以及 `bin/` 下的六个可执行。
- **位置与上下文**：`fxcorr/docker/` 从 6 个子目录收敛为一份 `Dockerfile` + `Makefile` +
  `README`，构建上下文 = 仓库根（v2 只有 builder 是根上下文，现在统一），根 `.dockerignore`
  照旧。一次 `docker build` 出镜像，不需要顺序编排。

**镜像内容：串行链路的全部命令**

| 命令 | 在哪一段链路 |
|---|---|
| `vex2difx` | `make_testdata.sh` 前处理：VEX → `.input` |
| `difxcalc` | 前处理：`.calc` → `.im` |
| `fxcorr-sim` | 造仿真 VDIF（datasim 的替身） |
| `fxcorr-f` | `run_batch.sh` 逐站 F 引擎 |
| `fxcorr-x` | `run_batch.sh` 单 batch X 引擎 |
| `difx2fits` | 后处理：SWIN → FITS |

**不进镜像**（与 v2 结论一致，未改）：`mpifxcorr`（MPI 环境不进容器，`run_bench.sh` 对拍
仍宿主直跑）；构建期内容（工具链、源码、`install-difx`）只活在 stage 1，不进运行段。

**配套改动**

- `fxc`：`run_in_container` 里那整段 `case "$tool" in` 映射删掉，`img` 固定为 `fxcorr/fxcorr`；
  其余一行不改——`--rm`、`-v "$WORKDIR:$WORKDIR"` 整体挂载、`-w "$(pwd)"` 保持 cwd 一致、
  `FXSIM_*` 按需透传。`make_testdata.sh` 与 `run_batch.sh` 各有一份同样的逻辑，两处同步改。
- **镜像命名**：`fxcorr/fxcorr`，tag `2.9.1` / `latest`——v2 定的"裸名 + push 到 registry 时
  再定前缀"在这里落定，前缀取 `fxcorr`（命名空间），本地即 `fxcorr/fxcorr:latest`。
- **Makefile**：仍是 `docker buildx build ... -t fxcorr/fxcorr:latest --load ../../..`，
  保留 builder 那份的 `--network=host`（装包要走网络）与 `--build-arg VERSION`。

**有意改动 v2 结论的两处**（实施时记明，v2-plan 是冻结文档不回头改）：镜像数 6 → 1，
v2 验收第 1 条"六镜像构建成功"由单镜像构建取代；镜像名由裸名改为带前缀的 `fxcorr/fxcorr`。

**验收（判据全部沿用 v2，不新建框架；2026-09-20 在测试机执行）**

| # | 判据 | 依据 | 结果 |
|---|---|---|---|
| 1 | 一次 `docker build` 出镜像；六个命令都在 PATH 且能打印用法 | 取代 v2 验收 1/6 | ✅ 一次 build 出 `fxcorr/fxcorr:{latest,2.9.1}`（**330MB**）；六命令均在 PATH 且各自打印版本/用法；`ldd` 扫 `lib/*.so*` 无缺失 |
| 2 | 容器 SWIN 与宿主直跑对拍全等 | v2 验收 3 的 `cmp_swin.py` 6/6 | ✅ 二者 **sha256 相同**（`cf014359…`），`cmp_swin.py` 6 条记录全等 |
| 3 | `FXCORR_RUN_MODE=container` 全链路：`make_testdata.sh` → `run_batch.sh` 跑通，产物与宿主直跑一致 | v2 验收 4/5 | ✅ 容器内 vex2difx + difxcalc + fxcorr-sim（common+2 站）+ fxcorr-f×2 + fxcorr-x 全程跑通，status `done` |
| 4 | `difx2fits` 在合并镜像内出 FITS | v2 验收 6 | ✅ `TEST.0.bin0000.source0000.FITS`（282240 B，头 `SIMPLE = T`） |
| 5 | 宿主直跑（默认模式）回归不变 | V1 行为 | ✅ 宿主产物与 **mpifxcorr 基准** `cmp_swin.py` 全等，且与容器产物 sha256 相同（三方逐字节一致） |

**收尾**：旧的 6 个镜像定义目录（`fxcorr/docker/{fxcorr-builder,fxcorr-base,fxcorr-f,fxcorr-x,fxcorr-sim,difx-tools}/`）
实施时删除，git 历史保留；镜像体积合并后 **330MB**（实测；事前按 v2 第 7 节推算为
213MB 底座 + difx-tools 增量 ≈ 320MB），`strip` 仍不做。

**实施记录（2026-09-20，验收 5/5）**

| 改动 | 落点 |
|---|---|
| 两段式单镜像 | 新增 `fxcorr/docker/Dockerfile`（stage1 `builder` / stage2 `runtime`） |
| 构建与说明 | 新增 `fxcorr/docker/Makefile`（上下文 `../..`、打 `latest`+`2.9.1`）与 `docker/README.md`（合并原 6 份，记明有意改动 v2 的两处） |
| 删旧定义 | 删 `fxcorr/docker/{fxcorr-builder,fxcorr-base,fxcorr-f,fxcorr-x,fxcorr-sim,difx-tools}/` |
| `fxc` 去映射 | `run_batch.sh` 与 `make_testdata.sh` 的 `run_in_container` 删 `case` 段，`img` 固定 `fxcorr/fxcorr` |
| 同步规则 | `fxcorr/Makefile` 的 rsync `--include` 由 `fxcorr/docker/*/Makefile` 改为 `fxcorr/docker/Makefile`（实测旧规则会漏掉上移一层的 Makefile） |
| 构建上下文 | 根 `.dockerignore` 补 `*.o/*.lo/*.la/*.a/.deps//.libs/` 排除——测试机上遗留 482 个宿主 `.o`，不排会被 `COPY . /src` 带进去按时间戳复用（上下文也因此从 421MB 降到 214MB） |
| **stationcmd 容器前缀**（见下） | `make_testdata.sh` 的 `stationcmd()` 本地分支补 `docker run` 分支；`FXSIM_PCAL` 补进 `ENVS` |
| 构建入口纳入 git | 根 `.gitignore` 补 `!fxcorr/docker/Makefile`——通用 `Makefile` 规则会吞掉它（v2 的 6 份子目录 Makefile 即因此从未进仓库，只靠 rsync 传） |

**验收中抓出的既有缺陷**：`fxcorr-sim station` 那一步在容器模式下直接 exec 了宿主的
`fxcorr-sim`（`bash: fxcorr-sim: command not found`）。该分支由 fxcorr-sim P2 的并行分发
（`3ef8404c4`）引入，**晚于 v2 的容器验收**，所以是既有缺口而非 P4 引入——只有
`make_testdata.sh` 的 common 与 `run_batch.sh` 的 f/x 走了容器。修法：`stationcmd()` 的本地
分支在 `FXCORR_RUN_MODE=container` 时输出 `docker run … fxcorr/fxcorr fxcorr-sim` 前缀，
与 `fxc` 同一镜像、同一套 `-e` 透传。

**实测**：镜像 330MB（估 320MB）；一次构建约 13 分钟（apt 353s + `install-difx` 411s，
8 核空载）——v2 记的"约 30 分钟"是含首次拉镜像的粗估。

**测试机收尾**（2026-09-20）：旧的 6 个镜像标签已 `docker rmi` 删除，测试机只剩
`debian:13` / `debian:13-slim` / `fxcorr/fxcorr:{latest,2.9.1}`；构建缓存 11.05GB 保留未清
（清了下次重建就失去缓存、回到全量编译）。验收现场留在 `/root/fxcortest/p4v5/`。

## P5：目录路径环境变量化 + `sim-common/` 改名（2026-09-20 定，待实施）

**依据**：现状是 **workdir 单根硬拼**——`fxcorr-f/src/main.cpp:261/290/340/352/412`、
`fxcorr-x/src/main.cpp:109/140/151/219/360`、`fxcorr-x/src/beamengine.cpp:90`、
`fxcorr-sim/src/main.cpp:107/648/665/774` 各自写 `workdir + "/<目录名>/..."`。单根在部署上不够用：
数据量大，不同类型的数据可能落在**全局存储或不同本地存储**（不同挂载点、不同容量策略），
目录必须能独立重定向。

**定案：`FXCORR_WORKDIR` + 九个 `_ROOT`**（`FXCORR_` 前缀，与 `RUN_MODE`/`LOGLEVEL` 一致）

| 变量 | 默认 | 内容 |
|---|---|---|
| `FXCORR_WORKDIR` | **当前目录的绝对路径**（未设置时取 `realpath(".")`） | 项目根；位置参数仍优先于它 |
| `FXCORR_CONFIG_ROOT` | `$FXCORR_WORKDIR/config` | 配置与模型（`.input`/`.vex`/`.v2d`/`.im`） |
| `FXCORR_BATCHES_ROOT` | `$FXCORR_WORKDIR/batches` | 批量元数据（batch.json，D9） |
| `FXCORR_RAW_ROOT` | `$FXCORR_WORKDIR/raw` | 原始基带 VDIF |
| `FXCORR_SIM_COMMON_ROOT` | `$FXCORR_WORKDIR/sim-common` | 仿真公共信号（D15，原 `common/`） |
| `FXCORR_FENGINE_ROOT` | `$FXCORR_WORKDIR/fengine` | f 输出（f 写、x 读） |
| `FXCORR_VIS_ROOT` | `$FXCORR_WORKDIR/vis` | SWIN 可见度 |
| `FXCORR_BEAM_ROOT` | `$FXCORR_WORKDIR/beam` | 相位阵波束（P8） |
| `FXCORR_PRODUCT_ROOT` | `$FXCORR_WORKDIR/product` | 最终产品；**fxcorr 程序不写它**，由调用 difx2fits 的脚本设置（Q7） |
| `FXCORR_META_ROOT` | `$FXCORR_WORKDIR/meta` | 索引、difxmsg、roots 记录 |

**三条解析规则**

1. **优先级**：命令行位置参数 > 环境变量 > 默认值。`FXCORR_WORKDIR` 未设置时取当前目录的
   绝对路径（不是字面 `.`），使派生出的各根一律是绝对路径。
2. **类别按用途定，不按路径内容定**（P5 讨论 Q1）：`FILE d/d:` 行 → RAW_ROOT；batch.json 的
   `config_file` → CONFIG_ROOT、`difx_dir` → VIS_ROOT；f 写/x 读的 `.sp` 目录 → FENGINE_ROOT；
   `fxcorr-sim` 的公共信号 → SIM_COMMON_ROOT——**一律按代码在此处"正在处理哪类数据"决定用哪个根**。
   实测依据：`fxcorr/test/test.v2d` 的 `file = TEST1.vdif` 经 vex2difx 后在 `.input` 里就是
   **裸文件名**（无 `raw/` 这一级），路径字符串本身不携带类别信息。
3. **绝对路径直接用、不拼根**（真实观测的 `FILE` 行就是绝对路径，见
   `sites/MPIfR/oneoff/difx2difx_testset/ma008_1.input:1715`）；只有相对路径才前缀对应根。
   batch.json 里 `config_file` / `difx_dir` 的相对基准同样从 workdir 改为各自根（Q3），
   与程序侧同规则。

**Q2 定案：软链保留，落点搬到 RAW_ROOT。** 合成数据的 `FILE` 行是裸名 `TEST1.vdif`，与真实文件
`raw/<st>/<st>_<batch_id>.vdif` **不同名**，这个映射只能由软链承担（`run_batch.sh:152-158`，
每个 batch 重指一次）。改法：`fxcorr-f` 对相对 `FILE` 行拼 `$FXCORR_RAW_ROOT`；`run_batch.sh`
把软链建到 `$FXCORR_RAW_ROOT/<FILE 行原样>`、**目标改绝对路径**（相对目标跨文件系统会断）。
效果是 raw 区整体落在大盘上、workdir 里不再散落软链；软链这一"文件名映射"载体不撤。
（讨论中否掉的：batch.json 加映射表去软链——要动 D9 格式且判据翻倍；程序内按约定猜路径——
该约定只对合成数据成立，等于让 `fxcorr-f` 承担编排职责。）

**Q4 定案：编排层落记录 + 程序启动比对。** f 写 fengine 与 x 读 fengine、sim common 写
sim-common 与 station 读，是跨进程交接点，两边根不一致就是静默读空。
`run_batch.sh` 开跑前把生效的根写 `$FXCORR_META_ROOT/roots/<batch_id>.json`；三个程序启动时
若该文件存在则与自解析结果比对，**不一致即报错退出**；文件不存在（单工具直跑、测试场景）跳过，
现有用法不受影响。这是在环境变量不进 batch.json 的前提下，唯一能事后追溯"这批数据用了哪套
布局"的手段。

**`common/` → `sim-common/` 改名**（Q8）：目录改，**子命令仍叫 `fxcorr-sim common`**
（"用 common 子命令生成 sim-common 目录"）。改名的理由是消歧：现在 `common` 一词三义——
`libraries/fxcorrcommon`（库）、`fxcorr-sim common`（子命令）、`common/`（目录）。
连带要改：`data-spec.md` 第 2 节 / D15 行 / 5.8 节 / 第 10 节命名汇总 / 第 9 节数据流图，
`fxcorr-sim/src/main.cpp:63/64/648/665`，`fxcorr/CLAUDE.md`、`usage.md`，以及测试资产里的引用。

**`work/` 从规范中删除**：全仓库无任何代码或脚本使用它（只在 `data-spec.md` 第 2 节出现）。

**适用范围**：**只覆盖串行部分**——fxcorr-f/x/sim 三工具与三个编排脚本。`mpifxcorr` 与
`run_bench.sh` 不适用（其输入路径由上游 `Configuration` 按 cwd 相对解析，环境变量改不了）；
对拍场景须用默认布局。

**配套改动**

- **程序侧**：三处 `workdir + "/<name>/..."` 改为"按用途取根"；入口统一把 workdir 绝对化。
  输出目录沿用现有"按需 mkdir"（`beam`/`meta/difxmsg` 已有先例），输入目录不存在时报明确错误。
- **脚本侧**：`make_testdata.sh` / `run_batch.sh` 实现同一套解析（bash 与 C++ 两处实现必须同规则，
  规则以 `data-spec.md` 为唯一权威）；软链改绝对目标（Q2）；写 `meta/roots/<batch_id>.json`（Q4）。
- **容器侧**（接 P4）：`run_in_container` 的 `-e` 透传从 `FXSIM_*` 扩到九个 ROOT；挂载从
  `-v "$WORKDIR:$WORKDIR"` 扩为按各根**同路径逐个挂载**（Q5）——因为 `.input`/batch.json 里可能
  是宿主绝对路径，容器内必须按同样路径可见，不能做重定向映射。
- **文档侧**：`data-spec.md`（第 2 节顶层结构、D 编号表、5.2.1 workdir 语义、5.8、第 10 节、
  第 12 节）、`fxcorr/CLAUDE.md`、`usage.md` 的环境变量一节。

**验收（实施后执行）**

| # | 判据 |
|---|---|
| 1 | 默认布局下（只给 `FXCORR_WORKDIR`）产物与当前逐字节相同——回归不破 |
| 2 | **非默认布局**：把 `FXCORR_RAW_ROOT` / `FXCORR_VIS_ROOT` 指到别处，全链路跑通、产物与默认布局一致（这条是 P5 的核心判据，没有它等于没测） |
| 3 | 绝对路径的 `FILE` 行（真实观测形态）不受影响，根变量不参与 |
| 4 | `meta/roots/<batch_id>.json` 落盘内容与各进程实际解析一致；故意把 x 的 FENGINE_ROOT 指错能报错退出 |
| 5 | `sim-common/` 改名后，`gaps/` 七个脚本与单测 71 + 48 全绿（改名是纯字符串替换，判据沿用现成的） |
| 6 | 容器模式（接 P4）多根挂载后全链路跑通 |

## P6：fxcorr-sim 造数能力（病态数据 + 压力测试数据）（2026-09-20 定，待实施）

**定位**：把 fxcorr-sim 从"能对拍的理想数据 + 三种病态开关"扩成**按需造数的唯一入口**，
两个并列使用面共用同一套接口与同一套帧结构实现（`vdifwriter`），差别只在**载荷来源**与**规模**：

- **病态数据**——按处方精确构造各类病态形态，供读模型与流水线的正确性回归（对拍、E1–E5 判据）；
- **压力数据**——结构真实、体量可放大的数据，供**后期流水线的压力测试**（吞吐、容量、并发、长稳）。

两者都不追求"信号在物理上真实"以外的额外东西：病态数据可以物理真实，压力数据只需要**结构
真实**——帧头、时间轴、band 布局、帧号规则、文件尺寸分布与真实观测同构，这正是读模型与
流水线真正在意的那部分。

**依据（病态这一半）**：现状三个造病能力各自一个环境变量——`FXSIM_GAPS`（缺口 + filler 三形态，
帧号与计数可分别指定）、`FXSIM_STARTOFFSET`（记录起点偏移）、以及信号参数类（`FXSIM_NOISE` 等，
物理量而非病态）。表达力只到"**一个中断、固定位置**"：P1 的 `run_mixed.sh` 要造"多组交错"，
已经是靠脚本反复调参拼出来的。随着 reader-model 的 A/B/C/D 四类缺陷回归与 v5 补的盲区继续增加，
这种拼装方式的成本在上升。

**依据（压力这一半）**：现在造数据走完整频域链（块 IDFT + 帧级三趟 FFT + 条纹旋转 + pcal），
速率决定了体量上限——v3-plan P3 的放大场景才 8 站 61s batch（那一次是**单点热点剖析与加速比**，
不是系统级压力）。后期流水线的压力测试要的是另一类问题：长 batch、多 batch 连续、多站并发、
磁盘与内存边界、长稳不退化——这些都需要**能造出任意体量的数据**，而造数本身不能成为瓶颈。

**定案（2026-09-20）**

1. **接口：处方文件**——一次实验一份，逐条描述"帧区间 + 病态类型 + 参数"，可叠加、可复现、
   可版本化；现有 `FXSIM_GAPS` / `FXSIM_STARTOFFSET` 保留为简写（等价于处方里的单条规则）。
   压力场景同样靠文件表达（规模参数与病态规则同一份），保证一次压测可原样重建。
2. **病态范围：时间轴层 + 数据内容层 + 布局层**（见下表），**不含 VDIF 帧头层**。
3. **压力数据：轻量模式 + 规模参数**——跳过物理信号链，按 band 布局直接填确定性伪随机 2bit
   载荷；帧头、时间轴、band 布局、帧号规则与完整链**完全一致**（复用 `vdifwriter`），种子驱动
   可复现。规模由"目标时长 / 目标体量 / 站数 / batch 数"给定，不再只能按帧数配。
4. **共用一条写入路径**：病态注入与规模放大都落在 `VDIFWriter::writeFrame` 内按帧号命中——
   legacy 与新路径两个调用点自动生效（现有实现的关键性质，P6 沿用），不为压力模式另开分支。

**处方文件**

- 形式：逐行一条规则（`<起帧>:<帧数>:<类型>[:参数]`），或等价的 JSON 数组；由环境变量给出
  路径（如 `FXSIM_PRESCRIPTION=<path>`），不放环境变量里拼长串。
- 与真值的接续：处方声明的是"哪段时间轴没有真数据、哪段数据不可信"，正对应
  `fxcorr/test/reader/file_truth.py` 从产物 VDIF 独立算出的真值——处方是**期望**、真值是**实测**，
  两者比对即 E1–E5 那套绝对判据，不需要新的判据框架。
- 位置语义：与 `FXSIM_GAPS` 同层——**在 `VDIFWriter::writeFrame` 内按帧号命中**，legacy 与新
  路径两个调用点自动生效（这是现有实现的关键性质，P6 沿用）。

**病态覆盖清单（三层）**

| 层 | 形态 | 对应既有缺陷 |
|---|---|---|
| **时间轴** | 起点偏移与缺口/filler **叠加**、多组交错（P1 的实测段序：filler 段紧跟一个缺口、组间 2–25 帧） | A 类、B1/B2/B7 |
| | 跨 subint 的长 filler（填满槽必须读到文件尾，B7 的触发条件） | B7 |
| | 缺口与 filler **长度不等**的组合（把读位置带偏的正是这个长度差） | C 类 |
| **数据内容** | 全零数据、量化饱和、DC 偏置、band 缺失（某 band 无信号） | D 类（"数据错了而元数据说它对"） |
| **布局** | band 间隙（已有）、band 数变化、多 datastream 混合、帧长与 band 布局不匹配 | P7 暴露的 band 语义问题 |

**明确不在 P6 范围**：VDIF 帧头层（invalid 位、帧号跳变/回绕异常、时间戳不连续、legacy 头
bit30、epoch 异常、帧长不符）——2026-09-20 定。其中 invalid 位现状由
`fxcorr/test/gaps/run_invalid.sh` 的**后处理改字节**承担（生成器不支持，因为 invalid 是"帧在位、
数据不可用"，与占位形态不同类，见 P3）；将来若要收进生成器，另立条目。

**压力数据：要覆盖的维度**

| 维度 | 目标 | 现状 |
|---|---|---|
| 体量 | TB 级原始数据 | 受生成速率限制，跑不动 |
| 时长 | 小时级 batch、多 batch 连续 | `-n N` 目前只到秒级 batch |
| 站数 / 基线数 | 8 站 28 基线（v3 的放大场景）往上 | 由 `.input` 驱动，能力已在 |
| 通道 / band | nChan 32768、多 band、间隙布局 | 能力已在（多 band 网格 P4 已修） |
| 生成并发 | 多站并行、跨节点 | 已在（`-p` / `--nodes`） |
| 流水线并发 | 多 batch 并行、多节点分发 | 编排层（scalebox）职责，不在本仓库 |
| 长稳 | 连续跑数小时不退化、不泄漏 | 未测 |
| 磁盘边界 | 写满、配额、清理策略 | 未测 |

**轻量模式**（承载体量与时长两个维度）

- 与完整链共用 `setupStation` 的帧结构推导与 `vdifwriter`，只替换载荷来源：确定性伪随机
  （种子 = f(seed, station, band)），保证可复现与跨站可区分。
- **不用于**正确性对拍（对拍仍用完整链）——它的判据是"结构一致"而非"数值一致"，见验收 3。
- 生成速率与体量的实测数字写进本节（实施后补），作为压测选规模的依据。

**验收（实施后执行）**

| # | 判据 |
|---|---|
| 1 | 处方文件驱动的场景与原环境变量拼出的形态**逐字节相同**——回归不破，`gaps/` 七个脚本全绿 |
| 2 | 新增三层病态各有判据：处方声明的期望与 `file_truth.py` 的实测一致（E1–E5），且判据能报红 |
| 3 | 轻量模式产物与完整链**同参数下逐帧头一致**（时间轴、帧号、band 布局），载荷为伪随机 |
| 4 | 处方可叠加、可复现：同一处方跑两次逐字节相同；多组交错能一次表达（不再靠脚本拼装） |
| 5 | **压力场景跑通一次**：按目标规模生成（小时级 batch 或多站并发），后期流水线 f → x → difx2fits（含容器与编排）全程跑完，记录吞吐、资源占用与磁盘增长 |
| 6 | 生成速率与体量的数字落档本节，供后续压测选规模 |

**与其他条目的关系**：P6 与 P5 无耦合（处方文件路径若也走环境变量，遵循 P5 的命名与解析规则）；
与 P4 的交叉在于压力场景也应在容器内跑一次（验收 5 含容器）；与"未完成第 3 条（病态数据的
对拍基准）"相关但不解决它——P6 造的是那类数据，而"mpifxcorr 自身在 filler 上丢字节"这个基准
问题仍需另解。

## 验收与回归

| 项 | 结果 |
|---|---|
| 单测 `test_timeline.cpp` | 53 → **71** 项全过（pattern 8 条：三种形态 + 假阳性 + 槽映射；invalid 10 条：走链与占槽） |
| 单测 `test_corrections.cpp` | 48 项全过 |
| `gaps/` 脚本 | **七个全绿**（`run_filler` / `run_window` / `run_boundary` / `run_startoffset` / `run_mixed` / `run_pattern` / `run_invalid`），各带"判据能报红"的自检 |
| 无中断路径逐字节不变 | 64 个 subint（含 delay）在 HEAD 与本轮两版各跑一次，`band_00.sp` 与 `autocorr.bin` **md5 相同** |
| 真机 t25362 | ds_2 与 ds_0 各 2181 subint **零 finding**、E4 = 0、`GAPCHECK summary` 与基线逐字段相同 |

**判据三层没有变**：单测（本地秒级）→ 合成（测试机几分钟）→ 真机（一次 ssh），V5 只往里
填了新的形态，没有新建框架——这正是 v4 阶段 D 想要的回报。

## 未完成 / 留给下一版

1. **真实数据**（范围表第 1 条）：FILL_PATTERN 与 invalid 位的**真实样本**仍然没有——本版本
   的两条定案都由"上游怎么处理"支撑，而不是"记录系统实际怎么写"。invalid 位那一处还与上游
   **有意分歧**（上游打折保留、fxcorr 整段清掉，见 P3 末），真实样本到手时要优先重估它。
   触发条件与挑数据依据仍是 `reader-model.md` 4.8 的形态清单。
2. **filler 修正量的形态**（`v4-plan.md` 未解决第 4 条）：仍是"累计"而非"以时间为自变量"。
3. **病态数据的对拍基准**（同第 5 条）：mpifxcorr 自身在 filler 上丢字节，这条线只能靠文件真值判据。
4. **两项待实施**：目录路径环境变量化 + `sim-common/` 改名（P5）、fxcorr-sim 造数能力（P6）
   ——方案与验收判据都已定，实施未开始，见各自小节。P5 的容器挂载接在 P4 的单镜像上扩展
   （多根挂载与透传）；P6 与两者无强耦合，可独立排期，只有压测那一条（验收 5）需要 P4 的
   镜像就位。
