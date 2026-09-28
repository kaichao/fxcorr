# fxcorr 工具命令行手册

**最后更新**：2026-09-28（**V6 的两级 `merge`（形态 A）已实施并验证**，其余 V6 项仍标 ⚠：`fxcorr-x merge <batch>` 现在只写 `vis-parts/<batch_id>/merged.part`、**新增的** `merge --experiment` 才写 SWIN 且是唯一写入者；四条定则（实验的认定与 config 来源、缺 batch 的判据取 `batches/*.json` 而非 `meta/batches.index`、按首记录时间定序 + 逐 batch 流式写出、根一致性检查）见 `data-spec` 5.9 末条，判据见 `fxcorr/test/multids/README.md` 判据 13–15。2026-09-27 补：两级 `merge`（形态 A）命令行形态、环境变量作用域说明与"其它实验级文件的并发语义（已核实，见 `v6-plan.md` S4.1 末表）"小节。路线见 `v6-plan.md`。**分片架构已落地**：`cmp_swin.py` 的 `--by-key` 按键匹配、`run_batch.sh` 的 `FXCORR_X_SHARD=1`。**分片架构的前三步全部落地**：`fxcorr-sim station` 增 `ds_index` 参数与多 datastream 生成、`fxcorr-x` 增 `ds_group` 分片模式与 `merge` 子命令——见各自小节，判据见 `fxcorr/test/multids/README.md`）

覆盖三个改造应用的命令行接口：`fxcorr-sim`（仿真数据生成器）、`fxcorr-f`（Station-based）、`fxcorr-x`（Baseline-based，含 `merge` 子命令）。目录布局、batch.json 格式与 D16 `vis-parts/` 规范见 `data-spec.md`，容量与分片方案的论证见 `data-volume.md` §7。

## 通用约定

- 三工具均**无 MPI、串行**，通过目录接口衔接。
- **目录根定位（V5 P5）**：四个可重定向的根 `FXCORR_{RAW,FENGINE,VIS,PRODUCT}_ROOT`，
  三档回退——该根已设置就用它，否则用 `<workdir>/<规范目录名>`；`workdir` 按
  "位置参数 > `FXCORR_WORKDIR` > 默认 `.`"解析并绝对化。`config/` `batches/` `meta/`
  `beam/` **没有独立根**，恒在 workdir 下。**规则权威见 `data-spec.md` 5.2.1**（各类相对
  路径的基准、只对相对路径拼根、`OUTPUT FILENAME` 的写法、根记录与一致性检查）。
  `FXCORR_PRINT_ROOTS=1` 可让工具打印各自解析出的根与来源。
- 只对**相对**路径拼根，**绝对路径一律原样**——真实观测的 `FILE` 行就是绝对路径，不受根变量影响。
- **`batch_id`（2026-09-27 修订）**：**8 位零填充顺序号**（`00000001`）——定宽、单调递增、`workdir` 内全局唯一，由切批规划步骤单点分配，规范见 data-spec 第 6 节。**名字不承载时间信息**（起点/时长/band 全在 batch.json 里）。**三工具不校验格式**：`batch_id` 只当不透明字符串用于路径拼接，**旧的时间编码格式（如 `60512_45000`）照常可用**（历史测试资产无需迁移）——但**本手册的示例已统一用 8 位顺序号**，因为示例就是读者会照着写的东西。
- batch.json 位于 `workdir/batches/<batch_id>.json`（单文件全字段），由编排脚本预写，工具只读不回写（位置语义见 data-spec 5.3）。
- 任务粒度：f 任务 = (batch_id, station, ds_index)（fxcorr-f 的 station/ds_index 参数即该两维；多 datastream 站每记录线程一个 f 任务）；x 任务 = (batch_id) 全站全基线（fxcorr-x 无站参数，站列表由 .input 枚举）。**分片模式下 x 任务细化为 (batch_id, ds_group)**——`ds_group` 是**一组 ds（跨站、含全部极化）**，不是单个 ds，见 fxcorr-x 节。**任务 ID 规范**（`<batch>-<station>-<ds>` / `<batch>-<g>` / `<batch>-merge`）见 data-spec 第 6 节。并行模型见 data-spec 第 12 节。
- 错误行为：参数不足或校验失败时打印原因到 stderr 并以非 0 退出；成功退出 0。
- 前置安装：各工具与 `fxcorrcommon` 库，构建见 `build.md`。

---

## fxcorr-sim

架构：单二进制两入口（`station` 单站生成 / 无子命令本机串行），设计见 `fxcorr-sim-arch.md`。**实施状态**：新架构 P0-P4 见于 2026-09-14 完成并验证；**V6 S2.5（2026-09-27）删除了 `common` 子命令**——公共信号不再落盘，改由每个 `station` 任务在本地合成（`data-spec` 5.8）。旧 4 参调用（`fxcorr-sim <batch_id> <station> [workdir] [tone_mhz...]`）已废止，报错提示改用 station 子命令。

