# fxcorr V5 计划

> **已收尾**（2026-09-19 开，2026-09-27 收；未完成项移交 `v6-plan.md`）——V5 处理 `v4-plan.md` 末节「后续方向」里条件已具备的两件半：
> **补两个合成盲区**（P1、P2）与 **invalid 位定案**（P3），三件均已完成；"要真实数据"那条
> 的触发条件未变，仍在等。
> **P4（容器镜像合并）、P5（目录路径环境变量化）、P6（fxcorr-sim 造数能力）不来自
> v4 的后续方向**，是 2026-09-19 / 09-20 新立的三项：P4 已完成（2026-09-20，验收 5/5），
> **P5 已于 2026-09-20 实施**（三轮复核后：根收敛到五个 + "环境变量三档回退、默认全共享"，
> 路径策略下沉到 fxcorrcommon；验收 9/10，实施记录与遗留问题见该节）；**P6 方案已定**：
> 2026-09-27 补"多 datastream 生成"一节（多 ds 对拍与分片架构落地共同的前置），**其生成侧已实施并验证**。
>
> **V5 收尾（2026-09-27）**：本文档范围内的事项已完成一轮闭环——读模型收尾三件、P1 副产品 B7、
> P4、P5，以及 09-26/27 新立的**分片架构前三项**（batch_id 8 位顺序号、多 datastream 生成、
> x 按 ds 组的分片 + `merge` 归并，含 SWIN 混跑检查；判据见 `test/multids/README.md` 与它引的
> 对拍）。**其余五项移交 v6**（见末节，去向已登记进 `v6-plan.md`）：真实数据（A 组四项）、
> P6 的病态数据处方与压力测试轻量模式、SQLite 索引、P5 复核遗留 3 条、filler 修正量的形态。
> **定案、证据与验收写在这里，根因与判据写在 `reader-model.md`**（4.10 B7、4.11 FILL_PATTERN、
> 4.12 invalid 位）——两处分工与 v1–v4 一致：本文件只承载路线与验收。
> **数据量与容量分析已独立成 `data-volume.md`**（2026-09-21）：各目录体量、算法级落盘粒度、
> 瓶颈排序与优化杠杆（含 `fengine` 的汇聚地板）都在那里，P6 的场景设计引用它，本文件不再重复。

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
| 6 | **各目录路径环境变量化** + `common/` 改名 `sim-common/` | ✅ 完成（P5，2026-09-20，验收 9/10） |
| 7 | **fxcorr-sim 造数能力**：病态数据处方 + 压力测试数据轻量模式 + **多 datastream 生成** | **多 datastream 生成已完成**（2026-09-27）；余下两项（处方文件、压测轻量模式）**移交 v6** |

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

## P5：目录路径环境变量化 + `sim-common/` 改名（2026-09-20 定，同日三轮复核修订，**已实施**）

**依据**：现状是 **workdir 单根硬拼**——`fxcorr-f/src/main.cpp:261/290/340/352/412`、
`fxcorr-x/src/main.cpp:109/140/151/219/360`、`fxcorr-x/src/beamengine.cpp:90`、
`fxcorr-sim/src/main.cpp:107/648/665/774` 各自写 `workdir + "/<目录名>/..."`。单根在部署上不够用：
数据量大，不同类型的数据可能落在**全局存储或不同本地存储**（不同挂载点、不同容量策略），
目录必须能独立重定向。

**定案：`FXCORR_WORKDIR` + 五个 `_ROOT`**（`FXCORR_` 前缀，与 `RUN_MODE`/`LOGLEVEL` 一致）

| 变量 | 常规位置（**设置该根时**的取值） | 内容 | 典型用途 |
|---|---|---|---|
| `FXCORR_WORKDIR` | **当前目录的绝对路径**（未设置时取 `realpath(".")`） | 项目根；位置参数仍优先于它 | 全部目录的默认基准 |
| `FXCORR_RAW_ROOT` | `$FXCORR_WORKDIR/raw` | 原始基带 VDIF | 指向本地大盘 |
| `FXCORR_SIM_COMMON_ROOT` | `$FXCORR_WORKDIR/sim-common` | 仿真公共信号（D15，原 `common/`） | 另挂一块大容量**共享**盘 |
| `FXCORR_FENGINE_ROOT` | `$FXCORR_WORKDIR/fengine` | f 输出（f 写、x 读） | 指向计算节点本地盘 |
| `FXCORR_VIS_ROOT` | `$FXCORR_WORKDIR/vis` | SWIN 可见度（接管 `OUTPUT FILENAME`，Q10） | 与下游处理 / 独立存储对接 |
| `FXCORR_PRODUCT_ROOT` | `$FXCORR_WORKDIR/product` | 最终科学产品（Q16） | 本地生成 → 迁移到全局 |

**没有独立根的四类目录**（Q17）：`config/`、`batches/`、`meta/`、`beam/`——恒在
`$FXCORR_WORKDIR/` 下。四者量小（KB~MB）、或必须全局一致可见、或本就按 batch 组织在 workdir 内，
独立重定向只有坏处没有用途。

**Q9 定案（2026-09-20 两轮复核）：按"环境变量是否设置"三档回退，默认全共享。**

**回退链**（纯值判断，不探测文件系统）：

1. `FXCORR_<X>_ROOT` 已设置 → 该类路径按它的值解析；
2. 否则 `FXCORR_WORKDIR` 已设置 → 按规范目录名解析为 `$FXCORR_WORKDIR/<name>`
   （`config/` 与 `meta/` 没有独立根，恒为这一档）；
3. 都没有 → 现状（位置参数给的 workdir，或 `workdir = "."`）。

**默认全共享**（复核第二轮的定案）：只设 `FXCORR_WORKDIR` 时所有目录都在它下面——即**所有节点
共享同一份全局存储**。需要本地化时才显式把对应 `_ROOT` 指向本地盘。`data-spec.md` 第 2 节那张
存储归属表要相应改口径：从"固定归属"改为"**默认全共享；本地化通过 `_ROOT` 启用**"。

**哪些目录支持独立根**，按"数据量 + 一致性"两条判：

| 目录 | 量级 | 独立根 | 判据 |
|---|---|---|---|
| `config/` `batches/` `meta/` | KB~MB | ❌ 已删（Q17） | 量小、必须全局一致可见 |
| `beam/` | MB | ❌ 已删（Q17） | 按 batch 组织在 workdir 内即可，位置稳定性由 workdir 保证 |
| `vis/` | MB~GB | ✅ | SWIN 跨 batch 追加同一组文件、多节点各写各的会分裂；留显式覆盖是为了把它放到独立存储与下游处理对接 |
| `raw/` | TB | ✅ | 本地记录，各站数据在各记录节点 |
| `fengine/` | 大 | ✅ | 每节点只持有自己写的站 |
| `sim-common/` | **最大** | ✅ | 量大，可能要另挂一块盘 |
| `product/` | 大 | ✅ | 可本地生成，再由编排层迁移到全局（Q16） |

