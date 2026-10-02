# fxcorr V8 计划

**最后更新**：2026-10-02（**首个定案：数据定位重构**（§2）+ **承接 V7 未完成项**（§3）。
V8 主题是**接入 scalebox 编排**（fxcorr 六工具封装为算法模块 + Go 主路由，在另一仓库
`app-fxcorr`）。编排落地过程中暴露一个既有设计问题：**raw 数据文件的定位**——FILE 行 +
软链的既有机制在"多 batch 并行 + 容器路径映射"下不成立（共享可变状态、绝对路径陷阱、
重指职责无主）。本日定案为**「real_path + 命名规则」两态定位，FILE 行/软链整条链退役**；
改动在 fxcorr-f（C++，三个文件）与模块 run.sh（app-fxcorr 仓库），主路由零改动。§3 把
`v7-plan.md` 的未完成项逐条落位——**主承接是判据 9b（P5b：跨节点流量实测，Q5 正题，
本就"等 scalebox 编排"）**。）

## 1. V8 的位置

**V7 交了计算侧的全部**：目标集群（Slurm）上的多核与跨节点实测，Q1/Q2/Q6 三个标定
数字（`v7-plan.md`）。

**V7 欠的一笔在编排**：§13 判据 9b（fxcorr 侧跨节点流量 ≈0）等 scalebox 编排；
`v6-plan.md` 移交的「多节点部署形态」同样落在编排层。

**V8 的主题是接入 scalebox 编排**：prep/sim/f/x/x-merge/fits 封装为算法模块，
主路由（Go，`app-fxcorr/router/`）承担任务生成、依赖触发（三组计数器）、状态记账、
流控协同（wait-queue gate + vtask 额度）。

（V8 的其余范围待定，本文随定案增补。）

## 2. 数据定位重构（2026-10-02 定案）

### 2.1 问题：FILE 行 + 软链在多 batch 编排下不成立

**既有机制**：raw 数据文件由 `.input` 的 DATA TABLE 指名——`FILE i/j:` 行是"数据文件名"，
按 **RAW 根**（`FXCORR_RAW_ROOT`，默认 `workdir/raw`）解析：相对名拼根、绝对名原样
（`configuration.cpp:1890-1901`，`FxcorrPath::under`）。

- **真实观测**：FILE 行直接写真实路径（`ma008_1.input`: `/data/ma008/...`）——**不需要映射**；
- **测试/仿真**：FILE 行是 vex 里的名字（`TEST1.vdif`），与 sim 的产物名
  （`raw/<站>/<站>_<batch>[_ds<N>].vdif`）不一致——既有做法是**软链**：
  `raw/TEST1.vdif -> T1/T1_00000002_ds0.vdif`（`fxinput.py` 的 `relink_data_table` 在
  `run_batch.sh` 里重指；test1 实例为相对目标，route1 中 make_testdata 建的是绝对目标）。

**编排下暴露三个问题**：

1. **软链是共享可变状态**：多 batch 并行（vtask 并发 ≥2）时，同一个 FILE 行名
   （`TEST1.vdif`）要指向不同 batch 的文件 → 冲突，且后果是**静默错数据**
   （f 读到别 batch 的数据，全程不报错）；
2. **绝对路径的容器陷阱**：make_testdata 建的软链目标是**宿主绝对路径**
   （`/tmp/scalebox/mydata/...`），容器内（`/cluster_data_root` 映射）解析不了
   ——f 的约定是**相对目标**（f README 的实测实例即相对目标）；
3. **重指职责无主**：`modules/fxcorr-f/README.md` 与 `modules/fxcorr-sim/README.md`
   都把「DATA TABLE 软链的编排职责」列为待办（"多 batch 并发时软链是共享资源"）。

**根因**：软链是"**外部名字空间**（FILE 行，来自观测配置）→ **fxcorr 数据布局**
（`raw/<站>/<站>_<batch>_dsN.vdif`）"的运行期映射物。当"一份 `.input` 服务多 batch"时，
该映射必须"每 batch 一个私有名字空间"才并发安全——共享软链做不到。

### 2.2 定案：把"数据在哪"作为任务参数显式传递

