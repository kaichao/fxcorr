# fxcorr-f 读取模型与读取缺陷分析

**版本**：1.0（2026-09-18）
**适用**：fxcorr-f 的数据读取层（`applications/fxcorr-f/src/datareader.{h,cpp}`）
**定位**：读取路径的**单一权威分析文档**——读模型、缺陷根因、症状指纹、验收判据、改造建议。

**与其他文档的关系**（本节是分工，不是重复）：

| 文档 | 分工 |
|---|---|
| `data-spec.md` 5.2 | **数据规格**：目录布局、帧结构、`anchorbytes` 与起点语义的约束、raw/ 的输入要求 |
| `applications/fxcorr-f/CLAUDE.md` | **该目录的结构与接口**：调用方式、文件职责、非读取类的实现要点 |
| `fxcorr/test/gaps/README.md` | **测试资产**：如何用 `FXSIM_GAPS` 造出病态数据、复现命令 |
| `algo-plan.md` P10/P11/P12 | **历史实施记录**：格式覆盖、reader 语义补全、真实数据病态处理当时怎么做的 |
| **本文** | **分析主体**：为什么读取反复出问题、缺陷如何分类、怎么诊断、后续怎么改 |

---

## 1. 问题的提出

### 1.1 现象

fxcorr-f 的改造可视为已完成（V1 全链路跑通、合成数据与 mpifxcorr 逐位全等），但**数据读取一直在改**：先是 P10 补格式覆盖，再是 P11 补读入语义，然后是 t25362 暴露的一连串定位缺陷。三轮改造的对象是同一个 `datareader.cpp`，且每轮修完都会在下一次真实数据上暴露新问题。

### 1.2 疑问与回答

**疑问**：mpifxcorr 早就支持 FILE 数据流（`vdiffile.cpp`），读取逻辑是现成的，为什么改到 fxcorr-f 就这么难对？

**回答**：不是"支持不支持"的问题，是**读模型换了**。两边都在读同一份 VDIF 文件，但：

- `mpifxcorr` 从文件头**顺序读**，每段数据的起始时间取自**数据内的帧时间戳**——"读到哪了"是数据自己说的；
- `fxcorr-f` 按 batch 时间轴**定位读**，`字节位置 = (时间 − batch 起点) × 速率`——"该读哪"是算出来的，算错了没有任何东西会当场报错。

顺序读里由 `vdifmux`/帧时间戳**顺带做对**的事，在定位读里全部变成**必须显式求解的量**。改造的难度不在搬代码，在于**识别出哪些量原来是隐性的**。

### 1.3 三轮演进的暴露条件

| 轮次 | 补什么 | 暴露条件 | 为什么前一轮测不出来 |
|---|---|---|---|
| P10 | 读**什么**（格式覆盖：Mk5B/LBA/多线程 VDIF/mark5access 通用流） | 换一种输入格式 | 合成数据只用单线程 VDIF |
| P11 | **怎么读**（上游 `calculateControlParams` 的延迟重对齐与 valid flag 跨段语义） | 几何 delay ≠ 0 | 合成测试 .vex 两站坐标重合、delay = 0 |
| P12（见 `algo-plan.md` P12 节；源码注释同编号） | 数据**不听话**时怎么办（起点偏移 / 缺口 / filler） | 真实观测 t25362 | 合成数据帧连续、起点恰在 batch 起点、无 filler |

三行合起来是一句话：**此前所有对拍都在"理想数据 × 理想配置"下进行，而这些前提在真实观测里一条都不成立。**

### 1.4 关于早期分析草稿的修正

改造期间有一份讨论稿（工作区 `tmp/fxcorr-f-file-read-vs-mpifxcorr.md`，**未纳入版本控制**）从"读模型不同"切入，方向正确，但有三处需要修正，本文以修正后的表述为准：

1. **"mpifxcorr 对缺口较免疫"——不准确**。mpifxcorr 并非免疫，而是把缺口交给 `vdifmux` 按帧号成流、在缺口处**补 invalid 帧**，于是它的无效是**帧级、时间对齐**的；fxcorr-f 的无效是**块级、位置需要自己算**。差异不在免不免疫，在**时间对齐由谁负责**（数据 vs 调用方）。
2. **filler 的描述不完整**。只说了"占字节不占时间轴"，漏了最凶的一点：**filler 段的两端往往同时夹着真实缺口**（记录中断在段的两端各留一处），t25362 的 BA ds_2 实测是 **143 帧缺口 + 1162 帧 filler**。只数 filler 不数缺口，会让该流的读取位置**持续偏后**（缺陷 C5/C6）。
3. **"要么更接近 mpifxcorr（batch 内也尽量顺读）"——不可选项**。定位读不是实现选择，而是 batch 架构的前提（见 7.1）。正确的提法不是"换回顺序读"，而是"让定位读的修正量可验证"。

---

## 2. 两种读模型

### 2.1 顺序读（mpifxcorr）

```
打开文件 → 顺序读帧 → 用帧内时间戳驱动处理 → 缺帧由 vdifmux 补 invalid 帧
```

- **时间来自数据**：每段的起点时间从帧头读出，文件起点相对 batch 起点的距离因此**自动获得**，无需计算。
- **缺口的处理在流里**：数据流层在缺口处产出 invalid 标记（`vdifmux` 的 `setVDIFFrameInvalid`，`libraries/vdifio/src/vdifmux.c:948`），输出流的时间轴始终完整，下游按"帧/样本是否有效"处理即可。
- **有效性来自帧头**：`Mk5Mode::unpack` 的 `goodsamples` 由 `mark5_unpack_with_offset` 与 `blank_vdif_EDV4` 判出的 invalid 样本共同决定，无效是**帧/样本级、时间对齐**的。
- 代价：必须从头扫，无法直接跳到任意时间段。

