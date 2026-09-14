# fxcorr-sim 目录说明

仿真 VDIF 数据生成器：无 MPI 串行程序，单二进制三入口（`common` 生成共享公共信号 / `station` 生成单站 VDIF / 无子命令本机串行）。架构设计（分布式形态、公共信号模型、阶段 P0-P3）见 `fxcorr/fxcorr-sim-arch.md`，本文是该目录的操作说明。datasim 的替身（上游 datasim 因 subband.{h,cpp} 硬编码 IPP 无法 --noipp 构建）。

**实施状态（2026-09-14）**：新架构 P0 已完成并验证（三入口、频域公共信号、station 新路径、legacy 挂接全过）。旧 4 参调用（`fxcorr-sim <batch_id> <station> [workdir] [tone_mhz...]`）已废止，报错提示改用 station 子命令。P1（共享存储多节点编排）/P2（SEFD 定标、延迟注入完整链、谱线+specres）待实施。

## 调用方式（目标状态）

```
fxcorr-sim common  <batch_id> [workdir]              # 只生成公共信号 → common/<batch_id>/
fxcorr-sim station <batch_id> <station> [workdir] [tone_mhz ...]   # 读公共信号，生成一个站
fxcorr-sim         <batch_id> [workdir]              # 默认：本机串行 common + 全站 station
```

- workdir 语义沿用：位置参数 > `FXCORR_WORKDIR` > `.`；`batches/<batch_id>.json`（编排预写）取 start_mjd / n_subints / config_file；.input 复用 fxcorrcommon 的 Configuration（非 MPI 构造），与 fxcorr-f 同一份解析语义。
- **legacy 模式**：station 子命令带 tone_mhz 位置参数 = 完全旧时域合成路径（tone/pcal/FXSIM_DELAY/FXSIM_FLUX/FXSIM_SEFD/FXSIM_ADAPTIVE 全部原样），供 simcmp 字节对拍回归；旧路径不再接新特性。
- 环境变量分工：
  - 新路径：`FXSIM_SEED`（公共种子，common 与 station 共用）、`FXSIM_NOISE`（station 端噪声 σ，默认 0.02，0 关闭）、`FXSIM_ADAPTIVE`（station 端量化，默认关）、`FXSIM_SPECRES`（specRes 缩放，P2）、`FXSIM_LINE`（谱线 freq,amp,rms，P2）。
  - legacy 路径：`FXSIM_NOISE` / `FXSIM_SEED` / `FXSIM_DELAY` / `FXSIM_FLUX`+`FXSIM_SEFD` / `FXSIM_ADAPTIVE`（语义与现有文档一致，见 usage.md）。
- `station` 模式公共未就绪（`common/<batch_id>/meta.json` 缺失或 status≠done）报错退出。
- PHASE CAL：`.input` 的 `PHASE CAL INT (MHZ)` > 0 时 station 端注入（新路径 P2；legacy 路径已实现，幅度 0.7、Configuration tone 网格，频率/计数与 fxcorr-f 提取端完全一致）。

## 文件

| 文件 | 作用 |
|---|---|
| main.cpp | 三入口分派（common/station/默认串行）、批处理上下文与站校验（setupStation，两路径共用）、旧 4 参调用报错提示 |
| commonsignal.{h,cpp} | 公共信号生成与文件读写：deriveGrid（specRes/numSamps 网格，datasim getSpecRes 移植+修 bug）、generate（gencplx 语义确定性频谱流、0.5s 块 .tmp+rename 写盘、meta.json running→done）、Reader（meta 校验 + 按块流式读） |
| signalgen.{h,cpp} | 两层：SignalGen（legacy 时域合成，原样保留，字节对拍路径）；FreqStationGen（station 新路径频域链：切频段→站噪声→归一化→Ormsby→块 IDFT→帧 DFT/DC 置零/扩展/2N IDFT 复转实→量化打包；fracsample/条纹旋转留 P2 插口；站噪声种子 = FNV-1a(station) 派生） |
| vdifwriter.{h,cpp} | VDIF 帧封装：帧头 8 字（**vdifio/mark5access 字布局，2026-09-13 修正**）、帧时间戳/帧号按 batch 起点换算逐帧自增、2^n band 数校验（不动） |

