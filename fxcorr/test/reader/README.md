# reader 对账：文件真值（`file_truth.py` / `check_reader.py`）

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

ds_2 的对账结论（当前代码，D-a/D-b 修复后重跑）：

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

**ds_0 对照（同一观测、无 filler、缺口更多）零 finding**：`2181 subints, 0 with a finding;
E3 extra 0 slots, missing 0 slots; E4 coverage: 0 data frames ... never fell inside any read
window`（退出码 0）。这既是读取路径在那条线上的干净证据，也是 E4 判据的假阳性检验——
无 filler 的文件里窗口连续覆盖，一条都不会报。

## 待办与边界

- **t25362 的残留定性**是本目录要回答的第一个问题（`reader-model.md` 4.7 的 ~40 帧/积分），
  数据在 `ssh difx`，`make sync` 不覆盖该机，需单独 rsync。
- E1 对**读取位置被钳在文件首帧**的那些 subint 报 `anchor` 而不判定：batch 起点之前的
  delay 修正被 `locate` 的跳块吸收成 `lastcount`，窗口起点与时间轴对不齐是语义而非缺陷。
- 台账按固定 stride 扫描，不处理非 VDIF 字节（`vdifmux` 会滑过丢弃的那种）：fxcorr-f 的
  `KIND_VDIF` 路径也不做滑过，两者同一假设。格式异常（帧长与首帧不符）的帧记为 `other`。
- 大文件（t25362 的 ds_2 为 1.5 GB / 19 万帧）按 4096 帧分块顺序读，整表扫描秒级。