**`FXCORR_SIM_COMMON_ROOT` 的方向性约束**：它**需要**重定向（≥16× 单站 2bit，全部目录里最大，
可能要另挂一块盘），但重定向目标**必须是全站可见的共享目录**——"全站读同一份公共信号"正是
跨站相干的来源（`fxcorr-sim-arch.md` 的一致性规则），各节点各存一份即失相干。**这一点代码
强制不了**（程序只看到一个路径字符串），只能靠文档约定 + 编排层在部署时校验。实现与文档都不要
把它与 RAW / FENGINE 并列成"本地存储"类。

> **2026-09-20 修订（P6 讨论中收窄）**：上面"必须全站可见的共享目录"是按"同一 batch 的站散在
> 各节点"写的。部署形态明确后（**计算单元 = batch，f 与 x 同节点**〔**2026-09-27 按分片修订为
> `(batch, ds 组)`**，见 `data-spec` 第 1 节〕，见 P6 节），约束应表述为
> **"同一 batch 的公共信号被它的全部 station 任务读到"**——该 batch 的站既然在同一节点，
> **本地盘即满足**，不再是必须共享。跨节点各生成一份不会失相干（公共信号只与 `(seed, batch)`
> 有关）；真正的违例是"同一 batch 的不同站读到不同的副本"（**2026-09-27 按分片修订为"同一 ds 组"**
> ——同一 batch 的不同 ds 组读的是公共信号的**不同区段**，各节点各存一份不影响相干，见 `data-spec` 5.2.1）。
> **必须全局可见的只有实验级的 `VIS` / `PRODUCT`**（SWIN 跨 batch 追加、difx2fits 一次读整个
> 实验）。已同步 `data-spec.md` 第 1 节与 5.2.1。

**"常规位置"与"未设置时的行为"是两件事**——上面那一列是**设置该根时**的取值。这些根按现状分
两类：**程序拼接类**（BATCHES、SIM_COMMON、FENGINE、BEAM 的目录本身）在现状里本就是
`workdir + "/<name>"` 拼接——对它们，"未设置"与"设为常规位置"**等价**，设置只是换个目录；
**cwd 类**（DATA TABLE 的 `FILE` 行、`OUTPUT FILENAME`、`CALC FILENAME`）在现状里按 cwd 解释，
只有按下面规则 2 的基准（或显式设根）才接管。行为改变只发生在后者。

原 Q9（"根是开关、只在显式设置时才接管"）只覆盖 cwd 类，已被上面这条替换。

**核实证据（2026-09-20，代码级，Q9–Q11 的事实基础）**

1. **SWIN 落点 = `.input` 的 `OUTPUT FILENAME`；`difx_dir` 是死参数。**
   `applications/fxcorr-x/src/integrate.cpp:20` 的 `difxdir` 形参在构造体内一次未被引用，
   该处注释自陈 "difx_dir in batch.json is metadata only"；落盘在
   `libraries/fxcorrcommon/src/visibility.cpp:1071/1083`——
   `sprintf("%s/DIFX_%05d_%06d.s%04d.b%04d", config->getOutputFilename(), ...)`，
   原样当目录字符串用，无 basename/dirname、无 workdir 前缀。
   实测（P4 现场 `/root/fxcortest/p4v5/`）：SWIN 落在 `config/test.difx/`，而 batch.json 写的是
   `vis/<batch_id>.difx`——`vis/` 目录**根本不存在**。
2. **`Configuration` 的一切路径相对进程 cwd。** 唯一打开入口是
   `libraries/fxcorrcommon/src/configuration.cpp:110` 的裸 `ifstream`：`.input` 的 `CALC FILENAME`
   （`:1200` → `:267` → `model.cpp:26`）、`.calc` 里的 `IM FILENAME`（`model.cpp:604-609`）、
   DATA TABLE 的 `FILE` 行（`configuration.cpp:1887` 原样存 → `fxcorr-f/src/datareader.cpp:245`
   裸打开）全部如此；fxcorr **不**走 `libraries/difxio` 的 `locateAltFilename`（"相对 .input
   所在目录"回退，仅 `--localdir` 开启时生效）。
3. **`run_batch.sh:153` 的 `os.path.join(workdir, OUTPUT FILENAME)` 不是第二条真相**——
   第二参数为绝对路径时 `os.path.join` 丢弃 `workdir`，语义与 C++ 侧恰好相同；两者只在
   cwd == workdir 时吻合，这正是 `fxcorr/CLAUDE.md:82`「run_batch.sh 调用时 cwd 须在 workdir」的由来。
4. **两类数据的路径形态正好相反**——这是"只对相对路径拼根"（规则 2）的依据：

   | 路径 | 合成数据（make_testdata.sh 产物） | 真实观测（`sites/MPIfR/oneoff/difx2difx_testset/ma008_1.input`） |
   |---|---|---|
   | `FILE d/d:` | `TEST1.vdif` **裸名** | `/data/ma008/eb_module/...` **绝对** |
   | `CALC FILENAME` / `OUTPUT FILENAME` | **绝对**（vex2difx 在 config/ 内跑，按 cwd 绝对化） | `ma008_1.calc` / `ma008_1.difx` **裸名** |
   | `IM FILENAME` / `FLAG FILENAME`（写在 .calc 里） | 绝对 | 未知（仓库内无样本） |

   一律拼根有两个后果：① 验收 2 在合成资产上**测不到 VIS_ROOT**（`OUTPUT FILENAME` 已是绝对
   路径，指到哪都不参与），判据假绿；② 真实观测的 SWIN 落点被无声改变。只对相对路径拼根，
   两类数据各取所需。
5. **配置加载期即可接管全部路径**（第二轮复核，纠正前一版的说法）：`.input` 的 `CALC FILENAME`、
   `.calc` 的 `IM` / `FLAG FILENAME` 虽在 `Configuration` / `Model` 构造期就被打开、
   `main.cpp` 拿不到中间字符串，但**打开动作就发生在 fxcorrcommon 里**
   （`configuration.cpp:110` 的 `mpiGetFileContent`）——在该处读环境变量并拼根即可，不必在
   `main.cpp` 层拦截。fxcorrcommon 是 V1 新建的库、mpifxcorr 用自己的源码副本，改它不影响
   mpifxcorr。**这是 P5 在库层的唯一改动点**（Q11）。

