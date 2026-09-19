# 缺口与 filler 处理检验（FXSIM_GAPS）

## 用途

回归 fxcorr-f 的**记录中断**处理：文件中间缺帧（`gapshiftbytes`）与 filler 帧占位
（`fillershiftbytes`）两条路径，以及它们**共同描述同一段时间**这一点。

这两个能力此前的全部覆盖都来自 t25362 真实观测——它是唯一暴露过它们的数据集，
而真实数据没有"正确值"可对照，改坏了只能靠对拍差异反推。本目录用 fxcorr-sim 造出
等价的病态数据，让"缺口/filler 没被改坏"能在合成数据上一键复现。

背景与实现见 `fxcorr/reader-model.md`（读模型对照与 C 类 filler 缺陷）与
`applications/fxcorr-f/CLAUDE.md`（datareader 实现要点）。

## 生成

`fxcorr-sim` 的 `FXSIM_GAPS` 环境变量（legacy 与新路径都生效；不设 = 行为不变）：

```
FXSIM_GAPS="<秒>:<帧数>[:f[<filler帧数>]],..."    # 逗号分隔多条
```

- `<秒>`：相对 batch 起点的时间，**必须先建好 workdir 目录**（见下方坑）
- `<帧数>`：该处**真实丢失**的帧数（帧号跳过这么多）
- `:f`：改用 filler 形式——额外写入**帧头全零**帧占位，默认每丢一帧写一帧
- `:f<N>`：filler 帧数单独指定（2026-09-18 加）。t25362 的中断就是这种形态：每次写
  81..508 帧全零，而帧号只跳过 10..63 帧。**`filler ≫ 缺口` 是把读位置带偏的形态**，
  `f470` 这类写法才能复现（见 `run_filler.sh`）

两种形式在**同一位置**造出同一个中断：缺口形式让文件比时间轴短，filler 形式让文件
比时间轴长（多出占位字节），而**帧号范围相同**。

```bash
# 先建目录（make_testdata.sh 只在目录已存在时才把它当 workdir）
mkdir -p <workdir>

# T2：缺口形式
FXSIM_GAPS="0.5:10,1.5:63" fxcorr-sim station <batch_id> T2 <workdir>

# T1：同位置、但用 filler 占位（读同一个 common 信号，内容一致）
FXSIM_GAPS="0.5:10:f,1.5:63:f" fxcorr-sim station <batch_id> T1 <workdir>
```

整链生成（含 vex2difx/difxcalc 与 batch.json）用
`FXSIM_GAPS=... fxcorr/make_testdata.sh <workdir>`，环境变量由脚本透传。

## 验收判据

数据侧（直接扫 VDIF 帧头；`FPS` 从 .input 帧结构反推，test.vex 下为 250）：

| | 文件帧数 | filler 帧 | 帧号缺口 | 帧号范围 |
|---|---|---|---|---|
| T1（`:f`） | 598 | **73**（段 125–134、375–437） | 73 | 598 |
| T2（无 `:f`） | 525 | 0 | **73**（缺 10、缺 63） | 598 |

**两者帧号范围必须相同**——这是"描述同一段时间"的硬约束，也是 filler 语义写错时
最先崩的一条（见下方坑）。

fxcorr-f 侧（`GAPCHECK summary`，info 级默认可见）：

| | missing frames | filler frames |
|---|---|---|
| T1（filler 形式） | 73 | **73** |
| T2（缺口形式） | 73 | 0 |

即：两种形式报出**相同的缺失量**，差别只在 filler 计数。

## 验证记录（2026-09-18，测试机 gjnode5）

```
T1 (filler 形式): 文件 598 帧, filler 73 帧 ['125-134', '375-437'], 缺口 2 处共 73 帧, 帧号范围 598
T2 (缺口 形式):   文件 525 帧, filler  0 帧 [],                     缺口 2 处共 73 帧, 帧号范围 598

fxcorr-f T1: GAPCHECK summary: buffers 4 frames 476 discontinuities 2 missing frames 73 filler frames 73
fxcorr-f T2: GAPCHECK summary: buffers 4 frames 532 discontinuities 2 missing frames 73 filler frames 0
```

同批验证中另有一条基线（`FXSIM_GAPS="1.0:10,2.0:63"`）：T2 报 `missing frames 73`，
T1 报 `filler frames 33`——**后者是测试数据设计问题，不是代码缺陷**：0.524288s 的
subint 下 batch 只覆盖约 533 帧位置，而 2.0s 处的中断落在 510 帧之后，其二段尾部
40 帧出了 batch 窗口，本就不该被统计。造数据时把中断放在窗口内（如 0.5s / 1.5s）。

## 缺口跨越 subint 边界（`run_boundary.sh`）

上表的判据只覆盖"缺口**被计数**"，不覆盖"缺口的**时间位置被放对**"——t25362 暴露的 B5 正好
落在两者之间：计数全正常，只有含缺口的那个积分偏。**根因、症状指纹见 `fxcorr/reader-model.md` 4.5。**

```bash
mkdir -p <workdir>
./run_boundary.sh <workdir>      # 三个场景各生成一次数据、跑一次 fxcorr-f
```

同站同 batch 生成三次（无缺口作对照、两个缺口位置），只差一个 `FXSIM_GAPS`：

