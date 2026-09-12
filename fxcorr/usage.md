# fxcorr 工具命令行手册

覆盖三个改造应用的命令行接口：`fxcorr-sim`（仿真数据生成器）、`fxcorr-f`（Station-based）、`fxcorr-x`（Baseline-based）。目录布局与 batch.json 格式见 `data-spec.md`。

## 通用约定

- 三工具均**无 MPI、串行**，通过目录接口衔接。
- `workdir` 参数省略时默认当前目录（`.`）；所有相对路径（batches、.input、raw、fengine、vis）均相对 `workdir` 解释。
- batch.json 位于 `workdir/batches/<batch_id>.json`（单文件全字段），由编排脚本预写，工具只读不回写（位置语义见 data-spec 5.3）。
- 任务粒度：f 任务 = (batch_id, station)（fxcorr-f 的 station 参数即该维度）；x 任务 = (batch_id, 站组对)，V1 全站一组 = 全基线，多子集并行属 V2（data-spec 第 6 / 12 节）。
- 错误行为：参数不足或校验失败时打印原因到 stderr 并以非 0 退出；成功退出 0。
- 前置安装：各工具与 `fxcorrcommon` 库，构建见 `build.md`。

---

## fxcorr-sim

```
fxcorr-sim <batch_id> <station> [workdir] [tone_mhz ...]
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识（`60512_45000` 或 `20260908_123000`），用于读 batch.json 与输出命名 |
| `station` | 站名（如 `T1`），输出目录 `raw/<station>/` 与文件名前缀 |
| `workdir` | 项目根目录，默认 `.` |
| `tone_mhz ...` | 基带 tone 频率（MHz，0 个 = 无 tone；1 个 = 所有 band 同频；nbands 个 = 逐 band） |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `FXSIM_NOISE` | `0.02` | 高斯噪声 σ；`0` 关闭（配合固定 seed 可逐字节复现） |
| `FXSIM_SEED` | 固定值 | mt19937 种子；默认固定保证可复现 |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file）。
- 读 `workdir/<config_file>`（.input，非 MPI 构造），band 结构/采样率/PHASE CAL 网格全部来自 .input；`PHASE CAL INT (MHZ)` > 0 时自动注入 pcal tone（幅度 0.1）。
- 输出 `workdir/raw/<station>/<station>_<batch_id>.vdif`（2bit VDIF，多 band 帧内样本 band 交织）。

程序内校验（不满足即报错退出）：实采样（complex 拒绝）、2bit（bytespersample 校验）、band 数 ∈ {1,2,4,8,16,32}；batch 起点 subint 边界（1µs 容差）→ 整秒 snap → 帧边界；batch 时长为整帧数。

示例：

```bash
# 无 tone，纯噪声（可复现：噪声 0.02 + 默认种子）
fxcorr-sim 60512_45000 T1

# 各 band 同频 tone 1.5 MHz、噪声关闭（与 gen_test_vdif.py 逐字节对拍场景）
FXSIM_NOISE=0 fxcorr-sim simcmp T1 . 1.5

# 逐 band tone（2 band：200/205 MHz band 各 1.5 MHz）
fxcorr-sim 60512_45000 T1 . 1.5 1.5
```

---

## fxcorr-f

```
fxcorr-f <batch_id> <station> [workdir]
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识 |
| `station` | 站名（须在 .input 的 DATA TABLE / datastream 中） |
| `workdir` | 项目根目录，默认 `.` |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file）。
- 读 `workdir/<config_file>`（.input）；Model 由 .calc 内建，无 .im 依赖。
- 原始数据文件路径直接取自 .input 的 DATA TABLE（相对进程 cwd，即 workdir）。
- 输出 `workdir/fengine/<batch_id>/<station>/`（自动创建）：`band_XX.sp`、`pcal.bin`（配置了 phasecal 时）、`autocorr.bin`（二进制布局见 data-spec 5.3）。

程序内校验：batch 起点须在 subint 边界（1µs 容差，吸收 start_mjd 的 f64 表示误差）——batch.json 的 start_mjd 建议写精确 repr（如 `58948.291666666664`），否则报错退出。

V1 边界：本地 VDIF（其他格式报错）、单 mux thread、单 scan、无 zoom band 落盘。

示例：

```bash
fxcorr-f 60512_45000 T1            # 当前目录为项目根
fxcorr-f 60512_45000 T1 /data/proj # 显式 workdir
```

---

## fxcorr-x

```
fxcorr-x <batch_id> [workdir]
```

| 参数 | 说明 |
|---|---|
| `batch_id` | 批量标识 |
| `workdir` | 项目根目录，默认 `.` |

输入输出：

- 读 `workdir/batches/<batch_id>.json`（取 start_mjd / n_subints / config_file / difx_dir）。
- 数据源 `workdir/fengine/<batch_id>/<station>/`（各站 band_XX.sp + autocorr.bin），station 列表 = .input 的全部 datastream（自动枚举，无站参数）。
- 输出目录由 **.input 的 OUTPUT FILENAME** 决定（SWIN 写盘语义，difx2fits 零改造前提），batch.json 的 difx_dir 仅为元数据；输出目录不存在时自动创建（mkdir -p OUTPUT FILENAME 目录）。

程序内校验：单 scan、单相位中心、maxproducts ≤ 2、intTime 为 subintNS 整数倍、batch 起点 subint 边界（1µs 容差，同 fxcorr-f）。

V1 边界：仅 fxcorr-f 的 .sp 输入（无 zoom 切片）；多相位中心/脉冲星/PCAL 文件生成不做。

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

以上手工步骤由编排脚本自动化（`make_testdata.sh` / `run_batch.sh`，规格见 impl-plan 2.4）。