**三条解析规则**（第二、三条按第二轮复核改写）

1. **优先级**：命令行位置参数 > 环境变量 > 默认值。`FXCORR_WORKDIR` 未设置时取当前目录的
   绝对路径（不是字面 `.`），使派生出的各根一律是绝对路径。
2. **只对相对路径拼根，绝对路径一律原样**（真实观测的 `FILE` 行就是绝对路径，见
   `sites/MPIfR/oneoff/difx2difx_testset/ma008_1.input:1715`）。三类相对路径的基准各自独立：

   | 路径 | 基准 |
   |---|---|
   | DATA TABLE 的 `FILE d/d:` | `FXCORR_RAW_ROOT` |
   | `.input` 的 `CALC FILENAME`；`.calc` 里的 `IM` / `FLAG FILENAME` | **`.input` 所在目录**——这一组文件是一套，整目录搬走即可用，比"相对 workdir"自然（不必写 `config/test.calc`） |
   | `.input` 的 `OUTPUT FILENAME` | `FXCORR_VIS_ROOT`（未设置则 `$FXCORR_WORKDIR/vis`） |

3. **类别按用途定，不按路径内容定**（Q1）：一律按代码在此处"正在处理哪类数据"决定用哪个根。
   实测依据：`fxcorr/test/test.v2d` 的 `file = TEST1.vdif` 经 vex2difx 后在 `.input` 里就是
   **裸文件名**（无 `raw/` 这一级），路径字符串本身不携带类别信息。
   （原表里的"`difx_dir` → VIS_ROOT"一项按 Q10 撤销；batch.json 的 `config_file` 按 Q3
   相对 `$FXCORR_WORKDIR` 解析。）

**Q2 定案：软链保留，落点搬到 RAW_ROOT。** 合成数据的 `FILE` 行是裸名 `TEST1.vdif`，与真实文件
`raw/<st>/<st>_<batch_id>.vdif` **不同名**，这个映射只能由软链承担（`run_batch.sh:152-158`，
每个 batch 重指一次）。改法：`fxcorr-f` 对相对 `FILE` 行拼 `$FXCORR_RAW_ROOT`；`run_batch.sh`
把软链建到 `$FXCORR_RAW_ROOT/<FILE 行原样>`、**目标改绝对路径**（相对目标跨文件系统会断）。
效果是 raw 区整体落在大盘上、workdir 里不再散落软链；软链这一"文件名映射"载体不撤。
（讨论中否掉的：batch.json 加映射表去软链——要动 D9 格式且判据翻倍；程序内按约定猜路径——
该约定只对合成数据成立，等于让 `fxcorr-f` 承担编排职责。）

**Q4 定案：编排层落记录 + 程序启动比对。** f 写 fengine 与 x 读 fengine、sim common 写
sim-common 与 station 读，是跨进程交接点，两边根不一致就是静默读空。
`run_batch.sh` 开跑前把生效的根写 `$FXCORR_WORKDIR/meta/roots/<batch_id>.json`（meta 无独立根，
Q17）；三个程序启动时
若该文件存在则与自解析结果比对，**不一致即报错退出**；文件不存在（单工具直跑、测试场景）跳过，
现有用法不受影响。这是在环境变量不进 batch.json 的前提下，唯一能事后追溯"这批数据用了哪套
布局"的手段。

**Q10 定案：`difx_dir` 退为记录字段，不再当路径用。** 实测该字段从未被读取（证据 1）。
`make_testdata.sh` 仍写它，值改为照抄 `.input` 的 `OUTPUT FILENAME` 原样（相对就写相对），
并加注"实际落点由运行时根 / cwd 决定"；`fxcorr-x/src/integrate.cpp` 那个死参数删掉（或改名
`swindir` 并注明只作记录）。`FXCORR_VIS_ROOT` 的作用面因此收敛到 `OUTPUT FILENAME` 一处。

**Q11 定案（第二轮复核重写）：路径策略下沉到 fxcorrcommon 的加载层，一处覆盖全部路径。**
在 `libraries/fxcorrcommon/src/configuration.cpp` 与 `model.cpp` 的打开动作处按规则 2 的基准表
拼根——`mpiGetFileContent`（`configuration.cpp:110`）是唯一打开入口，改动集中在一处。要点：

- **只拼相对路径**，绝对路径一律原样（规则 2）；
- 基准三类各自独立：`FILE` 行 → `FXCORR_RAW_ROOT`；`CALC` / `IM` / `FLAG` → `.input` 所在目录；
  `OUTPUT FILENAME` → `FXCORR_VIS_ROOT`；
- 这是 fxcorrcommon **首次引入路径策略**（此前它是上游代码的忠实副本），要记进
  `libraries/fxcorrcommon/CLAUDE.md` 并说明与 mpifxcorr 同名文件的差异；
- **程序层的 `workdir + "/<name>/..."` 仍然照改**（Q9 回退链）——两者分工：库层管
  `.input`/`.calc` 内部路径，程序层管规范目录本身。

**这条是 P5 的语义边界，写进 `data-spec.md` 5.2.1。**

**Q12 定案（第二轮复核）：容器挂载不由 P5 负责，交给外部编排平台。**
`run_in_container` 保持现状的 workdir 挂载，**只把 `-e` 透传从 `FXSIM_*` 扩到五个根**——程序在
容器内仍需知道用哪个根。多根挂载由 scalebox 按部署实际统一处理，脚本里不实现。文档写一条
**约定**："容器内各根须按宿主同路径可见"，由平台保证。

**Q13 定案（复核建议 6）：`common/` → `sim-common/` 硬切，不兼容读旧名。** 公共信号可重新生成
（`fxcorr-sim common` 一次），兼容读旧名会让"目录名"这一契约变成两个。文档写明旧布局需重跑
公共信号。

**Q14 定案（第二轮复核）：两个脚本都写记录；每个程序只比对"自己真的会用到"的那几个根。**

- **两份都写**：`make_testdata.sh` 与 `run_batch.sh` 各写一次 `meta/roots/<batch_id>.json`。
  理由是 sim common 写 sim-common、station 读 sim-common 同样跨进程（P5 自己列的交接点），
  只由 `run_batch.sh` 写会漏掉造数阶段。
