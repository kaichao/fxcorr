# fxcorr 改造工作区

本目录是 fxcorr 改造的工作区：文档 + bash 编排脚本（**脚本直接放本目录，与 README / data-spec 平级，不再设 scripts/ 子目录**）+ test/ 测试资产。算法实现见 `applications/fxcorr-f`（已建成，见其 CLAUDE.md）、`applications/fxcorr-x`（已建成，见其 CLAUDE.md），仿真数据生成器见 `applications/fxcorr-sim`（已建成，见其 CLAUDE.md；验证档案见其 VERIFICATION.md），共享代码见 `libraries/fxcorrcommon`（已建成）。

## 文档分工

- **README.md**：需求（R1-R7）、总体架构、实现阶段（V1/V2/V3）、设计要点。改架构/需求时改它。
- **data-spec.md**：数据规范（版本 1.1）—— 目录布局、D1-D13 数据类型、模块 I/O、band_XX.sp / pcal.bin / autocorr.bin / SWIN 二进制格式、时间轴与通道/偏振映射、切批约束。**改数据接口/文件格式时必须先同步它**。
- **impl-plan.md**：V1 实施方案——组件源码清单、core.cpp 切分落点、datareader 改造、install-difx 注册、验收标准。改实施步骤时改它。
- **v2-plan.md**：V2 计划——定位（scalebox 编排外置）、镜像体系（fxcorr-builder/base/f/x/sim/difx-tools）、容器构建链、算法改进清单、验收标准。改 V2 范围或镜像设计时改它。
- **algo-plan.md**：V2 算法改进需求与设计——每项动机分类（功能未迁移/串行环境新变化）、要解决的问题、预期效果、设计要点、优先级（P0-P5）。改改进范围或设计时改它。
- **fxcorr-sim-arch.md**：fxcorr-sim 分布式架构——单二进制三入口（common/station/默认串行）、频域公共信号模型与数据量依据、一致性规则、datasim 特性差距、P0-P4 阶段。改 fxcorr-sim 架构或公共信号模型时改它。
- **usage.md**：三工具（fxcorr-sim / fxcorr-f / fxcorr-x）命令行手册——参数、环境变量、输入输出、程序内校验、示例。改工具命令行接口时改它。
- **build.md**：构建手册——集成构建（install-difx）与独立构建（单包 autotools）两条路径、依赖、测试机工作流。改构建体系时改它。

## 核心约定（源自 data-spec.md）

- **batch_id**：`60512_45000`（MJD+秒，推荐）或 `20260908_123000`（紧凑日期时间）；全局唯一，字符串排序即时间顺序；同一 batch 在 fengine/ 与 vis/ 用相同 batch_id。
- **目录**：`config/`（.vex/.v2d/.input/.calc/.im/.flag）、`batches/`（`<batch_id>.json`，D9 批量元数据，共享存储）、`common/<batch_id>/`（仿真公共信号 D15，fxcorr-sim common 一次生成、各站只读；≥16× 单站 2bit 数据量，batch 站任务全完成后可删）、`raw/<station>/`（TB 级原始基带）、`fengine/<batch_id>/<station>/`（band_XX.sp 复数频谱 + pcal.bin + autocorr.bin，二进制布局见 data-spec 5.3）、`vis/<experiment>.difx/`（SWIN 文件集，跨 batch 追加）、`product/`、`meta/`、`work/`（临时）。均不进 git。多节点存储归属（共享/本地）与计算本地化原则见 data-spec 第 1/2 节。
- **batch.json**（`batches/<batch_id>.json`，D9 全字段单文件）：batch_id / start_mjd / start_time / duration_sec / stations / station_groups（可选，空间切分组）/ baselines / config_file / calc_file / im_file / n_subints / subint_ns / integration_sec / n_channels / polarizations / difx_dir / status / 版本字段。编排脚本一次写全，三工具只读。
- **数据流**：vex2difx + difxcalc（实验级一次）→ [仿真分支：fxcorr-sim common（D15 公共信号，每 batch 一次）→ fxcorr-sim station × 各站（D7 VDIF）] → fxcorr-f × 各站（D3+D4+D6+D7 → D8+D9）→ fxcorr-x（D3+D4+D6+D8+D9 → D10+D9）→ difx2fits / difx2mark4（按需）。fxcorr-sim 架构（三入口/公共信号模型）见 fxcorr-sim-arch.md。
- **三个敲定决策**：UVW 由 fxcorr-x 读 .calc/.im 求值（D1）；可见度直出 SWIN、difx2fits 零改造（D2）；偏振是 band 属性、偏振组合在 x 侧按 BASELINE TABLE 选（D3）。V1 边界与实施步骤见 impl-plan.md。
- **SWIN 输出目录**：由 .input 的 OUTPUT FILENAME 决定（Visibility 写盘语义，difx2fits 零改造的前提），batch.json 的 difx_dir 仅为元数据。

