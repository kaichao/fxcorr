# fxcorr-sim 验证记录

本文是 fxcorr-sim 的验证档案（测试机 /root/fxcortest/），按阶段与路径归档；最新条目在前的小节内按时间追加。源文件地图/实现要点见本目录 `CLAUDE.md`，架构与阶段见 `fxcorr/fxcorr-sim-arch.md`，命令行手册见 `fxcorr/usage.md`，测试资产与对拍流程见 `fxcorr/CLAUDE.md` 与 `fxcorr/test/`（各特性子目录 README.md）。

## legacy 路径（旧时域合成，字节对拍回归）

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

## P0（新架构基础，测试机 /root/fxcortest/simp0/）

- **legacy 字节对拍回归**：CLI 改三入口后 `FXSIM_NOISE=0 fxcorr-sim station simcmp T1 . 1.5` vs `gen_test_vdif.py /tmp/ref.vdif 4.096 1.5 8`，1024 帧 8,224,768 字节 **BYTE-IDENTICAL**（对拍配置 test-sim-**nopcal**.input——test-sim.input 带 PHASE CAL INT 1 会注入 pcal，参考文件无 pcal）。
- **common 生成**：`fxcorr-sim common simcmp .` → common/simcmp/ 9 块（8×16MB + 末块 3,072,000B 截断）、meta.json status=done、specRes=0.5MHz/numsamps=8/minStartFreq=200 与手算一致；.tmp+rename 完成可见性生效。
- **跨站相干**：两站 `FXSIM_NOISE=0 fxcorr-sim station simcmp T1/T2 .` 输出 1024 帧**逐位一致**（单源公共信号天然保证）；`FXSIM_NOISE=0.02` 两站数据自 byte 34 起不同（站噪声独立注入、帧头一致）。
- **相干成分定量**：σ=1.0 两站 VDIF 解码后零滞后相关系数 0.444（理论 1/(1+σ²)=0.5，2bit 量化 + Ormsby 边缘损失，方向量级吻合）；量化健康（输出功率 1.24、无饱和）。
- **新路径全链路**：common → 两站 station（FXSIM_NOISE=0.02）→ fxcorr-f ×2 → fxcorr-x → SWIN 6 记录（2 积分 × 3 基线）、weight 0.989/1.0、可见度峰 ~4450 落自相关通道（σ=0.02 下公共信号主导，互相关≈自相关水平）。
- **make_testdata.sh 两段式**：无 tone 自动 `common` 一次 + 逐站 `station`；带 tone 走 legacy 逐站（无 common）——两分支均实测。
- 坑：difxcalc 生成的 .input 里 OUTPUT FILENAME 可能是绝对路径（如 cmp5 资产 `/root/fxcortest/cmp5/config/test.difx`），复制目录做新验证时须 sed 改相对路径，否则 fxcorr-x 写到原目录。

## P1（编排并行，测试机 /root/fxcortest/p1reg/；编排层，fxcorr-sim 本体无改动）

- make_testdata.sh 加 `-p P`（station 任务本地并行，xargs -P）与 `--nodes "host:st1,st2 ..."`（ssh 节点映射，未列出站本地跑）；`-n 3 -p 4` 3 batch × 2 站全部生成。
- **远程/本地逐位一致**：`--nodes "localhost:T1 localhost:T2"` ssh 远程产物 vs 本地串行重生成 **BYTE-IDENTICAL**（同 seed 确定性 + 并发读 common 无冲突）。
- **失败传播**：节点不可达（nosuchhost）→ xargs 124 → 脚本非零退出、产物未写；恢复后重生成成功。幂等回归：产物全在时全部跳过 rc=0。
- **并行产物全链路**：run_batch.sh 跑 batch1 → SWIN 12 记录（4 积分 × 3 基线）、时间步 0.256s、weight 0.956/1.0（首积分 subint 边界折算）。
- 实现中修的两个脚本 bug：`--nodes` 长选项须任意位置摘出（原仅开头识别）；任务列表为空时 GNU xargs 仍执行一次 `bash -c`（空输入保护 `if [ -s "$TASKS" ]`）。

## P2（SEFD 定标/延迟注入/谱线，测试机 /root/fxcortest/p2reg/ 与 /root/fxcortest/p2delay/）