- **只比用到的**：文件里记录 `FXCORR_WORKDIR` 与五个根的值，各程序拿自己会读写的子集去比——
  fxcorr-f 比 RAW / FENGINE / VIS（后者是因为它往 SWIN 目录写 PCAL 与 SWITCHEDPOWER 文本，
  见遗留第 12 条）；fxcorr-x 比 FENGINE / VIS；fxcorr-sim common 比 SIM_COMMON；
  fxcorr-sim station 比 SIM_COMMON / RAW。
  若全部拿来比，fxcorr-f 会因为"用户改了 `FXCORR_PRODUCT_ROOT`"这种与它无关的差异而报错退出——
  那是误报。

**Q15 定案（复核建议 9）：`run_bench.sh` 与七个 `gaps/*.sh` 维持默认布局。** 前者受 mpifxcorr 的
cwd 语义限制（不可改），后者是回归资产（判据只要求改名后全绿）。**代价写进文档**：这些脚本在
设了根变量的环境下会读到别处——脚本开头对五个根做 unset 或显式报错（实施时二选一，验收 10）。

**Q16 定案（第二轮复核）：新增 `fxcorr/wrap_difx2fits.sh`，把 difx2fits 纳入根体系并入镜像。**
脚本职责：按规范定位 `$FXCORR_VIS_ROOT` 下的 SWIN → 调 difx2fits → 产物落
`$FXCORR_PRODUCT_ROOT`。这是"bash 做规范适配层"（Q15 的原则）的又一处应用——difx2fits 不认
fxcorr 的根变量，由脚本完成转换。

- **`FXCORR_PRODUCT_ROOT` 因此有了真实读者**，不再是空承诺。本地产出 + 迁移到全局的用法：
  脚本写到本地 product 根，**迁移由编排层负责，P5 不实现迁移逻辑**，只在文档写清。
- 脚本**打包进容器镜像**（`fxcorr/docker/Dockerfile` 的 COPY 清单加一项，`docker/README.md`
  的镜像内容表同步）；容器模式下仍用 `docker run … fxcorr/fxcorr wrap_difx2fits.sh` 调用。
- **它是实验级操作，不是 batch 级**：SWIN 跨 batch 追加、difx2fits 一次读整个 `.difx` 目录，
  所以**不能放进 `run_batch.sh` 逐 batch 调用**，只能是该实验所有 batch 跑完后单独调一次；
  触发者是编排层（scalebox），不属 `run_batch.sh` 的职责。

**Q17 定案（第二、三轮复核）：删除四个根——`CONFIG` / `BATCHES` / `META` / `BEAM`。**
四类目录恒在 `$FXCORR_WORKDIR/` 下：

- `config/`、`batches/`、`meta/`：量小（KB~MB）、必须全局一致可见，独立重定向只有坏处；
- `beam/`：MB 级、按 batch 组织，位置稳定性由 workdir 保证（第三轮复核时删）。

连带：`meta/roots/`（Q4）、`meta/difxmsg/`（f/x 的状态落盘）、`meta/batches.index`、
`beam/<batch_id>/beam.bin` 都固定，不再参与根解析。

**Q18 定案：跨 batch 的实验级根要做一致性检查。** `vis/`、`beam/`、`product/` 是**实验级**的
（SWIN 跨 batch 追加同一组文件），中途改了 `FXCORR_VIS_ROOT`（换 shell、改部署配置）会让同一
实验的产物分裂在两处，**difx2fits 读不全且没有任何报错**。`meta/roots/<batch_id>.json`（Q4）
只能事后追溯、不能预防。做法：`run_batch.sh` 开跑前找同实验已有 batch 的 `roots.json`，比对
这三个实验级根，**不一致即报错退出**；batch 级根（RAW / FENGINE / SIM_COMMON）不参与——
它们按 batch 组织，换根是合法的。

**Q19 定案：根目录由编排层创建，程序只建自己输出目录的下一级。** `run_batch.sh` /
`make_testdata.sh` 开跑前 `mkdir -p` 各根；程序遇到根不存在时报明确错误、**不自动创建**——
程序分不清"根不存在"是配置写错还是首次运行，自动建会把配置错误吞掉。现状的"按需 mkdir"
只保留在 `fengine/`、`beam/`、`meta/difxmsg/` 这类**已知输出目录的下一级**上。

**Q20 定案：路径值的书写与绝对化规则。** 两条：

- **`.input` 的 `OUTPUT FILENAME` 相对 `FXCORR_VIS_ROOT` 写**——写 `test.difx`，**不要**写
  `vis/test.difx`，否则拼出来是 `$VIS_ROOT/vis/test.difx`。写进 `data-spec.md` 5.2.1。
- **环境变量取相对值时一律相对进程 cwd 绝对化**（与 `FXCORR_WORKDIR` 同规则），**不会**相对
  `FXCORR_WORKDIR` 二次解析——否则 `RAW_ROOT=raw` 会有两种可能的含义。

**Q21 定案：加 `FXCORR_PRINT_ROOTS` 自检开关。** `=1` 时三个程序在启动阶段打印各目录的最终
解析路径与"落在三档回退的哪一档"。它是 Q18 的检查、roots.json 比对、以及"根到底生效没有"
这三个场景的共同基础设施，成本极低。属诊断输出，归 `FXCORR_LOGLEVEL` 的 `info` 级。

**`common/` → `sim-common/` 改名**（Q8）：目录改，**子命令仍叫 `fxcorr-sim common`**
（"用 common 子命令生成 sim-common 目录"）。改名的理由是消歧：现在 `common` 一词三义——
`libraries/fxcorrcommon`（库）、`fxcorr-sim common`（子命令）、`common/`（目录）。
连带要改：`data-spec.md` 第 2 节 / D15 行 / 5.8 节 / 第 10 节命名汇总 / 第 9 节数据流图，
`fxcorr-sim/src/main.cpp:63/64/648/665`，`fxcorr/CLAUDE.md`、`usage.md`，以及测试资产里的引用。

**`work/` 从规范中删除**：全仓库无任何代码或脚本使用它（只在 `data-spec.md` 第 2 节出现）。

**适用范围**：**只覆盖串行部分**——fxcorr-f / x / sim 三工具与编排脚本（含 Q16 新增的
`wrap_difx2fits.sh`）。`mpifxcorr` 与 `run_bench.sh` 不适用：前者用**自己那份**源码副本
（fxcorrcommon 的库层改动传不过去），后者的输入路径由 mpifxcorr 的 `Configuration` 按 cwd 解析。

