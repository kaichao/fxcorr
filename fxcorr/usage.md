# fxcorr 工具命令行手册

覆盖三个改造应用的命令行接口：`fxcorr-sim`（仿真数据生成器）、`fxcorr-f`（Station-based）、`fxcorr-x`（Baseline-based）。目录布局与 batch.json 格式见 `data-spec.md`。

## 通用约定

- 三工具均**无 MPI、串行**，通过目录接口衔接。
- `workdir` 定位：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认当前目录（`.`）；所有相对路径（batches、.input、raw、fengine、vis）均相对 `workdir` 解释。
- batch.json 位于 `workdir/batches/<batch_id>.json`（单文件全字段），由编排脚本预写，工具只读不回写（位置语义见 data-spec 5.3）。
- 任务粒度：f 任务 = (batch_id, station, ds_index)（fxcorr-f 的 station/ds_index 参数即该两维；多 datastream 站每记录线程一个 f 任务）；x 任务 = (batch_id) 全站全基线（fxcorr-x 无站参数，站列表由 .input 枚举）。并行模型见 data-spec 第 12 节。
- 错误行为：参数不足或校验失败时打印原因到 stderr 并以非 0 退出；成功退出 0。
- 前置安装：各工具与 `fxcorrcommon` 库，构建见 `build.md`。

---

## fxcorr-sim

架构：单二进制三入口（`common` 共享公共信号 / `station` 单站生成 / 无子命令本机串行），设计见 `fxcorr-sim-arch.md`。**实施状态（2026-09-14）**：新架构 P0-P4 全部完成并验证（下述即现行接口；验证记录见 applications/fxcorr-sim/VERIFICATION.md）；旧 4 参调用（`fxcorr-sim <batch_id> <station> [workdir] [tone_mhz...]`）已废止，报错提示改用 station 子命令。