## 关键实现要点（易错，改前必读）

现有坑（legacy 路径与打包层共同）：

- **采样率 ≠ band 带宽**：采样率从 `.input` 的帧结构反推（`getFramePayloadBytes × 4 × getFramesPerSecond`，同 fxcorr-f datareader.cpp），不能取 `getFreqTableBandwidth`——4 MHz band 以 8 Ms/s 记录。
- **FREQ 表单位是 MHz**：`getFreqTableFreq/Bandwidth` 返回 MHz（`BW (MHZ)` atof 直存），换算 Hz 需 ×1e6；pcal tone 频率（`getDRecordedFreqPCalToneFreqHz`）是 Hz。
- **2bit 校验**：`getDBytesPerSampleNum/Denom` 含 band 数因子（num/denom = nbands/4），单 band 1/4、双 band 1/2，不能写死 1/4。
- **帧头字布局 = vdifio/mark5access 布局（2026-09-13 修正，P6 暴露）**：word0 [29:0] 秒 + bit30 legacymode=0（=1 会被 mark5access 判为 legacy 16 字节头）+ bit31 invalid=0；word1 [23:0] 帧号 + [29:24] ref epoch（0 = 2000.0，与 word0 自 2000 起的累计秒一致）；word2 [23:0] 帧长（8 字节单位）+ [31:29] version；word3 station/thread/nbits/iscomplex。原按 VDIF 2010 官方 spec 布局（word1 = 秒高位、word2 = epoch/frame、word3 = version/len）写，主路径 vdifmux 不查这些字段故从未暴露，但 mark5access（switched power 路径）解析失败——详见 `fxcorr/test/tcal/README.md`。
- **帧对齐三重校验**（数据连续性，分布生成的前提）：batch 起点须 subint 边界（1µs 容差，同 fxcorr-f）→ 帧边界整除（1µs 容差吸收 start_mjd 的 f64 表示误差）；**1µs 内接近整秒的起点 snap 到整秒**（帧号从 0 起，字节级对拍依赖的历史行为），其余任意帧边界起点帧号从秒内偏移起算（VDIF 帧号按秒回绕 0..fps-1，全局连续）。**batch 时长无须帧长整数倍**：文件生成到下一个帧边界取整，fxcorr-f 只读 batch 段。帧对齐 subint（128ms）会触发 mpifxcorr vdifmux 帧号 bit7 错读，对拍统一 test.input 配置（0.524288s subint）。
- **逐字节对拍要求**（vs gen_test_vdif.py）：tone 相位按 `2.0*M_PI*tone_mhz*i/rate_mhz` 左结合浮点序（与 Python 版逐位一致）；量化用 `rint`（IEEE round-half-even = Python `round`）；帧头 8 字逐字照 Python 版。噪声关闭时同参数输出须逐字节一致。
- **多 band 帧布局**：帧内样本 band 交织（字节内 4 样本跨 band，band = 样本序 % nbands），对齐 mark5access `vdif_decode_2channel_2bit` 泛化；word3 的 nchan 字段 = log2(nbands)；payload = 8000×nbands。
- **nbands = band 数非频率数（2026-09-13 修复，P7 暴露）**：main.cpp 的 nbands 原用 `getDNumRecordedFreqs`（不同频率数）——dual-pol 同频率（R/L 同 200MHz）时 freq 数=1 而 band 数=2，2bit 校验与 tone/pcal 网格全部按 band 语义使用 → 校验误报。已改 `getDNumRecordedBands`。

频域链要点（新路径，照 datasim 移植时）：

