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
FXSIM_GAPS="<秒>:<帧数>[:f],..."    # 逗号分隔多条
```

- `<秒>`：相对 batch 起点的时间，**必须先建好 workdir 目录**（见下方坑）
- `<帧数>`：该处**真实丢失**的帧数（帧号跳过这么多）
- `:f`：改用 filler 形式——额外写入同样多的**帧头全零**帧占位

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

## 未覆盖场景：缺口跨越 subint 边界（2026-09-18 诊断）

上表的判据只覆盖"缺口**被计数**"，不覆盖"缺口的**时间位置被放对**"——t25362 暴露的缺陷正好
落在两者之间：计数全正常，只有含缺口的那个积分的 weight 偏（实测差 1.6e-3）。**根因、症状
指纹与修复方向见 `fxcorr/reader-model.md` 4.5 与 7.5。**

复现要点：`FXSIM_GAPS` 的中断位置要选在**缺口能跨越 subint 边界**的地方，且缺口总长
**大于**跨界那个 subint 的剩余部分（t25362 是 subint 5.12 ms、第一组缺口 72 帧 = 4.5 ms，
落在 subint 834 的末尾并伸进 835）。

验收判据必须用 `READPOS`，summary 看不出来：

| | 判据 |
|---|---|
| 计数 | 与上表一致（`missing frames` = 缺口总帧数） |
| **定位** | 每个 subint 的 `firstfno` = 该 subint 起点的时间轴帧号（`batchrel × fps` 的秒内余数，相邻 subint 差 81.92 帧取整）；跨界后那个 subint 偏早"尚未到达的缺口字节数"即为本条缺陷 |
| 产物 | 含缺口的积分 weight 与 mpifxcorr 基准一致（当前**未达标**，即为待修目标） |

**注意 `GAPCHECK`/`READPOS` 的编号与 .sp 的 subint 索引可能不同**（t25362 实测差 18，成因见 `fxcorr/reader-model.md` 4.5 末段）——对号入座前先确认。

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