### 2.2 定位读（fxcorr-f）

```
读 batch.json 得 batch 时间窗
  → 算 anchorbytes（文件起点相对 batch 起点的字节偏移）
  → 逐 subint：字节位置 = 锚点 + (subint 起点 − batch 起点) × 速率
  → seek → 读 → 修正（缺口/filler）→ 整帧对齐 → 标注有效块 → Mode::unpack
```

- **位置来自时间**：`locate()` 的线性换算 `framesin = bufbytes / payloadbytes`，`*fileoffset = anchorbytes + framesin × framebytes`。
- **时间轴是算出来的**：文件自身不携带"这一段属于哪个 subint"的信息，映射一旦有偏差，读到的是**别的时间的数据**，且不会报错。
- **有效性是派生量**：`gapinvalid` → `fillValidFlags`，无效块由"数据不在该位置"推算，而不是从数据里读出来。

### 2.3 隐性 ↔ 显式对照

| 量 | mpifxcorr（顺序读） | fxcorr-f（定位读） |
|---|---|---|
| 文件起点偏移 | 由帧时间戳自动获得 | 必须算 `anchorbytes`（首帧 (sec, frame number) → `floor(Δ×fps + 0.5) × framebytes`） |
| 读到哪里了 | 数据自己说的 | `locate()` 线性换算 |
| 中间缺帧 | 数据流层（`vdifmux`）在缺口处标 invalid 位 | `checkFrameContinuity` 检测 + `gapspan`/`gapshiftAt` 修正读取位置 + `shiftFrameGaps` 整帧后移 + `gapinvalid` 标无效 |
| filler 帧 | 顺读时与普通帧一同流过（上游**无专门识别**；这类帧对 mpifxcorr 结果的实际影响**未实测**） | 必须单独识别（`vdifIsFiller`）、**方向与缺口相反**地修正、并从帧号链里跳过 |
| 有效性 | 来自帧头（`unpack` 的 goodsamples） | 由位置推算（`fillValidFlags` 按块判界） |
| 出错的表现 | 局部、可定位 | **整段静默错位**——读到了错误时间的数据，全部标为有效 |

最后一行是全部麻烦的来源：**定位读的错误是静默的**。顺序读读错了地点，帧时间戳会当场揭穿；定位读读错了地点，数据照常解包、照常出谱、照常写 SWIN，只有和基准比对时才会表现为"某站/某 ds/某积分的大片差异"。

**可解性**：`anchorbytes` 与缺口/filler 修正量都**由文件自身推出**（首帧的 (秒, 帧号) + 帧号连续性扫描），**不需要额外元数据**——给 fxcorr-f 喂数据时不必附带任何定位辅助文件。

---

## 3. 被破坏的三条上游假设

mpifxcorr 的数据路径建立在三条假设上。它们在**顺序读模型下天然成立**，因此既没有写进上游代码注释，也没有写进任何规范——拆分时无从识别。换成定位读后，每一条都变成必须显式求解的量。

### 3.1 假设一：文件从 batch 起点开始

- **上游为何不写**：顺序读不关心文件起点在哪，读到第一帧就知道了。
- **真实观测否证**：各站记录系统的帧计数器不同相，文件可以从某一秒的中途开始。t25362：BA 首帧帧号 1269、比 batch 起点晚 **79.312 ms**（起点偏移 = 1269 帧 × 62.5 µs），S6 恰在秒边界。
- **定位读的后果**：整个文件按线性时间读偏，**该站参与的所有记录**（含无缺口积分）可见度差约 1%、weight 差 0.4%。
- **显式解**：`anchorbytes`，见缺陷 A1。

### 3.2 假设二：帧号连续（字节流与时间轴等长）

- **上游为何不写**：数据流层保证输出流的时间轴连续，缺帧处被标为 invalid，读到的是"帧在、内容无效"而不是"帧不在"。
- **真实观测否证**：两种形态同时存在，且**方向相反**——
  - **缺口**：文件中间直接缺帧，字节流**短**于时间轴，线性换算的位置**偏后**（t25362 的 BA 每个 ds 有 7–8 处、单处缺 1–64 帧，合计 144–148 帧／12 秒；S6 无中间缺口、仅部分 ds 在文件末尾少 1 帧）；
  - **filler**：记录系统写入帧头全零的帧占位，字节流**长**于时间轴，线性换算的位置**偏前**（t25362 的 BA ds_2 有 1162 帧 filler / 8 段）。
- **定位读的后果**：两种偏移**持续累积**，之后读到的每一段都是错误位置的数据。
- **显式解**：`gapspan`（按时间轴位置查询，`gapshiftAt`）/ `fillershiftbytes`，见缺陷 B 类与 C 类。

### 3.3 假设三：读到即有效

- **上游为何不写**：有效性从帧头读出，与读取位置无关。
- **真实观测否证**：缺口/filler 覆盖的时间段内，文件里没有对应数据（或只有占位字节）。
- **定位读的后果**：线性换算照常返回一个位置，`readSubint` 照常读到字节（可能是缺口之后的数据），valid flags 全置 1——**数据内容错了，但元数据说它是对的**。
- **显式解**：`gapinvalid` → `fillValidFlags`，且必须**每个 subint 重建**，见缺陷 B4。

### 3.4 方法论教训

拆分上游代码时，凡涉及**"位置 ↔ 时间"互换**的式子，都要问一遍：

> 如果数据不连续、文件起点不听话，这个式子还成立吗？

答案是否定的地方，就是必须新增的显式修正量。三条假设对应的三个修正量（`anchorbytes` / `gapspan`+`fillershiftbytes` / `gapinvalid`）**都不是新算法**，而是把上游隐式承担的责任搬到调用方。