- **specRes 网格**：全站 band 频率差/带宽的 GCD，0.5MHz 起二分到 1/2^10，找不到报错退出（datasim getSpecRes）；`numSamps = maxChanFreq/specRes`（全站 band 覆盖跨度）、`stime = 1/specRes` µs；station 端 startIdx = (band 频率 − minStartFreq)/specRes、blksize = bw/specRes（非整数报错）。
- **确定性分层**：公共信号只与 (seed, batch) 有关、与站无关；站噪声种子 = f(seed, station) 派生，与公共种子分离——混用会让站噪声破坏跨站相干。
- **DC/Nyquist 置零**（datasim 语义）：fabricatedata 后 DC 须为 0（assert）；帧校正链里 procbuffreq[0] 置零；Hermitian 扩展时 DC 与 Nyquist 置零。
- **DFT 规格映射**：IPP `IPP_FFT_DIV_INV_BY_N`（正变换除 N）+ `ippsDFTInv`（逆变换不除）→ fftwf 正变换后手动 ×1/N、逆变换不缩放，保证 DFT→IDFT 恒等；块生成的小 blksize IDFT 与帧级三趟 FFT（vpsamps/vpsamps/2·vpsamps）共用 plan 复用。
- **复转实**：Hermitian 扩展 2N 点 IDFT 后虚部须 ≈0（assert < EPSILON），取实部出实数样本——复数基带（vpsamps 样本/帧）到实数（2·vpsamps 样本/帧）的 2 倍过采样即在此。
- **延迟注入单位**（P2）：τ 用秒制直接乘 f_Hz（2π·f·(delay+rate·t)），datasim 的 µs/秒混用（initSubbands 里 tempcoeffs[1]*1e-6 当 offsettime）不照抄；相位取小数部分（fraction_of）减小大相位浮点误差。procptr 整数移位 + d_shift 累计照抄 updatevalues（fracerr 超 ±0.5 采样时间时 procptr ±1）。
- **procptr 漂移限制**：复基带缓冲双 0.5s 块（1s 长），delay 漂移须远小于块长（现实场景 µs/s 级，datasim 同限制）。
- **量化**：datasim 的 3 电平 ±thresh×sign 映射**不照抄**（实测弱信号 F=1 下比四电平 rint 效率低 4.5 倍），移植的是门限自适应（FXSIM_ADAPTIVE，d_tmul 语义）+ 固定四电平 rint；四电平码序与物理电平反相 = VDIF 惯例。

## datasim 特性差距（2026-09-14 定稿，架构归属版）

定位差异：datasim 仿真「可相关出条纹的 VLBI 观测」（科学信号 + 几何模型注入）；fxcorr-sim 新架构以频域公共信号 + 分层注入对齐此语义，落盘形态按 (batch, station) 任务模型重排。特性归属（datasim → fxcorr-sim）：

| datasim 特性 | 归属 | 状态 |
|---|---|---|
| 公共频域信号 + 子带切分（跨站相干来源） | common 端（频域 S 落盘共享） | **P0 已完成**（commonsignal） |
| 站噪声 + SEFD/通量定标（fabricatedata） | station 端 | **σ 语义已完成**（FreqStationGen，scale = 0.5/(√(1+σ²)·√blksize)）→ P2 SEFD 定标 |
| 几何延迟/条纹注入（procptr+fracsample+条纹旋转） | station 端 | P2（tone 纯相位版已实现于 legacy FXSIM_DELAY） |
| 谱线 -l（gengaussianfilter） | common 端 | P2 |
| specres -r（specRes 缩放） | common 端 | P2 |
| pcal 注入 | station 端 | legacy 已实现（.input 驱动）；新路径 P2 |
| 量化阈值自适应（quantize d_tmul） | station 端打包层 | 已实现（FXSIM_ADAPTIVE，四电平版） |
| 测试模式 -t / MPI 并行 / 多站一次生成+zipper/cat | — | 等价覆盖：batch.json 定时长、batch 编排分片、(batch,station) 任务 + band 交织帧直接生成 |
| 依赖（GSL/IPP/MPI） | — | mt19937+Box-Muller / fftw3f / 无 MPI（fxcorr-sim 已链接 fftw3f，零新依赖） |

