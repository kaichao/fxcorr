# fxcorr-sim 目录说明

仿真 VDIF 数据生成器：无 MPI 串行程序，单二进制三入口（`common` 生成共享公共信号 / `station` 生成单站 VDIF / 无子命令本机串行）。datasim 的替身（上游 datasim 因 subband.{h,cpp} 硬编码 IPP 无法 --noipp 构建）。新架构 P0-P4 全部完成并验证（2026-09-14）。

本文是该目录的操作说明（源文件地图、关键实现要点、构建注册）。命令行与环境变量见 `fxcorr/usage.md`（fxcorr-sim 段），架构设计（分布式形态、公共信号模型、datasim 特性差距、阶段 P0-P4）见 `fxcorr/fxcorr-sim-arch.md`，验证记录见本目录 `VERIFICATION.md`，测试资产与对拍流程见 `fxcorr/CLAUDE.md` 与 `fxcorr/test/`。

## 文件

| 文件 | 作用 |
|---|---|
| main.cpp | 三入口分派（common/station/默认串行）、批处理上下文与站校验（setupStation，两路径共用）、FXSIM_* 解析（SPECRES/LINE 两入口共用、FLUX/SEFD 列表按 dsindex 取值、DELAY 开关、PCAL 梳齿）、旧 4 参调用报错提示 |
| commonsignal.{h,cpp} | 公共信号生成与文件读写：deriveGrid（specRes/numSamps 网格，datasim getSpecRes 移植+修 bug，FXSIM_SPECRES 缩放因子）、generate（gencplx 语义确定性频谱流 + FXSIM_LINE 谱线逐 slice 乘、0.5s 块 .tmp+rename 写盘、meta.json running→done 含谱线参数）、Reader（meta 校验 + 按块流式读） |
| signalgen.{h,cpp} | 两层：SignalGen（legacy 时域合成，原样保留，字节对拍路径）；FreqStationGen（station 新路径频域链：切频段→站噪声/SEFD 定标→归一化→Ormsby→块 IDFT（滚动缓冲）→帧 DFT/DC 置零/fracsample 校正/条纹旋转/扩展/2N IDFT 复转实→pcal 注入→量化打包；datasim updatevalues 延迟吸收；站噪声种子 = FNV-1a(station) 派生） |
| vdifwriter.{h,cpp} | VDIF 帧封装：帧头 8 字（**vdifio/mark5access 字布局，2026-09-13 修正**）、帧时间戳/帧号按 batch 起点换算逐帧自增、2^n band 数校验（不动） |
| VERIFICATION.md | 验证档案（legacy 路径 + P0-P4 全部验证记录与坑） |

## 关键实现要点（易错，改前必读）

现有坑（legacy 路径与打包层共同）：

