# fxcorr-x 目录说明

baseline-based 相关器后端（X-Engine）：无 MPI 串行程序，读 fxcorr-f 的 .sp / autocorr.bin 产物，做 XMAC 与长期积分，可见度直出 SWIN（`vis/<experiment>.difx/`）。算法照 mpifxcorr 的 `Core::processdata()` 切分移植，写盘复用 fxcorrcommon 的 Visibility（零改造）。

## 调用方式

```
fxcorr-x <batch_id> [workdir]
```

- `workdir` 定位：位置参数 > 环境变量 `FXCORR_WORKDIR` > 默认 `.`。
- 读 `workdir/batches/<batch_id>.json`（run_batch.sh 预写），取 start_mjd / n_subints / config_file / difx_dir。
- 数据源 `workdir/fengine/<batch_id>/<station>/`（band_XX.sp + autocorr.bin），station 列表即 .input 的全部 datastream。
- 输出目录由 **.input 的 OUTPUT FILENAME** 决定（SWIN 写盘沿用 config 语义，difx2fits 零改造），batch.json 的 difx_dir 仅为元数据。
- **DifxMessage 状态发送**（algo-plan P1，difxmonitor 封装）：mpiId = 0（manager 角色），identifier = .input basename。节奏：Starting → 每积分写盘一条 Running（Integrator::sendRunning，writedata 后、increment 前——increment 清零 floatresults，时序同上游 fxmanager loopwrite；weight 照抄 visibility.cpp:1100-1146，f32 截断点一致，对拍逐位一致）→ Ending → Done；错误路径 Alert + Aborting（fail helper）。host 模式组播（DIFX_MESSAGE_GROUP/PORT 未设即静默）；`FXCORR_RUN_MODE=container` 落盘 `meta/difxmsg/<exp>_<batch>.xml`（构造时截断，重跑幂等）。

## 文件与 mpifxcorr 对照

| 本目录 | 作用 | 对照源 |
|---|---|---|
| main.cpp | batch.json 解析、V1 边界检查、逐 subint 驱动（xcblockcount/maxxcblocks 批次、尾批、时间推进） | core.cpp:657-784（批控制）、:985-991 / :1055-1060（uvshift 触发）；fxmanager.cpp:650-698 的单 Visibility 串行替代 |
| spreader.{h,cpp} | .sp 读取：256 字节头校验 + 按 subint fseek 读头/flags/weights/spectra（线性 FFT 序）；**zoom 切片视图（P4a 2026-09-13）**：构造参数 (channeloffset, nchanoverride) 时每 FFT 块只读父 .sp 的切片段，spectra()/numChannels() 语义不变 | 布局见 data-spec 5.3；对应 fxcorr-f 的 FEngineWriter |
| xmac.{h,cpp} | XMAC 批循环 + baselineweight 累加 + uvshiftAndAverage | core.cpp:867-982（删 pulsar/phased array）、:1005-1052、:1431-1938（单相位中心、单线程：无 rotator、无锁、无 decorr 段）；vis2 逐块 `vectorConj_cf32` 后 `vectorAddProduct_cf32`（等价 getConjugatedFreqs）；zoom band 由 config 表驱动零改动（band index = ds total 序，readers 已含 zoom 视图） |
| integrate.{h,cpp} | 单 Visibility（numvis=1）：addData 满 intTime → writedata → increment；autocorr.bin 逐批次累加进 results 自相关段/acweight 段 | fxmanager.cpp:168-185（构造+polnames）、:114-133（todiskbuffer 预算）；core.cpp:1260-1370（autocorr 累加，**P4a 2026-09-13 起覆盖 total bands**：nbands 校验 getDNumTotalBands、freqindex 用 getDTotalFreqIndex、acbuf 取最大 nchan，zoom 的 weight 已是 f 侧换算好的父 band 值） |

## 关键实现要点（易错，改前必读）

