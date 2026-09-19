# fxcorr V4 计划

V4 范围经 2026-09-19 讨论定案：把读取路径从"改一次、跑一次全链对拍、看差异面积"的循环，换成"**修正量本身可断言**"。与此无关的工作不进本版本。

起点是 V3 的 P12 遗留项：定位读在 filler 上的**窗口长度缺口**——文件里有、却从未进入任何读取窗口的 80 帧（`reader-model.md` 4.7，`fxcorr/test/reader/` 的 E4 断言）。**分析与判据见 `reader-model.md`，本文件只承载路线与验收。**

- 阶段 A：把未被资产覆盖的形态补上（判据先行）
- 阶段 B：修 E4（窗口语义）
- 阶段 C：分层重构（`reader-model.md` 7.2 的三层）
- 阶段 D：判据固化

## 定案决策（2026-09-19）

1. **顺序不可换**，三条：资产先于改码（沿用 B5 的顺序，`reader-model.md` 7.4）；语义定案先于实现（4.7 末段"重读该 subint"已被实测否决一次，修法空间不能靠试）；修 E4 先于分层重构——E4 暴露的是"读取窗口应该多长"这个量**尚未定义**，量没定义就分层，等于把未定义的行为搬进新结构。
2. **每个节点一个 commit**，但 commit 粒度由"**能独立通过全部回归**"定义，不为粒度而切——阶段 C 的单次提交会显著大于 A/B（见下）。
3. **不新增真实观测数据**（理由与触发条件见末节）。

## 阶段 A：把未被资产覆盖的形态补上

依据 `reader-model.md` 5.2 与 7.5：A 类（起点偏移）**无合成资产**、改坏了无回归可依；"缺口 + filler 同段并存"只在真机上见过；而 E4 这个形态，现有四个合成场景（无中断 / 缺口 / filler / filler 紧随缺口）**全部 E4 = 0**。

| 节点 | 做什么 | 验收 |
|---|---|---|
| ✅ A1 | 复现 E4 形态（2026-09-19，**无需改生成器**）：现有 `FXSIM_GAPS` 语法即可，差别只在中断位置——`1.0:40:f400` 落在 subint 窗口后部、跳过的区段全是 filler（E4 = 0），前移到 `0.6:40:f400` 落在窗口前部，段后数据落在窗口之外。净损失 = `span − (A − t_k) − G`（A = filler 段时间轴帧号、t_k = 所在 subint 窗口起点、G = 缺口帧数）。资产：`fxcorr/test/gaps/run_window.sh` | `check_reader.py` 报 **E4 = 69 帧** + E3 extra 71 槽（红）✓；无中断对照 E4 = 0 ✓；现有四场景与 `1.0:40:f400` 在新判据下 E4 全为 0（不误报）✓ |
| ✅ A2 | 起点偏移资产（2026-09-19）：生成器加 `FXSIM_STARTOFFSET=<帧数>`（跳过 batch 开头这么多帧——它们既不在文件里、也不占字节，文件首帧的时间戳因此是 batch 起点 + N 帧，`anchorbytes` 为负）；**同时补了 E5 绝对时间锚**——E1–E4 都抓不到 A 类（E1 只看相邻差，E2/E3 是自洽性判据，`anchorbytes` 整体偏时两边一起偏），E5 用 `batch.json` 的 `start_mjd` 作独立参照。资产：`fxcorr/test/gaps/run_startoffset.sh` | 对照与偏移 18 帧：E5 偏差 3 帧（容差 8）、退出码 0 ✓；自检（各 subint 的 readoff 减掉 `anchorbytes`，模拟 A1 漏算）报 **21 帧**、退出码 1 ✓；E5 跳过时脚本明确报错（**跳过也是退出码 0**，不拦就形同虚设）✓；`FXSIM_STARTOFFSET` 不设 = 行为不变（生成器两处调用点）✓ |
| ✅ A3 | 把 `run_filler.sh` / `run_boundary.sh` 的判据接到 `check_reader.py`（2026-09-19）：两脚本的每个场景在旧判据之外另判 `check_reader` 退出码（E1–E3 无 finding）与 **E4 = 0**，并各带自检——把旧判据抓的缺陷形态造进日志（filler 形式读位置整体前移 400 帧 / 跨界 subint 的 `firstfno` 减 30 / 清空该 subint 的 holes），绝对判据必须同样报红 | 六个场景全绿（两脚本各三个；`run_filler` 的对照 E5 3 帧、含中断场景 E5 按设计跳过、E4 全 0，`run_boundary` 同）、三条自检全红 ✓；旧判据不删，仍作交叉核对 ✓ |