配套的第二条教训：**合成数据全绿不等于真实数据能跑对**。`fxcorr/test/` 的 P0–P11 全部特性测试都用 fxcorr-sim 生成的理想数据，上述病态情况在 t25362 之前**从未被测试覆盖**。

---

## 4. 缺陷总表

按**根因**分四组。每条给出：病根 → 症状指纹（对外表现，用于反查）→ 实测数字 → 判据 → 状态。

状态的三个取值：**已修**（修复已验证）、**未修**（已定位、有方案）、**未覆盖**（已知场景、尚无测试资产）。

### 4.1 A 类：起点——文件起点 ≠ batch 起点

| 编号 | 病根 | 症状指纹 | 实测 | 状态 |
|---|---|---|---|---|
| **A1** | `anchorbytes` 未算进读取位置 | 该站参与的**所有**记录（含无缺口积分）可见度差 ~1%、weight 差 0.4%，**另一站完全一致** | 锚点偏一帧 = 62.5 µs（16 kfps） | 已修 |
| **A2** | `anchorbytes` 为负时取整用 C++ 截断（= 向上取整）而非 `floor(x+0.5)` | PCAL 里**序号为奇数的 tone 全部相位翻转**（每个 ds、每个积分都是"隔一个反号"） | 62.5 µs 比 5 MHz pcal 周期的整数倍恰好差 100 ns | 已修 |
| **A3** | `fileoffset < 0` 时把**整个 subint** 判无效 | 该积分凭空少掉一整段（起点所在 subint 是"头部无数据、尾部有数据"） | 合成数据基准 w 0.912 → 0.5；修复后 t25362 积分 0 的 w = 0.903898 对基准 0.903949 | 已修 |

**A 类的辨识要点**：症状覆盖面是**"一个站的全部"**。与 B/C 类的关键区别是——**无缺口的积分也错**。A1 与 A2 极易被误判成缺口问题（都表现为量级不小的系统性偏差），A2 更易被误读成共轭或 sideband 问题（符号模式规整）。

**A3 的正确做法**：`skippedframes = ceil(-fileoffset/framebytes)` → `skippedblocks = ceil(skippedframes×payloadbytes/blockbytes)` 折进 `lastcount`、再从**文件头**读起；只有 `skippedblocks >= blockspersend`（整个 subint 落在起点之前）才 `INVALID_SUBINT`。

**A2 的影响范围**：四条格式路径（KIND_VDIF / KIND_MARK5B / KIND_MK5STREAM / KIND_LBA）的 `anchorbytes` 现已**统一用 `floor`**；其中后三条原先靠 `batchstartabsns < filefirstabsns` 的前置检查保证该值非负，故未暴露——统一为 `floor` 属**防御性修改**。

### 4.2 B 类：缺口——字节流**短**于时间轴

| 编号 | 病根 | 症状指纹 | 实测 | 状态 |
|---|---|---|---|---|
| **B1** | 缺口不能用线性时间换算（不修正就读到缺口之后的错位数据，且全部标为有效） | 缺口之后的数据"读到了、且全部标为有效" | — | 已修 |
| **B2** | 缺口只在 buffer **内部**找——跨 buffer 的帧号步进是"读取位置按 subint 长度推进、帧号按帧长推进"的**正常漂移** | 跨 buffer 边界报出大量**假**缺口 | 合成无缺口数据实测 ±4 帧 | 已修 |
| **B3** | 缺口去重键错误（用帧号——每秒回绕不可比；或用未修正位置——同一缺口在不同 buffer 里相差整数个 subint） | `missing frames` 虚增 3–4 倍 | 145 虚增到 536 | 已修 |
| **B4** | `gapinvalid` 未按 subint 重建 | weight **全局塌陷**（缺口之后的所有 subint） | — | 已修 |
| **B5** | 缺口按"**已检测到**"而非"**时间位置在读取点之前**"累计 | **只有含缺口的积分** weight 偏，其余积分与基准一致在 7e-7 浮动内 | 积分 4 差 1.6e-3、积分 10 差 3.3e-3 | 已修（2026-09-18，见 4.5） |

**B3 的正解**：去重键用**修正后**的读取位置（`lastfileoffset + i*framebytes`）——它随缺口被计入而**单调**（即使修正量增大时会回退也仍单调可比）。

**B5 详见 4.5**。

### 4.3 C 类：filler——字节流**长**于时间轴

C 类的核心语义：**filler 帧占文件字节、不占时间轴**。判据是段后帧号只跳过**真实丢失**的帧数，与 filler 帧数无关。t25362 的 BA ds_2 实测：段前帧号 4053、段后 4064（只缺 10 帧），而该段**本身有 81 帧**。识别**不需要额外元数据**——填充帧的帧头本身就是判据（invalid 位置位，或 word0/word1/word2 全零使 invalid 位=0、帧长度字段=0）。

**方向不能反**：缺口让读取位置**减**（文件短），filler 让读取位置**加**（文件长）。`readSubint` 的修正是 `fileoffset += fillershiftbytes - gapshiftAt(本 subint 的帧序号) × framebytes`。

