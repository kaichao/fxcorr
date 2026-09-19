# reader 对账：文件真值（`file_truth.py` / `check_reader.py`）

**最后更新**：2026-09-19（V4 阶段 D：`run_t25362.sh` 真实数据回归固化；诊断契约见 `fxcorr/reader-model.md` 6.6）

## 用途

给 fxcorr-f 的读取路径一个**绝对**判据。`fxcorr/test/gaps/` 的判据都是相对的（缺口形式对
filler 形式、`firstfno` 不早于对照、比无中断对照多 N 块），两边错成一样就抓不到；而
`cmp_swin.py` 在有缺口的数据上无效（基准自身用 `vdifmux` 丢字节，见
`fxcorr/reader-model.md` 4.6/4.7）。本目录把 **VDIF 文件本身**变成真值：帧号就是时间槽，
所以"哪些槽有数据"是文件的纯属性，不经过任何读取过程。

```bash
file_truth.py <file.vdif> [--fps N] [--json out.json] [--quiet]
check_reader.py --log <f 侧 verbose 日志> --vdif <file.vdif> \
                [--truth truth.json] [--batch-json batches/<id>.json] [--fps N]
```

判据依赖 `FXCORR_LOGLEVEL=verbose`（`READPOS` 与 `GAPCHECK holes` 两行都在 verbose）。

**调用者（`gaps/` 的四个脚本，2026-09-19 起全部接入）**：`run_window.sh` 判 E4 = 0；
`run_startoffset.sh` 判 E5 启用且无超差（并自检"报红"）；`run_filler.sh` / `run_boundary.sh`
判退出码（E1–E3 无 finding）与 E4 = 0，各带一条自检把旧相对判据抓的缺陷形态造进日志
（`fxcorr/v4-plan.md` 的 A3）。**跳过也是退出码 0**——靠 E5 判定的脚本必须另行确认那一行
不是 `skipped`（`run_startoffset.sh` 的 `require_e5_active`）。

## 真值是什么

| 文件里的形态 | 时间槽 | 台账 |
|---|---|---|
| 帧号连续的数据帧 | 占用 | `data` 段 |
| 帧号跳过的若干帧 | 空 | 段间帧号差 = 缺口 |
| 全零头 / invalid 位置的帧（filler） | 空 | `filler` 段（占文件字节） |

两类损坏在台账里都只是"槽未被占用"——**缺口与 filler 在真值里一视同仁**，这正是
`gapinvalid` 想表达的语义，但后者由读取过程顺带发现、只能自证。缺口形式与 filler 形式
"描述同一段时间"这条硬约束因此不需要再手工对照，两边算出的真值洞天然相同。

fps 从文件的秒回绕点推断（回绕到 0 的那一帧，其前一帧帧号即 fps-1，取多个候选的最大值
以躲开落在秒边界的缺口）；文件不足一秒时必须 `--fps`。

## 五条断言

| | 查什么 | 抓什么 |
|---|---|---|
| **E1 定位** | 读取窗口起点 `f_lo` 在时间轴上的位置（相邻差 == subint 跨度） | 位置**跳变**（B/C 类：阶段偏、读过中断后偏）。整体偏移不在这里——相邻差恒定，见 E5 |
| **E2 数据** | `readoff` 处文件里的第一个数据帧，帧号应等于该 subint 报的 `firstfno` | 位置与数据对不上 |
| **E3 落点** | 文件真值算出的空洞槽区间 == `GAPCHECK holes` 报的槽区间 | B5 / 4.6 类"计数正常但落点错"，并区分**多标**与**漏标** |
| **E4 覆盖** | 所有读取窗口的并集覆盖了 batch 内的哪些数据帧 | **净损失**——文件里有、却从未进入任何窗口的帧。与 E3 的"标记口径"是两回事：E3 多标可能只是口径差异，E4 不会 |

E3 的两侧口径要对齐才可比：`gapinvalid`（→ `holes`）不是无效块的唯一来源，A 类起点偏移由
`lastcount` 承担（`fillValidFlags` 的前若干块），工具用 `uncorr < 0` 按同一公式反推补上。
窗口里没有数据帧（整段 filler）的 subint 读不出帧号来定锚，用相邻已知 `f_lo` 加跨度递推。