A1 若现有 `FXSIM_GAPS` 语法造不出目标形态，则先给生成器加参数（**生成器 1 commit + 场景 1 commit**），节点数由探路结果决定。

## 阶段 B：修 E4（窗口语义）

| 节点 | 做什么 | 验收 |
|---|---|---|
| ✅ B1 | **语义定案**（2026-09-19，不动代码）：读上游读取路径——定位（`vdiffile.cpp:348-473`）、`dataRead`（`:668-859`）、读线程（`:293-335`）、`vdifmux` 主循环（`libraries/vdifio/src/vdifmux.c:496-1033`）。结论：上游的窗口长度定义**在输出侧**——输出槽数固定（`destSize/outputFrameSize`）、槽号就是时间轴帧号，输入侧按需消费到填满（filler 跳字节不占槽、空槽输出 invalid 占位帧），`startOutputFrameNumber` 逐次连续推进使整个模型**闭环**；本实现反过来，长度固定在**输入侧**（`sendbytes` 字节），换算隐含"每帧都提供净荷"的假设 | `reader-model.md` 4.8 给出**唯一解**：窗口按"填满 N 个时间槽"定义、读入字节数可变，重算 A1 的 69 帧（继续读约 72 帧即与下一个窗口衔接）、并说明 `1.0:40:f400` 与无中断路径为何逐字节不变 ✓；另附 filler 形态清单（上游认五种，fxcorr 只覆盖两种）|
| ✅ B2 | 按 B1 的结论实现（2026-09-19）：`shiftFrameGaps` 的源帧数与目标槽数拆开并加 `dryrun`（填槽与"够不够"共用一份算法）、`readSubint` 第一趟照旧直读进输出缓冲、填不满才改用可增长的 `inbuf` 翻倍多读、扫描范围截断到"填满所需"；`READPOS` 增 `slots` 字段，`check_reader.py` 的 E3 与 E4 的 batch 上界随之改用槽数（E4 的窗口位置仍用 `nframes`）；SwitchedPower 喂入加 `lastReadContiguous()` 保护 | A1 转绿（**E4 69 → 0**）✓；`run_filler.sh` 判据不退（169 == 169）、`run_boundary.sh` / `run_startoffset.sh` 全绿、三条自检仍报红 ✓；无缺口、无 filler 的 64-subint 数据 **md5 逐字节不变**（HEAD 版 A/B 实测）✓。实施与实测见 `reader-model.md` 4.9 |
| ✅ B3 | 真机复跑 t25362（`ssh difx`，2026-09-19）：BA 站 ds_2（有 filler）与 ds_0（对照）各重跑一遍（verbose），`test/reader/check_reader.py` 对账 | ds_2 的 E4 由 **80 → 0** ✓、E3 extra 63 → 0 ✓、2181 subints 零 finding ✓；ds_0 对照仍零 finding ✓；`GAPCHECK summary` 的 `missing 143` / `filler 1162`（ds_2）与修复前逐项相同 ✓；**多读路径确实被走到**——ds_2 有 4 个 subint 的 `nframes > slots`（最长 540 帧 vs 83 槽，全在 filler 段上），ds_0 一次都没有 ✓ |

## 阶段 C：分层重构

目标见 `reader-model.md` 7.2：把 `checkFrameContinuity` / `shiftFrameGaps` / `countFillerRange` / `locate` / `readSubint` 之间的成员状态耦合，拆成三层——**帧时间轴层**（纯函数：帧号序列 + 文件位置 → 时间轴映射与缺口表）、**修正量层**（`gapshift(t)` / `fillershift(t)`，定义为"时间位置在 t 之前"的累计量）、**I/O 层**（退化为坐标变换）。B3/B5/C6 这类缺陷都是状态机错误，而状态机此前没有测试面。

三层的接口由 B2 的实现形态定下来（`reader-model.md` 4.9），三次提交对应三层（`refactor(fxcorr-f): ...`），每步的验收 = **阶段 A 全部资产绿 + 无缺口数据逐字节不变**。