| 编号 | 病根 | 症状指纹 | 实测 | 状态 |
|---|---|---|---|---|
| **C1** | 把 filler 的帧号跳跃当成缺口（filler 帧头全零使帧号回到 0，按模 fps 算出一个整秒的假缺失） | `missing frames` 虚增数百倍 | 报 64066，真值 143（**448 倍**） | 已修 |
| **C2** | filler 帧未被识别，该 ds 的读取位置从第一个 filler 段起持续偏移 | **某个 ds 参与的所有 frq** 从第一个 filler 段起错位，**其他 ds 完全正常** | — | 已修 |
| **C3** | 发现 filler 后按"跳过距离 = 全 filler"推断，但跳过的区段里**还跟着数据帧** → 下一次前移更多、正反馈失控 | `missing frames` 虚增到几万，读取位置前移累积到数百 MB | 虚增到 44650；前移 514 MB／4 秒 | 已修 |
| **C4** | 位置前移会**跳过**一段从未扫描的文件（长度恰等于刚发现的 filler 字节） | filler 扫描漏计 | 只计到 626／1162 | 已修 |
| **C5** | 缺口落在"位置前移"让出的区段、或补扫区段与 buffer 的**接缝**上——主循环与补扫两边都不检测 | 某 ds 参与的**所有**组合可见度分量**符号相反、数值无关**，而 **weight 完全相同（都 1.0）** | 真值 143 帧只记 2 帧，偏后 70 帧 = 4.375 ms | 已修 |
| **C6** | 补扫推进了帧号链却没把新链尾存回去，后续每个补扫都从同一个旧帧号重新比较 | 同 C5 但 `missing frames` 虚增到真值的 1.5 倍以上 | 143 虚增到 252 | 已修 |

**C3 的正解**：必须**实扫该区段**逐帧判定（`countFillerRange`），不能按距离推断。段长几百 KB、只在发现 filler 段时触发。

**C5 的辨识要点**（极易误判）：`weight` 完全一致（都 1.0）而可见度**符号相反、数值无关**——`cmp_swin.py` 的 rel 分母被 `max(1.0,·)` 顶到 1，报出的是绝对差 3–5e-2，而信号幅度只有 ~7e-3。**别被"幅度均值比值 0.98"误导**，那只是两者都是同类噪声分布。覆盖形态是"**只有该 ds**，同站其他 ds 与另一站全部正常"。

**C5/C6 的修复四处**（缺一不可）：
1. 补扫增加帧号链，用与主循环**同一去重键**把 missing 计入同一份缺口账（当时的 `gapshiftbytes`，现为 `gapspan`）；
2. 补扫把区段末帧号回传，主循环用它作 `pfr` 初值——**接缝**正是另外两个缺口所在；
3. **全 filler 的 buffer 不再把 `gapchecklastfr` 清成 -1**（长 filler 段跨整个 buffer，清链会让其后的缺口没有前驱可比）；
4. 补扫结束后**把链推进到区段末帧**，否则链一保持不断，每个后续补扫都从同一旧帧号重新比较。

**C5 验收**：`missing frames` = 143 与外部扫描一致、`filler frames` = 1162 不变、`firstfno` 与 ds_3（缺帧形式、同位置）的差异由 1365 个 subint 降到 8 个（残留全在 filler 段内部/边界，是两种记录形式的天然差异）、`step range` 上界 6661 → 1。

### 4.4 D 类：有效性

| 编号 | 病根 | 症状指纹 | 状态 |
|---|---|---|---|
| **D1** | "读到即有效"——缺口/filler 覆盖的块必须显式记入 `gapinvalid` 并逐块清掉 | 缺口位置的数据被当作有效数据参与积分 | 已修（机制见 B4） |

D 类是 A/B/C 三类共同的**落点**：检测与修正解决"读哪"，`gapinvalid` 解决"信不信"。两者坐标系必须一致——`fillValidFlags` 的块映射 `f*payloadbytes/blockbytes + lastcount` 与判界式 `(i-lastcount)*blockbytes < validbytes` 用的是同一个 `lastcount`（块↔帧换算系数 = `payloadbytes/blockbytes`，t25362 为 8000/512 = **15.625**）。

### 4.5 B5——缺口跨 subint 边界时修正超前（2026-09-18 已修）

**病根**：`gapshiftbytes` 记的是"**已检测到**的缺口总量"；而缺口在哪个 buffer 被**检测到**、与它的**时间位置**属于哪个 subint，是两回事。

**t25362 实测（BA ds_0）**：

- buffer 816（= .sp 的 subint 834）的读取窗口让**全部 4 个缺口**（11+10+29+22 = 72 帧）都在它内部曝光（`GAPCHECK buffer 816 frame 38/50/75/76`），`gapshift` 一次加到 578304 字节 = 72 帧；
- 但缺口 3、4（29+22 帧）的**时间位置**落在 subint 835，即**晚于** 835 的起点——于是 buffer 817 的读取位置被多减 51 帧，`firstfno 4026` 而 835 起点对应的帧号应为 4098（时间轴上早了 72 帧）；
- 后果是 `shiftFrameGaps` 的 dst 坐标系整体偏移：它在 subint 835 的 dst 28..38、51..60 标了无效（那是**缺口 1、2** 的位置，实际属于 834），而真正该无效的 dst 13..64（缺口 3、4）被标成有效。核对 .sp：该 subint 的无效块区间 `[437,610) ∪ [796,954)` 正是 dst 28..38 ∪ 51..60；
- buffer 818 起位置自动恢复（缺口累计已"追上"该 subint 起点），因此**错位只发生在跨界的那一个 subint**。

**症状**：只有含缺口的积分 weight 偏——积分 4 差 1.6e-3、积分 10 差 3.3e-3（f 侧无效量 2.58e-3 对基准 4.31e-3，差 1.73e-3 ≈ 缺口 3 的 29 帧），其余八个积分与基准一致在 7e-7 浮动内。**同一缺口的两端落在不同 subint 时，只有与检测到它的 buffer 同 subint 的那一半被正确标记。**

**为什么此前所有判据都发现不了**：`missing frames`、`filler frames`、去重计数**全部正常**。计数类判据只问"缺口有没有被数到"，问不到"数到之后算在了哪一段"。