E3 的窗口起点由**帧号反推**（`f_lo ≡ framens (mod fps)`，取 ≤ 第一个数据帧的那个），不采信
读到的缓冲区自身——否则就是让 reader 自证。槽坐标 = `(帧号 − framens) mod fps`，与
`shiftFrameGaps` 的 `dst` 同一坐标系；块换算是 `槽 × payloadbytes/blockbytes + lastcount`，
只在要对到 `.sp` 时才需要，`GAPCHECK holes` 本身已经是槽区间。

## 与层 1 并列的层 2 单测：`test_corrections.cpp`

`applications/fxcorr-f/src/corrections.h` 是第二层（C2）：`Ledger` 把缺口**按时间槽**记账，
`gapShift(t)` 回答"时间位置 t 之前丢了多少帧"（并把落在洞里的 t 推到洞尾），`fillerFrames()`
回答"t 之前的 filler 有多少帧"。同样的零依赖：

```bash
g++ -std=c++11 -Wall -Iapplications/fxcorr-f/src fxcorr/test/reader/test_corrections.cpp \
    -o /tmp/test_corrections && /tmp/test_corrections
```

48 项断言，覆盖这一层存在的理由：**B5**（缺口跨 subint 边界——仍在前方的缺口不缩短本次读）、
**B6**（filler 之后的缺口——不减掉前面的 filler 就会晚那么多槽生效，测试里同时造了"错算"
的对照）、A 类起点偏移的两个符号、去重（同一 offset 的缺口只记一次、水位之下的 filler 不重复
计数）、两个缺口的连锁、以及层 1 与层 2 的级联（`walkFrameChain` 找出的缺口喂给 `Ledger`
再按时间查询）。

## 层 1 单测：`test_timeline.cpp`

`applications/fxcorr-f/src/frametimeline.h` 是 reader 分层的第一层（`v4-plan.md` 阶段 C）：
两个纯函数管"帧号 → 时间槽"的映射——`walkFrameChain`（帧号链、filler、缺口）与
`placeFrames`（槽映射与洞）。它们不依赖 fxcorrcommon、不碰文件、不打日志，所以单测是
**零依赖**的（本地机器即可跑）：

```bash
g++ -std=c++11 -Wall -Iapplications/fxcorr-f/src fxcorr/test/reader/test_timeline.cpp \
    -o /tmp/test_timeline && /tmp/test_timeline
```

54 项断言，覆盖：连续帧 / 缺口 / 秒回绕（不算缺口）/ 重复帧号 / 两种 filler 形态（全零头
与 invalid 位）/ 跨段接链 / 全 filler 段；槽映射的起点偏移 / 缺口占槽 / filler 丢弃 /
缺口盖过整窗 / 尾部不足；以及 **dry run 与实跑逐项相同**——B2 的"填槽与'够不够'共用一份
算法"这条约束自此可测。

**改 `frametimeline.h` 必须重跑它**：这是那一层唯一的测试面（`reader-model.md` 7.2）。

## 验证记录（2026-09-19，测试机）

合成四个场景（`fxcorr-sim` 的 `FXSIM_GAPS`，test.vex 配置，4 个 subint）：

| 场景 | 文件形态 | 真值洞（逐 subint） | E3 |
|---|---|---|---|
| 无中断 | 525 帧，无异常 | — | 绿 |
| 缺口 `1.0:40` | 525 帧，1 处缺 40 | sub2 `[122,133)`、sub3 `[0,31)` | 绿 |
| filler `1.0:40:f400` | 925 帧，400 filler | **同上** | 绿 |
| filler+紧随缺口 `1.0:40:f400,1.02:20` | 925 帧，400 filler，缺 60 | sub2 `[122,133)`、sub3 `[0,51)` | 绿 |

第三、四行是两个独立结论：① 缺口与 filler 两种形式算出的真值洞**逐项相同**（同一段时间），
② 当前实现（D-a/D-b 修复后）在这些形态下落点正确。

工具自检（篡改日志模拟已知缺陷，验证"能报红"）：把 `[0,51)` 改成 `[0,31)` → `missing 20 slots`；
再加上 `[60,70)` → `extra 10 slots`；两者退出码均为 1。