历史分析（A/B 类分类、方案 A 修上游 datasim 的讨论）见 fxcorr-sim-arch.md 第 10 节与 git 历史，不再维护两套表述。

## V1 边界

- 实采样（complex 报错）、2bit、各 band 同采样率（VDIF 帧约束）、band 数 ∈ {1,2,4,8,16,32}。
- 单 scan；帧 payload 取 .input 的 `getFramePayloadBytes`（如 8000，4ms @ 8Ms/s）。
- 新路径现状：无延迟注入（帧校正链插口已留）、无 SEFD 定标（FXSIM_NOISE σ 语义）、无谱线/specres（FXSIM_LINE/FXSIM_SPECRES 未实现）——这些在 P2。帧须为整数 slice（vpsamps % blksize == 0，即 1e6×specRes/fps 整数）、0.5s 块须为整数帧（fps 偶数），不满足报错。

## 构建与注册

- 模板照 fxcorr-f：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f + vdifio——vdifio 仅用 vdifio.h 的 vdif_header 位域结构与 getVDIFFrameBytes() static inline，不链接其库）。
- install-difx 注册 4 处（components 字典、setNormalComponentsFalse、apptargets dompicxx=True、--doonly 帮助），照 fxcorr-f。

## 测试（测试机 /root/fxcortest/）

已通过（legacy 路径）：

- **延迟注入三态验证**（2026-09-14，p11 对跖点配置，Δτ≈22.4ms、Δrate≈0.46µs/s、tone 1.5MHz @ 8Ms/s）：`FXSIM_DELAY=1 fxcorr-sim` 注入 +2π·f_RF·τ(t) → fxcorr-f/x 全链 → SWIN 互相关（bl 258）三态行为：都不注入 tone 可见度 ≈0；**都注入 tone 峰 13607、相位 0.00-0.02°（两积分间稳定）**；只 T1 注入 → 崩回噪声级（amp 2-52）——注入延迟与 f/x 侧 .calc 模型精确抵消、符号正确。weight 分布与无注入一致（0.989/1.0/0.993，P11 跳块语义不变）。无注入字节对拍回归 BYTE-IDENTICAL。
- **SNR 定标验证**（2026-09-14，cmp5 零基线）：两站 FXSIM_FLUX=100/FXSIM_SEFD=1000 → SWIN 互相关 tone 峰 441.3；T2 改 SEFD=4000 → 互相关 228.8（实测比 1.929 vs 理论 √(4100/1100)=1.931）、T1 站内不变 441.2、T2 站内 119.5（∝1/S=0.268 吻合）——互相关幅度 ∝ 1/√(S1·S2)、站内 ∝ 1/S 定量成立。
- **自适应量化验证**（2026-09-14）：σ=1.0 纯噪声电平码直方图——固定门限 40% 饱和（码序反相 = VDIF 惯例，与理论逐位吻合）；自适应开启 → thresh 收敛到数据 rms（实测 1.000）、rms 归一健康分布。datasim 三电平映射实测更差（3.6 vs 7.6）故未采用。
- **vex/v2d 生成脚本验证**（2026-09-14）：`fxcorr/test/gen_vex_v2d.sh test-gen <dir>`（CALC=1 走全链）→ .vex/.v2d/threads → vex2difx → .input/.calc → difxcalc → .im（DELAY 多项式生成，单站几何延迟 11.2ms）→ fxcorr-sim 两站 tone → fxcorr-f ×2 → fxcorr-x → SWIN 写出（全链路闭环）。
- **位序逐字节对拍**：`FXSIM_NOISE=0 fxcorr-sim simcmp T1 . 1.5` vs `gen_test_vdif.py /tmp/ref.vdif 4.096 1.5 8`，1024 帧 8,224,768 字节 **BYTE-IDENTICAL**（对拍专用 config/test-sim.input；此对拍同时修复 gen_test_vdif.py 帧号公式 bug）。
- **非整秒起点**（多 batch 连续切分前提）：起点 25200.128s → 第一帧 epoch 639730800、帧号 32 起，f/x 全链路跑通、SWIN 6 条记录。
- **全链路对拍**（单 band，config/test.input）：4.096s 数据 → f ×2 站 → x（4 subints 2.097s 对拍段）→ 与 mpifxcorr cmp_swin.py **6/6 记录全等**（对拍段须留 ≥1 subint 数据余量，mpifxcorr vdifmux 滞后）。
- **pcal 链路**：PHASE CAL INT 1MHz → 4 tones（201-204MHz）全部检出、subint 间相位稳定、Nyquist 边缘 tone 折叠纯实。
- **多 band**：test2b（2 band 200/205MHz）→ fxcorr-f 出 band_00/01.sp、autocorr 峰 chan 768/512 ✓ → fxcorr-x 144 条 SWIN、可见度峰落位正确 ✓。mpifxcorr 读 2 band VDIF 不可用（vdiffile.cpp corner-turn 路径 vs mark5access 2channel 交织解码不匹配，上游路径未考验），2 band 对拍以物理验证为准。
- difx2fits 对 simcmp SWIN 出 FITS ✓。