**自检判据**：`READPOS` 的 `firstfno` 应等于该 subint 起点的时间轴帧号（= `batchrel × fps` 的秒内余数，相邻 subint 差 `subint 时长 × fps`，t25362 为 81.92 帧取整）；跨界处会偏早"尚未到达的缺口字节数"。**对照同站另一个 ds 的同一 subint 是最快的方法**（两者的 `firstfno` 应相同或差 0–1 帧）。

**修复（2026-09-18，两条，`applications/fxcorr-f/src/datareader.cpp`）**：

1. **读取位置按缺口的时间轴位置过滤**——每个缺口记入 `gapspan`（**缺口后第一帧的文件位置** + 丢失帧数，去重键不变）；`readSubint` 用 `gapshiftAt(locate 给的帧序号)` 取"时间轴位置**早于本 subint** 的缺口帧数"，跨界那部分不计入。这一条修的是**读哪**（seek 位置）。
   - 判据 `fxcorr/test/gaps/run_boundary.sh` 场景 A：缺口 [130,160) 跨 131.072 边界，修复前 subint 2 的 `firstfno` 读成 98（对照 128），修复后回到 128。
2. **`dst` 由帧号算出**——`shiftFrameGaps` 的起始槽 = `(buffer 第一个数据帧的帧号 − locate 给的时间轴帧号) mod fps`（locate 侧新存 `lastframens`），不为 0 时把 `[0, dst)` 记入 `gapinvalid`。这一条修的是**标哪**（valid flags），覆盖"缺口把 subint 起点盖住、读取位置只能落到缺口之后"的情形——此时 buffer 内帧号**连续**，`reorder` 不会被置起，不按偏移判断就根本不会调用 `shiftFrameGaps`。
   - 判据 `run_boundary.sh` 场景 B：缺口 [120,150) 盖住读取位置 128，修复后 subint 2 的前 86 块权重为 0（= 22 帧 × `blocks_per_send` ÷ 子带帧数）。

与 7.2 的关系：`gapshiftAt(t)` 把修正量写成以**时间**为自变量的函数，B5 因此不再是"要打补丁的地方"，而是这个定义的自然结果。

**上面 t25362 的数字已由重跑定案（2026-09-18）**：「多减 51 帧」与「`firstfno` 4026 → 4098（差 72）」并不矛盾——前者是**文件字节位置**之差（536601856 − 536192224 = 409632 字节 = 51 帧），后者是**时间轴帧号**之差，中间隔着缺口 1、2 的 21 帧（51 + 21 = 72）。重跑实测：`gapshift` 由 578304 字节（72 帧，全量）降到 168672 字节（21 帧），`firstfno` 回到 4098。

**注意**：`gapchecksubints` 从 1 起计，而 `fileoffset < 0` 早退的 subint 不调用 `checkFrameContinuity`（t25362 前 19 个），因此 **.sp 索引 = buffer 编号 + 19**（2026-09-18 实测订正：buffer 816 的 `gapinvalid` 块区间 `[593,765) ∪ [953,1109)` 正是 .sp 835 记录的 `valid_flags`，buffer 817 的 `[218,673) ∪ [687,1032)` 正是 .sp 836 的）。起点的子数据若恰在 batch 起点则无此偏移。

### 4.6 未决：filler 时间槽的判定与 mpifxcorr 不同（对拍未归零的原因）

B5 修好后 t25362 仍是 576 条差异。逐 ds 拆开看，积分 4/10 的差异**几乎全部来自 ds_2**（filler 那个 datastream）：

| ds | 积分 4 的无效帧 | 积分 10 的无效帧 |
|---|---|---|
| ds_0/1/3/4/5/6/7 | 73–75 | 75 |
| **ds_2** | **459** | **1135** |

fxcorr 把 filler 帧**占用的时间槽**判为无效（那里确实没有真实数据），基准不判、weight 因此偏高。这是 C 类 filler 的语义差异，不是 B5。副作用：2.3 表里那条「filler 顺读时与普通帧一同流过（上游**无专门识别**；实际影响**未实测**）」——现在实测了，影响就是 weight。

未决的两条出路：

1. 认可「filler 时间槽无真实数据、应判无效」，改用 L1/L2 判据（`READPOS` 的 `firstfno` + `.sp` 的无效块落位）验收 t25362，不再要求 `cmp_swin.py` 全等；
2. 或查 `mpifxcorr` 的 `VDIFDataStream` 为什么不做帧号连续性判定——2.1 节说"缺口的处理在流里（`vdifmux`）"，但那是 **muxed VDIF** 的路径；单线程 VDIF 文件走的是哪条、有没有等价的补 invalid 动作，**尚未核实**。

---

## 5. 当前状态盘点

### 5.1 t25362 对拍现状

观测：BA/S6 两站、11.264 s、BA 有 8 个 ds、32 MHz band ×8、2bit、16000 fps、帧 8032 字节（payload 8000）、subint 5.12 ms、积分 = 200 subint = 1.024 s、共 11 个积分。

基准：`bench/T25362.difx/DIFX_61037_024611.s0000.b0000`（2816 条记录）；对拍 `cmp_swin.py <基准> <fxcorr> 128`。