```
fxcorr-sim common  <batch_id> [workdir]                    # 只生成公共信号
fxcorr-sim station <batch_id> <station> [workdir] [tone_mhz ...]   # 读公共信号，生成一个站
fxcorr-sim         <batch_id> [workdir]                    # 默认：本机串行 common + 全站 station
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识（`60512_45000` 或 `20260908_123000`），用于读 batch.json、common/ 与输出命名 |
| `station` | 站名（如 `T1`），输出目录 `raw/<station>/` 与文件名前缀（station 子命令） |
| `workdir` | 项目根目录，默认 `.`（环境变量 `FXCORR_WORKDIR` 亦可定义，位置参数优先） |
| `tone_mhz ...` | **仅 station 子命令**，0 个 = 新路径；出现即 legacy 模式（旧时域合成路径，字节对拍回归专用）：0 值 = 无 tone；1 值 = 所有 band 同频；nbands 值 = 逐 band |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `FXSIM_SEED` | 固定值 | 公共信号种子（mt19937，common 与 station 共用）；新路径下公共信号只与 (seed, batch) 有关、与站无关（跨站相干来源），站噪声种子由 (seed, station) 派生 |
| `FXSIM_NOISE` | `0.02` | station 端高斯噪声 σ；`0` 关闭（两站同参数全关闭 = 输出逐位一致，跨站相干校验） |
| `FXSIM_ADAPTIVE` | 关 | station 端自适应量化门限（`1` 开启；datasim d_tmul 语义：量化前按数据 rms 缩放、每帧更新、1M 样本封顶），默认关 |
| `FXSIM_SPECRES` | 1 | specRes 缩放因子（正整数）：公共信号频谱分辨率 ÷N、样本数 ×N（datasim --specres 语义）；网格缩放后的一致性检查照跑，station 须用同一值（meta.json 网格不匹配即报错） |
| `FXSIM_LINE` | 关 | 谱线 `freq,amp,rms`：freq = 绝对 MHz（须落在公共信号带内，越界报错），amp = 幅度（滤波器乘 √amp），rms = 网格点数（datasim gengaussianfilter 语义，re=im 分量同乘）；common 端 gencplx 后逐 slice 频域注入、跨站相干；meta.json 记录三个参数 |
| `FXSIM_DELAY` | 开（`0` 关） | **新路径**：完整延迟链（datasim updatevalues + processdata 语义）——每帧 .im 模型求 delay/rate（order=1）、fracsamperror 累积超半复样本整样本移位（滚动基带缓冲）、频域亚样本校正（e^{j·2π·bandwidth·idx/vpsamps·fracerr}）、时域条纹旋转（band 起始频率，fraction_of = x−rint(x−0.5) 小数相位）；`0` = 延迟无关恒等链（字节回归）。**legacy**：`1` = 把 .calc 几何延迟注入 tone 相位（+2π·f_RF·τ(t)，每帧 order=1 求值、帧内线性；pcal 不注入） |
| `FXSIM_FLUX` / `FXSIM_SEFD` | 关 / 1000 | **新路径**：flux > 0 启用 datasim fabricatedata 定标链（×√F → +√SEFD 站噪声 → ÷√(F+SEFD) 归一），替代 FXSIM_NOISE 路径（显式设 NOISE 时报错）；SEFD = 单值全站共用或逗号列表按 .input datastream 序逐站取值（datasim -s 语义），flux 设了而 SEFD 没设时用默认 1000。**legacy**：源流量 / 站 SEFD（Jy），**两者都设**才启用 SNR 定标（tone 幅度 = 0.5·√(2F/(F+SEFD))、噪声 σ = 0.5·√(SEFD/(F+SEFD))，覆盖 FXSIM_NOISE） |
| `FXSIM_PCAL` | 关 | **新路径**：datasim `-p` 梳齿间隔（MHz）——基带 k·interval MHz（k < 复带宽/interval，datasim 循环边界）各注入一根梳齿，幅度 1/500（datasim applyphasecal），相位从 batch 起点连续累积（datasim 的 mod 周期 = 2·带宽个样本 = 恰 1 秒 = 整数梳齿周期，等价），并做帧边 taper（首 3 样本 0/×½/×⅘、末 3 样本 ×⅘/×½/×0，datasim applyphasecal 尾部）；注入点在延迟校正链之后（pcal 不随几何延迟移动，datasim/legacy 同构）。与 .input PHASE CAL 网格 tone 独立叠加 |
| `FXCORR_WORKDIR` | `.` | 项目根目录；`workdir` 位置参数优先 |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file）。
- 读 `workdir/<config_file>`（.input，非 MPI 构造），band 结构/采样率/PHASE CAL 网格全部来自 .input；common 的 specRes/numSamps 网格由**全站** band 布局推导（GCD 二分，找不到报错）。
- `common` 输出 `workdir/common/<batch_id>/`：meta.json（版本/dtype/specRes/numSamps/minStartFreq/块表/seed/谱线参数 line_freq_mhz/line_amp/line_rms（0 = 无）/status）+ data_XX.bin（每 0.5s 块一个，float32 频域 slice 顺序；先临时名再 rename，meta status=done 后 station 才可启动）。格式见 data-spec 5.8。
- `station` 输出 `workdir/raw/<station>/<station>_<batch_id>.vdif`（2bit VDIF，多 band 帧内样本 band 交织）；公共未就绪报错退出；延迟注入默认开（FXSIM_DELAY=0 关）。
- 默认模式 = 本机串行 `common` + 对 `.input` 全部 datastream 逐个 `station`；仅限单机，多节点须编排分别调用两入口。

程序内校验（不满足即报错退出）：实采样（complex 拒绝）、2bit（bytespersample 校验）、band 数 ∈ {1,2,4,8,16,32}；单 scan；batch 起点 subint 边界（1µs 容差）→ 整秒 snap → 帧边界；batch 时长非帧整数倍时文件生成到下一个帧边界取整（fxcorr-f 只读 batch 段）；common 的 band 频率须落在 specRes 网格（(freq−minStartFreq)/specRes 整数）；帧须为整数 slice（vpsamps % blksize == 0，即 1e6×specRes/fps 整数）、块总复样本须为帧复样本整数倍（nslices×blksize % vpsamps == 0，含末块）。

示例：

```bash
# 新路径：本机一键（common + 全站 station）
fxcorr-sim 60512_45000

# 新路径：分布式两段（编排调用；station 可多节点并行）
fxcorr-sim common 60512_45000 /data/proj
fxcorr-sim station 60512_45000 T1 /data/proj

# 跨站相干校验：两站噪声全关 → 输出逐位一致（单源公共信号）
FXSIM_NOISE=0 fxcorr-sim common 60512_45000
FXSIM_NOISE=0 fxcorr-sim station 60512_45000 T1
FXSIM_NOISE=0 fxcorr-sim station 60512_45000 T2

# P2：谱线（201.5 MHz，幅度 10，rms 3 网格点）+ 4 倍频谱分辨率
FXSIM_LINE=201.5,10,3 FXSIM_SPECRES=4 fxcorr-sim common 60512_45000

# P2：datasim 定标链（源 100 Jy；T1 SEFD 100、T2 SEFD 10000 按 datastream 序）
FXSIM_FLUX=100 FXSIM_SEFD=100,10000 fxcorr-sim station 60512_45000 T1

# P2：延迟注入默认开（.im 几何延迟完整链）；FXSIM_DELAY=0 回恒等链（字节回归）
FXSIM_DELAY=0 fxcorr-sim station 60512_45000 T1

# P4：datasim 梳齿（每 1 MHz 一根，1/500 幅度 + 帧边 taper；与 .input PHASE CAL 网格叠加）
FXSIM_PCAL=1 fxcorr-sim station 60512_45000 T1

# legacy 模式（字节对拍回归）：与 gen_test_vdif.py 逐字节一致
FXSIM_NOISE=0 fxcorr-sim station simcmp T1 . 1.5