## 本目录脚本（规格见 impl-plan.md 2.4）

| 脚本 | 作用 | 状态 |
|---|---|---|
| `make_testdata.sh` | 构建 data-spec 布局的标准测试数据（前处理 + 仿真 VDIF + batch.json），支持多 batch | 已实现 |
| `run_bench.sh` | difx 原命令基准：mpifxcorr 固化流程出基准 SWIN 供 cmp_swin.py 对拍 | 已实现 |
| `run_batch.sh` | fxcorr 流水线：前置校验对齐 → DATA TABLE 软链重指本 batch → 逐站 fxcorr-f → fxcorr-x → 更新 status/meta/batches.index | 已实现 |

`watch_and_dispatch.sh` 已砍（V1 静态数据集无轮询场景）；流式监视与多节点调度 V2 由 scalebox 承担，容器化同列 V2（scalebox Module 需容器镜像）。

调用示例：

```bash
./fxcorr/make_testdata.sh [workdir] [tone_mhz ...]   # -n N 连续 N 个 batch；-p P station 本地并行；--nodes "host:st1,st2" ssh 分发；FXSIM_NOISE/SEED/ADAPTIVE/SPECRES/LINE/FLUX/SEFD（DELAY 由程序默认开）、BATCH_NSUBINTS 环境变量
./fxcorr/run_bench.sh [workdir]            # 出基准 SWIN 到 bench/（对拍基准，NP 覆盖 mpirun 进程数）
./fxcorr/run_batch.sh 60512_45000         # fxcorr 流水线（batch.json 须已由 make_testdata.sh 写好）
```

**容器模式**（V2，验收 4/5 已过）：`FXCORR_RUN_MODE=container` 时两脚本内 `fxc` 封装按工具→镜像映射（fxcorr-f/x/sim、vex2difx/difxcalc/difx2fits→difx-tools）加 `docker run --rm -v $WORKDIR:$WORKDIR -w $(pwd)` 前缀，FXSIM_NOISE/SEED 透传；默认 host = 宿主直跑（V1 行为不变）。run_batch.sh 调用时 cwd 须在 workdir（软链相对解析）。

make_testdata.sh 实现要点（实测）：config 资产缺才复制 fxcorr/test/ 的 test.vex/test.v2d；**单 batch 用 difxcalc 原产物 test.input（0.524288s subint，6/6 对拍同配置），仅 `-n N` 多 batch sed 出 128ms SUBINT 变体**（test-sim.input，多 batch 连续切分起点须帧边界；128ms 帧对齐会触发 mpifxcorr vdifmux 帧号 bit7 错读，多 batch 不与 mpifxcorr 对拍）；batch.json 全字段一次写全（start_mjd 精确 repr）；`-n N` 时 n_subints 自动提升到每 batch 时长 ≥ 1s（batch_id 秒唯一）；软链指向最后 batch。**对拍 mpifxcorr 须 `FXSIM_NOISE=0`**（带噪 2bit 数据触发 vdifmux 读端错乱，见 memory）。**并行分发（P1，实测 p1reg）**：`-p P` xargs 本地并行、`--nodes "host:st1,st2"` ssh 远程（全路径 + `LD_LIBRARY_PATH=$DIFXROOT/lib`、BatchMode/accept-new、FXSIM_* 透传）；远程/本地产物 BYTE-IDENTICAL、失败传播非零退出、container 模式互斥；详见 impl-plan 2.4 实施记录。

run_bench.sh 实现要点（实测）：batch 定位走 DATA TABLE 软链 target 的 batch_id（fallback batches/ 最新 json）；EXECUTE TIME 截断 = `floor(initsec + (N−1)×intTime) + 1`（mpifxcorr 停写判定按积分起点、整秒字段，N = batch 时长/intTime 不整除即报错）；OUTPUT FILENAME sed 指 `bench/<exp>.difx`；mpirun 在 workdir 内跑（DATA TABLE 相对路径）、root 加 --allow-run-as-root、`LD_LIBRARY_PATH=$DIFXROOT/lib`；mpifxcorr 拒绝覆盖已有 SWIN（脚本先 rm -rf）。batch 时长 < 1s 无整秒解（mpifxcorr 多写 weight-0 积分），默认配置 2.097s 无碍。

