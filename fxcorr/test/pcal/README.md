# phasecal（P0）检验

> **验证记录（冻结）**——本目录记录 P0 的检验资产与验证过程，**截止 2026-09-13，此后未再更新**。
> 脚本与其判据仍是当前的；本文记的是当时的验证结论。

本目录是 phasecal（PCAL_*.pcal 文件生成）特性的检验资产与验证记录（algo-plan P0，2026-09-13 实施完成）。设计、算法详解与逐字节一致的六个关键点见 `algo-plan.md` P0 节。

## 资产

- `test-pcal.vex`：带 phasecal 的测试配置（$PHASE_CAL_DETECT 段 def 内直接追加 tone 列表 `2 : 3 : 4 : 5`——VEX 词法无 `tones` 关键字，数值列表紧跟 `phase_cal_detect = &NoCal` 引用之后）
- `test-pcal.v2d`：配套 vex2difx 配置（phaseCalInt = 0 → 1，MHz）

用法：复制成 config/test.vex / test.v2d 后复用 make_testdata.sh 全流程；vex2difx 将 tone 序号 × 1MHz + base(0) 生成 .input PHASE CAL（difx 0-based 序号 1-4 → 201-204MHz，band 4MHz 内 4 tone）。

## 验证方法与结果（2026-09-13）

- 对拍环境：cmp5（config 用 test-pcal 资产）+ `FXSIM_NOISE=0 make_testdata.sh`；基准 `run_bench.sh`（mpifxcorr 出 bench/<exp>.difx/PCAL_*，同一份 raw 数据）。
- **单 batch**（test.input，0.524288s subint、intTime 1.048576s、2 intTime/batch）：`diff` PCAL T1/T2 与基准**逐字节一致**；SWIN 对拍（cmp_swin.py）同时全等（时域数据一致性佐证）。
- **多 batch**（test-sim.input，128ms subint、intTime 0.256s、4 intTime/batch，-n 2）：两 batch 顺序跑后每站 8 数据行、pcalmjd 严格递增；重跑 batch1 后行数不变、时间序保持。此配置不与 mpifxcorr 对拍（128ms 帧对齐触发 vdifmux 帧号 bit7 错读，既有已知限制）。
- **容器模式**：重建 builder/base/f 镜像后 `FXCORR_RUN_MODE=container` 跑批，PCAL 落挂载的 vis/ 内、与基准逐字节一致。
- 独立对照实验（定位共轭问题时）：同一段合成数据（0.7×sin(2π×1MHz×n/8MHz)）分别链接 libfxcorrcommon 与 mpifxcorr 源码编译提取，两侧输出逐位相同（im 均负）——证实提取层无差异、差异在写盘层。
