# 交叉极化自相关（P7）检验

本目录是交叉极化自相关（WRITE AUTOCORRS / maxproducts>2）特性的检验资产与验证记录（algo-plan P7）。设计见 `algo-plan.md` P7 节；实现易错点见 `applications/fxcorr-f/CLAUDE.md` 与 `applications/fxcorr-x/CLAUDE.md` 关键实现要点。

## 资产

- `test-pols.vex`：test2b.vex 基础上把第 2 band 改为**同频率 Lcp**（chan_def 两条同为 200.00 MHz，&CH02/&BBC02 挂 L 极化 IF）——dual-pol 观测（RCP+LCP 同频率）的 VEX 表达。$TRACKS 的 `track_frame_format = VDIF/16032/2`（2 band 帧长，见下"资产修复"）。
- `test-pols.v2d`：test.v2d 改 vex=test-pols.vex。vex2difx 生成的 .input：`REC BAND 0 POL: R` / `REC BAND 1 POL: L`（同 `REC BAND INDEX 0` = 同一频率 → Mode 的 matchingRecordedBand 对两 band 同时成立、count=2 → crosspol 累加触发）、BASELINE 段 `POL PRODUCTS: 4`（maxproducts=4）、`WRITE AUTOCORRS: TRUE`（vex2difx 默认值，configuration 的 CONFIG 段必须行）。

## 资产修复（实施中发现的配置 bug）

- **test2b.vex 的 $TRACKS 帧长**：原 `track_frame_format = VDIF/8032/2`（1 band 帧长）而 2 band VDIF 帧实为 16032 字节 → vex2difx 生成 `DATA FRAME SIZE: 8032` → mpifxcorr vdifmux 按 8032 期望读 16032 帧 → "databytesperpacket change" 错乱、SWIN 头字段垃圾（2026-09-13 修正为 16032，vdifmux 警告清零、SWIN 头字段正常）。
- **fxcorr-sim 的 nbands 语义**：原用 `getDNumRecordedFreqs`（不同频率数）——dual-pol 同频率时 freq 数=1 而 band 数=2，2bit 校验（bytespersample num/denom）与 tone/pcal 网格全部按 band 语义使用 → 校验误报 "2-bit samples only"。已改 `getDNumRecordedBands`（多频场景两值相等、行为不变，位序对拍 BYTE-IDENTICAL 佐证）。

## 检验步骤（测试机可一键复现）

```bash
cd /root/fxcortest && rm -rf crosspol && mkdir -p crosspol/config crosspol/batches crosspol/raw
cp /root/fxcorr/fxcorr/test/crosspol/test-pols.vex crosspol/config/
cp /root/fxcorr/fxcorr/test/crosspol/test-pols.v2d crosspol/config/test.v2d
cd crosspol/config && source /root/fxcorr/setup.bash && vex2difx test.v2d && difxcalc test.calc
grep -E "REC BAND|POL PRODUCTS|WRITE AUTOCORRS" test.input      # R/L 同 freq index、POL PRODUCTS 4、WRITE AUTOCORRS TRUE
cd .. && python3 << 'PYEOF'                                     # batch.json（起点 = scan 起点 25200s，subint 0.262144s）
import json
b = {"batch_id": "58948_25200", "start_mjd": 58948.0 + 25200/86400.0,
     "start_time": "2020-04-09T07:00:00", "duration_sec": 32*0.262144,
     "stations": ["T1", "T2"], "config_file": "config/test.input",
     "n_subints": 32, "subint_ns": 262144000, "integration_sec": 1.310720, "status": "running"}
json.dump(b, open("batches/58948_25200.json", "w"), indent=2)
PYEOF
source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib
FXSIM_NOISE=0 fxcorr-sim 58948_25200 T1 . 1.5          # 1 个 tone 参数 = 两 band 同 1.5MHz
FXSIM_NOISE=0 fxcorr-sim 58948_25200 T2 . 1.5
ln -sf raw/T1/T1_58948_25200.vdif TEST1.vdif && ln -sf raw/T2/T2_58948_25200.vdif TEST2-usb.vdif
fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2
# autocorr.bin 应为 version 2 + crosspol=1 + nbands=2，记录 = 平行 2 band 段 + crosspol 2 band 段
fxcorr-x 58948_25200
# SWIN 72 条 = 6 积分 × 12（基线 T1-T2 4 pol + 自相关 257/514 各 RR/LL/RL/LR）
# 谱峰验证：自相关 4 条记录峰均在 chan 768（1.5MHz）、交叉功率 = 平行功率
```

**单 pol + WRITE AUTOCORRS 对拍（自相关写盘路径与 mpifxcorr 逐位对拍）**：