```
fxcorr-sim station <batch_id> <station> [workdir] [ds_index] [tone_mhz ...]  # 生成一个站的某个 datastream
fxcorr-sim         <batch_id> [workdir]                    # 默认：本机串行，跑 .input 里的全部站×全部 ds
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识（8 位顺序号，见「通用约定」与 data-spec 第 6 节），用于读 `batches/<id>.json` 与输出命名 |
| `station` | 站名（如 `T1`），输出目录 `raw/<station>/` 与文件名前缀（station 子命令） |
| `workdir` | 项目根目录，默认 `.`（环境变量 `FXCORR_WORKDIR` 亦可定义，位置参数优先） |
| `ds_index` | **仅 station 子命令**：站内 datastream 序号（0-based，缺省 0），与 `fxcorr-f` 的 `ds_index`、`raw` 的 `_ds<N>` 后缀、`fengine/<bid>/<st>/ds_<N>/` 四处同一口径（data-spec 5.2）。多 datastream 站每 ds 一次调用、每 ds 一个文件。**它的位置固定在 `workdir` 之后**，所以 legacy 的 tone 参数要写在它后面：`station <bid> <st> <workdir> 0 1.5`（缺了 ds_index 会把 1.5 当成 ds 序号报错） |
| `tone_mhz ...` | **仅 station 子命令**，0 个 = 新路径；出现即 legacy 模式（旧时域合成路径，字节对拍回归专用）：0 值 = 无 tone；1 值 = 所有 band 同频；nbands 值 = 逐 band |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `FXSIM_SEED` | 固定值 | 公共信号种子（mt19937）。**V6 S2.5 之后它随 `batches/<id>.json` 的 `seed` 字段走**，本变量只用于 `make_testdata.sh` 生成 batch.json 时选种子；一个 batch 的全部 station 任务必须读到同一个值，否则跨站相干静默消失。公共信号只与 (seed, batch) 有关、与站无关；站噪声种子由 (seed, station, ds_index) 派生——**ds 维度必需**：dual-pol 站的 X/Y 两个 ds 覆盖同一频段，不带 ds 就会得到逐位相同的两份数据 |
| `FXSIM_LIGHT` | 关 | **压力测试轻量模式**（V6 S2，`1` 开启）：跳过整条物理信号链（公共信号、块 IDFT、帧级 FFT、延迟链、量化），载荷改由 `(seed, station, ds_index)` 派生的确定性伪随机流填充。**结构完全不变**——帧头、时间轴、帧号、band 布局与完整链**逐帧头相同**（实测 525/525），但数值不含物理意义、跨站不相干，**明确不用于正确性对拍**。用途是体量与时长：t25362 配置（2 站 16 ds、1.024 s、2.0 GB）实测 **0.275 s**，完整链同配置 **6m22s**（1389×）。`FXSIM_GAPS` / `FXSIM_STARTOFFSET` 照常生效。**只作用于新路径**：与 tone 参数（legacy）同时给出时报错退出 |
| `FXSIM_NOISE` | `0.02` | station 端高斯噪声 σ；`0` 关闭（两站同参数全关闭 = 输出逐位一致，跨站相干校验） |
| `FXSIM_ADAPTIVE` | 关 | station 端自适应量化门限（`1` 开启；datasim d_tmul 语义：量化前按数据 rms 缩放、每帧更新、1M 样本封顶），默认关 |
| `FXSIM_SPECRES` | 1 | specRes 缩放因子（正整数）：公共信号频谱分辨率 ÷N、样本数 ×N（datasim --specres 语义）；网格缩放后的一致性检查照跑，station 须用同一值（meta.json 网格不匹配即报错） |
| `FXSIM_LINE` | 关 | 谱线 `freq,amp,rms`：freq = 绝对 MHz（须落在公共信号带内，越界报错），amp = 幅度（滤波器乘 √amp），rms = 网格点数（datasim gengaussianfilter 语义，re=im 分量同乘）；站端合成公共信号时 gencplx 后逐 slice 频域注入、跨站相干（每站用同一 seed 与同一顺序，注入结果逐位相同） |
| `FXSIM_DELAY` | 开（`0` 关） | **新路径**：完整延迟链（datasim updatevalues + processdata 语义）——每帧 .im 模型求 delay/rate（order=1）、fracsamperror 累积超半复样本整样本移位（滚动基带缓冲）、频域亚样本校正（e^{j·2π·bandwidth·idx/vpsamps·fracerr}）、时域条纹旋转（band 起始频率，fraction_of = x−rint(x−0.5) 小数相位）；`0` = 延迟无关恒等链（字节回归）。**legacy**：`1` = 把 .calc 几何延迟注入 tone 相位（+2π·f_RF·τ(t)，每帧 order=1 求值、帧内线性；pcal 不注入） |
| `FXSIM_FLUX` / `FXSIM_SEFD` | 关 / 1000 | **新路径**：flux > 0 启用 datasim fabricatedata 定标链（×√F → +√SEFD 站噪声 → ÷√(F+SEFD) 归一），替代 FXSIM_NOISE 路径（显式设 NOISE 时报错）；SEFD = 单值全站共用或逗号列表按 .input datastream 序逐站取值（datasim -s 语义），flux 设了而 SEFD 没设时用默认 1000。**legacy**：源流量 / 站 SEFD（Jy），**两者都设**才启用 SNR 定标（tone 幅度 = 0.5·√(2F/(F+SEFD))、噪声 σ = 0.5·√(SEFD/(F+SEFD))，覆盖 FXSIM_NOISE） |
| `FXSIM_PCAL` | 关 | **新路径**：datasim `-p` 梳齿间隔（MHz）——基带 k·interval MHz（k < 复带宽/interval，datasim 循环边界）各注入一根梳齿，幅度 1/500（datasim applyphasecal），相位从 batch 起点连续累积（datasim 的 mod 周期 = 2·带宽个样本 = 恰 1 秒 = 整数梳齿周期，等价），并做帧边 taper（首 3 样本 0/×½/×⅘、末 3 样本 ×⅘/×½/×0，datasim applyphasecal 尾部）；注入点在延迟校正链之后（pcal 不随几何延迟移动，datasim/legacy 同构）。与 .input PHASE CAL 网格 tone 独立叠加 |
| `FXSIM_GAPS` | 关 | **记录中断**（缺口/filler 处理回归，检验步骤见 `test/gaps/README.md`）：逗号分隔 `<秒>:<帧数>[:<形态>[<filler帧数>]]`——在距 batch 起点该秒处让帧号跳过 `<帧数>`（真实丢失的帧）；形态字母 `f` = **帧头全零**占位帧（t25362 的形态，默认每丢一帧写一帧，`:f<N>` 单独指定帧数）、`p` = 整帧写 vdifio 的 `FILL_PATTERN`（0x11223344）、`h` = 只有帧首 4 字节是该模式——后两种对应 `vdifmux` 认的两处（帧尾/帧首，跳过的长度不同，见 `reader-model.md` 4.8 的形态清单）。两种形式在**同一位置**造出同一中断、**帧号范围相同**，与 t25362 的真实中断同构（同一观测中一个 datastream 用 filler、其余用缺帧）。**`filler ≫ 缺口` 才是把读位置带偏的形态**（t25362 是 81..508 帧 filler 对 10..63 帧缺口），`f<N>` 就是为它加的，判据见 `test/gaps/run_filler.sh`。legacy 与新路径都生效；不设 = 行为完全不变 |
| `FXSIM_STARTOFFSET` | 0 | **记录起点偏移**（A 类读取缺陷回归，检验步骤见 `test/gaps/run_startoffset.sh`）：生成器跳过 batch 开头的这么多**帧**——它们既不在文件里、也不占文件字节，于是文件第一帧的时间戳就是 batch 起点 + N 帧，而 `anchorbytes` 为**负**（t25362 的 BA 首帧晚 79.312 ms = 1269 帧，S6 恰在秒边界）。帧号仍按时间轴推进，所以文件内容的时间是对的、错的只能是读法——`fxcorr/reader-model.md` 4.1 的 A1（漏算）/ A2（取整）/ A3（负位置整段判无效）三类。legacy 与新路径都生效；不设或 `0` = 行为完全不变 |
| `FXCORR_WORKDIR` | `.` | 项目根目录；`workdir` 位置参数优先 |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file / **seed**）。
- 读 `workdir/<config_file>`（.input，非 MPI 构造），band 结构/采样率/PHASE CAL 网格全部来自 .input；公共信号的 specRes/numSamps 网格由**全站** band 布局推导（候选两段：0.5 → 1/1024 MHz 优先，全不适配时放宽到 1、2、4… MHz；候选还要让每帧占整数个 slice，见下面"程序内校验"）。
- **公共信号不再落盘**（V6 S2.5）：每个 station 任务在本地合成整条 slice 流、只取自己 band 的那一段，不写也不读任何公共文件——原来那个 `sim-common/` 目录、它的 `meta.json` 与 `data_XX.bin` 全部取消。规范见 data-spec 5.8。
- `station` 输出 `workdir/raw/<station>/<station>_<batch_id>[_ds<N>].vdif`（2bit VDIF，多 band 帧内样本 band 交织）；延迟注入默认开（FXSIM_DELAY=0 关）。**`_ds<N>` 后缀只在多 datastream 站出现**（N = 站内序号 = `ds_index`），单 datastream 站的文件名与加多 ds 支持之前逐字相同——树里的测试资产全是单 ds 配置。
- 默认模式 = 对 `.input` 全部 datastream（每站每 ds 一个文件）逐个 `station`，本机串行；仅限单机，多节点时编排层分别调用 `station`。

程序内校验（不满足即报错退出）：实采样（complex 拒绝）、2bit（bytespersample 校验）、band 数 ∈ {1,2,4,8,16,32}；单 scan；batch 起点 subint 边界（1µs 容差）→ 整秒 snap → 帧边界；batch 时长非帧整数倍时文件生成到下一个帧边界取整（fxcorr-f 只读 batch 段）；common 的 band 频率须落在 specRes 网格（(freq−minStartFreq)/specRes 整数）；帧须为整数 slice（vpsamps % blksize == 0，即 1e6×specRes/fps 整数）、块总复样本须为帧复样本整数倍（nslices×blksize % vpsamps == 0，含末块）。

示例：

```bash
# 本机一键（.input 里全部站 × 全部 ds）
fxcorr-sim 00000001