**原则（Q15，第二轮复核提炼）**：新目录规范**只约束上述三个二进制**；凡调用原 difx 程序
（vex2difx / difxcalc / difx2fits / mpifxcorr），一律由 **bash 脚本做"当前规范 → 原程序所需
形式"的转换**——`run_bench.sh` 里 sed 改 `OUTPUT FILENAME` 是现成例子，`wrap_difx2fits.sh`
（Q16）是第二例。这条要写进 `data-spec.md` 作为总则，否则以后每加一个原程序调用都要重新讨论。

**配套改动**

- **库层**（新，Q11）：`libraries/fxcorrcommon/src/configuration.cpp` / `model.cpp` 在
  `mpiGetFileContent` 处按规则 2 的基准表拼根（只拼相对路径）；记进该库的 CLAUDE.md。
- **程序侧**：三处 `workdir + "/<name>/..."` 改为"按用途取根"（Q9 三档回退）；入口统一把
  workdir 绝对化。`integrate.cpp` 的死参数按 Q10 处理。输出目录沿用现有"按需 mkdir"
  （`beam`/`meta/difxmsg` 已有先例），输入目录不存在时报明确错误。
- **脚本侧**：`make_testdata.sh` / `run_batch.sh` 实现同一套解析（bash 与 C++ 两处实现必须同规则，
  规则以 `data-spec.md` 为唯一权威）；开跑前 `mkdir -p` 各根（Q19，失败即退出）；
  **跨 batch 的实验级根一致性检查**（Q18）；软链改绝对目标（Q2，**两处硬编码的 `raw/` 路径要
  同步**）；写 `meta/roots/<batch_id>.json`（Q4、Q14）；`difx_dir` 按生效的 SWIN 目录回填（Q10）；
  新增 `fxcorr/wrap_difx2fits.sh`（Q16）。
- **容器侧**（接 P4）：`run_in_container` 的 `-e` 透传从 `FXSIM_*` 扩到五个根；
  挂载按 Q12 交给平台，脚本不实现（**不做容器内的根可见性防呆**：Q12 的约定由平台保证，
  容器里报"目录不存在"即宿主的根没挂进来）。`fxcorr/docker/Dockerfile` 的 COPY 清单加
  `wrap_difx2fits.sh`（Q16），`docker/README.md` 的镜像内容表同步。
- **文档侧**：`data-spec.md`（第 2 节顶层结构、D 编号表、**存储归属表改口径**（Q9）、
  5.2.1 workdir 语义 → 改为"根语义" + `OUTPUT FILENAME` 的写法（Q20）、5.8、第 10 节、
  第 12 节）、`fxcorr/CLAUDE.md`（`common/` → `sim-common/`；`run_batch.sh` 调用时 cwd 须在
  workdir 一条按 Q2/Q9 改写）、`usage.md` 的环境变量一节（五个根 + 三档回退 + Q21 开关）。

**验收（实施后执行）**

| # | 判据 |
|---|---|
| 1 | **零根回归**：不设任何根、只给 `FXCORR_WORKDIR`，产物与当前逐字节相同（Q9 的三档回退：默认路径一字不改） |
| 2 | **非默认布局**：把 `FXCORR_RAW_ROOT` / `FXCORR_VIS_ROOT` 指到别处，全链路跑通、产物与默认布局逐字节相同；**判据自身要能报红**——先在"不设根"下确认读的是默认位置，否则"指到别处也对"可能只是根没生效（假绿） |
| 3 | **两类数据各验一次**：合成数据（`FILE` 裸名 → RAW_ROOT 生效）与真实形态（`FILE` 绝对 + `OUTPUT FILENAME` 裸名 → VIS_ROOT 生效）；后者用 `ma008_1.input` 改写一份最小样本 |
| 4 | **库层接管生效（Q11）**：把 `.input` 的 `CALC FILENAME` 与 `.calc` 的 `IM FILENAME` 都写成裸名，`.input`/`.calc`/`.im` 整体搬到另一目录后仍能跑通——这是库层改动唯一能证明生效的判据 |
| 5 | `meta/roots/<batch_id>.json` 落盘内容与各进程实际解析一致；故意把 x 的 FENGINE_ROOT 指错能报错退出；改一个与 f 无关的根（如 `FXCORR_PRODUCT_ROOT`）**不**触发 f 报错 |
| 6 | **跨 batch 检查（Q18）**：同一实验连跑两个 batch，第二个开跑前把 `FXCORR_VIS_ROOT` 改掉 → **报错退出**；只改 RAW / FENGINE 则正常通过 |
| 7 | **根不存在时的行为（Q19）**：把某个根指到不存在的路径，脚本报错退出；绕过脚本直跑程序也报明确错误、**不自动建目录** |
| 8 | `sim-common/` 改名后，`gaps/` 七个脚本与单测 71 + 48 全绿（改名是纯字符串替换，判据沿用现成的） |
| 9 | 容器模式（接 P4）全链路跑通；`wrap_difx2fits.sh` 在容器内可调用，FITS 落到 `FXCORR_PRODUCT_ROOT` 且 `SIMPLE = T` 可读（Q16） |
| 10 | 设了根变量后跑 `gaps/*.sh`，按 Q15 的选择（unset / 报错）行为符合预期；`FXCORR_PRINT_ROOTS=1` 的输出与 roots.json 一致（Q21） |
| 11 | **两处实现一致**（遗留第 4 条）：`fxcorr/test/roots/run_consistency.sh` —— 三档回退下 `roots.sh` 与 `FxcorrPath` 逐项相同，自检能报红 |

**实施记录（2026-09-20，验收 11/11 全过）**