| 项 | 定案 |
|---|---|
| **body** | `<batch>-<station>-<ds>`（三个 id 的组合，**自然键不变**——去重/断点/日志依赖它环境无关）|
| **headers** | 可选 **`real_path`** = 数据文件路径（**逗号分隔列表**，容多段）；相对路径按 RAW 根解析、绝对路径原样（与 FILE 行同规则，复用 `FxcorrPath::under`）|
| **优先级** | ① `real_path` → ② 命名规则推导 |
| **命名规则** | `<RAW_ROOT>/<station>/<station>_<batch>[_ds<N>].vdif`（N = 站内 ds 序号；仅多 ds 站带后缀——与 `fxinput.py`/`fxcorr-sim` 同规则）|
| **FILE 行 / 软链** | **退役**——f 不再读 FILE 行（`.input` 仍加载，供频率/时间等元数据）|

**两个场景**：

- **调试（共享存储直读，主要调试用）**：任务 headers 带 `real_path`（相对或绝对），f 直读
  ——headers 里一眼可见"这个 f 读哪个文件"；
- **正常运行（拷贝到本地）**：数据落到规范路径（拷贝者负责，如后置的 raw-copy），headers
  不带 `real_path`，f 按命名规则推导——"数据放哪、f 读哪"由同一个 `FXCORR_RAW_ROOT` 自洽
  （与「节点本地化：raw/fengine 落 tmpfs」的演进是同一机制）。
  **拷贝时可顺手把目录调整为规范路径**（拷贝本来就在写目标路径，零成本）；
  若沿用源布局，则与调试态同法：带 `real_path`。

### 2.3 改动清单

**A. fxcorr-f（C++，3 个文件）**

- `src/datareader.h`：构造加可选参数 `const std::vector<std::string> *dataFilesOverride
  = nullptr`；新增成员 `std::vector<std::string> dataFileList`（覆盖时持有，
  保证 `datafilenames` 生命周期）；
- `src/datareader.cpp`（构造内，现 `datareader.cpp:46-51`）：覆盖列表非空时用它；
  否则走 `getDNumFiles`/`getDDataFileNames`（现状）。数据文件入口是单点
  （`datareader.cpp:51`），改动局部；
- `src/main.cpp`：新增 `resolveDataFiles(rawroot, station, batchid, dsarg)`——
  读 `FXCORR_REAL_PATH`（逗号分隔，逐项 `FxcorrPath::under` 解析）；无则命名规则
  两候选（`_ds<N>` 后缀 → 无后缀）以 `stat()` 探测；都缺 → 报错并提示 `real_path`
  （`DataReader` 构造在 `main.cpp:387` 处传入）。

**B. 模块 run.sh（app-fxcorr 仓库，`modules/fxcorr-f/code/run.sh`，+4 行）**

- headers 正则取 `real_path` → `export FXCORR_REAL_PATH`（空串视为未设置；
  与 x-merge 的 headers 解析同手法）。

**C. 主路由（app-fxcorr 仓库）：零改动**

- 不做软链；`from_head` 的 sim 跳过判据（raw 探测）语义不变；
- 调试需要时手工投任务带 `real_path` 即可。

**D. 文档（2026-10-02 已改）**

- `data-spec.md`（头部 + §5.2 数据定位条 + §5.2.1 根接管与两类路径表）、
  `usage.md`（头部 + 通用约定 + `FXCORR_REAL_PATH` + fxcorr-f 输入输出 + 端到端示例）、
  `workdir-template/raw/README.md`（"数据定位"节重写）、
  `applications/fxcorr-f/CLAUDE.md`（头部 + 调用方式条）；
- app-fxcorr 仓库：`modules/fxcorr-f/README.md`（输入契约、待办划掉软链职责）、
  `modules/README.md`（headers 契约条 + §3 路径规则）、`router/README.md`
  （§6 sim 跳过判据注 + §14 #24）。

**随代码改动同步**（本轮未动——它们描述的是**当前代码**的行为，改代码时一起改）：

- `fxcorr/README.md:166`（`run_batch.sh` 行："DATA TABLE 软链重指本 batch"步骤）；
- `fxcorr/fxinput.py` 的 `relink_data_table`（退役）；`fxcorr/run_batch.sh` 的调用点；
- `fxcorr/make_testdata.sh` 的软链（清理与否见 §2.5-4）。

### 2.4 判据

1. **无 `real_path`**：test1/route1 现有数据直接命中
   （`raw/<站>/<站>_<batch>[_ds<N>].vdif`）——**软链删掉仍跑通**（软链不再参与）；