| 阶段 | 结果 |
|---|---|
| A 类修复后 | 积分 0 的 w = 0.903898 对基准 0.903949（差 5e-5，边界取整残差；未修复时会差 0.4 量级） |
| C 类修复后 | 积分 4、10 仍偏 → 归因 B5 |
| **B5 未修时** | **576 条差异**，全部在 BA 站：积分 0、4、10（各 192 条），即 bl 257 自相关、bl 258 BA-S6；其余 8 个积分与基准一致在 7e-7 浮动内，S6 站完全正常 |
| **B5 修复后（2026-09-18，重跑实测）** | 合成判据全绿、无缺口路径逐字节不变（见 7.4）；真实数据上 `firstfno` 与无效块落位都正确。**但对拍仍是 576 条**——条数不变而根因换了：积分 0 的 192 条是 5e-5 的边界取整残差，积分 4/10 的差异几乎全部来自 **ds_2**（459 / 1135 帧无效，其余 ds 只有 73–75 帧），即 fxcorr 把 filler 时间槽判无效、基准不判（见 4.6） |

### 5.2 测试资产覆盖

| 场景 | 资产 | 状态 |
|---|---|---|
| 缺口（直接缺帧） | `fxcorr/test/gaps/` T2 形式（`FXSIM_GAPS` 无 `:f`） | 已覆盖，判据 = `missing frames` |
| filler 占位 | `fxcorr/test/gaps/` T1 形式（`:f`） | 已覆盖，判据 = filler 计数 + 帧号范围 |
| **缺口跨越 subint 边界** | `fxcorr/test/gaps/run_boundary.sh` | 已覆盖（两个场景：跨边界不盖起点、盖住起点，判据见 7.5） |
| 起点偏移（文件 ≠ batch 起点） | 无专门资产 | **未覆盖**（A1–A3 只在 t25362 上验证过） |
| delay ≠ 0 | `fxcorr/test/p11/` | 已覆盖 |
| 多格式 | `fxcorr/test/p10/` | 已覆盖 |

**验收的盲区**：现有 gaps 判据只覆盖"缺口**被计数**"，不覆盖"缺口的**时间位置被放对**"。B5 恰好落在两者之间。

---

## 6. 诊断方法

### 6.1 三个层次的判据

| 层 | 看什么 | 能发现 |
|---|---|---|
| 计数 | `GAPCHECK summary`（`missing frames` / `filler frames`） | C1–C4、C6 类（计数错） |
| 定位 | `READPOS` 的 `firstfno` 序列 | A、B5 类（读取位置错） |
| 无效块 | `.sp` 每 subint 权重数组的零块区间（布局见 `data-spec.md` 5.3） | 缺口的**落点**错（B5 第 2 条：读取位置对了但洞标在别处） |
| 产物 | `cmp_swin.py` 的差异覆盖面 | 全部，但**不指向具体病因**；有缺口的数据**不能**用它做判据（两边的 subint 窗口差一个 delay 修正，见 `fxcorr/test/gaps/README.md`） |

**核心提醒：summary 全对不等于定位对。** B5 的 `missing`/`filler`/去重计数全部正常，只有 `firstfno` 能看出来。

### 6.2 按差异覆盖面分流

| 覆盖面 | 指向 |
|---|---|
| 某站**全部**记录（含无缺口积分） | A 类（`anchorbytes`） |
| **某个 ds** 参与的全部组合 | 该 ds 的读取位置链（C2/C5） |
| 该 ds 的**全部 frq** | 该 ds 的定位链（filler/缺口） |
| **某几个积分**（先看这些积分是否含缺口） | 含缺口 → B5；不含 → 上游现象（vdifmux 滞后，见 `v1-plan.md` 2.3） |
| 无缺口数据也出现跨 buffer 的假缺口 | B2 |

### 6.3 症状指纹速查

- **weight 相同（都 1.0）但可见度符号相反、数值无关** → C5/C6（缺口漏检导致整段错位，去相关）
- **PCAL 奇数序号 tone 相位翻转** → A2（锚点整偏一帧）
- **`missing frames` 虚增数百倍** → C1（filler 帧号当缺口）
- **`missing frames` 虚增到几万 + 位置前移数百 MB** → C3（按距离推断 filler 而非实扫）
- **weight 全局塌陷** → B4（`gapinvalid` 未重建）
- **只有含缺口的积分 weight 偏** → B5

### 6.4 诊断输出

- `GAPCHECK summary`（**info** 级，默认可见）：`buffers / frames / discontinuities / missing frames / filler frames / boundaries`——缺口多的数据这一行就是体检结论。
- `GAPCHECK buffer ... frameno A -> B (step, missing)`（verbose）：每个缺口一条；补扫区段里发现的为 `GAPCHECK skipped ...`，格式相同。另有 `GAPCHECK boundary ...`（读取位置异常漂移，上限 40 条）。
- `GAPCHECK holes buf N: [a,b) [c,d)`（verbose）：每个被重建过的 buffer 一条，列出 `shiftFrameGaps` 留下的洞（**post-shift 帧槽区间**，转成块时 × `payloadbytes/blockbytes`）。这是 `.sp` 里 `valid_flags` 的直接来源，B5 修好后用它核对落位（t25362：buf 816 → `[38,49) [61,71)`，正是 .sp 835 的无效区）。
- `READPOS subint N: readoff O firstfno F lastfno L nframes N missing M filler K gapshift G fillershift H gapframes X dst D framens S`（verbose）：逐 subint 读取位置诊断，**对照两个 ds 即可判定定位是否正确**。`gapframes` 是本次实际生效的缺口帧数、`dst` 是 `shiftFrameGaps` 的起始槽、`framens` 是 locate 给的时间轴帧号——B5 就表现为「跨界的那个 subint `firstfno` 早于对照」。
- 级别由 `FXCORR_LOGLEVEL`（`error`/`warn`/`info`/`verbose`/`debug`，默认 `info`）控制。

### 6.5 外部交叉验证