# 分布式（编排调用；每个 station 任务可在不同节点，公共信号各自本地合成）
fxcorr-sim station 00000001 T1 /data/proj

# 跨站相干校验：两站噪声全关 → 输出逐位一致（同一 seed、同一合成顺序）
FXSIM_NOISE=0 fxcorr-sim station 00000001 T1
FXSIM_NOISE=0 fxcorr-sim station 00000001 T2

# P2：谱线（201.5 MHz，幅度 10，rms 3 网格点）+ 4 倍频谱分辨率
FXSIM_LINE=201.5,10,3 FXSIM_SPECRES=4 fxcorr-sim station 00000001 T1

# P2：datasim 定标链（源 100 Jy；T1 SEFD 100、T2 SEFD 10000 按 datastream 序）
FXSIM_FLUX=100 FXSIM_SEFD=100,10000 fxcorr-sim station 00000001 T1

# P2：延迟注入默认开（.im 几何延迟完整链）；FXSIM_DELAY=0 回恒等链（字节回归）
FXSIM_DELAY=0 fxcorr-sim station 00000001 T1

# P4：datasim 梳齿（每 1 MHz 一根，1/500 幅度 + 帧边 taper；与 .input PHASE CAL 网格叠加）
FXSIM_PCAL=1 fxcorr-sim station 00000001 T1