- **results 三层结构**：subintresults（coreresultlength，每 subint 清零）→ XMAC/uvshift/自相关累加 → `Visibility::addData` 加进长积分。offset 体系（threadresult*/coreresult*）全部沿用 configuration 预算，**resultindex 累加顺序必须与 populateResultLengths 一致**（f→x→baseline→pol）。
- **uvshiftAndAverage 简化版**：nsoffset/nswidth 参数保留但单相位中心下不用（rotator/decorr 全跳过）；频谱平均段照 core.cpp:1829-1853（virtualplacement/bin 平均，非 d260 老版）。
- **自相关批次节奏**：autocorr.bin 每 subint 存 ac_batches=ceil(bps/maxacblocks) 条记录（f 侧每批次 averageFrequency+zero），x 侧必须**全部读入逐条累加**，与 mpifxcorr 的 averageAndSendAutocorrs 节奏一致——否则 weight 与数值都对不上。
- **zoom 两条易错点（P4a，2026-09-13 实测教训）**：① x 侧**每 subint 的时间戳校验循环必须遍历全部 band 视图**（readers[ds].size()，含 zoom 视图）——只读 recorded 时 zoom 视图的 specbuf 恒 0、互相关全零而自相关正常（f 侧 autocorr.bin 独立落盘），首轮对拍 zoom 段 max rel 1.0 即此因；② BASELINE 段的 `D/STREAM A/B BAND` 行 key 序号是 **pol product 序号**不是 freq 序号（configuration.cpp:1016-1019 按 k 取行），自造 .input 时写错会导致 zoom band 静默解析为 0 且无报错。
- **weight 语义链**：.sp weights = 每 FFT 块 dataWeight（槽式回填，见 fxcorr-f CLAUDE.md）→ baselineweight = Σ weight1×weight2 → floatresults（bweightoffset×2 处）；autocorr weight = 各批次 getWeight 累加 → acweightoffset×2 处。writedata 内部除以 fftsperintegration 归一。
- **V1 启动边界检查**（main.cpp）：单 scan、单相位中心、maxproducts≤2、intTime 为 subintNS 整数倍、无 pulsar/phased array；batch 起点 subint 边界校验（容差 1µs，同 fxcorr-f）。
- **executeseconds 语义**（Visibility::writedata 停写判定）：executeseconds 以 **scan 起点**为基准（mpifxcorr EXECUTE TIME 语义），batch 起点偏移 initsec 时须 `executeseconds = batch时长 + initsec + 1`，否则 batch 起点非 scan 起点的 batch 全部静默不写盘（2026-09-12 修复，batch 起点 scan 起点时退化为原语义、对拍回归 6/6）。
- **mpifxcorr mux 滞后**：对拍时 mpifxcorr 数据后段会有确定性的 invalid 边界 subint（vdifmux 流式管线滞后，两次运行可复现），fxcorr 无此滞后——对拍 batch 取数据完整覆盖段（见 impl-plan 2.3 实施记录）。

## V1 边界

- 输入仅 fxcorr-f 的 .sp（**zoom band 已支持（P4a 2026-09-13）**：x 侧按 .input 的 zoom 定义对父 .sp 做通道切片视图，无新文件）；多相位中心/脉冲星/STA/PCAL 文件生成均不做（V2）。
- 无流式：整 batch 逐 subint 顺序读 .sp（每 subint 各站各 band 一次 fseek），谱数据按 subint 常驻（blockspersend×nchan cf32/站/band）。

## 构建与注册

- 模板照 fxcorr-f：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f）/ Makefile.am / src/Makefile.am。
- install-difx 注册 4 处（components、setNormalComponentsFalse、apptargets dompicxx=True、--doonly 帮助）。

## 测试

- 资产与对拍工具在 `fxcorr/test/`：cmp_swin.py（SWIN 逐记录比较，验收标准 2）；**zoom 检验资产（P4a）**：gen_test_zoom.py（从 test.input 生成 test-zoom.input + EXECUTE TIME 截断变体）、cmp_swin_zoom.py（按 SWIN 头 freqindex 分拆多 nchan 对拍）。
- 测试机 /root/fxcortest/：f 侧产物 fengine/58948_25200/ → `fxcorr-x 58948_25200` → config/test.difx/DIFX_*.s0000.b0000；对拍 mpifxcorr 用 EXECUTE TIME 截断到完整积分段（test-mpi2.input，EXECUTE TIME=2）。
- 已验证：2 站 4 秒实验 4 subint（2 积分）SWIN 与 mpifxcorr 逐记录全等（6/6，可见度 <1e-6、weight 逐位一致）；difx2fits 出 FITS 成功；多 batch 第 2 个 batch（起点 = scan 起点 + 1.024s，test-sim 配置 8 subints）4 积分 12 条记录全链路跑通。

**zoom（P4a）检验步骤**（2026-09-13 验证过，测试机可一键复现）：

```bash
cd /root/fxcortest && rm -rf zoom && mkdir zoom
bash /root/fxcorr/fxcorr/make_testdata.sh zoom 1.5     # 单 band 只能 1 个 tone（tone 落 zoom 中心）
cd zoom
python3 /root/fxcorr/fxcorr/test/gen_test_zoom.py config/test.input
#   -> config/test-zoom.input（父 4MHz/4096ch + zoom 201.5MHz 1MHz/1024ch）
#   -> config/test-zoom-mpi2.input（EXECUTE TIME=2 截断，mpifxcorr 基准用）
python3 - <<'EOF'
import json
j = json.load(open('batches/58948_25200.json'))
j['config_file'] = 'config/test-zoom.input'
json.dump(j, open('batches/58948_25200.json','w'), indent=2)
EOF
# fxcorr 链路
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200'
mv config/test.difx/DIFX_58948_025200.s0000.b0000 /tmp/zoom_fxcorr.s0000.b0000
# mpifxcorr 基准
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  mpirun --allow-run-as-root -np 4 /usr/local/difx/bin/mpifxcorr config/test-zoom-mpi2.input'
# 对拍（按 freqindex 分拆：主带 4096ch、zoom 1024ch）
python3 /root/fxcorr/fxcorr/test/cmp_swin_zoom.py \
  /tmp/zoom_fxcorr.s0000.b0000 config/test.difx/DIFX_58948_025200.s0000.b0000 0=4096,1=1024
# 无 zoom 回归：batch.json config_file 改回 config/test.input，rm -rf fengine config/test.difx，
# 重跑 f/x 与 mpifxcorr（test-mpi2.input），cmp_swin.py ... 4096 对拍 6/6
```

验收判据：zoom 对拍 12/12 记录 ALL OK（主带 6 + zoom 6，头字段含 weight/uvw 与可见度 max rel ≤1e-6）；无 zoom 回归 6/6。
