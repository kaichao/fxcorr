# fxcorr V5 计划

> **进行中**（2026-09-19 开）——V5 处理 `v4-plan.md` 末节「后续方向」里条件已具备的两件半：
> **补两个合成盲区**（P1、P2）与 **invalid 位定案**（P3），三件均已完成；"要真实数据"那条
> 的触发条件未变，仍在等。
> **定案、证据与验收写在这里，根因与判据写在 `reader-model.md`**（4.10 B7、4.11 FILL_PATTERN、
> 4.12 invalid 位）——两处分工与 v1–v4 一致：本文件只承载路线与验收。

## V5 范围（2026-09-19 定）

来源是 `v4-plan.md` 末节的三条后续方向，加上补盲区时**当场抓出来的一个缺陷**：

| # | 事项 | 状态 |
|---|---|---|
| 1 | 带形态清单要真实数据（暴露 FILL_PATTERN / invalid 位的真实样本） | 未做——触发条件未变，手上仍只有 t25362 一份真实观测（零头形态） |
| 2 | 补两个合成盲区：缺口 + filler 同段并存、`FILL_PATTERN` | ✅ 完成（P1、P2） |
| 3 | invalid 位定案 | ✅ 完成（P3：定案"占槽 + 数据标无效"并实施） |
| 4 | **P1 的副产品**：B7——filler 段长于一个 subint 时整段漏读 | ✅ 已修（P1，`reader-model.md` 4.10） |

三条的共同前提是 v4 阶段 D 已经就位的三层判据（单测秒级、合成几分钟、真机一次 ssh），
所以这一轮没有新建任何验收框架，只是把新的形态填进现成的判据里。

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