# legacy 模式（字节对拍回归）：与 gen_test_vdif.py 逐字节一致
FXSIM_NOISE=0 fxcorr-sim station simcmp T1 . 0 1.5

# legacy：几何延迟注入（delay≠0 的 VLBI 观测仿真，tone 纯相位版）
FXSIM_DELAY=1 fxcorr-sim station 00000001 T1 . 0 1.5

# legacy：SNR 定标（源 100 Jy、站 SEFD 1000 Jy）
FXSIM_FLUX=100 FXSIM_SEFD=1000 fxcorr-sim station 00000001 T1 . 0 1.5

# 多 datastream 站（.input 每站 2 个 ds）：逐 ds 调用，各出一个文件
fxcorr-sim station 00000001 T1 . 0     # -> raw/T1/T1_00000001_ds0.vdif
fxcorr-sim station 00000001 T1 . 1     # -> raw/T1/T1_00000001_ds1.vdif
fxcorr-sim 00000001                    # 或默认模式一次生成全部站的全部 ds
```

---

## fxcorr-f

```
fxcorr-f <batch_id> <station> [workdir] [ds_index]
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识 |
| `station` | 站名（须在 .input 的 DATA TABLE / datastream 中） |
| `workdir` | 项目根目录，默认 `.`（环境变量 `FXCORR_WORKDIR` 亦可定义，位置参数优先） |
| `ds_index` | 站内 datastream 序号（0-based，默认 0；多 datastream 站每记录线程一个 f 任务）；输出目录 `fengine/<batch_id>/<station>/ds_<ds_index>/` |

环境变量（DifxMessage 状态发送，algo-plan P1）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `DIFX_MESSAGE_GROUP` / `DIFX_MESSAGE_PORT` | 未设（静默） | host 模式组播目标（setup.bash 默认 224.2.2.1:50201）；未设时不发状态消息 |
| `FXCORR_STA` | 未设 | `1` 时每 autocorr 批次向 `DIFX_BINARY_GROUP/PORT` 组播 DifxMessageSTARecord（STA_AUTOCORRELATION）；minpostavfreqchannels ≥ STA 通道数时自动走频域平均分支（与 mpifxcorr 一致，P9） |
| `FXCORR_KURTOSIS` | 未设 | `1` 时每 subint 末向 `DIFX_BINARY_GROUP/PORT` 组播 DifxMessageSTARecord（STA_KURTOSIS，谱峰度，无 weight 门槛与归一化，P9） |
| `DIFX_BINARY_GROUP` / `DIFX_BINARY_PORT` | 未设 | STA 二进制组播目标；未设时 STA 静默 |
| `FXCORR_RUN_MODE` | 未设 | `container` 时状态/STA 降级为落盘 `meta/difxmsg/`（见 data-spec 5.6） |
| `OMP_NUM_THREADS` | 未设（串行） | **V3 P3**：块级并行线程数（Mode 副本分块并行，结果与串行逐位一致）；未设 = 单线程（V2 行为不变） |
| `FXCORR_LOGLEVEL` | 未设（`info`） | 诊断输出详略，取 `error`/`warn`/`info`/`verbose`/`debug`（无法识别时报错一行并回落 `info`）。**管住 f/x 的全部输出**：工具自己的打印（`FXLOG` 宏）与 fxcorrcommon 里上游代码的 `Alert` 流（`cinfo`/`cverbose`/`cdebug`，配置加载阶段约 85 行）。`info` 出错误、告警、单行小结（`GAPCHECK summary`）与配置加载进度（约 21 行）；`verbose` 追加逐条 gap/filler/boundary 明细（boundary 上限 40 条）；`warn` 及以下只剩错误与告警。只影响输出，不影响产物 |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file）。
- 读 `workdir/<config_file>`（.input）；Model 由 .calc 内建，无 .im 依赖。
- 原始数据文件路径直接取自 .input 的 DATA TABLE（相对进程 cwd，即 workdir）。
- 输出 `workdir/fengine/<batch_id>/<station>/`（自动创建）：`band_XX.sp`、`pcal.bin`（配置了 phasecal 时）、`autocorr.bin`（二进制布局见 data-spec 5.3）。配置 phasecal 时另在 OUTPUT FILENAME 目录（`vis/<exp>.difx/`）写实验级 `PCAL_<mjd>_<sec>_<station>` 文本（每 intTime 一行、重跑幂等，格式见 data-spec 5.4）。
- 状态消息节奏（mpiId = dsindex+1，datastream/core 角色）：Starting（启动）→ 每 subint 两条 Diagnostic（DataConsumed/InputDatarate）→ Ending → Done；错误时 Alert + Aborting。RUNNING 不发（归 fxcorr-x，manager 角色）。