- **A 块 specres**：FXSIM_SPECRES=2/4 → meta.json spec_res 0.25/0.125 MHz、numsamps 16/32、slices_per_block 翻倍；station 用不同 specres 派生网格 vs meta → 报错拒绝（一致性保护）；specres=0 报错；specres=3（帧时长非切片整数倍）报错。
- **A 块谱线**：FXSIM_LINE=201.5,10,3 → common 产物频点 rms [0.032, 0.498, 2.585, **4.472**, 2.583, 0.499, 0.032, 0.001]（idx3 = √(2×10) ✓ 高斯边缘 ✓）；两站 NOISE=0 **BYTE-IDENTICAL**（跨站相干）；VDIF 投影 1.5 MHz 处无谱线底 46 → amp=2 时 680 → amp=10 时 925（随 amp 单调，2bit 削波压缩比例）；199/204 MHz 越界、格式错、amp/rms 非正均报错。
- **B 块 SEFD 定标**：恒等回归 FLUX=1 SEFD=0 与 P0 基线 **BYTE-IDENTICAL**；两站时域相关系数扫描（理论 ρ=F/√((F+SEFD1)(F+SEFD2))）：SEFD=0 → r=1.0；SEFD=0.01 → 0.958；SEFD=1 → 0.483（理论 0.5）；SEFD=100/10000 → 0.091（**2bit 量化非线性地板**，噪声主导时量化边界效应，非 bug）；**列表语义** SEFD=1,2 → r=0.4075（理论 1/√6=0.4082，0.2% 吻合）；FLUX=4 SEFD=1 → r=0.749（理论 0.8）。SEFD 大时自相关归一为 1 无法区分（÷√(F+SEFD)），判据必须用互相关。
- **C 块延迟注入**：
  - 恒等回归：FXSIM_DELAY=0 与 P0 基线 **BYTE-IDENTICAL**（滚动缓冲改造后仍成立；过程中修了 blockstartglobal 提前推进 bug——整块帧读 tail 零区出 0xAA 垃圾）。
  - test 配置（T1/T2 坐标差 ~100m → delay ~330ns）：默认延迟链注入数据 fxcorr 链 vs mpifxcorr 对拍 **6/6 全等**（<1e-6）——注入与两链 delay 对齐精确互逆（注入生效 + 注入量 = .im 模型值）。FXSIM_DELAY=0 数据对拍同样 6/6。
  - P11 配置（对跖点，.im DELAY 11.2ms，/root/fxcortest/p2delay/）：fxcorr-sim 注入数据全链跑通（跳块 + shift 吸收无越界）；对拍 4/6 全等、record 3（互相关）~12% + record 5（自相关）~2e-5 差、weight 0.99316/0.99203——**与无注入数据对拍差异完全同构**（对照实验），即该差异是先存于 fxcorr-f/vdifmux 的（白噪声数据 + 11.2ms 跳块段 valid 判定微差，P11 当时用 tone 数据未暴露），**非延迟注入引入**，记录在案待独立排查（fxcorr-sim 无改动）。
- **对拍注意**：config/<exp>.difx 跨 run_batch 追加（fxcorr-x 语义），多次验证须先 rm，否则 cmp 混入旧轮记录（曾误判 1e-3 差异）。mpifxcorr 读 sed 后 `OUTPUT FILENAME: bench.difx` 会写成 `ch.difx`（getinputkeyval 第 20 列坑，取整行第 21 字符起）。

## P3（性能评估不实施 + 附带修复）

- **多 band 新路径（2026-09-14）**：test2b 带 1MHz 间隙（200+205MHz）原报 "band at 205 MHz does not fit the spectrum grid"——deriveGrid 的 span 照抄 datasim band0带宽×band数（连续假设），且 datasim 对间隙布局会静默越界读公共信号。修正 span = max(freq+bw) − min(freq)（间隙切片生成不读取）→ common 成功（specRes 0.5MHz、numsamps 18 覆盖 200-209）→ 两站 station 2098 帧 → f（band_00/01.sp）→ x 42 条 SWIN（7 积分 × 2 频段 × 3 基线）、自相关峰 chan 768/512 与 legacy 验证一致 ✓。同时暴露并修复 per-band 帧结构 bug（vpsamps/ratehz/nsampframe 未 ÷nbands，见 CLAUDE.md 实现要点）。单 band 回归：common 与 P2 产物 BYTE-IDENTICAL、station 两次生成逐位一致 ✓。
- **性能基线**（test 配置 4MHz/2.097s batch 单任务）：common 0.84s、station 2.45s/站（0.86× 实时）、fxcorr-f 0.46s、fxcorr-x 0.09s——sim station 虽是流水线最慢单任务（f 的 5.3×），但离线造数无压力、P1 的 (batch,station) 进程级并行已覆盖多核场景 → **进程内并行判定不实施**。

## P4（新路径 pcal 注入，测试机 /root/fxcortest/p4 + p4c）

- **网格**（test-pcal 资产，PHASE CAL INT 1 → 201-204MHz 4 tone）：新路径全链路 → PCAL_ 文件 4 tone 全部检出、两积分相位稳定（201 实部 −0.10247/−0.10211）、204（Nyquist 边缘）折叠纯实——与 mpifxcorr 对同一份 raw 的提取**逐字节一致**；SWIN 与 mpifxcorr 基准 cmp_swin.py **6/6 全等**；tone 幅度/相位模式与 legacy（simp0）一致。
- **梳齿**（test 资产 + FXSIM_PCAL=1，k=0..3 → 1/2/3MHz）：自相关谱 chan 1024（1MHz）两站 × 两积分 **4/4 高出邻居 50-100%**、chan 2048（2MHz）弱但一致、chan 3072（3MHz）被谱高端底噪淹没（1/500 幅度在 2bit 量化下的传递极限）；帧边 taper 直接证据——525 帧首样本 2bit 值**恒为 2**（realc[0]=0 → rint(0)+2），无 pcal 对照产物首样本 0/1/2/3 均匀分布（79/117/135/194）。
- **默认回归**：无 pcal 配置 P4 前/后产物 T1/T2 **BYTE-IDENTICAL**（pcalhz 为空时注入块整体跳过）。