**E4 的合成复现（2026-09-19 补，`fxcorr/test/gaps/run_window.sh`）**：上表四个场景的 E4
**全部为 0**——它们的 filler 段后面没有跟着数据，被跳过的区段**全是 filler**，所以判据在
它们身上抓不到东西。t25362 的形态不同：filler 段后面跟着一段数据，而定位读按字节数读一段
**连续**区域，窗口跨进 filler 段时被占掉的宽度不会补回来，段后那段数据就落在窗口之外
（`reader-model.md` 4.7 的窗口长度缺口；前三次修复都只处理读位置的修正量）。

复现条件 = `span - (A - t_k) - G > 0`（A = filler 段的时间轴帧号、t_k = 它所在 subint 的窗口
起点、G = 同一次中断的缺口帧数）：`1.0:40:f400` 算出来是 −27.9，前移到 `0.6:40:f400` 是 69，
与实测逐项吻合。

| 场景 | E3 | E4 |
|---|---|---|
| `1.0:40:f400`（现有 filler 场景） | 0 槽 | **0 帧** |
| `0.6:40:f400`（`run_window.sh`） | extra 71 槽 | **69 帧** `[261,+69)` |
| 无中断（对照） | 0 槽 | **0 帧** |

E3 是**标记面**（洞的尾巴延伸到缓冲区末尾）、E4 是**数据面**——本形态两者同时错，判据在
无中断数据上不误报。

**E5 与起点偏移资产（2026-09-19 补，`fxcorr/test/gaps/run_startoffset.sh`）**：E1–E4 都
抓不到 A 类——E1 只看相邻差（整体偏时它恒定），E2/E3 是自洽性判据（文件里的帧号 vs
fxcorr 报的帧号，`anchorbytes` 整体偏掉时两边一起偏）。E5 引入**独立参照**：读窗口起点
的绝对时间（帧头给出）应等于 batch 起点 + 序号 × subint 跨度。

配套数据由 `FXSIM_STARTOFFSET=<帧数>` 造（文件起点晚于 batch 起点，`anchorbytes` 为负）。
E5 在三种情况下跳过——VDIF 秒与 `batch.json` 的 `start_mjd` 不同源（真实观测常态，
t25362 差 180 天）、READPOS 没覆盖每个 subint、文件含缺口/filler（B/C 类，读取位置本就该
偏离名义轴）。**跳过时退出码同样是 0**，所以脚本另外检查那一行不是 `skipped`。

| 场景 | E5 偏差 | 退出码 |
|---|---|---|
| 无中断对照 / 起点偏移 18 帧 | 3 帧（容差 8） | 0 |
| 自检：各 subint 的 readoff 减掉 `anchorbytes`（模拟 A1 漏算） | **21 帧** | **1** |

容差 8 只够抓 A1（整帧量级）；**A2 的 1 帧取整抓不到**（症状是 pcal 奇数序号 tone 相位
翻转，需要相位判据），见 `gaps/README.md` 的同一节。

## 真实验证：t25362（2026-09-19，`ssh difx`）

BA 站 ds_2（有 filler，1.5 GB / 192076 帧）与 ds_0（无 filler 的对照），batch `61037_24611`
（2200 subint × 5.12 ms，fps 16000，A 类起点偏移 18 帧）。

**台账与人工扫描逐项吻合**：ds_2 的 8 段 filler 为 81/82/229/180/17/49/16/508 = 1162 帧、
8 处缺口共 143 帧——正是 `reader-model.md` 4.6 记下的数字（当时靠手写脚本数出来）。

ds_2 的对账结论（**B2 之前**，D-a/D-b 修复后重跑）：

```
  sub   readoff     f_lo         framens first  E1  truth holes             fxcorr holes  verdict
  816   536111904   249225844016 4016   4016   ok  [38,48) [61,71)          [38,83)       extra [71,83)
  817   537919104   249225844098 4098   4098   ok  [14,42) [44,66)          [14,83)       extra [66,83)
  2018  1331038944  249225942484 6484   6484   ok  [32,34) [36,42) [47,49)  [32,34) [36,83)  extra [49,83)
summary     : 2181 subints, 3 with a finding; E3 extra 63 slots, missing 0 slots
E4 coverage : 80 data frames inside the batch never fell inside any read window
              -> [66830,+24) [67055,+18) [165800,+38)
              (12406 further frames outside the batch window, expected ...)
```