| 节点 | 做什么 | 验收 |
|---|---|---|
| ✅ C1 | **帧时间轴层**（2026-09-19）：新增 `applications/fxcorr-f/src/frametimeline.h`——`vdifFrameNumber` / `vdifIsFiller` 移入，`walkFrameChain`（帧号序列 → 首末帧号、数据帧数、filler 位置、缺口列表）与 `placeFrames`（槽映射：filler 丢弃、缺口占槽、没人认领的槽报为洞）成为**无成员、无 I/O、无日志**的纯函数（`dst` 传 NULL 即 dry run，填槽与"够不够"共用一次走查，B2 的约束在此固化）；`shiftFrameGaps` 退化为薄包装（scratch 缓冲 + 诊断 + `gapinvalid`），`checkFrameContinuity` 的主循环与 `countFillerRange` 的块循环改用 `walkFrameChain`——**同一套"帧号链 + 跳 filler + 记缺口"逻辑此前写了三遍**。新增单测 `fxcorr/test/reader/test_timeline.cpp`（`-Iapplications/fxcorr-f/src` 直接编译，零依赖） | 单测 **54 项断言全过**（本地 clang 与测试机 gcc 各一次）✓；`gaps/` 四个脚本全绿（E4 = 0、`169 == 169`、边界两场景、E5 3 帧）且三条自检仍报红 ✓；无缺口数据（`cmp5` 的 58948_25200，含 delay）`band_00.sp` / `autocorr.bin` / `pcal.bin` 与 HEAD(B2) 二进制跑出的**md5 全同** ✓；**真机 t25362 ds_2**（`ssh difx`）：C1 版日志与 B2 版 **diff 0 行**（2218 行，含全部 `READPOS` / `GAPCHECK` 明细）、`GAPCHECK summary` 逐项相同、`band_00.sp` / `autocorr.bin` / `pcal.bin` md5 全同 ✓ |
| ✅ C2 | **修正量层**（2026-09-19）：新增 `applications/fxcorr-f/src/corrections.h`——`Ledger` 持有缺口表（**槽坐标**：换算在 `noteGap` 记录时做一次，`gapshiftAt` 里 `(offset − anchorbytes)/framebytes + lost − fillerbefore` 那行反算消失）、filler 台账与两个去重水位，`gapShift(t)`（时间位置在 t 之前丢了多少，并把 t 推出它落在的洞）与 `fillerFrames()` 是这一层的全部查询面；`checkFrameContinuity` 与 `countFillerRange` 的扫描循环改成**按帧序归并** filler 与缺口后依次 `noteFiller` / `noteGap`（缺口"之前有多少 filler"由 ledger 自己回答，层 1 的 `FrameGap.fillersbefore` 随之删掉），三处循环共用同一套记账。接链状态（`gapchecklastfr` / `gapstretchseam`）**留在 DataReader**——它是扫描进度不是修正量，归 C3 的 I/O 层（与原规划的这一处差别记录在此）。新增单测 `fxcorr/test/reader/test_corrections.cpp` | 单测 **48 项断言全过**（含 B5 缺口跨边界、B6 filler 之后的缺口、A 类起点偏移两个符号、去重、两缺口连锁、两层级联）✓；`gaps/` 四脚本全绿（E4 = 0、`169 == 169`、边界两场景、E5 3 帧）且三条自检仍报红 ✓；无缺口数据（`cmp5`）三个产物 **md5 与 C1/HEAD 全同** ✓；**真机 t25362 ds_2**：日志与 B2 版 **diff 0 行**、`GAPCHECK summary` 逐项相同、三个产物 md5 全同 ✓ |
| ✅ C3 | **I/O 层**（2026-09-19）：`scanSkippedStretch` + `countFillerRange` 合成 **`scanStretch`**（读一段文件并记账的单一入口——水位在哪、逐块读、层 1 走链、层 2 记账全在一处）；`settleReadPosition` 收敛循环里的位置计算抽成 **`correctedPosition`**（纯算术，只碰 ledger），循环本身只剩"纯函数 + 一次 `scanStretch`"；`readSubint` 的"读够为止"（B2 的 inbuf 翻倍）抽成 **`readWindow`**，`readSubint` 退化为编排：locate（坐标）→ settle（修正量）→ readWindow（读+扫描+重建）→ 交回 | 单测 53 + 48 项全过（本地 clang 与测试机 gcc 各一次）✓；`gaps/` 四脚本全绿且三条自检仍报红 ✓；无缺口数据三个产物 md5 与 C1/C2 全同 ✓；**真机 t25362 ds_2**：日志与 B2 版 **diff 0 行**（2218 行）、`GAPCHECK summary` 逐项相同、产物 md5 全同 ✓ |

## 阶段 D：判据固化

- **真实数据回归项**：把"跑 t25362 → `GAPCHECK summary` 与 `READPOS` 序列符合预期"固化为 reader 改动的常规项（数据在 `ssh difx`，`make sync` 不同步到该机，需单独 rsync）。
- **诊断契约**：`READPOS` / `GAPCHECK holes` 的字段名与语义冻结（`reader-model.md` 7.6）——它们是定位类缺陷的主要工具，重构中丢了就没了。
- 文档收尾：`reader-model.md` 的缺陷表状态、`applications/fxcorr-f/CLAUDE.md` 的操作边界。

## 规划条件评估（2026-09-19）

**具备 commit 级规划条件的是阶段 A、B**：工具齐（`fxcorr/test/reader/` 已在真机上验证过一轮）、判据是绝对的（不依赖基准）、真机可复跑。