程序内校验：batch 起点须在 subint 边界（1µs 容差，吸收 start_mjd 的 f64 表示误差）——batch.json 的 start_mjd 建议写精确 repr（如 `58948.291666666664`），否则报错退出。

V1 边界（P10 已补齐输入格式，2026-09-14）：本地文件输入，支持 VDIF/VDIFL（单线程）、INTERLACEDVDIF（多线程 corner-turn）、MARK5B（mark5bfix 修复）、LBASTD/LBAVSOP/LBA8BIT/LBA16BIT（ASCII 头 + raw payload）、MKIV/VLBA/VLBN/KVN5B/CODIF（mark5access 通用流）；K5VSSP/K5VSSP32 报错退出（上游 mark5access 亦不可用）；硬件访问（StreamStor/Mark6）不迁移；单 scan、无 zoom band 落盘。

示例：

```bash
fxcorr-f 00000001 T1            # 当前目录为项目根
fxcorr-f 00000001 T1 /data/proj # 显式 workdir
FXCORR_WORKDIR=/data/proj fxcorr-f 00000001 T1  # 环境变量定义（位置参数优先于它）
```

---

## fxcorr-x

两份职责：**处理**（一个 batch 的互相关，产可见度）与**归并**（把分片产出合成为正式 SWIN）。

```
fxcorr-x       <batch_id> [workdir]              # 处理一个 batch，直接写 SWIN（现行）
fxcorr-x       <batch_id> [workdir] <ds_group>   # 分片模式：只处理一个 ds 组 → vis-parts/<batch_id>/ds<G>.part（现行）
fxcorr-x merge <batch_id> [workdir]              # ① batch 级归并 → vis-parts/<batch_id>/merged.part
fxcorr-x merge --experiment [workdir]            # ② 实验级归并 → SWIN，唯一写入者
```

> **`merge` 的两级形态（2026-09-28 实施，取代 2026-09-27 的单级行为）**：`merge <batch_id>` 读
> `vis-parts/<batch_id>/ds*.part`、按整数纳秒归并，写出 **`vis-parts/<batch_id>/merged.part`**
> （**不碰 SWIN**）；新增的 `merge --experiment` 读本实验全部 batch 的 `merged.part`、按**数据
> 时间序**写出 SWIN——**它才是 SWIN 的唯一写入者**。`run_batch.sh` 的 `FXCORR_X_SHARD=1` 逐组
> 跑完会**自动调 batch 级那次**；**实验级不进 `run_batch.sh`**（它与 `wrap_difx2fits.sh` 同为
> 实验级操作，由编排层在该实验全部 batch 跑完后调一次）。被取代的旧行为是"batch 级直写 SWIN +
> 编排层保证同实验按时间序串行"——单节点成立、多节点并行 batch 时静默错数据。实施细则见
> `data-spec` 5.9 末条，本节末「两级 `merge`」小节是接口侧摘要。

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识 |
| `workdir` | 项目根目录，默认 `.`（环境变量 `FXCORR_WORKDIR` 亦可定义，位置参数优先） |
| `ds_group` | **分片模式**，位置在 `workdir` 之后（要指定它必须先给 `workdir`，同 fxcorr-f 的 `ds_index`）：只处理指定的 **ds 组**（0-based，按频段升序）——一组 = **跨站、含全部极化**的若干 datastream，它们覆盖同一个频段组（互相关要求两站同一 freq 同时在场，且要算全极化组合；t25362 实测为 4 组，每组 2 站 × 2 极化 = 4 个 ds）。**给出该参数即进入分片模式，不写 SWIN**，改出 `vis-parts/<batch_id>/ds<G>.part`；**省略 = 现行行为**（整 batch 全部 ds，直接写 SWIN，逐位不变） |