| # | 结果 |
|---|---|
| 1 | ✅ 默认布局全链路跑通；SWIN 与 mpifxcorr 基准 6 记录逐条相等（`cmp_swin.py`） |
| 2 | ✅ 非默认 `RAW`/`VIS` 根：全链路跑通、对拍相等，且与默认布局产物 **sha256 完全相同**（`5dd2e120…`） |
| 3 | ✅ 真实形态（`FILE` 绝对 + `OUTPUT FILENAME` 裸名）：新建 workdir 造配置，`FXCORR_RAW_ROOT` 指向**空目录**、`FXCORR_VIS_ROOT` 指向别处——f/x 全链路跑通（GAPCHECK 与基准一致）、SWIN 落 `$VIS_ROOT/real.difx`、空 RAW 根**始终为空**（绝对路径不拼根）。产物 sha256 与合成布局相同 |
| 4 | ✅ 从 `/tmp` 跑（cwd ≠ workdir）fxcorr-f 正常完成——`.input` 的相对 `CALC FILENAME` 被拼到**配置目录**而不是 cwd（改动前必然失败，这是库层接管的直接判据） |
| 5 | ✅ 程序侧比对生效：改 `FXCORR_VIS_ROOT` 报错并打印"recorded / resolved"两值；改 `FXCORR_PRODUCT_ROOT`（f 用不到的根）照常跑完 |
| 6 | ✅ 脚本侧 Q18：改 `VIS` 根**报错退出**、只改 `RAW` 根**通过**（batch 级根不参与） |
| 7 | ✅ 根不存在：脚本 `mkdir -p` 建齐（Q19）；绕过脚本直跑程序被 roots 比对拦下，**未自动建目录** |
| 8 | ✅ `gaps/` 七个脚本全 PASS（boundary/filler/window/startoffset/mixed/pattern/invalid）+ 单测 71 + 48 全绿 |
| 9 | ✅ 容器全链路跑通（造数 + 跑批）；`wrap_difx2fits.sh` 在容器内可调用，FITS 落 `FXCORR_PRODUCT_ROOT` 且 `SIMPLE = T`；**容器产物与宿主 sha256 相同**（`5dd2e120…`） |
| 10 | ✅ `FXCORR_PRINT_ROOTS` 输出与 `roots.json` 一致；gaps 脚本在默认布局下不受影响 |
| 11 | ✅ 新增 `fxcorr/test/roots/run_consistency.sh`：默认 / 只设 `WORKDIR` / 五个根全设（含相对值）三档下，脚本侧与程序侧逐项一致；自检改坏一侧能报红 |

**实施中抓出并修掉的三处**（都不是设计问题，是落地时的连带）：

1. **规范化打破了 `run_bench.sh`**——`wrap_vex2difx` / `wrap_difxcalc` 把 `.input`/`.calc` 的绝对路径改回
   相对后，mpifxcorr（按 cwd 解析）找不到 `.calc`，第一次跑直接卡死。按 Q15 补上：run_bench
   复制到 `bench/` 的 `.input`/`.calc` 把 `CALC FILENAME`、`FILE` 行、`IM`/`FLAG FILENAME`
   绝对化。**这是 Q15 原则的第一个真实用例**——原 difx 程序的适配层不是可选项。
2. **`fxcorr_check_roots` 初版跳过本 batch 自己的记录**，于是"同一个 batch 重跑换了根"漏检
   （产物同样分裂）；已改为与自己也比——调用方保证 check 发生在 write 之前，首次运行仍无记录。
3. **`make_testdata.sh` 解析 `CALC FILENAME` 时假设它是绝对路径**，规范化后失效（`relpath`
   会按 cwd 算）；已改为相对时拼 `.input` 所在目录。

**落地形态小结**（改了什么）：

- 库层：新增 `fxcorrcommon/src/fxcorrpath.{h,cpp}`（`FxcorrPath`）；`configuration.cpp` 三个
  打开点 + `model.cpp` 一个打开点接线。
- 程序层：三个 `main.cpp` 各 `init()` + `print()` + `checkRoots()`（各自用到的根不同），
  f/x/sim 的 `fengine`/`vis`/`sim-common`/`raw` 输出改用根；`Integrator` 的 `difxdir` 死参数删除。
- 脚本层：新增 `roots.sh`（两脚本 source）、`wrap_vex2difx.sh`、`wrap_difxcalc.sh`、
  `wrap_difx2fits.sh`；两脚本改用根、`mkdir -p` 各根、软链落 RAW 根且目标绝对、`difx_dir` 照抄
  `OUTPUT FILENAME`、写 `meta/roots/<batch_id>.json`、加 Q18 检查；`run_bench.sh` 补原程序适配。
- 容器层：Dockerfile 加四个脚本的 COPY（`roots.sh` + 三个 `wrap_*.sh`）。

**遗留与风险（2026-09-20 列出，实施后清理为 3 条）**

> **已落实的 13 条**（实施时解决，不再单列）：验收 2 假绿（→ 验收 3 补真实形态样本）、软链搬家
> 破坏旧用法（→ 已搬 + 两处硬编码同步 + 文档改）、D 编号表漏改（→ 第 9/10 节已核）、
> `difx_dir` 静态不可定（→ 按 Q10 实现）、库层引入路径策略的代价（→ 记入该库 CLAUDE.md）、
> `SIM_COMMON` 须指向共享目录（→ 写进 5.2.1 与存储归属表）、VIS_ROOT 下有 f 写的文件（→ 写进
> 根表）、文档原先写错（→ usage.md / data-spec 已改）、batch.json 三个记录字段（→ 5.3 已标注
> `difx_dir` 没有程序读它）、三档回退第三档靠 cwd / `FXCORR_WORKDIR` 已存在 / 容器防呆不做
> （→ 信息性，已写进 5.2.1 与 Q12）。

1. **编号断档**：Q1–Q4、Q8–Q21 有出处；Q5（容器挂载）与 Q7（PRODUCT_ROOT）在第一轮修订中
   分别改写为 Q12 与 Q16，旧编号不再引用；**Q6 在讨论中未留记录**。若 Q6 曾是被否掉的方案，
   请补一句说明，否则后续编号从 Q22 续。
2. **真实观测的 `IM FILENAME` / `FLAG FILENAME` 形态仍未知**：仓库内无真实 `.calc` 样本。
   库层接管（Q11）之后形态是绝对还是相对都不影响正确性，但**验收 4 目前只用合成数据构造过**；
   真实数据到手后要补一次（V5 未完成清单第 1 项）。
3. **一致性判据的边界**：`fxcorr/test/roots/run_consistency.sh` 只比**五个根的值**，不比
   "哪类路径归哪个根"的映射（`FILE` 行 → RAW、`OUTPUT FILENAME` → VIS 那部分只能靠评审与
   验收 3/4）。**规则再变时先扩这个脚本**；它带自检，改坏了会自己报红。

## P6：fxcorr-sim 造数能力（病态数据 + 压力测试数据）（2026-09-20 定；**多 datastream 生成 2026-09-27 已实施**，余下两项——处方文件、压力数据轻量模式——**移交 v6**）

**定位**：把 fxcorr-sim 从"能对拍的理想数据 + 三种病态开关"扩成**按需造数的唯一入口**，
两个并列使用面共用同一套接口与同一套帧结构实现（`vdifwriter`），差别只在**载荷来源**与**规模**：

- **病态数据**——按处方精确构造各类病态形态，供读模型与流水线的正确性回归（对拍、E1–E5 判据）；
- **压力数据**——结构真实、体量可放大的数据，供**后期流水线的压力测试**（吞吐、容量、并发、长稳）。

