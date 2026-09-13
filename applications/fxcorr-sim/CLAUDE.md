# fxcorr-sim 目录说明

仿真 VDIF 数据生成器：无 MPI 串行程序，按（batch、站）生成 `raw/<station>/<station>_<batch_id>.vdif`（数据规范见 `fxcorr/data-spec.md` 5.2）。读 `.input` 复用 fxcorrcommon 的 Configuration（非 MPI 构造）——与 fxcorr-f 同一份解析语义，band 结构/采样率/PHASE CAL 网格全部来自 `.input`，这是分批次对齐要求的硬理由。datasim 的替身（上游 datasim 因 subband.{h,cpp} 硬编码 IPP 无法 --noipp 构建）。

## 调用方式

```
fxcorr-sim <batch_id> <station> [workdir] [tone_mhz ...]
```

- 读 `workdir/batches/<batch_id>.json`（make_testdata.sh 预写），取 start_mjd / n_subints / config_file。
- tone 参数（基带频率 MHz）：0 个 = 无 tone；1 个 = 所有 band 同频率；nbands 个 = 逐 band。
- 环境变量：`FXSIM_NOISE`（噪声 σ，默认 0.02，0 关闭）、`FXSIM_SEED`（mt19937 种子，默认固定）、`FXCORR_WORKDIR`（项目根目录，`workdir` 位置参数优先）。
- PHASE CAL：`.input` 的 `PHASE CAL INT (MHZ)` > 0 时按 Configuration 的 tone 网格自动注入（幅度 0.7，避开 2bit 量化器电平陷阱——0.1 会被 rint(v×2)+2 量化吞掉），频率/计数与 fxcorr-f 提取端完全一致。

## 文件

| 文件 | 作用 |
|---|---|
| main.cpp | 参数/batch.json 解析、.input 接入（非 MPI 构造）、subint 对齐校验、帧时序校验、pcal tone 网格换算、驱动 |
| signalgen.{h,cpp} | 信号合成：tone（sin）+ 高斯噪声（Box-Muller）+ pcal tone 注入；2bit 量化（rint = Python round 的 round-half-even）+ 低位先打包 |
| vdifwriter.{h,cpp} | VDIF 帧封装：帧头 8 字（**vdifio/mark5access 字布局，2026-09-13 修正**）、帧时间戳/帧号按 batch 起点换算逐帧自增、2^n band 数校验 |

## 关键实现要点（易错，改前必读）

- **采样率 ≠ band 带宽**：采样率从 `.input` 的帧结构反推（`getFramePayloadBytes × 4 × getFramesPerSecond`，同 fxcorr-f datareader.cpp），不能取 `getFreqTableBandwidth`——4 MHz band 以 8 Ms/s 记录。
- **FREQ 表单位是 MHz**：`getFreqTableFreq/Bandwidth` 返回 MHz（`BW (MHZ)` atof 直存），换算 Hz 需 ×1e6；pcal tone 频率（`getDRecordedFreqPCalToneFreqHz`）是 Hz。
- **2bit 校验**：`getDBytesPerSampleNum/Denom` 含 band 数因子（num/denom = nbands/4），单 band 1/4、双 band 1/2，不能写死 1/4。
- **帧头字布局 = vdifio/mark5access 布局（2026-09-13 修正，P6 暴露）**：word0 [29:0] 秒 + bit30 legacymode=0（=1 会被 mark5access 判为 legacy 16 字节头）+ bit31 invalid=0；word1 [23:0] 帧号 + [29:24] ref epoch（0 = 2000.0，与 word0 自 2000 起的累计秒一致）；word2 [23:0] 帧长（8 字节单位）+ [31:29] version；word3 station/thread/nbits/iscomplex。原按 VDIF 2010 官方 spec 布局（word1 = 秒高位、word2 = epoch/frame、word3 = version/len）写，主路径 vdifmux 不查这些字段故从未暴露，但 mark5access（switched power 路径）解析失败——详见 `fxcorr/test/tcal/README.md`。
- **帧对齐三重校验**（数据连续性，分布生成的前提）：batch 起点须 subint 边界（1µs 容差，同 fxcorr-f）→ 帧边界整除（1µs 容差吸收 start_mjd 的 f64 表示误差）；**1µs 内接近整秒的起点 snap 到整秒**（帧号从 0 起，字节级对拍依赖的历史行为），其余任意帧边界起点帧号从秒内偏移起算（VDIF 帧号按秒回绕 0..fps-1，全局连续）。**batch 时长无须帧长整数倍（2026-09-12 放宽）**：文件生成到下一个帧边界取整，fxcorr-f 只读 batch 段——0.524288s subint（131.072 帧）的 test.input 配置由此可生成数据（对拍场景）。注意：帧对齐 subint（128ms）会触发 mpifxcorr vdifmux 帧号 bit7 错读（帧号 0x80..0xFF 丢帧，256 帧周期，见 impl-plan 2.4），对拍统一 test.input 配置。
- **逐字节对拍要求**（vs gen_test_vdif.py）：tone 相位按 `2.0*M_PI*tone_mhz*i/rate_mhz` 左结合浮点序（与 Python 版逐位一致）；量化用 `rint`（IEEE round-half-even = Python `round`）；帧头 8 字逐字照 Python 版。噪声关闭时同参数输出须逐字节一致。
- **多 band 帧布局**：帧内样本 band 交织（字节内 4 样本跨 band，band = 样本序 % nbands），对齐 mark5access `vdif_decode_2channel_2bit` 泛化；word3 的 nchan 字段 = log2(nbands)；payload = 8000×nbands。
- **nbands = band 数非频率数（2026-09-13 修复，P7 暴露）**：main.cpp 的 nbands 原用 `getDNumRecordedFreqs`（不同频率数）——dual-pol 同频率（R/L 同 200MHz）时 freq 数=1 而 band 数=2，2bit 校验与 tone/pcal 网格全部按 band 语义使用 → 校验误报 "2-bit samples only"。已改 `getDNumRecordedBands`（多频场景两值相等、行为不变；单 band 位序对拍 BYTE-IDENTICAL 回归佐证）。