环境变量（DifxMessage 状态发送，algo-plan P1）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `DIFX_MESSAGE_GROUP` / `DIFX_MESSAGE_PORT` | 未设（静默） | host 模式组播目标（setup.bash 默认 224.2.2.1:50201）；未设时不发状态消息 |
| `FXCORR_RUN_MODE` | 未设 | `container` 时状态降级为落盘 `meta/difxmsg/`（见 data-spec 5.6） |
| `OMP_NUM_THREADS` | 未设（串行） | **V3 P3**：基线循环并行线程数（scratch 按线程私有化，结果与串行逐位一致）；未设 = 单线程（V2 行为不变） |
| `FXCORR_X_MERGE_FORCE` | 未设（严格） | **两个层级的 `merge` 都认**：batch 级是"ds 组不齐"、实验级是"batch 不齐"，语义相同——强制写出已到齐的部分（缺失者留空洞 + stderr 列明缺了哪些），未设 = 报错退出不写 |
| `FXCORR_X_SWIN_CONFLICT` | 未设（严格） | **三种写 SWIN 的模式共用（全量 / 分片 / 实验级 `merge`）**：目标 SWIN 里若已有**本 batch 时间范围内**的记录（说明这个 batch 已经被另一次运行写过），默认报错退出——再写会追加重复记录、破坏 SWIN 的时间单调（difx2fits 顺序读、时间回退的记录被静默丢弃）。设 `allow` 强制继续。**重跑分片与 batch 级 `merge` 不受影响**（两者都不写 SWIN），被拦的是"`merge` 之后又跑全量、或又 `merge`"。**2026-09-28 起移交实验级**：检查由 `merge --experiment` 做，范围扩为"**本次将写出的全部 batch**"；batch 级 `merge` 与分片任务都不写 SWIN、**都不查** |
| `FXCORR_X_ALLOW_EMPTY` | 未设（严格） | **空输入 / 空产物防呆（2026-09-28，`v6-plan.md`「进行中的发现」2）**。两条互补的检查，挡的是同一种最坏的静默——每个程序都退 0、日志照报 "N integrations written"，产物却是空文件：① **整个 batch 一块有效数据都没有**（`.sp` 的 valid flags 全 0；那是 f 写的"这些块没有数据"，x 以前完全不用它，只靠 weight 门控间接判断，于是"数据根本没读到"与"读到了但权重为 0"分不开）；② 全量模式下 batch 算完却**一条记录都没进 SWIN**（数据有效，但每条基线的 weight 都成了 0）。两者都可能由 **raw 与 `batch.json` 的时间轴不符**引起（软链指错 batch、VDIF 幂等跳过而重新规划改了 batch 时长……）。设 `1`（或 `allow`）跳过检查。**正常数据不触发**：部分 subint 无效是常态（gap / filler / 真实观测丢帧），只有**全无效**才报；分片模式写 `.part`，只做① |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file / difx_dir）。
- 数据源 `workdir/fengine/<batch_id>/<station>/`（各站 band_XX.sp + autocorr.bin），station 列表 = .input 的全部 datastream（自动枚举，无站参数）。
- 输出目录由 **.input 的 OUTPUT FILENAME** 决定（SWIN 写盘语义，difx2fits 零改造前提），batch.json 的 difx_dir 仅为元数据；输出目录不存在时自动创建（mkdir -p OUTPUT FILENAME 目录）。
- **分片模式（给了 `ds_group`）**：输入只读**本组**的 fengine（该频段组在各站的 `ds_<N>/` 子目录），输出 `vis-parts/<batch_id>/ds<G>.part`（D16），**不写 SWIN**。不同组可由不同进程并行，各写各的文件，互不干扰。
- **相位阵配置（.input `PHASED ARRAY TRUE` + `PHASED ARRAY CONFIG FILE`）时不写 SWIN**，改出 `beam/<batch_id>/beam.bin`（波束加权和，per ACC TIME 窗口记录；布局见 data-spec 5.5）。
- 状态消息节奏（mpiId = 0，manager 角色）：Starting（启动）→ 每积分写盘一条 Running（visibilityMJD = 积分中心、各站 band 平均 weight、jobstart/jobstop）→ Ending → Done；错误时 Alert + Aborting。与 mpifxcorr 基准逐字段对拍通过（visibilityMJD/weight 逐位一致）。相位阵模式只发 Starting/Ending/Done（无积分写盘）。

程序内校验：单 scan、单相位中心（脉冲星 binning 时多源免检）、intTime 为 subintNS 整数倍（相位阵时跳过）、batch 起点 subint 边界（1µs 容差，同 fxcorr-f）。

### 分片模式与 `merge` 子命令（2026-09-27 已实施）

**动因**：目标计算节点为 30 核 / 120 GB SATA SSD / 60 GB tmpfs，而 t25362 参数下一个**最小** batch（1 个 intTime = 1.024 s）的 `fengine` 就是 67.4 GB——占 SSD 56%、tmpfs 装不下。整 batch 处理在这台机器上不可行。量化与三条路线的对比见 `data-volume.md` §7。

**分片**：x 的处理单元从 `(batch_id)` 细化为 `(batch_id, ds_group)`——每个分片任务只读本组的 fengine、只算本组的 baseline，产出一个 `.part`。切分维度取 **ds 组而不是单个 freq**：raw 与 fengine 都天然按 ds 分离，且 zoom 子带与其父 band 在同一 ds 内（按 freq 切会让父 band 与子带落在不同分片，读取直接失败）。

**一组 ds 是跨站、含全部极化的**——这是互相关决定的：两站同一 freq 必须同时在场，且 baseline 有四种极化组合（RR/LL/RL/LR），X 与 Y 两个极化不能拆到不同分片。**不同极化本身是独立计算的**（每种组合是 .input BASELINE TABLE 里一条独立条目，绑定一对具体的 ds），但一条 baseline 要它声明的两个 ds 同时在场——所以**分组成员必须从 BASELINE TABLE 推导，不是按 ds 序号猜**（机理与实测见 `data-spec` 第 8 节）。t25362 实测（`.input` DATASTREAM 表）：每站 8 个 ds = **4 个频段组 × 2 个极化（X/Y）**，所以 x 分片任务共 **4 个**；而 f 任务按 ds 走，是 2 站 × 8 = **16 个**——**两者的数量不同**，编排层要按组维护依赖（一个 x 分片任务只等属于该组的那些 f 任务，不是等全部）。实测映射见 `data-volume.md` §7.3。

**归并**：`fxcorr-x merge <batch_id> [workdir]` 读 `vis-parts/<batch_id>/` 下的全部 `.part`，按**整数纳秒时间戳**归并，追加写出该 batch 时间范围的正规 SWIN 记录。复用 x 已有的 SWIN 写入路径（`visibility.cpp` 的 `writeSWIN`），不新增独立程序。