`filler frames` 应与**外部帧号扫描**逐一一致（t25362 的 BA 实测 8 个 ds 为 0/0/1162/0/0/0/0/0）；`missing frames` 同理（应等于真值 143）。扫描时 `% FPS` 用错会把正常回绕当成巨量缺口（曾把缺 10 帧算成缺 7760 帧），fps 从 .input 的 `getFramePayloadBytes × 4 × getFramesPerSecond` 反推，**不要硬编码**。

---

## 7. 对后期改造的建议

### 7.1 前提：定位读不可回退

"batch 内也尽量顺读"不是一个可选项。定位读是 batch 架构的直接前提：

- **多节点各自持有本地 raw**（data-spec 第 2 节存储归属）：每个节点只持有自己那部分数据，无法"从头顺读到目标段"；
- **任意 batch 可独立处理**：切批与重跑约束（data-spec 12 节）要求单批可独立执行，顺序读的时间成本随 batch 位置线性增长；
- **按 subint 切块的驱动方式**已成既定接口（`readSubint(scan, offsetsec, offsetns, ...)` 签名即此语义）。

所以改造的方向不是换模型，而是**让定位读的修正量变得可验证**——把当前"改一次、跑一次全链对拍、看差异面积"的循环，换成"修正量本身可断言"。

### 7.2 结构：把 reader 拆成可单测的三层

**现状问题**：`checkFrameContinuity` / `shiftFrameGaps` / `countFillerRange` / `locate` / `readSubint` 通过成员状态（`gapshiftbytes` / `gapcountedthrough` / `fillershiftbytes` / `gapchecklastfr` / `gapinvalid` / `lastcount`）跨 subint 耦合，逻辑嵌在 I/O 循环里，**无法单独测试**"给定一段帧号序列，应得多少修正量"。这正是 B3/B5/C6 这类缺陷反复出现的原因——它们都是**状态机错误**，而状态机没有测试面。

**建议的分层**：

| 层 | 职责 | 可测性 |
|---|---|---|
| **帧时间轴层**（纯函数，无 I/O） | 输入：帧号序列 + 每帧的文件位置；输出：帧号 → **时间轴位置**的映射，以及缺口的（时间位置，长度）列表 | 可单元测试：喂合成帧号序列，断言映射与缺口列表 |
| **修正量层**（纯函数） | 由时间轴映射导出 `gapshift(t)` / `fillershift(t)`——**定义为"时间位置在 t 之前"的累计量**（这一定义直接消解 B5） | 可单元测试：断言任意 t 处的修正量 |
| **I/O 层** | 按修正量 seek/read、整帧对齐、`gapinvalid` → valid flags | 需要文件，但逻辑已退化为一组坐标变换 |

第 2 层的定义值得强调：`gapshiftbytes` 的**正确语义**是"时间位置早于当前读取点的缺口字节总量"，而不是"到目前为止检测到的缺口字节总量"。当前实现是后者的**近似**——在缺口不跨 subint 时两者相等，跨了就不等（B5）。把它写成以**时间**为自变量的函数，B5 不再是"要修的 bug"，而是定义的自然结果。

### 7.3 验收：分层递进，不要一步跳到对拍

现状是每次改完直接跑全链对拍，靠差异面积反推病因——一轮十几分钟且信息量低。建议按层验收：

| 层 | 判据 | 何时可用 |
|---|---|---|
| L1 帧时间轴 | `READPOS firstfno` 序列 == 期望时间轴帧号（相邻 subint 差恒定、跨缺口处跳变值正确） | 合成数据即可，无需 mpifxcorr |
| L2 有效区间 | `gapinvalid` 合并后的块区间 == 外部帧号扫描推算的区间 | 合成数据 + 脚本判据 |
| L3 产物 | `cmp_swin.py` 全等 | 与 mpifxcorr 对拍 |

**L1 是关键补位**：它不需要基准、不需要 mpifxcorr、可以在合成数据上跑，且恰好覆盖 B5 这类"计数正常但位置错"的缺陷。

### 7.4 B5 的实施记录（2026-09-18）

按"先合成、后改码"的顺序做的，每一步都留下了判据：

1. **先补合成资产与判据**（7.5 第 1 项，`fxcorr/test/gaps/run_boundary.sh`）——改码前先确认它对这个缺陷报红（场景 A `firstfno` 98 对 128、场景 B 零权重块 0 个）。
2. **改 `gapshiftAt` + `gapspan`**（读哪）——缺口按"时间轴位置早于本 subint"过滤，跨界那部分延后到下一个 subint 生效。
3. **改 `shiftFrameGaps` 的起始槽**（标哪）——由缓冲区第一个数据帧的帧号与 locate 的时间轴帧号之差决定；差值不为 0 时**即使帧号连续也要重建**（第 3 步的必要性是这样发现的：只改第 2 步时场景 B 的洞仍被当数据，因为 `reorder` 不置起、`shiftFrameGaps` 根本没被调用）。
4. 验证：`run_boundary.sh` 两个场景转绿；`fxcorr/test/gaps/` 的 T1/T2 计数判据不变（73/73 与 73/0）；cmp5 无缺口数据的 `band_00.sp` 与改前 md5 相同（**逐字节不变**）；边界用例 `fxcorr/test/gaps/README.md` 的验证记录。
5. **真实数据**（`ssh difx` 的 `/data/scalebox/t25362work`，2026-09-18 重跑 f/x 两侧）：BA ds_0 的 `firstfno` 4026 → **4098** ✓；无效块落位经外部帧号扫描核对 ✓；4.5 末尾那处待核的数字定案（两者坐标系不同，都对）✓。**对拍未归零**——576 条仍在，条数不变而根因换成了 ds_2 的 filler 语义差异，见 4.6（未决）。

改动落在 `datareader.cpp` 的 `checkFrameContinuity` / `gapshiftAt` / `shiftFrameGaps` / `locate` / `readSubint` 五处（`gapshiftbytes` 成员已删，缺口账改为 `gapspan`）。

