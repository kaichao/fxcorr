# 相位阵频率域波束形成（P8）检验

> **验证记录（冻结）**——本目录记录 P8 的检验资产与验证过程，**截止 2026-09-13，此后未再更新**。
> 脚本与其判据仍是当前的；本文记的是当时的验证结论。

本目录是相位阵（PHASED ARRAY）特性的检验资产与验证记录（algo-plan P8）。设计见 `algo-plan.md` P8 节；实现见 `applications/fxcorr-x/src/beamengine.{h,cpp}` 与 `main.cpp` 的 beam 分支。

## 背景：上游是死代码，无对拍

mpifxcorr 的相位阵链路是半成品：f 侧 core.cpp:818-865 把各站频谱按 DWeight 加权求和写入 threadcrosscorrs（布局 = Σ_freq Σ_pol nchan，无 baseline 维），但消费端 uvshiftAndAverage 仍按 freq×baseline offset 解包（相位阵时 offset 表未分配），输出端（padomain/paoutputformat/DIFX/VDIF/TIMESERIES）无消费者——上游跑不通。因此 **P8 不做 mpifxcorr 对拍**，验证方式为：与 fxcorr-f 的 .sp 频谱按相同累加顺序手算加权和、逐位核对（cmp_beam.py），加上无相位阵配置的回归对拍。

## 资产

- `gen_test_phasearr.py`：从 test.input 生成 test-phasearr.input + test-phasearr.pa（相位阵配置文件）。参数：`<input> [w_t1] [w_t2] [acc_ns]`，默认权重 0.5/0.5、ACC TIME 20480000ns（10 FFT 块 = 1 个 numbufferedffts 批，numaccs=4 窗口/subint）。
- `cmp_beam.py`：读两站 band_00.sp 与 beam.bin，按与 BeamEngine 相同的累加顺序（acc 窗口 → FFT 块序 → 站序 → 通道序）手算 `Σ_ds w_ds × Σ_fft spec_ds`，逐位核对频谱与每条记录的时间戳（窗口起点 = subint 起点 + acc 序号 × acc_ns）。用法：`cmp_beam.py <sp1> <sp2> <beam.bin> <w1> <w2> [max_subints]`。

## 关键坑：SUBINT 必须是 numbufferedffts 的整数倍

上游 Configuration 校验（configuration.cpp populateResultLengths 相位阵段）：`ACC TIME (NS)` 换算的 FFT 块数 accffts 必须是 `NUM BUFFERED FFTS` 的整数倍。标准 test.input 的 SUBINT = 524288000ns = 256 块，256 不是 10 的整数倍——**任何 ACC TIME 都会被上游校验拒绝**。因此 gen_test_phasearr.py 把 SUBINT 改为 81920000ns（40 块 = 4 个 numbufferedffts 批），ACC TIME 20480000ns（10 块）→ 每 subint 4 个 acc 窗口。SUBINT 变化后 blockspersend 自动变 40（= subintns/blockns，blockns = 2048000ns 由帧结构定），fxcorr-f/x 读同一 .input 网格自动一致。

## 检验步骤（测试机可一键复现）

```bash
cd /root/fxcortest && rm -rf phasearr && mkdir -p phasearr/config phasearr/batches phasearr/raw
cp /root/fxcorr/fxcorr/test/test.vex phasearr/config/
cp /root/fxcorr/fxcorr/test/test.v2d phasearr/config/test.v2d
cd phasearr/config && source /root/fxcorr/setup.bash && vex2difx test.v2d && difxcalc test.calc
python3 /root/fxcorr/fxcorr/test/phasearr/gen_test_phasearr.py test.input 0.5 0.5
grep -A1 "PHASED ARRAY" test-phasearr.input          # TRUE + CONFIG FILE 行
cat test-phasearr.pa                                  # OUTPUT TYPE/FORMAT、ACC TIME、权重
cd .. && python3 << 'PYEOF'                           # batch.json（起点 = scan 起点 25200s；40 块 subint）
import json
b = {"batch_id": "58948_25200", "start_mjd": 58948.0 + 25200/86400.0,
     "start_time": "2020-04-09T07:00:00", "duration_sec": 50*0.08192,
     "stations": ["T1", "T2"], "config_file": "config/test-phasearr.input",
     "n_subints": 50, "subint_ns": 81920000, "status": "running"}
json.dump(b, open("batches/58948_25200.json", "w"), indent=2)
PYEOF
source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib
FXSIM_NOISE=0 fxcorr-sim 58948_25200 T1 . 1.5
FXSIM_NOISE=0 fxcorr-sim 58948_25200 T2 . 1.5
ln -sf raw/T1/T1_58948_25200.vdif TEST1.vdif && ln -sf raw/T2/T2_58948_25200.vdif TEST2-usb.vdif
fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2
fxcorr-x 58948_25200
ls -la beam/58948_25200/                              # beam.bin 存在；vis/ 不应有该 batch 的 SWIN
```