**两种模式对同一 batch 互斥**：不分片（一次全 ds、直写 SWIN）与分片（多次写 `.part` + 一次 `merge`）**不能混跑**——分片任务不写 SWIN 而 `merge` 会追加，混跑产生重复记录、破坏 SWIN 的时间单调。**f 侧不受影响**：`fxcorr-f` 天然每 ds 一个任务，两种模式下 `fengine/` 产出完全相同，选哪种只决定 x 跑几次。

三条硬约束（**2026-09-28 起的两级形态**；单级时代的对应条款见 `v6-plan.md` S4.1）：

1. **实验级 `merge` 是 SWIN 的唯一写入者**，分片任务与 batch 级 `merge` 都不写 SWIN。SWIN 的追加顺序必须严格按时间——difx2fits 顺序读记录并按天线检查时间单调（`fitsUV.c:1227` 的 `RecordIsOld`），时间回退的记录被**静默丢弃**（结尾只打一行计数，不报错）。分析见 `data-volume.md` §7.5。
2. **实验级 `merge` 按各 `merged.part` 的首记录时间定序**，与 batch 的**完成**顺序无关——这正是"数百节点并行处理不同 batch"能成立的原因（单级时代靠"同实验按时间序串行"这条编排层约定，节点数上去后无法保证）。batch_id 序与时间序不一致时**报错**而非静默选一。
3. **缺片/缺 batch 时两个层级的 `merge` 都报错退出、不写**（batch 级缺哪个 ds 组报哪个、实验级缺哪个 batch 报哪个），避免写出不完整的时间段。设 `FXCORR_X_MERGE_FORCE=1` 可**强制写出**已到齐的部分——缺失者留空洞，并在 stderr 明确列出缺了哪些。**强制写的后果是 SWIN 静默缺段**（difx2fits 不会因此报错，只是那些时间段/频段没有记录），只应在明知缺失原因、且确定不重跑时使用。

`vis-parts/` 的目录规范（与 `vis/` 的隔离边界、生命周期）见 data-spec 5.9。

**实现要点（2026-09-27 落地）**：ds 组由 `fxcorr-x` 从 `.input` 的 BASELINE TABLE 用并查集推导（连通分量 = 一组，组序 = 组内最小 ds 序）；`run_batch.sh` 的 `FXCORR_X_SHARD=1` 走分片路径（逐组 + 一次 `merge`），它自己也算一遍组数——**两处实现必须同改**（同 `roots.sh` 与 `FxcorrPath` 的关系）。

#### 两级 `merge`（形态 A，2026-09-28 实施）

**问题**：上面三条硬约束在多节点部署下失效——数百个节点并行处理**不同 batch** 时，"每个 batch 跑完立刻 merge 写 SWIN"会让**时间回退**（节点 B 先完成先写、节点 A 后写），而 difx2fits 对回退记录**静默丢弃**（约束 1 的机理）。约束 2 把串行责任推给编排层，但节点数上去后它无法保证。

**形态 A（2026-09-27 定）**：SWIN 的写入收敛到**实验级一次**，`merge` 分两级：

| 级 | 调用 | 读 | 写 |
|---|---|---|---|
| **batch 级** | `fxcorr-x merge <batch_id> [workdir]` | `vis-parts/<batch_id>/ds*.part` | **`vis-parts/<batch_id>/merged.part`**（不碰 SWIN） |
| **实验级** | `fxcorr-x merge --experiment [workdir]` | 全部 batch 的 `merged.part` | **SWIN——它才是唯一写入者** |

实验级的四条接口约定（细则与理由见 `data-spec` 5.9 末条）：

1. **实验的认定**：实验级没有 `batch_id`，读不到 `batches/<batch_id>.json`。它扫 `batches/*.json`、**把已有 `merged.part` 的 batch 当候选**，用候选的 `config_file` 构造配置；**候选之间 `config_file` 不一致即报错**（workdir 混了多个实验），候选为空同样报错。
2. **缺 batch 的判据**：应有集 = `batches/*.json` 中 `config_file` 与本次选定者相同的**全部** batch（那是切批规划的权威清单）——**不是** `meta/batches.index`（它是"完成流水"，会让 `failed` 的 batch 静默消失）。缺 `merged.part` 即缺失，**默认报错退出、不写**；`FXCORR_X_MERGE_FORCE=1` 强制写出已到齐的部分并列明缺了哪些。`status` 字段不参与判据，但随缺失清单逐条列出。
3. **定序与流式写出**：按各 `merged.part` 的**首记录时间**定序，**不是** `batch_id` 数值序（编号只保证由单点分配，未规定"编号 = 时间序"）；两者不一致时报错而非静默选一。写出**逐个 batch 流式进行**（读一个、批内排序、追加写出、释放），**不做全局排序**——各 batch 时间范围不重叠，故内存峰值 = 单个 batch。
4. **根一致性检查**：对**每个候选 batch** 逐个比 `meta/roots/<batch_id>.json` 里的 `VIS` 值（实验级唯一用到的根）。取候选集而非应有集——缺 `merged.part` 的 batch 不参与写出。它拦的是"记录存在、但本次运行解析出的根与记录不符"（换了 `FXCORR_VIS_ROOT`、或工作区被两套编排写过）。