```bash
cd /root/fxcortest && rm -rf pol1 && mkdir -p pol1/config pol1/batches pol1/raw
cd pol1/config && cp /root/fxcorr/fxcorr/test/test.vex . && cp /root/fxcorr/fxcorr/test/test.v2d .
source /root/fxcorr/setup.bash && vex2difx test.v2d && difxcalc test.calc
# vex2difx 默认 WRITE AUTOCORRS TRUE（maxproducts=1 时只写平行自相关）
cd .. && python3 << 'PYEOF'   # batch.json（subint 0.524288s、n_subints 8）
import json
b = {"batch_id": "58948_25200", "start_mjd": 58948.0 + 25200/86400.0,
     "start_time": "2020-04-09T07:00:00", "duration_sec": 8*0.524288,
     "stations": ["T1", "T2"], "config_file": "config/test.input",
     "n_subints": 8, "subint_ns": 524288000, "integration_sec": 1.048576, "status": "running"}
json.dump(b, open("batches/58948_25200.json", "w"), indent=2)
PYEOF
source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib
FXSIM_NOISE=0 fxcorr-sim 58948_25200 T1 . 1.5 && FXSIM_NOISE=0 fxcorr-sim 58948_25200 T2 . 1.5
ln -sf raw/T1/T1_58948_25200.vdif TEST1.vdif && ln -sf raw/T2/T2_58948_25200.vdif TEST2-usb.vdif
fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200
mv config/test.difx/DIFX_58948_025200.s0000.b0000 /tmp/pol1_fxcorr.swin
sed 's/^EXECUTE TIME (SEC):.*/EXECUTE TIME (SEC): 2/' config/test.input > config/test-mpi2.input
mpirun --allow-run-as-root -np 4 /usr/local/difx/bin/mpifxcorr config/test-mpi2.input
mv config/test.difx/DIFX_58948_025200.s0000.b0000 /tmp/pol1_mpi.swin
python3 /root/fxcorr/fxcorr/test/cmp_swin.py /tmp/pol1_fxcorr.swin /tmp/pol1_mpi.swin 4096 6
# 前 6 条 = 2 积分 × [基线 T1-T2 + 自相关 257 + 自相关 514] 全等
```

**无 WRITE AUTOCORRS 回归**：pol1 的 test.input `sed -i 's/WRITE AUTOCORRS:.*/WRITE AUTOCORRS:    FALSE/'` 后重跑 f/x（autocorr.bin v2 crosspol=0）+ mpifxcorr 基准（test-mpi2.input 重新 sed），cmp_swin.py 前 2 条（每积分仅 1 基线记录）全等。

## 验收判据

- dual-pol：autocorr.bin version 2、crosspol=1；SWIN 每积分 = 互相关 4 pol（RR/RL/LR/LL）+ 自相关伪基线 257/514 各 4 条（平行 [p,p] + 交叉 [p,opposite(p)]）；自相关谱峰落 tone 频率、交叉谱功率 = 平行谱功率（同 tone 完全相关时的物理判据）、weight=1。
- 单 pol + WRITE AUTOCORRS：与 mpifxcorr 前 2 积分 6/6 逐位全等（含自相关记录）。
- 无 WRITE AUTOCORRS：autocorr.bin crosspol=0、SWIN 对拍 2/2 全等；位序对拍 BYTE-IDENTICAL。
- 已知边界：mpifxcorr 读 2 band 样本交织 VDIF 不可用（vdiffile.cpp:538-550 corner-turn 路径输出 32032B 帧与 mark5access 2channel 交织解码不匹配，上游此路径本身未经考验），dual-pol 无法直接对拍，以单 pol 场景对拍（自相关写盘路径共享）+ dual-pol 链路自洽验证覆盖。

## 验证记录（2026-09-13）

- 环境：/root/fxcortest/crosspol/（test-pols 资产，2 band R/L 同 200MHz）+ /root/fxcortest/pol1/（单 pol 对拍）。
- **dual-pol 全链路**：autocorr.bin v2 crosspol=1 nbands=2（平行 2 band 段 + crosspol 2 band 段，weight 9-10/批次）；SWIN 72 条 = 6 积分 × 12（基线 258 的 RR/RL/LR/LL + 自相关 257/514 各 RR/LL/RL/LR，顺序同 visibility.cpp:913-944）；自相关 8 条记录谱峰全部落 chan 768（1.5MHz tone）、交叉谱峰功率 2.016e8 ≈ 平行 2.016e8、weight=1.0。
- **单 pol + WRITE AUTOCORRS 对拍**：前 2 积分 6/6 记录逐位全等（基线号 258/257/514、时间戳、polpair、weight、可见度——自相关写盘路径与 mpifxcorr 一致）。
- **无 WRITE AUTOCORRS 回归**：autocorr.bin v2 crosspol=0（记录与 v1 布局一致）、SWIN 对拍 2/2 全等。
- **位序回归**：fxcorr-sim 单 band 输出与 gen_test_vdif.py BYTE-IDENTICAL（8032000 字节公共前缀，nbands 修复不影响单 band/多频场景）。
- **实施中实测的坑**：① 自相关伪基线号 257×(tel+1) 与单 pol 的互相关 T1-T1/T2-T2 基线号（257/514）**撞号**——SWIN 里按 polpair/记录序区分，解析对拍时勿把自相关当互相关基线（本检验单 pol 实验 BASELINE ENTRIES=1，257/514 全部是自相关记录）；② test2b.vex 的 $TRACKS 帧长 8032→16032（见"资产修复"）；③ fxcorr-sim nbands 语义 freq 数→band 数（见"资产修复"）。