数值核对（0.5/0.5 权重）：

```bash
cd /root/fxcortest/phasearr
python3 /root/fxcorr/fxcorr/test/phasearr/cmp_beam.py \
  fengine/58948_25200/T1/band_00.sp fengine/58948_25200/T2/band_00.sp \
  beam/58948_25200/beam.bin 0.5 0.5
```

## 验收判据

1. **beam.bin 数值**：cmp_beam.py 输出 `50 subints x 4 accs all identical - PASS`（50 条 subint × 每 subint 4 窗口，频谱逐位 = 手算加权和，时间戳 = subint 起点 + acc×20480000ns）。
2. **单站等效变体**：重新生成权重 `1.0 0.0` 的 .pa/.input 后重跑 fxcorr-x，`cmp_beam.py ... 1.0 0.0` PASS——波束 = T1 频谱、T2 无贡献（DWeight 生效的直接证据）。
3. **波束物理**：波束谱峰在 chan 1536（1.5MHz tone：beam.bin 存 .sp 全谱 4096 chan 覆盖 4MHz，1.5MHz/976.5625Hz = 1536；SWIN 的 768 是 0-2MHz 半谱语境），幅值 = 两站 tone 加权和（0.5/0.5 时两站同源同幅 → 谱峰 ≈ 单站谱峰）。
4. **不写 SWIN**：相位阵 batch 后 vis/ 下无新 SWIN 文件（beam 分支跳过互相关/Visibility 路径）。
5. **无相位阵回归**：标准 test.input（无 PHASED ARRAY 段）全链路 SWIN 与 mpifxcorr 对拍 6/6 不变（fxcorr-x 改动不漂移既有路径）。

## 验证记录（2026-09-13 实施完成）

- **0.5/0.5 权重**：fxcorr-x 输出 `50 subints of beam output written`；cmp_beam.py 50×4 窗口逐位 PASS（频谱 f32 相同累加序逐位一致、时间戳 = subint 起点 + acc×20480000ns）。beam.bin 尺寸 6556265 = 256 头 + 9 段表 + 50×4×32780 ✓。
- **1.0/0.0 权重**：重生成 .pa/.input 后重跑，cmp_beam.py 1.0/0.0 逐位 PASS——T2 权重 0 时波束 = T1 频谱（DWeight 生效）。
- **波束物理**：首记录 freq=0 pol=R nchan=4096，scan 0 sec 0 ns 0，谱峰 chan 1536（= 1.5MHz）、功率 5.326e9。
- **不写 SWIN**：相位阵 batch 后 vis/ 目录不存在 ✓。
- **无相位阵回归**：p8reg 目录（标准 test.input 8 subints）fxcorr-x 出 24 条 SWIN 记录，与 mpifxcorr 基准 cmp_swin.py 前 6 条全等 ✓（intTime 校验限定非相位阵后重确认）。
- **实施中踩坑**：① 生成脚本最初按 `key: value` 直觉写 .input/.pa 行——DiFX 的 getinputkeyval 从固定第 20 列起取 val（DEFAULT_KEY_LENGTH=20），短 key 的值必须空格 pad 到第 20 列，否则值被截断/带空格（"config/test-phasearr.pa" 带前导空格导致 fopen 失败、ACC TIME 读到 "8000"）；已加 kv() 函数（照 pulsar 资产先例）。② 相位阵分支最初做成 if/else 包裹重构（提公共 lambda），标准配置回归出现堆损坏（subint 1 的 fread 触发），逐段二分无果后改为**早退分支**（旧版路径逐字节不动 + 相位阵时提前 return），回归 6/6 恢复全等——重构未继续深挖，但早退方案隔离性最好，保留。