- **采样率 ≠ band 带宽**：采样率从 `.input` 的帧结构反推（`getFramePayloadBytes × 4 × getFramesPerSecond`，同 fxcorr-f datareader.cpp），不能取 `getFreqTableBandwidth`——4 MHz band 以 8 Ms/s 记录。
- **getFramePayloadBytes 是帧总 payload（2026-09-14 test2b 暴露）**：含全部 band，非 per-band。setupStation 里 deriveFrame 后的 bytesperbandframe/nsampframe/ratehz 已统一 ÷nbands（vpsamps、VDIFWriter、legacy tone 相位、payloadbytes 计算全部是 per-band 语义）；doCommon 的局部 deriveFrame 只取 framens 不受影响。单 band 下总=per-band，此前从未暴露。
- **FREQ 表单位是 MHz**：`getFreqTableFreq/Bandwidth` 返回 MHz（`BW (MHZ)` atof 直存），换算 Hz 需 ×1e6；pcal tone 频率（`getDRecordedFreqPCalToneFreqHz`）是 Hz。
- **2bit 校验**：`getDBytesPerSampleNum/Denom` 含 band 数因子（num/denom = nbands/4），单 band 1/4、双 band 1/2，不能写死 1/4。
- **帧头字布局 = vdifio/mark5access 布局（2026-09-13 修正，P6 暴露）**：word0 [29:0] 秒 + bit30 legacymode=0（=1 会被 mark5access 判为 legacy 16 字节头）+ bit31 invalid=0；word1 [23:0] 帧号 + [29:24] ref epoch（0 = 2000.0，与 word0 自 2000 起的累计秒一致）；word2 [23:0] 帧长（8 字节单位）+ [31:29] version；word3 station/thread/nbits/iscomplex。原按 VDIF 2010 官方 spec 布局（word1 = 秒高位、word2 = epoch/frame、word3 = version/len）写，主路径 vdifmux 不查这些字段故从未暴露，但 mark5access（switched power 路径）解析失败——详见 `fxcorr/test/tcal/README.md`。
- **帧对齐三重校验**（数据连续性，分布生成的前提）：batch 起点须 subint 边界（1µs 容差，同 fxcorr-f）→ 帧边界整除（1µs 容差吸收 start_mjd 的 f64 表示误差）；**1µs 内接近整秒的起点 snap 到整秒**（帧号从 0 起，字节级对拍依赖的历史行为），其余任意帧边界起点帧号从秒内偏移起算（VDIF 帧号按秒回绕 0..fps-1，全局连续）。**batch 时长无须帧长整数倍**：文件生成到下一个帧边界取整，fxcorr-f 只读 batch 段。帧对齐 subint（128ms）会触发 mpifxcorr vdifmux 帧号 bit7 错读，对拍统一 test.input 配置（0.524288s subint）。
- **逐字节对拍要求**（vs gen_test_vdif.py）：tone 相位按 `2.0*M_PI*tone_mhz*i/rate_mhz` 左结合浮点序（与 Python 版逐位一致）；量化用 `rint`（IEEE round-half-even = Python `round`）；帧头 8 字逐字照 Python 版。噪声关闭时同参数输出须逐字节一致。
- **多 band 帧布局**：帧内样本 band 交织（字节内 4 样本跨 band，band = 样本序 % nbands），对齐 mark5access `vdif_decode_2channel_2bit` 泛化；word3 的 nchan 字段 = log2(nbands)；payload = 8000×nbands。
- **nbands = band 数非频率数（2026-09-13 修复，P7 暴露）**：main.cpp 的 nbands 原用 `getDNumRecordedFreqs`（不同频率数）——dual-pol 同频率（R/L 同 200MHz）时 freq 数=1 而 band 数=2，2bit 校验与 tone/pcal 网格全部按 band 语义使用 → 校验误报。已改 `getDNumRecordedBands`。

频域链要点（新路径，照 datasim 移植时）：