**阶段 C 的 commit 级规划**曾列三个条件，现已全部具备：

1. ✅ B1 的语义定案（2026-09-19 完成）——上游的窗口长度定义在输出侧（输出槽数固定、槽号即
   时间轴帧号、输入按需消费到填满），唯一解是"按时间槽填满"（`reader-model.md` 4.8）;
2. ✅ A2 的起点偏移资产（2026-09-19 完成）;
3. ✅ A3 的判据入口收敛（2026-09-19 完成：`gaps/` 四个脚本的验收判据全部走 `check_reader.py`）。

**阶段 A 与阶段 B 就此收尾**（2026-09-19）：A1–A3 全绿，四个 gaps 资产都是绝对判据
（`run_window` / `run_startoffset` 从一开始就是，`run_filler` / `run_boundary` 在 A3 补上，
旧相对判据保留作交叉核对）；B1 的结论见 `reader-model.md` 4.8、B2 的实施与实测见 4.9、
B3 的真机复跑见 4.9 的实测表末行。**阶段 C 完成**（2026-09-19，C1–C3 全绿）——三层的边界是 B2 划出来的
（帧时间轴层 = `shiftFrameGaps` 的槽算法、修正量层 = `gapshiftAt` / `fillershiftbytes`、
I/O 层 = `readSubint` 的读够为止），重构按此切，最后落成三个纯函数头/结构
（`frametimeline.h` / `corrections.h` / `readWindow` + `scanStretch` + `correctedPosition`）。

每步的等价性证据都是三层，一层比一层强：无缺口二元对比（md5）、合成缺口/filler 四场景
（绝对判据）、真机 t25362（日志逐行 + 产物 md5）。**三步的真机 diff 都是 0 行**——说明整个
重构只是把算法搬了家、把内部表示换成了时间槽，没有动行为：C2 的"记录时算成槽"与原来的
"读取时反算"在 2200 个 subint 上给出逐字节相同的日志与产物，这正是它要证明的。全程
`READPOS` / `GAPCHECK` 的每个字段都与 B2 版一致（`reader-model.md` 7.6 的契约）。

新增的测试面是这一步的另一个产出：`fxcorr/test/reader/test_{timeline,corrections}.cpp`
（53 + 48 项断言），把 B2/B5/B6/A 类四种形态从"要靠合成数据跑一遍才能验"变成"本地一条
命令、秒级"。阶段 D 要固化的正是这两件东西——判据与诊断契约。

**已知风险**：E4 的修法空间被探过一次并否决（4.7 末段），所以 B2 之前不允许动代码；只有一份真实数据（t25362），且它在 E4 形态下的基准不可用（mpifxcorr 的 `vdifmux` 丢字节 + 纯字节数判据，4.6），阶段 C 的真实侧验收只有这一条线。

## 真实数据获取策略

**结论：现阶段不新增真实观测数据。**

1. 眼下的已知未修项（E4）用不上它——t25362 的基准在有 filler 的 ds 上自身就是坏的，E4 修完只能靠绝对判据验收，而那条判据靠合成数据就能造、可复现、秒级出结果。
2. 真实数据的价值是**发现未知形态**，而现在已知的还有两层没做完（E4 未修、A 类无资产）；再加一份只会拉长未处理清单。
3. 成本结构不对：一份新数据要配基准（mpifxcorr 全链跑一遍）+ 人工核对，而它能回答的问题多半能用 `FXSIM_GAPS` 造出判据更强的版本。

**触发条件**：阶段 B、C 完成、合成资产全绿之后，若要验证修法对**其他 filler 形态**的泛化，再取。届时带**形态清单**去要，而不是泛泛地要"更多数据"——当前只有"短数据段夹在 filler 之间"一种形态（t25362 的 BA ds_2，8 段、81–508 帧），真实观测里 filler 还有几种形态我们并不知道。

**形态清单的来源**：阶段 B1 读上游语义时，顺手从 `vdifmux.c` 与记录系统文档里列出"filler 可能长什么样"，那份清单是后面挑数据的依据。候选形态举例：填充段跨整个 subint、filler 落在 batch 起点之前、filler 与缺口的交叠方式不同、非 BA 记录系统产生的 filler。

**成本最低的路径不是新观测**，而是问观测方要 t25362 剩下的部分：BA 有 8 个 ds 而只有 ds_2 有 filler（其余 7 个是现成对照）、同一 session 若有其他 scan、或上海 S6 / 天马 T6 站自己的观测。测试机现有数据集见 `reader-model.md` 8.4（`/mnt/VGOS/DiFX_test_data/` 下目前只有 t25362 一份）。