P0 已验证（2026-09-14，测试机 /root/fxcortest/simp0/）：

- **legacy 字节对拍回归**：CLI 改三入口后 `FXSIM_NOISE=0 fxcorr-sim station simcmp T1 . 1.5` vs `gen_test_vdif.py /tmp/ref.vdif 4.096 1.5 8`，1024 帧 8,224,768 字节 **BYTE-IDENTICAL**（对拍配置 test-sim-**nopcal**.input——test-sim.input 带 PHASE CAL INT 1 会注入 pcal，参考文件无 pcal）。
- **common 生成**：`fxcorr-sim common simcmp .` → common/simcmp/ 9 块（8×16MB + 末块 3,072,000B 截断）、meta.json status=done、specRes=0.5MHz/numsamps=8/minStartFreq=200 与手算一致；.tmp+rename 完成可见性生效。
- **跨站相干**：两站 `FXSIM_NOISE=0 fxcorr-sim station simcmp T1/T2 .` 输出 1024 帧**逐位一致**（单源公共信号天然保证）；`FXSIM_NOISE=0.02` 两站数据自 byte 34 起不同（站噪声独立注入、帧头一致）。
- **相干成分定量**：σ=1.0 两站 VDIF 解码后零滞后相关系数 0.444（理论 1/(1+σ²)=0.5，2bit 量化 + Ormsby 边缘损失，方向量级吻合）；量化健康（输出功率 1.24、无饱和）。
- **新路径全链路**：common → 两站 station（FXSIM_NOISE=0.02）→ fxcorr-f ×2 → fxcorr-x → SWIN 6 记录（2 积分 × 3 基线）、weight 0.989/1.0、可见度峰 ~4450 落自相关通道（σ=0.02 下公共信号主导，互相关≈自相关水平）。
- **make_testdata.sh 两段式**：无 tone 自动 `common` 一次 + 逐站 `station`；带 tone 走 legacy 逐站（无 common）——两分支均实测。
- 坑：difxcalc 生成的 .input 里 OUTPUT FILENAME 可能是绝对路径（如 cmp5 资产 `/root/fxcortest/cmp5/config/test.difx`），复制目录做新验证时须 sed 改相对路径，否则 fxcorr-x 写到原目录。

## 相关

- 架构设计（分布式形态/公共信号模型/阶段）：`fxcorr/fxcorr-sim-arch.md`
- 命令行手册：`fxcorr/usage.md`；数据规范（common/ 产物格式）：`fxcorr/data-spec.md` 5.8
- 测试资产与对拍流程：`fxcorr/CLAUDE.md`；编排（make_testdata.sh/run_batch.sh 两段式调用）：`fxcorr/impl-plan.md` 2.4