run_batch.sh 实现要点：前置校验在脚本端 python 做（batch 起点 subint 边界 1µs 容差、时长 intTime 整数倍、intTime 为 subint 整数倍，语义同 fxcorr-f/x 程序内校验）；**DATA TABLE 软链每次重指本 batch 的 VDIF**（make_testdata.sh 多 batch 时软链停在最后 batch，跑其他 batch 必须重做；raw 数据不存在即报错）；站列表取 batch.json stations（缺失时从 .input DATASTREAM 解析）；status 流转 running→done/failed 写回 batch.json（json.dump 保字段序）；done 时追加 `meta/batches.index` 一行 `<batch_id>,done,<UTC 时间戳>`。

## test/ 测试资产

**目录组织**（2026-09-13 定）：根目录放共享/通用件（base 配置、造数、通用对拍工具，各特性检验都依赖）；每个特性一个独立子目录（`zoom/`、`pcal/`，后续 P4b/P4c/P6-P11 各建），放该特性的专属脚本/配置，**检验步骤与验证记录写在各特性目录的 `README.md`**。新特性资产一律进特性目录，不再平放根目录。

| 文件 | 作用 |
|---|---|
| `test.vex` | 上游 `tests/Synthetic/test-usb.vex` 原版（2 站 T1/T2、单 band 4MHz USB、2bit、2020y100d07h00m00s） |
| `test.v2d` | 配套 vex2difx 配置（antennas=T1,T2，tInt=1，nChan=4096） |
| `gen_test_vdif.py` | 生成 2bit 单 band VDIF 测试数据（datasim 因上游 IPP 依赖无法 --noipp 构建，此脚本替代；**低位先打包**对齐 mark5access 位序；fxcorr-sim 的位序逐字节对拍参照，对拍已验证 BYTE-IDENTICAL；帧号公式已修为 `n % fps`（原 `n % 8000000 // 32000` 恒为 0，对拍时发现）） |
| `test2b.vex` / `test2b.v2d` | 2 band 测试配置（test.vex 加 205MHz 第 2 band），fxcorr-sim 多 band 验证资产；**2026-09-13 修正 $TRACKS 帧长 8032→16032**（2 band VDIF 帧实长，原值让 vex2difx 生成错误 DATA FRAME SIZE、mpifxcorr vdifmux 帧长错乱） |
| `cmp_swin.py` | SWIN 逐记录比较（74 字节头 + 可见度复数），通用对拍工具（impl-plan 验收标准 2） |
| `pcal/test-pcal.vex` / `pcal/test-pcal.v2d` | 带 phasecal 的测试配置（PHASE_CAL_DETECT tone 列表 `2:3:4:5`、phaseCalInt=1 → .input 4 tones 201-204MHz），PCAL_*.pcal 对拍验证资产（algo-plan P0） |
| `pcal/README.md` | P0 检验资产用法 + 验证方法与结果记录 |
| `zoom/gen_test_zoom.py` | 从无 zoom 的 .input 生成 test-zoom.input + EXECUTE TIME 截断变体（P4a zoom 检验资产） |
| `zoom/cmp_swin_zoom.py` | 按 SWIN 头 freqindex 分拆多 nchan 记录的逐记录比较（zoom 对拍用，同文件多 freq 各不同 nchan） |
| `zoom/README.md` | P4a 检验步骤（完整可复现命令）+ 验收判据 + 验证记录 |
| `mpc/test-mpc.v2d` / `mpc/README.md` | P4b 多相位中心检验资产（addPhaseCentre=TEST2 走 vex2difx 原生链路）+ 检验步骤/验收判据/验证记录 |
| `pulsar/gen_test_pulsar.py` / `pulsar/README.md` | P4c 脉冲星 binning 检验资产（.input 变体 + pulsar config + 自造 tempo polyco，--scrunch/--negative-weight 变体）+ 检验步骤/验收判据/验证记录 |
| `tcal/test-tcal.v2d` / `tcal/README.md` | P6 SwitchedPower 检验资产（两站 tcalFreq=80 → .input TCAL FREQUENCY）+ 检验步骤/验收判据/验证记录（含生成器帧头两个 bug 的记录：legacy 位、vdifio/mark5access 字布局） |
| `crosspol/test-pols.vex` / `crosspol/test-pols.v2d` / `crosspol/README.md` | P7 交叉极化自相关检验资产（RCP+LCP 同 200MHz dual-pol → .input POL PRODUCTS 4 + WRITE AUTOCORRS）+ 检验步骤/验收判据/验证记录（含 test2b $TRACKS 帧长修复与 fxcorr-sim nbands 语义修复两个坑） |
| `phasearr/gen_test_phasearr.py` / `phasearr/cmp_beam.py` / `phasearr/README.md` | P8 相位阵检验资产（.input 变体 + 相位阵配置文件生成，SUBINT 改 numbufferedffts 整数倍过上游 accffts 校验；beam.bin 与手算加权和逐位核对）+ 检验步骤/验收判据/验证记录（含 getinputkeyval 第 20 列坑与早退分支决策记录；上游 mpifxcorr 相位阵死代码无法对拍） |
| `sta/sta_ctrl.c` / `sta/gen_test_sta.py` / `sta/cmp_sta.py` / `sta/README.md` | P9 STA/kurtosis 检验资产（sta_ctrl：difxmessage 控制消息发送 + BINARY_STA 组播抓包；CHANS TO AVG 4 变体覆盖 STA 频域平均分支；原始 record 流逐位对拍）+ 检验步骤/验收判据/验证记录（含 P1 cf32 stride bug 修复与基准控制消息时序坑） |
| `p10/gen_test_mk5b.py` / `gen_test_lba.py` / `gen_test_ivdif.py` / `gen_test_p10.py` / `verify_lba.py` / `p10/README.md` | P10 输入格式检验资产（Mk5B 10016 帧生成器、LBA 16 字节 ASCII 头+2bit 低位先生成器、fanout 多线程 VDIF 生成器、.input 变体、LBA 自洽验证脚本）+ 检验步骤/验收判据/验证记录（含 LBA 位序、VDIF word3 布局、EDV4 三坑等 bug 记录） |
| `p11/gen_test_p11.py` / `p11/README.md` | P11 reader 语义检验资产（TEST2 对跖点 → 几何 delay 11.2ms 的 test-delay.vex/v2d 生成器，触发 delay 重对齐跳块语义）+ 检验步骤/验收判据/验证记录（含 vex2difx 从 cwd 找 vex 的坑） |
| `gen_vex_v2d.sh` | vex/v2d 生成脚本（datasim scripts/genv2dvex.sh 的 fxcorr 版；test.vex/test.v2d 模板参数化：站坐标/源/时间/频率/帧长/EOP 环境变量或 obs_info 文件覆盖，`CALC=1` 续跑 vex2difx→difxcalc 链；已全链验证：生成→.input/.calc/.im→sim→f→x→SWIN，详见 applications/fxcorr-sim/CLAUDE.md） |
| `make_testdata.sh` | 数据构建脚本（已实现，见上方脚本表） |
| `testdata-min/` | 最小数据集（规划）：对拍最小子集 + sha256 入仓库，待 2 秒配置对拍实测干净后定 |