| 场景 | `FXSIM_GAPS` | 缺口时间跨度 | 判据 |
|---|---|---|---|
| A | `0.52:30` | [130,160)，跨 subint 1/2 的边界 131.072 | 每个 subint 的 `firstfno` **不得早于**对照（缺口按"已检测到"累计时，subint 2 读成 98 而对 128） |
| B | `0.48:30` | [120,150)，把 subint 2 的读取位置 128 盖住 | 该 subint 的权重数组**从第 0 块起连续为 0**，块数 = 帧差 22 × `blocks_per_send` ÷ 子带帧数 |

场景 B 查的是另一条：读取位置落到缺口之后（帧 150）时，buffer 的第一帧**不再等于** subint
起点，`shiftFrameGaps` 必须按帧号把它放到 22 帧处、并把这个洞标成无效。此时帧号是连续的，
不按偏移判断就根本不会调用它，洞会被当作数据积分。

计数判据（两个场景共用）：`missing frames 30`、`filler frames 0`。

**注意 `GAPCHECK`/`READPOS` 的编号与 .sp 的 subint 索引可能不同**（t25362 实测差 18，成因见 `fxcorr/reader-model.md` 4.5 末段）——对号入座前先确认。判据依赖 fps，见下方坑。

## 验证记录（2026-09-18，测试机，修复 B5 前后各一次）

修复前（`datareader.cpp` 的缺口按"已检测到"累计、`shiftFrameGaps` 从槽 0 起）：

```
场景 A: subint 2: firstfno 98 < 对照 128  → FAIL
场景 B: subint 2 零权重块 0 个            → FAIL
```

修复后：

```
--- 无缺口（对照）---   1:0  2:128  3:9  4:140
--- 场景 A 0.52:30 ---  READPOS 与对照逐项相等（subint 2 回到 128）；missing 30 / filler 0
--- 场景 B 0.48:30 ---  subint 2 firstfno 150（对照 128，跳过 22 帧）
                        零权重块 0-85 = 86 块，期望 86  ✓
PASS
```

同批回归：`cmp5` 无缺口数据的 `band_00.sp` 与修复前 md5 相同（逐字节不变）；T1/T2 计数判据
（上表 73/73 与 73/0）与 README 记录一致。

**mpifxcorr 对拍在有缺口数据上不是有效判据**：两者的 subint 窗口差一个 delay 修正
（fxcorr 的块起点是 delay 修正后的时刻），缺口与窗口的交叠不同，weight 必然不同。
有缺口数据的验收用本脚本的定位判据，不用 `cmp_swin.py`。

## filler 远长于真实缺口（`run_filler.sh`）

t25362 的 BA ds_2：每次中断写 81..508 帧全零 filler，帧号却只跳过 10..63 帧。
`filler ≫ 缺口` 把读位置带偏（见 `fxcorr/reader-model.md` 4.6/4.7），而 `:f` 的
默认写法（filler 帧数 == 丢失帧数）复现不了。`run_filler.sh` 用 `:f<N>` 造这种形态：

```bash
mkdir -p <workdir>
./run_filler.sh <workdir>        # 默认 GAPSPEC="1.0:40"、FILLSPEC="1.0:40:f400"
```

同一位置生成两次数据（**只缺 40 帧** / **缺同样 40 帧 + 写 400 帧全零**），另加一次无中断
对照，判据是**两种形式逐 subint 的无效块一致**（±2 块）：两种形式描述同一段时间，
filler 只是多留了占位字节。

| | 无中断对照 | 缺口形式 | filler 形式 |
|---|---|---|---|
| 修复前 | 11 | 169（subint 3/4 = 122/0） | **1071（subint 3/4 = 512/512，整块无效）** |
| 修复后 | 11 | 169 | **169（subint 3/4 = 122/0）** |

（测试机实测，2026-09-18；`GAPCHECK summary` 两侧都报 `missing frames 40`、filler 分别 0 与 400，
即计数本来就对，坏的是落位。）

**为什么单看总数不够、还要看对照**：缺口形式的 169 = 对照的 11（与中断无关的常数头块）
+ 158（≈ 40 帧 × 3.906 块/帧 = 156），所以判据取「两种形式逐 subint 相等」，而不是硬编码期望值——
块/帧随配置变（test.vex 下 `bps=512`、subint 131.072 帧 → 3.906），硬编码会随配置漂。

## 坑（续）

## 坑

- **filler 必须推进帧号**：中断是"真实过去了的时间"，帧号要跳过丢失的帧数；filler
  只是额外留下占位**字节**。首版实现让 filler 不推进帧号，结果 filler 形式的数据
  帧号范围只有 525 而缺口形式是 598——同一段观测被描述成了两个长度，f 侧据此算出的
  `firstfno` 序列整体错位。`reader-model.md` 的 filler 判定「帧号跳跃只反映真实丢失，与 filler 帧数
  无关」说的就是这件事。
- **`make_testdata.sh` 要求 workdir 已存在**：它用 `[ -d "$1" ]` 区分 workdir 与
  tone 位置参数，目录不存在时会静默退化到 cwd，把数据写进当前目录。
- **PATH**：`make_testdata.sh` 只在 `vex2difx` 不在 PATH 时才 source `setup.bash`；
  测试机上若系统 PATH 已有另一套 difx，fxcorr 的工具不会进 PATH，需显式
  `export PATH=/usr/local/difx/bin:$PATH`。
- **判据依赖 fps**：扫帧号时 `% FPS` 用错会把正常回绕当成巨量缺口（曾把缺 10 帧
  算成缺 7760 帧）。fps 从 .input 的 `getFramePayloadBytes × 4 × getFramesPerSecond`
  反推，不要硬编码。