两者都不追求"信号在物理上真实"以外的额外东西：病态数据可以物理真实，压力数据只需要**结构
真实**——帧头、时间轴、band 布局、帧号规则、文件尺寸分布与真实观测同构，这正是读模型与
流水线真正在意的那部分。

**部署形态与存储分析**（2026-09-20/21）：**计算单元 = batch**（**2026-09-27 按分片修订为
`(batch, ds 组)`**——一个 batch 的多个 ds 组可分散在多节点，见 `data-spec` 第 1 节；一个 batch 的 common → 全部站
station → 全部站 f → 一次 x 同节点完成，f 与 x 同节点）、各根的可见性要求、`difx2fits` 的实验级
语义，见 `data-spec.md` 第 1 节与 5.2.1；**各目录体量、流水线各步的落盘粒度（细到 slice / FFT
块 / subint / intTime）、瓶颈排序与优化杠杆（含 `fengine` 的汇聚地板）见 `data-volume.md`**。
P6 的场景设计按 `data-volume.md` §3 的参数表挑配置即可——t25362 的实测数字（每站 1.03 GB/s、
覆盖跨度 6816 MHz、1.024 s 的 batch ≈ 127 GB）都在那里。

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
   可复现。规模由"目标时长 / 目标体量 / 站数 / **频段跨度** / batch 数"给定，不再只能按帧数配
（频段跨度是 2026-09-21 加的一项：`sim-common` 的量由它决定、与站数无关——实测见
`data-volume.md` §3/§4）。
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

**多 datastream 生成（2026-09-27 补）**

上面病态清单的"多 datastream 混合"是**造畸形态**给 reader 测试；这一条是造**正常的多 ds
布局**，动机不同，单独记。真实观测的常态是每站多个 datastream（t25362 每站 8 个，见
`data-volume.md` §3），而 fxcorr-sim 现在只生成该站**第一个** ds（`setupStation` 的 `break`，
`main.cpp:164-170`），输出文件名也不含 ds 编号。

**需求（两条，同一件事）**：① **多 ds 对拍**——mpifxcorr 按 `.input` 的 DATA TABLE 读全部 ds，
`fxcorr-f` 按 `(batch_id, station, ds_index)` 每 ds 一个任务，两边各取所需的前提是每个 ds 都有
文件；② **分片架构落地**（`data-volume.md` §7）——按 ds 分片要求每 ds 独立成文件。

**关键结论：不需要"两份数据"，也不存在"等价性"问题。** 曾评估过"生成全 band 与单 band 两份
数据、要求两者等价"的方案，其障碍是公共信号按**网格索引**派生：两个 `.input` 给出两套
`minstartfreq`，同一绝对频率落在不同索引上、值就不同；解法要么"共用一份 common"，要么实现
按绝对频点可寻址的 seed 派生（`data-volume.md` §6 杠杆 6）。**两条都不需要**——同一份字节，
mpifxcorr 读全部文件、fxcorr 的每个 ds 任务读一个文件，两边看到的是同一批数据。

**生成链已是逐 band 隔离的**（这是"单 ds 生成与全 ds 生成在同一 ds 上逐位相同"的依据）：

| 环节 | 依据 |
|---|---|
| 频域切片 | `bd.startidx = (bandfreq − grid.minstartfreq) / grid.specres`——只依赖该 band 的**绝对频率**，与 band 集合无关 |
| 频→时变换 | `bd.planidft` **每个 band 一套 FFT plan**（`signalgen.h:198`） |
| 噪声 | `engine.seed(stationNoiseSeed(seed, station, band))`（`signalgen.cpp:357`）——种子只依赖站名与 band 索引 |
| 量化 | `bd.thresh` / `bd.square` / `bd.sampcount` 都在 `Band` 内，自适应统计**逐 band** 攒（`signalgen.h`） |

**改造清单**

| # | 改动 | 位置 |
|---|---|---|
| 1 | `setupStation` 取 ds 改为**可指定**（现在 `break` 在第一个匹配的 ds 上） | `main.cpp:164-170` |
| 2 | **`stationNoiseSeed` 加 ds 索引**——现签名 `(seed, station, band)`，同站两个覆盖相同频段的 ds（t25362 的 ds0/ds1）会拿到**同一个种子、生成完全相同的噪声** | `signalgen.cpp:178` |
| 3 | 输出文件名含 ds 编号，每个 ds **软链到 `.input` DATA TABLE 的对应行** | `make_testdata.sh` |
| 4 | 循环生成每站全部 ds | `fxcorr-sim station` + `make_testdata.sh` |

**不用改**：`fxcorr-f` 已按 ds 分片；`run_batch.sh` 的 f 循环已在遍历 ds（`DSTATION` 数组）；
mpifxcorr 按 DATA TABLE 读全部行。

**对拍的两段式验证链**（与 `data-volume.md` §7.5 的"分片 + 合并"配套）：

1. **单 ds 层**——`fengine/<batch>/<station>/ds_N/` 与"不分片、一次跑全部 ds"时逐位相同；
2. **SWIN 层**——与 mpifxcorr 对拍，按 `(baseline, frq, mjd, sec)` 匹配（`cmp_swin.py` 现在按
   记录序号逐条比，多 ds 下需改为按键匹配）。这一层同时验证"分片不改变结果"——mpifxcorr
   从来不分片，它就是基准。

**对拍资产（生成侧已就绪，对拍未做）**：现有 `test/` 下的对拍资产**全是单 ds 配置**
（`data-volume.md` §3 的参数表里"每站 datastream 数 = 1"）。`test/multids/gen_multids_input.py`
已能造出多 ds 的 `.input`（DATASTREAM 表展开 + BASELINE 表取笛卡尔积），本步只验到**生成侧**
（每 ds 一个文件、样本互不相关、单/多 ds 同一 ds 逐字节相同）。**与 mpifxcorr 的
多 ds 对拍 2026-09-27 完成**：`cmp_swin.py` 加了 `--by-key`（按
`(baseline, frq, 极化对, 整数纳秒)` 配对，顺序不参与），用来比"记录集合相同"——多 ds 下两边
的记录顺序不再一一对应。实测（4 ds / 4 baseline 的合成配置）：不分片与分片 + merge 两条路径
都是 **16/16 全等**。`FXSIM_*` 的 SEFD/FLUX 按 datastream 序取值早已支持（现有实现即按 dsindex）。

**实施顺序建议**：先只做生成侧（改 1–4），用一个两 ds 的最小配置验证"两份文件确实生成、
样本各自独立（噪声不相关）"，再接对拍——这样能把"生成对不对"与"相关器算得对不对"分开定位。

