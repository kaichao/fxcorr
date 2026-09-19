# SwitchedPower（P6）检验

> **验证记录（冻结）**——本目录记录 P6 的检验资产与验证过程，**截止 2026-09-13，此后未再更新**。
> 脚本与其判据仍是当前的；本文记的是当时的验证结论。

本目录是 SwitchedPower（TCAL 噪声功率）特性的检验资产与验证记录（algo-plan P6）。设计见 `algo-plan.md` P6 节；实现易错点见 `applications/fxcorr-f/CLAUDE.md` 关键实现要点。

## 资产

- `test-tcal.v2d`：test.v2d 基础上两站 ANTENNA 段加 `tcalFreq = 80`——vex2difx 生成 .input DATASTREAM 段的 `TCAL FREQUENCY: 80` 行（applycorrparams.cpp setTcalFrequency）。80Hz = VLBA 惯例开关频率，2.097s 数据约含 167 个完整开关周期，每整秒窗口 on/off 各 40 个半周期。
- 输出文件 `SWITCHEDPOWER_<mjd>_<sec>_<dsid>`（dsid = .input DATASTREAM 表 0-based 序号，T1→0、T2→1），写在 OUTPUT FILENAME 目录（与 PCAL 文本同目录），每行 = mjd0 mjd1 + 每通道 powerOn sigmaOn powerOff sigmaOff。

注意：本检验**不需要真实 TCAL 信号**——switched power 统计在原始 2bit 样本上无条件进行（tone 数据的 on/off 两组统计值相同，足以验证喂入节奏、窗口对齐、功率换算与文本格式的逐位一致性）。

## 实施中发现并修复的两个生成器帧头 bug（重要）

switched power 路径用 mark5access 解析 VDIF 帧头（喂入的块构造 mark5_stream_memory），主相关路径（vdifmux）不查这些字段所以此前从未暴露：

1. **legacy 标志误置**：word0 bit30 必须为 0（=1 表示 legacy 16 字节头）。gen_test_vdif.py 原写 `(1 << 30)`（误当 sync 位）→ mark5access 按 16 字节头解析 32 字节帧 → 帧长字段读错、全部帧 blank（validate fail）→ SWITCHEDPOWER 恒空（mpifxcorr 上游同样空）。
2. **帧头字布局与 mark5access/vdifio 不一致**：DiFX 生态的 VDIF 头布局（vdifio.h vdif_header / mark5access format_vdif.c）是 word0 = [29:0] 秒 + [30] legacymode + [31] invalid；word1 = [23:0] 帧号 + [29:24] ref epoch；word2 = [23:0] 帧长（8 字节单位）+ [28:24] log2 nchan + [31:29] version；word3 = station/thread/nbits/iscomplex。gen_test_vdif.py / fxcorr-sim 原按 VDIF 2010 官方 spec 布局（word1 = 秒高位、word2 = epoch/frame、word3 = version/len）写 → mark5access 把"帧号"当帧长（`dataframelength = word2 低 24 位 × 8`）、epoch 读错（word1 高 6 位 = 秒高位 0，误判 2000 年恰好凑对 MJD）→ 帧长探测错乱、跨帧 validate 失败。已把两个生成器改为 DiFX 布局，epoch 字段写 0（2000.0，与 word0 自 2000 起的累计秒一致）。修复后位序对拍仍 BYTE-IDENTICAL（FXSIM_NOISE=0）、SWIN 对拍 6/6 回归通过。

## 检验步骤（测试机可一键复现）