## V1 边界

- 实采样（complex 报错）、2bit、各 band 同采样率（VDIF 帧约束）、band 数 ∈ {1,2,4,8,16,32}。
- 单 scan；帧 payload 取 .input 的 `getFramePayloadBytes`（如 8000，4ms @ 8Ms/s）。

## 构建与注册

- 模板照 fxcorr-f：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f + vdifio——vdifio 仅用 vdifio.h 的 vdif_header 位域结构与 getVDIFFrameBytes() static inline，不链接其库）。
- install-difx 注册 4 处（components 字典、setNormalComponentsFalse、apptargets dompicxx=True、--doonly 帮助），照 fxcorr-f。

## 测试（已通过，测试机 /root/fxcortest/）

- **位序逐字节对拍**：`FXSIM_NOISE=0 fxcorr-sim simcmp T1 . 1.5` vs `gen_test_vdif.py /tmp/ref.vdif 4.096 1.5 8`，1024 帧 8,224,768 字节 **BYTE-IDENTICAL**（对拍专用 config/test-sim.input：SUBINT 128000000 = 125×1.024ms FFT 块 = 32 帧；batch.json start_mjd 58948.291666666664、n_subints 32）。此对拍同时发现并修复了 gen_test_vdif.py 的帧号公式 bug（原 `n % 8000000 // 32000` 恒为 0，已改 `n % fps`）。整秒 snap 放宽为帧边界后此对拍回归仍 BYTE-IDENTICAL（起点整秒时帧号从 0 起，输出不变）。
- **非整秒起点**（多 batch 连续切分的前提，2026-09-12 放宽整秒 snap 后验证）：起点 25200.128s（batch 起点 = scan 起点 + 1.024s，128ms 帧网格）→ 第一帧 epoch 639730800（VDIF 2000.0 基准）、帧号 32 起（128ms/4ms），fxcorr-f/x 全链路跑通、SWIN 6 条记录（2 积分 × 3）。
- **全链路对拍**（单 band，原 config/test.input）：4.096s 数据 → fxcorr-f ×2 站 → fxcorr-x（4 subints 2.097s 对拍段）→ 与 mpifxcorr（EXECUTE TIME 2）cmp_swin.py **6/6 记录全等**。对拍段须留 ≥1 subint 数据余量（mpifxcorr vdifmux 滞后）。
- **pcal 链路**（首次验证）：PHASE CAL INT 1MHz → fxcorr-sim 注入 → fxcorr-f 出 pcal.bin，4 tones（201-204MHz）全部检出、subint 间相位稳定、Nyquist 边缘 tone 折叠纯实。
- **多 band**：test2b.vex/v2d（2 band 200/205MHz，见 fxcorr/test/）→ 2 band 数据（16032B 帧）→ fxcorr-f 出 band_00/01.sp、autocorr 峰 chan 768/512（=1.5/1.0MHz tone）✓ → fxcorr-x 144 条 SWIN（24 积分×3 基线×2 band，每 band 独立记录 = DiFX SWIN 语义）、可见度峰落位正确 ✓。**mpifxcorr 读 2 band 样本交织 VDIF 不可用**（2026-09-13 P7 重测定根因）：test2b.vex 的 $TRACKS 帧长 8032→16032 修正后 vdifmux 帧长警告清零、SWIN 头字段正常，但 vdiffile.cpp:538-550 对 nrecordedbands>nthreads 走 corner-turn 路径（setvdifmuxinputchannels(2) 输出 32032B 帧），与 mark5access 的 2channel 交织解码期望不匹配，仍只有首积分有效（weight~0.003）——上游此路径本身未经考验，非本程序问题；2 band 对拍以物理验证（tone 峰落位）与 dual-pol 自洽验证（crosspol/README.md）为准。
- difx2fits 对 simcmp SWIN 出 FITS ✓（全链路闭环）。

## 相关

- 测试资产与对拍流程：`fxcorr/CLAUDE.md`；数据规范：`fxcorr/data-spec.md` 5.2/12。
- 编排（make_testdata.sh/run_batch.sh 调用本程序）：`fxcorr/impl-plan.md` 2.4。