2. **有 `real_path`**：相对路径（按 RAW 根）与绝对路径各一例；逗号多文件一例；
3. 手工调试流程不变：`fxcorr-f <batch> <station> <workdir> <ds>` 无 env 直接可用；
4. 现有模块测试（app-fxcorr 的 `modules/*/test.yaml`）全链回归。

### 2.5 待验证 / 风险

1. **difx2fits 对 FILE 行的依赖**：初查 `applications/difx2fits/` 未引用 `datafilenames`，
   大概率只解析不打开；联调时实测（`wrap_difx2fits.sh` 本有"复制 .input + 改写"的手法
   兜底）。若确有依赖，.input 生成时把 FILE 行写成规范路径即可（不改本设计）；
2. **命名规则同改清单变四处**：`fxinput.py`（权威）/ `fxcorr-sim` / app-fxcorr 主路由的
   `fxin.StationOutPath` / **`fxcorr-f`（本次新增）**——四处注释互引；
3. **多文件 datastream**：命名规则只覆盖"一 ds 一文件"的仿真布局；多段真实数据必须走
   `real_path` 列表（文档写明）；
4. **软链清理**：`make_testdata.sh` 仍建软链（指向最后 batch）——本设计下 f 不再使用，
   是否清理待 difx2fits 验证后定（无害则留）。

## 3. 承接：V7 未完成项（2026-10-02 整理）

V7 §13 的 11 条判据过了 10 条（`v7-plan.md`）；未过与遗留项逐条落位如下（编号沿用 v7）：

| 项 | v7 状态 | V8 落位 |
|---|---|---|
| **9b：P5b——fxcorr 侧跨节点流量 ≈0 实测（Q5 正题）** | 等 scalebox 编排（§16.1） | **V8 主承接**——V8 主题即编排接入。按 v6 部署形态落地（计算单元 = `(batch, ds 组)`、`fengine` 落各节点 tmpfs、`vis-parts/`+`vis/` 落共享存储、实验级 `merge --experiment` 单点写 SWIN），测跨节点流量——**预期 ≈0；若不为 0，说明有设计外的依赖，这是最有价值的证伪点**（§16.3） |
| 多节点部署联调（v6 移交最重的一条） | P5b | 与 9b 一体 |
| **长稳**（多节点持续跑） | 属 P5b | 与 9b 一体 |
| **S5.4 `PCAL_*` 并发写**（同站两 ds 同组并行，PCAL `ios::trunc` 整文件重写、后写者丢行；P3 已改按 ds 独立文件，**合并留 P5b**，`v7-plan.md:467`） | P5b（多节点时暴露） | 随 9b 联调时合并 |
| 判据 8 遗留：`/work2` 读速率的**随作业采集**机制（§12 表末行） | 未做 | 跨节点阶段补（不影响本仓库联调） |
| S5.3 真实数据四项（FILL_PATTERN / invalid 位 / filler 修正量 / 真实 `.calc`） | 待**第二份真实观测**，挂起 | 维持挂起（无新真实数据前不动） |
| S5.5 `executeseconds` 时间基准不一致（`getScanStartSec() != 0` 时静默不写盘） | V1 挡着（单 scan 边界） | 维持——**若 V8 放开多 scan，必须先修** |
| S5.1 挂起项、"已知但有意未做"三条 | 维持 | 维持（处置见 `v6-plan.md` 末节） |

**V7 明确的范围外（长期目标，`v7-plan.md` §15）**：

- **百节点级 MPI vs scalebox 对照**：需 P1–P4 结论 + scalebox 调度到位——V8 编排落地后
  **前提具备**，是否纳入 V8 待定；P5a 已交付三条硬约束（两侧固定核数才可比、
  对拍用「集合 + 容差」、超订红线，§16.4）；
- **DCU/GPU 适配**：§19 只记录不实施；两条选型约束（f 的实测 TFLOPS、x 侧并行化后的
  真实瓶颈位置）仍待；
- **数百节点调度规模**：scalebox 的范畴；
- **cf16**：触发条件（"分片后 tmpfs 容量成为单节点并行度上限"）未成立。

## 4. 范围外

- **raw-copy（真实数据源搬运）**：本设计已给它留位置（落规范路径 或 传 `real_path`），
  落地另行设计；
- **节点本地化（raw/fengine 落 tmpfs）**：`FXCORR_RAW_ROOT` 重定向即可，与本设计自洽
  （P5b 的部署形态本就要求 fengine 落 tmpfs）；
- V8 的其余范围待定。