- **specRes 网格**：全站 band 频率差/带宽的 GCD，0.5MHz 起二分到 1/2^10，找不到报错退出（datasim getSpecRes）；`numSamps = (maxStartFreq+maxBW − minStartFreq)/specRes`（全站 band 实际跨度，**band 间有间隙也覆盖**——datasim 的 band0带宽×band数 假设连续、间隙布局会静默越界读公共信号，2026-09-14 修正不照抄）、`stime = 1/specRes` µs；station 端 startIdx = (band 频率 − minStartFreq)/specRes、blksize = bw/specRes（非整数报错）。
- **确定性分层**：公共信号只与 (seed, batch) 有关、与站无关；站噪声种子 = f(seed, station) 派生，与公共种子分离——混用会让站噪声破坏跨站相干。
- **DC/Nyquist 置零**（datasim 语义）：fabricatedata 后 DC 须为 0（assert）；帧校正链里 procbuffreq[0] 置零；Hermitian 扩展时 DC 与 Nyquist 置零。
- **DFT 规格映射**：IPP `IPP_FFT_DIV_INV_BY_N`（正变换除 N）+ `ippsDFTInv`（逆变换不除）→ fftwf 正变换后手动 ×1/N、逆变换不缩放，保证 DFT→IDFT 恒等；块生成的小 blksize IDFT 与帧级三趟 FFT（vpsamps/vpsamps/2·vpsamps）共用 plan 复用。
- **复转实**：Hermitian 扩展 2N 点 IDFT 后虚部须 ≈0（assert < EPSILON），取实部出实数样本——复数基带（vpsamps 样本/帧）到实数（2·vpsamps 样本/帧）的 2 倍过采样即在此。
- **pcal 注入（P4）**：注入点在 realc（2N IDFT 后、量化前）——datasim applyphasecal 的同构位置（processdata 完整校正链之后、packetize 之前），因此 pcal **不随几何延迟移动**（datasim/legacy 同构语义）。相位用实样本率 ratehz（= 2×复速率；默认 2×band 带宽 MHz，datasim 的 2·d_bandwidth 约定）从 batch 起点连续累积：datasim 的 `sidx % (2*d_bandwidth)` mod 周期 = 恰 1 秒 = 整数梳齿周期（interval/bandwidth 均整数 MHz 时），连续相位与 mod 逐位等价。梳齿数 = 复带宽 MHz/interval（datasim `d_bandwidth/pcalinterval` 循环边界，**不是**实样本率/interval——后者会多一倍梳齿、一半超 Nyquist）；k=0 梳齿为 DC（sin(0)=0 恒零，datasim 同样照算）。帧边 taper 仅在梳齿（FXSIM_PCAL）启用时做（datasim applyphasecal 尾部：首 3 样本 0/×½/×⅘、末 3 样本 ×⅘/×½/×0，作用于整帧信号非仅 pcal）；网格 tone 无 taper（legacy 语义）。
- **延迟注入单位**（P2 已实现）：τ 用秒制直接乘 f_Hz（2π·f·(delay+rate·t)），datasim 的 µs/秒混用（initSubbands 里 tempcoeffs[1]*1e-6 当 offsettime）不照抄；条纹旋转相位取小数部分（fraction_of = val − rint(val−0.5)，datasim 原样），参考频率 = band 起始频率（datasim d_freq = getDRecordedFreq）。updatevalues 照抄：每帧 fracerr += coeffs[1] − (prevdelay+prevrate)，超 ±0.5 复采样时间 shift ±1（if 单次，非 while）；fracsample 校正相位 = 2π×(idx/vpsamps)×复带宽×fracerr（= 2π/framesec×idx×fracerr）。**帧序必须用全局帧号**（frameglobal，模型时间与块内 framecounter 不同）。
- **procptr 漂移限制（滚动缓冲）**：baseband 为滚动缓冲（前块尾 BASEBAND_TAIL=4096 复样本保留，memmove 拼接），帧窗口起点 = frameglobal×vpsamps + shift − blockstartglobal + tail（越界 clamp）；blockstartglobal 在**本块最后一帧填完**才推进（早推进会让整块帧读错位置——已踩）。首块 tail 区填 0（正 delay 吸收不回头读）。shift 吸收速率 = 每帧 ±1 复样本，大 delay（如 11.2ms）的整样本部分靠长时间吸收，批长秒级时绝大部分 delay 由条纹旋转承载（datasim 同款语义）。
- **记录中断 `FXSIM_GAPS`（2026-09-18 加）**：`vdifwriter` 支持在指定帧号处造记录中断，两种形式——缺口（帧号跳过、不写字节，文件比时间轴**短**）与 filler（额外写帧头全零的帧占位，文件比时间轴**长**），与 t25362 的真实中断同构（data-spec 5.2）。**两种形式都必须推进帧号**：中断是"真实过去了的时间"，帧号要跳过丢失的帧数，filler 只是额外留下占位**字节**——首版让 filler 不推进帧号，于是 filler 形式的数据帧号范围比缺口形式短 73 帧，同一段观测被描述成两个长度（5.2 的「帧号跳跃只反映真实丢失，与 filler 帧数无关」正是这条）。实现在 `VDIFWriter::writeFrame` 内部（命中 `atframe` 时处理），legacy 与新路径两个调用点自动生效；不设该变量时行为完全不变。检验步骤与验收判据见 `fxcorr/test/gaps/README.md`。
- **量化**：datasim 的 3 电平 ±thresh×sign 映射**不照抄**（实测弱信号 F=1 下比四电平 rint 效率低 4.5 倍），移植的是门限自适应（FXSIM_ADAPTIVE，d_tmul 语义）+ 固定四电平 rint；四电平码序与物理电平反相 = VDIF 惯例。

## 构建与注册

- 模板照 fxcorr-f：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f + vdifio——vdifio 仅用 vdifio.h 的 vdif_header 位域结构与 getVDIFFrameBytes() static inline，不链接其库）。
- install-difx 注册 4 处（components 字典、setNormalComponentsFalse、apptargets dompicxx=True、--doonly 帮助），照 fxcorr-f。

## 相关

- 命令行与环境变量：`fxcorr/usage.md`（fxcorr-sim 段）
- 架构设计（分布式形态/公共信号模型/datasim 特性差距/阶段 P0-P4）：`fxcorr/fxcorr-sim-arch.md`
- 数据规范（common/ 产物格式）：`fxcorr/data-spec.md` 5.8
- 验证档案：本目录 `VERIFICATION.md`
- 测试资产与对拍流程：`fxcorr/CLAUDE.md`；编排（make_testdata.sh/run_batch.sh 两段式调用）：`fxcorr/v1-plan.md` 2.4