**安全边界**：无缺口、无 filler 的数据路径**逐字节不变**（S6 的全部产物、以及所有合成数据对拍）。实现上以"`gapspan` 空则一切照旧"保证——`gapshiftAt` 不被调用、`shiftFrameGaps` 的起始槽检查不触发。每次改动后回归 `fxcorr/test/gaps/` 与 cmp5 目录。

### 7.5 待补的测试覆盖

按优先级：

1. ~~**缺口跨 subint 边界**（对应 B5）~~ —— **已补，2026-09-18**：`fxcorr/test/gaps/run_boundary.sh`。两个场景——缺口跨边界但不盖住读取位置（判据 `READPOS firstfno` 不得早于对照；summary 看不出来）、缺口盖住读取位置（判据 `.sp` 从第 0 块起权重为 0）。`FXSIM_GAPS` 的中断位置按帧号选：subint 131.072 帧，取 `0.52:30`／`0.48:30`。
2. **起点偏移**（对应 A1–A3）——造"文件起点晚于 batch 起点"的数据。现有 `FXSIM_GAPS` 无此能力，需要生成器侧支持（写起始帧号偏移）。A1–A3 目前只在 t25362 上验证过，改坏了无回归可依。
3. **缺口 + filler 同段并存**——t25362 的 ds_2 是现实样本，`FXSIM_GAPS` 的 `:f` 形式可以造，但现有资产里缺口与 filler 是**分别**测的，没有一组同时含"filler 段两端夹真实缺口"。

### 7.6 其他建议

- **诊断输出的稳定性**：`READPOS` 这类逐 subint 诊断是定位类缺陷的主要工具，建议在后续任何 reader 重构中作为**契约**保留（字段名与语义不变），否则排查手段会一并丢失。
- **真实数据回归集**：t25362 是目前唯一暴露过三类缺陷的数据集，建议把"跑 t25362 → `GAPCHECK summary` 与 `READPOS` 序列符合预期"固化为 reader 改动的常规回归项（数据在 `ssh difx`，`make sync` 不同步到该机，需 rsync 单文件）。

---

## 8. 材料索引

### 8.1 上游对照代码

| 文件 | 作用 |
|---|---|
| `mpifxcorr/src/datastream.cpp` | 基类 `DataStream::calculateControlParams`（:381-394 延迟/首 offsetns、:516-573 采样→字节偏移与延迟重对齐、:463-470 −nsinc 早退、:538-568 跳块/tosubtract、:757-761 `bytesbetweenintegerns` 累加、:600-604 valid flags 判界） |
| `mpifxcorr/src/vdiffile.cpp` | `VDIFDataStream`（:396-401 muxed 帧参数、:417-444 帧对齐、:945-964 switched power 喂入） |
| `libraries/vdifio/src/vdifmux.c` | 顺序读模型的缺口承担者（`:948` 缺帧输出标 invalid 位） |
| `libraries/fxcorrcommon/src/mk5mode.cpp` | `unpack`（`mark5_unpack_with_offset` + `blank_vdif_EDV4`） |
| `applications/fxcorr-f/src/datareader.{h,cpp}` | 本文件分析的对象；头文件注释含 P10/P11/P12 的分层说明 |

### 8.2 历史实施记录

- `fxcorr/v1-plan.md` 2.2：datareader 的 V1 边界与实施偏差（`Mode` 零改造、process 槽复用、`sendbytes` 直接用 `getDataBytes`）。
- `fxcorr/algo-plan.md` P10：格式补齐（六类读取路径的上游映射、四改造项、验收判据与实施记录）。
- `fxcorr/algo-plan.md` P11：reader 语义补全（与 P10 的"读什么/怎么读"分层关系、等价映射论证、delay≠0 对拍）。
- `fxcorr/algo-plan.md` **P12**（2026-09-18 立章）：真实观测的病态数据——动机分类、实施记录（含未修项 B5）、验证方法与验收判据。**分析部分**（读模型、缺陷根因、症状指纹、诊断判据）由本文承载，改造期间散在 data-spec 5.2 与 `applications/fxcorr-f/CLAUDE.md` 的内容已收敛至此。

### 8.3 测试资产

- `fxcorr/test/gaps/README.md`：`FXSIM_GAPS` 用法（缺口与 filler 两种形式在**同一位置**、**帧号范围相同**）、T1/T2 验收判据表、坑记录（filler 必须推进帧号、workdir 必须已存在、判据依赖 fps）。
- `fxcorr/test/gaps/run_boundary.sh`：缺口跨越 subint 边界的两个场景（跨边界不盖起点 / 盖住起点），判据分别是 `READPOS firstfno` 与 `.sp` 的零权重块区间；一次跑三段数据（无缺口 + 两个缺口位置）。
- `fxcorr/test/p10/`、`fxcorr/test/p11/`：格式覆盖与 delay 语义的检验资产。
- `fxcorr/test/cmp_swin.py`：SWIN 逐记录比较（第 4 参数可限制最大记录数）。

### 8.4 真实数据集（t25362）

首个用于验证读取路径的真实 VGOS 观测，也是"真实数据比仿真脏"的第一个样本——**起点偏移、缺口、filler 三者同时出现**，本文 A/B/C 三类的缺陷全部由它暴露。

观测结构与对拍现状见本文 5.1；缺陷的实测证据见 4.5。数据集与工作目录位于项目测试环境（**不在本仓库内**），运行该测试机需 `LD_LIBRARY_PATH=$DIFXROOT/lib`，且本仓库的 `make sync` 只同步到构建机、不同步到持有真实数据的机器。