**实施记录（2026-09-27）：生成侧已完成**（改造清单 1–4 全部落地，测试机验证通过）。

| 项 | 落点 |
|---|---|
| 1 | `setupStation` 的 `break` 改为按 **(station, 站内 ds 序号)** 定位——遍历全部 datastream、只数同站的，第 N 个即该 ds；顺带记下 `ndsinstation` 供命名用（`main.cpp`） |
| 2 | `FreqStationGen::init` 加 `dsindex`（末位默认参数），`stationNoiseSeed` 的 FNV 哈希**在 `dsindex != 0` 时**混入 ds 项——ds=0 的种子与加多 ds 之前**逐位相同**，所以树里全部单 ds 资产的产出逐字节不变 |
| 3 | 输出名 `raw/<st>/<st>_<bid>[_ds<N>].vdif`：**后缀只在多 ds 站出现**（`ndsinstation > 1`，与 `make_testdata.sh` 的 `vdifrel()` 是同一规则的两处实现） |
| 4 | `make_testdata.sh` 按 (station, ds) 展开任务与 DATA TABLE 软链；`fxcorr-sim station` 增 `ds_index` 位置参数（缺省 0，在 workdir 之后，legacy tone 要写在它后面）；默认模式遍历全部站的全部 ds |

**测试资产**：新增 `fxcorr/test/multids/gen_multids_input.py`——把单 ds 的 `.input` 展开成
"每站 N 个 ds"（同一频段、R/L 极化交替），避免走 vex2v2d 的 thread 拆分语义（那条路需要
freqId 配合，单 band 的 test.vex 上直接失败）。用法与两个 `.input` 解析坑（**值必须从第 20
列开始**、**块内不能有空行**，否则 `getinputkeyval` 的 `substr(20)` 抛异常直接崩）见
`test/multids/README.md`。

**判据（全部通过，实测记录见 `test/multids/README.md`）**：① 单 ds 配置产物**逐字节不变**；
② 同站 ds0/ds1 有噪时不同、`FXSIM_NOISE=0` 时**逐字节相同**；③ **多 ds 配置下的 ds0 与单 ds
配置下的同一 ds 逐字节相同**（`18dea81e…` 两边一致）——这一条就是上面"生成链逐 band 隔离"
那张表的实测确认；④ 多 ds 下重跑幂等（逐文件 skip）。

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
| 7 | **多 ds**（2026-09-27 补，**同日验证通过**）：每站每 ds 一个文件、文件名含 ds 编号、各自软链到 DATA TABLE 对应行；同站两份**覆盖相同频段、不同极化（X/Y）**的 ds，其样本**互不相关**（噪声种子含 ds 索引，改 2 的判据）；**与 mpifxcorr 的多 ds 对拍**（4 ds / 4 baseline，`FXSIM_NOISE=0`）按 `(baseline, frq, mjd, sec)` 匹配 **16/16 全等**，分片 + merge 路径同样 16/16 |

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

## 未完成 / 移交 v6

**V5 于 2026-09-27 收尾**：本文档范围内的事项已完成一轮闭环。下面几项**移交 v6**——它们要么在等
外部条件，要么属于"规模与部署"这一新主题（v6 的主线，与 v1–v5 的"让 fxcorr 跑对、跑起来"
不同源）。继续挂在 V5 下只会让这个版本开着不动。

> **移交去向（2026-09-27）**：下表各项已**全部登记进 `v6-plan.md`**——第 1、5 项进它的
> **S5 条件触发**，第 2 项是 **S2（轻量模式）与 S3（规模验证）**，第 3 项是 **S4.3**，
> 第 4 项进 **S6 工程债**。**接手这些项时读 `v6-plan.md`**，本文档不再维护它们的进展。
> V6 另有两项不来自本表的新内容：**S1 真实规模验证**（V5 收尾时发现的判据缺口——分片的端到端
> 判据全跑在合成小配置上）与 **S4.1 两级 merge**（形态 A，多节点部署的前置）。

| # | 项 | 为什么移交 |
|---|---|---|
| 1 | **真实数据**（范围表第 1 条）：FILL_PATTERN 与 invalid 位的**真实样本**仍然没有 | 等外部条件，触发条件未变（`reader-model.md` 4.8 的形态清单）。本版本的两条定案都由"上游怎么处理"支撑，其中 invalid 位那处还与上游**有意分歧**（上游打折保留、fxcorr 整段清掉，见 P3 末），真实样本到手时要优先重估 |
| 2 | **P6 余下两项**：病态数据处方文件、压力测试数据轻量模式 | 这两项的目标本身就是压测与规模化（P6 节有完整设计与验收表），与 v6 同源；"多 datastream 生成"已完成，是它们的前置 |
| 3 | **SQLite 索引**（scalebox 阶段） | 编排层职责，随 scalebox 一起做；规范已写在 `data-spec` 5.7（JSON 权威、sqlite 派生可重建、单写者） |
| 4 | **P5 复核遗留 3 条**（编号断档 Q6、真实 `.calc` 的 `IM`/`FLAG FILENAME` 形态未知、一致性判据只比"根的值"不比"哪类路径归哪个根"）——**注**：这一格此前写"遗留十六条"，是笔误：16 = **13 条已落实 + 3 条遗留**（P5 末节的"清理为 3 条"才是对的），真正移交的只有这 3 条 | 见 P5 末节"遗留与风险"，继续实现 P5 相关工作时**先逐条过** |
| 5 | **filler 修正量的形态**（`v4-plan.md` 未解决第 4 条）：仍是"累计"而非"以时间为自变量"；以及**病态数据的对拍基准**（mpifxcorr 自身在 filler 上丢字节，这条线只能靠文件真值判据） | 都依赖第 1 项的真实样本或新的基准来源，一并移交 |

**开 v6 时要一并交接的三条"已知但有意未做"**（别让它们以后被当成 bug）：

- 分片模式**不支持多相位中心与 pulsar binning**，相位阵不参与——程序内明确报错退出，不是静默降级（取舍见 `usage.md` 的 fxcorr-x 节）；
- `vis-parts/` 的**清理时机**与**"分片迟到怎么办"**（全量等待 / 超时跳过 / 告警）是编排层职责，目前只有 `data-spec` 5.9 的规范、没有实现；
- `run_bench.sh` 定位 batch 时读软链的路径与 `make_testdata.sh` 写软链的位置**不在同一层**（前者按 workdir 拼、后者落在 RAW 根下），目前靠 fallback（取最新 batch.json）兜住。