**连带**：① `FXCORR_X_SWIN_CONFLICT` 移交**实验级**，作用范围从"本 batch"扩为"本次写出的全部 batch"；② `run_batch.sh` 的分片路径改为"逐组 + batch 级 merge"；③ **实验级 merge 不进 `run_batch.sh`**——它与 `wrap_difx2fits.sh` 同为实验级操作，由编排层在该实验全部 batch `done` 后调一次；④ **增量合并不做**（滚动窗口必须先定义"分片迟到时等 / 超时跳过 / 告警"，没有真实需求驱动时引入只会变成"看起来在跑、实际卡住"）。`.part` 的 glob 是 `ds*.part`，与 `merged.part` 不冲突——命名即隔离。

**为什么保留 batch 级那一层**：24 h 观测按 t25362 参数是 84,375 batch × 4 组 = **337,500 个 `.part`**；batch 内归并把文件数收敛 4 倍，且这一层归并**本来就必须做**，失败时还能只重跑该 batch。

路线与验收见 `v6-plan.md` S4.1，目录规范见 `data-spec` 5.9 末条。

```bash
# 形态 A 落地后的调用形态
for g in 0 1 2 3; do fxcorr-x 00000001 . $g & done; wait   # ① 分片（可并行、可跨节点）
fxcorr-x merge 00000001                                    # ② batch 级 → merged.part
fxcorr-x merge --experiment                                   # ③ 实验级 → SWIN（全部 batch 跑完后一次）
```

#### 其它实验级文件的并发语义（⚠ 待确认，V6）

多节点并行 batch 时，**同一实验的所有 batch 会写同一批 `PCAL_*` / `SWITCHEDPOWER_*` 文本**——它们用 `ios::app` 追加、文件名含**实验起点**（`visibility.cpp:122`）而不是 batch 起点。`O_APPEND` 对 `PIPE_BUF`（4096 B）以内的写是原子的，短行不会截断交错，但**行序不再等于时间序**、且每次打开都会重写一遍注释头。**V6 实施前要核实 difx2fits 的容忍度**（`v6-plan.md` S4.1 待确认项，`data-spec` 第 12 节有表）。

**当前限制（程序内明确报错退出，不是静默降级）**：分片模式只支持**单相位中心**、**无 pulsar binning**，且不适用于相位阵配置。

V1 边界：仅 fxcorr-f 的 .sp 输入；zoom band（P4a）、多相位中心（P4b）、脉冲星 binning（P4c）、交叉极化自相关（P7）、相位阵波束形成（P8）均已支持；PCAL/STA 文件生成不做。

示例：

```bash
mkdir -p vis/experiment.difx    # OUTPUT FILENAME 所在目录（自动创建亦可）
fxcorr-x 00000001
```

分片模式与归并：

```bash
# ① 分片：每个 ds 组一个任务，可并行、可跨节点
for g in 0 1 2 3 4 5 6 7; do fxcorr-x 00000001 . $g & done
wait

# ② batch 级归并：本 batch 全部组到齐后调用 → vis-parts/00000001/merged.part
fxcorr-x merge 00000001

# ③ 实验级归并：本实验全部 batch 跑完后一次 → SWIN（唯一写入者）
fxcorr-x merge --experiment
```

---

## 典型端到端流程

与 `fxcorr/CLAUDE.md` 测试流程一致（测试机 /root/fxcortest/）：

```bash
./fxcorr/wrap_vex2difx.sh . test.v2d     # 前处理 1：出 .input/.calc（含路径规范化）
./fxcorr/wrap_difxcalc.sh . test.calc    # 前处理 2：出 .im
./fxcorr/make_testdata.sh               # 造数 + 写 batch.json + 建 DATA TABLE 软链
./fxcorr/run_batch.sh <batch_id>        # 逐站 f + 一次 x
./fxcorr/wrap_difx2fits.sh . test        # 后处理：SWIN -> FITS（落 FXCORR_PRODUCT_ROOT）
```

手工等价的调用（cwd 取 workdir）：

```bash
fxcorr-sim <batch_id> T1                      # 逐站生成 raw 数据（落 FXCORR_RAW_ROOT）
ln -sf $FXCORR_RAW_ROOT/T1/T1_<batch_id>.vdif  $FXCORR_RAW_ROOT/<DATA TABLE 文件名>
fxcorr-f <batch_id> T1                        # 逐站
fxcorr-x <batch_id>                           # SWIN 落 $FXCORR_VIS_ROOT/<OUTPUT FILENAME>
```

分片模式（`run_batch.sh` 的 `FXCORR_X_SHARD=1`）下，最后一行改为"逐 ds 组跑分片 + 一次 **batch 级** `merge`"（各组可并行，`merge` 须等本 batch 全部组到齐）——**它不产出 SWIN**，SWIN 由实验级的 `fxcorr-x merge --experiment` 写出（见下一段），见 fxcorr-x 节。

> **2026-09-28 起**：`merge` 分两级——上面那次是 **batch 级**（改写 `vis-parts/<batch_id>/merged.part`，不碰 SWIN），**实验级** `fxcorr-x merge --experiment` 在该实验全部 batch 跑完后由编排层调一次、才写出 SWIN。"同实验跨 batch 按时间序串行"这条约束随之消失。见 fxcorr-x 节的「两级 `merge`」小节与 `v6-plan.md` S4.1。

以上手工步骤由编排脚本自动化（`make_testdata.sh` / `run_bench.sh` / `run_batch.sh`，
规格见 v1-plan 2.4；**前处理与后处理的原 difx 程序请走 `wrap_*.sh` 封装**——它们不认识根
变量、且按 cwd 解析配置里的相对路径，封装负责那层转换）。