测试流程（测试机 /root/fxcortest/）：`vex2difx test.v2d` → `difxcalc test.calc`（出 .input/.calc/.im）→ 数据生成两种方式：**fxcorr-sim**（读 batch.json 生成 `raw/<station>/<station>_<batch_id>.vdif`，软链到 .input DATA TABLE 文件名）或 `gen_test_vdif.py TEST1.vdif 4 1.5 8`（对拍参照）→ 写 `batches/<batch_id>.json`（start_mjd 用精确 repr，否则 fxcorr-f 对齐校验报错）→ `fxcorr-f <batch_id> T1` / `T2` → `fxcorr-x <batch_id>` → `cmp_swin.py` 与 mpifxcorr 对拍。以上手工步骤已由 `make_testdata.sh` 自动化（tInt=0.25 变体 test-sim.input，SUBINT 128ms）。已验证（2026-09-12，fxcorr-sim 数据）：tone 峰落 1.5/1.0MHz 通道；SWIN 对拍 6/6 记录全等（可见度 <1e-6、weight 逐位一致）；difx2fits 出 FITS；pcal 链路 4 tones 检出；2 band（test2b）全链路跑通（mpifxcorr 读不了 2 band VDIF，2 band 对拍以物理验证为准，详见 applications/fxcorr-sim/CLAUDE.md）。对拍注意 mpifxcorr 的 mux 滞后（impl-plan 2.3 实施记录）；**对拍数据须 `FXSIM_NOISE=0` 生成**（带噪 2bit 数据触发 mpifxcorr vdifmux 读端错乱，0 积分输出）。

## 相关指引

- 拆分缝隙（core.cpp processdata）与可复用清单：`mpifxcorr/CLAUDE.md`
- 依赖库与 fxcorrcommon 样板：`libraries/CLAUDE.md`
- 构建注册（install-difx 4 处）：根 `CLAUDE.md`；构建流程（集成/独立两条路径）：`build.md`
- 工具命令行用法：`usage.md`