# legacy：几何延迟注入（delay≠0 的 VLBI 观测仿真，tone 纯相位版）
FXSIM_DELAY=1 fxcorr-sim station 60512_45000 T1 . 1.5

# legacy：SNR 定标（源 100 Jy、站 SEFD 1000 Jy）
FXSIM_FLUX=100 FXSIM_SEFD=1000 fxcorr-sim station 60512_45000 T1 . 1.5
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
| `FXCORR_LOGLEVEL` | 未设（`info`） | stderr 诊断详略，取 `error`/`warn`/`info`/`verbose`/`debug`（无法识别时报错一行并回落 `info`）。`info` 出错误、告警与 `GAPCHECK summary` 等单行小结；`verbose` 追加逐条 gap/filler/boundary 明细（boundary 上限 40 条）。只影响输出，不影响产物 |

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
fxcorr-f 60512_45000 T1            # 当前目录为项目根
fxcorr-f 60512_45000 T1 /data/proj # 显式 workdir
FXCORR_WORKDIR=/data/proj fxcorr-f 60512_45000 T1  # 环境变量定义（位置参数优先于它）
```

---

## fxcorr-x

```
fxcorr-x <batch_id> [workdir]
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识 |
| `workdir` | 项目根目录，默认 `.`（环境变量 `FXCORR_WORKDIR` 亦可定义，位置参数优先） |

环境变量（DifxMessage 状态发送，algo-plan P1）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `DIFX_MESSAGE_GROUP` / `DIFX_MESSAGE_PORT` | 未设（静默） | host 模式组播目标（setup.bash 默认 224.2.2.1:50201）；未设时不发状态消息 |
| `FXCORR_RUN_MODE` | 未设 | `container` 时状态降级为落盘 `meta/difxmsg/`（见 data-spec 5.6） |
| `OMP_NUM_THREADS` | 未设（串行） | **V3 P3**：基线循环并行线程数（scratch 按线程私有化，结果与串行逐位一致）；未设 = 单线程（V2 行为不变） |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file / difx_dir）。
- 数据源 `workdir/fengine/<batch_id>/<station>/`（各站 band_XX.sp + autocorr.bin），station 列表 = .input 的全部 datastream（自动枚举，无站参数）。
- 输出目录由 **.input 的 OUTPUT FILENAME** 决定（SWIN 写盘语义，difx2fits 零改造前提），batch.json 的 difx_dir 仅为元数据；输出目录不存在时自动创建（mkdir -p OUTPUT FILENAME 目录）。
- **相位阵配置（.input `PHASED ARRAY TRUE` + `PHASED ARRAY CONFIG FILE`）时不写 SWIN**，改出 `beam/<batch_id>/beam.bin`（波束加权和，per ACC TIME 窗口记录；布局见 data-spec 5.5）。
- 状态消息节奏（mpiId = 0，manager 角色）：Starting（启动）→ 每积分写盘一条 Running（visibilityMJD = 积分中心、各站 band 平均 weight、jobstart/jobstop）→ Ending → Done；错误时 Alert + Aborting。与 mpifxcorr 基准逐字段对拍通过（visibilityMJD/weight 逐位一致）。相位阵模式只发 Starting/Ending/Done（无积分写盘）。

程序内校验：单 scan、单相位中心（脉冲星 binning 时多源免检）、intTime 为 subintNS 整数倍（相位阵时跳过）、batch 起点 subint 边界（1µs 容差，同 fxcorr-f）。

V1 边界：仅 fxcorr-f 的 .sp 输入；zoom band（P4a）、多相位中心（P4b）、脉冲星 binning（P4c）、交叉极化自相关（P7）、相位阵波束形成（P8）均已支持；PCAL/STA 文件生成不做。

示例：

```bash
mkdir -p vis/experiment.difx    # OUTPUT FILENAME 所在目录（自动创建亦可）
fxcorr-x 60512_45000
```

---

## 典型端到端流程

与 `fxcorr/CLAUDE.md` 测试流程一致（测试机 /root/fxcortest/）：

```bash
vex2difx config/test.v2d
difxcalc config/test.calc                     # 出 .input / .calc / .im
# 写 batches/<batch_id>.json（全字段一次写全，start_mjd 用精确 repr）
fxcorr-sim <batch_id> T1                      # 逐站生成 raw 数据
ln -sf raw/T1/T1_<batch_id>.vdif <DATA TABLE 文件名>
fxcorr-f <batch_id> T1                        # 逐站
mkdir -p vis/<experiment>.difx    # OUTPUT FILENAME 所在目录（自动创建亦可）
fxcorr-x <batch_id>
difx2fits <experiment>.input                 # 后处理按需
```

以上手工步骤由编排脚本自动化（`make_testdata.sh` / `run_bench.sh` / `run_batch.sh`，规格见 v1-plan 2.4）。