**这 80 帧是净损失，不是标记口径**：它们落在 batch 内、文件里有数据，却从未进入任何读取
窗口——定位读按字节数读**一段连续**区域，本 subint 的数据被中间的 filler 隔开时，段外的
部分既读不到、也不会出现在任何 `.sp` 里（`shiftFrameGaps` 只是诚实地把没填满的槽标成
无效，就是那 63 槽 extra）。上游 `vdifmux` 的顺序读会滑过 filler 继续填满输出缓冲，所以
mpifxcorr 没有这个形态。

**B2 之后（2026-09-19 重跑，同一条命令）**：

```
summary     : 2181 subints, 0 with a finding; E3 extra 0 slots, missing 0 slots
E4 coverage : 0 data frames inside the batch never fell inside any read window
              (12406 further frames outside the batch window, expected ...)
```

**80 帧的净损失归零，E3 那 63 槽 extra 同时消失**（同一根因的标记面症状：洞的尾巴一直
延伸到缓冲区末尾），2181 个 subint 零 finding。`GAPCHECK summary` 的 `missing 143` /
`filler 1162` 与修复前逐项相同——修的是窗口长度，账没有变。ds_2 有 **4 个 subint 的
`nframes > slots`**（最长 540 帧 vs 83 槽），全落在 filler 段上（正是 4.6 记的过渡区
816/817/2019），这是多读路径确实被走到的证据；ds_0 一次都没有。

**ds_0 对照（同一观测、无 filler、缺口更多）零 finding**：`2181 subints, 0 with a finding;
E3 extra 0 slots, missing 0 slots; E4 coverage: 0 data frames ... never fell inside any read
window`（退出码 0）。这既是读取路径在那条线上的干净证据，也是 E4 判据的假阳性检验——
无 filler 的文件里窗口连续覆盖，一条都不会报。

## 真实数据回归：`run_t25362.sh`（阶段 D 固化）

```bash
./fxcorr/test/reader/run_t25362.sh [--no-sync] [--no-build] [--no-run] [host]
```

从**本地**驱动：`make sync` → `ssh difx` 上编译安装 fxcorr-f → 跑 BA 的 ds_2（有 filler）
与 ds_0（无 filler 对照）→ 两项判据：

1. **基线**：`GAPCHECK summary` 的每个字段与写死的基线逐项比对（ds_2 `buffers 2181 frames
   180892 … missing 143 filler 1162 …`、ds_0 `… missing 145 filler 0 …`）。基线是 B2 与
   阶段 C 三次重构后**逐字节不变**的实测值——任何 reader 改动都该先解释它为什么不该动；
2. **真值对账**：`check_reader.py` 的 E1–E4（绝对判据），两个 ds 都必须零 finding、E4 = 0。

`--no-run` 用现成日志重复对账，不必重跑（改判据工具时用）。

**自检（2026-09-19）**：篡改 difx 上的日志（`missing frames 143 → 999` 加一条假 `READPOS`）
→ 两条判据都报红、退出码 1，而 ds_0 对照仍绿；恢复日志后退出码 0。

## 待办与边界

- **t25362 的残留定性**是本目录要回答的第一个问题（`reader-model.md` 4.7 的 ~40 帧/积分），
  数据在 `ssh difx`，`make sync` 不覆盖该机，需单独 rsync。
- E1 对**读取位置被钳在文件首帧**的那些 subint 报 `anchor` 而不判定：batch 起点之前的
  delay 修正被 `locate` 的跳块吸收成 `lastcount`，窗口起点与时间轴对不齐是语义而非缺陷。
- 台账按固定 stride 扫描，不处理非 VDIF 字节（`vdifmux` 会滑过丢弃的那种）：fxcorr-f 的
  `KIND_VDIF` 路径也不做滑过，两者同一假设。格式异常（帧长与首帧不符）的帧记为 `other`。
- 大文件（t25362 的 ds_2 为 1.5 GB / 19 万帧）按 4096 帧分块顺序读，整表扫描秒级。