```bash
cd /root/fxcortest && rm -rf tcal && mkdir tcal && cd tcal && mkdir config
cp /root/fxcorr/fxcorr/test/test.vex config/
cp /root/fxcorr/fxcorr/test/tcal/test-tcal.v2d config/test.v2d
cd /root/fxcortest && bash /root/fxcorr/fxcorr/make_testdata.sh tcal 1.5   # 出 config/test.input（含 TCAL FREQUENCY: 80）
cd tcal
grep "TCAL" config/test.input                          # 两站各一行 TCAL FREQUENCY: 80
# fxcorr 链路（SWITCHEDPOWER 由 fxcorr-f 写 OUTPUT FILENAME 目录）
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2'
wc -l config/test.difx/SWITCHEDPOWER_*                # 各 3 行（2 整秒窗 + 尾窗）
mkdir /tmp/tcal_fxcorr && mv config/test.difx/SWITCHEDPOWER_* /tmp/tcal_fxcorr/
# mpifxcorr 基准（EXECUTE TIME=2 截断；先清 SWIN 否则拒绝覆盖）
rm -f config/test.difx/DIFX_*
sed 's/^EXECUTE TIME (SEC):.*/EXECUTE TIME (SEC): 2/' config/test.input > config/test-mpi2.input
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  mpirun --allow-run-as-root -np 4 /usr/local/difx/bin/mpifxcorr config/test-mpi2.input'
# 对拍：共同时间窗（前 2 个完整整秒窗 [25200,25201) 与 [25201,25202)）逐行全等
for ds in 0 1; do
  echo "== ds $ds =="
  diff <(head -2 /tmp/tcal_fxcorr/SWITCHEDPOWER_58948_25200_$ds) \
       <(head -2 config/test.difx/SWITCHEDPOWER_58948_25200_$ds) && echo "first 2 lines OK"
done
# 无 tcal 回归：config/test.v2d 换回根目录 test.v2d，rm test.input/.im/.calc 重新
# vex2difx/difxcalc（无 TCAL 行），rm -rf fengine config/test.difx，重跑 f/x 与
# mpifxcorr（test-mpi2.input），cmp_swin.py ... 4096 对拍 6/6
```

## 验收判据

- 两站 SWITCHEDPOWER 文件均生成且**前 2 个完整整秒窗行逐位全等**（mjd0/mjd1 14 位精度 + 4 个功率/误差 8 位精度，文本 diff 判据同 P0）；
- 时间窗说明：mpifxcorr EXECUTE TIME=2 截断（vdifmux 尾部多读 ~12ms）与 fxcorr batch 2.097s 的时间窗不同，**行数相等但尾窗不必相等**——共同覆盖的整秒窗行必须全等，尾窗验证时间戳起点一致、窗口长按各自数据终点、功率量级与前窗一致；
- 无 tcal 回归：SWIN 6/6（退化路径不漂移）；生成器帧头修复后位序对拍 BYTE-IDENTICAL（FXSIM_NOISE=0）。

## 验证记录（2026-09-13）

- 环境：/root/fxcortest/tcal/，test-tcal.v2d 全链路（.input TCAL FREQUENCY: 80 ×2 站），make_testdata 造数。
- **switched power 对拍**：mpifxcorr 基准（EXECUTE TIME=2）vs fxcorr-f 两站，**前 2 个完整整秒窗逐位全等**（两站 ds0/ds1 均 diff 无差异：mjd0/mjd1 14 位精度、powerOn/σOn/powerOff/σOff 8 位精度逐字符一致）；尾窗 mjd0 相同、窗口终点按各自数据尾（fxcorr 25202.0875s vs mpifxcorr 25202.0999s，mpifxcorr vdifmux 多读 12ms）、power 量级一致（1.5045/1.5071 vs 1.5046/1.5068）。
- **喂入节奏对拍生效证明**：喂入粒度（4×maxbytes 块）与 subint 重叠剔除后统计值与 mpifxcorr 逐位一致——若按 subint 喂或忽略重叠，sigma 必炸（半周期统计分组由块起点帧号决定）。
- **SWIN 回归 6/6**：生成器帧头修复后 fxcorr 链路与 mpifxcorr 基准 cmp_swin.py 6/6 记录全等（主路径不漂移）。
- **位序对拍回归**：FXSIM_NOISE=0 重新造数后 gen_test_vdif.py 与 fxcorr-sim 输出 BYTE-IDENTICAL（4208768 字节公共前缀全等，文件长差 1 帧为帧对齐取整语义）。
- **无 tcal 回归 6/6**：config 换回根目录 test.v2d 重新 vex2difx/difxcalc（TCAL FREQUENCY 行 0 条），f/x 与 mpifxcorr（test-mpi2.input）SWIN 对拍 6/6 全等（退化路径不漂移）。
- **实施中实测的坑（已入 fxcorr-f CLAUDE.md）**：① subint 读入含帧对齐 guard 重叠（sendbytes = 133 帧 vs subint 131.07 帧），直接拼块会导致块内帧号不连续、mark5access validate fail、统计全 blank——喂入必须跳过与上 subint 重叠的字节（datareader 暴露 getLastFileOffset）；② 生成器帧头两个 bug（legacy 位、字布局）见上方"实施中发现"。
