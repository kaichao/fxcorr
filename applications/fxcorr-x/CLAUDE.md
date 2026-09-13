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

## 文件与 mpifxcorr 对照

| 本目录 | 作用 | 对照源 |
|---|---|---|
| main.cpp | batch.json 解析、V1 边界检查、逐 subint 驱动（xcblockcount/maxxcblocks 批次、尾批、时间推进） | core.cpp:657-784（批控制）、:985-991 / :1055-1060（uvshift 触发）；fxmanager.cpp:650-698 的单 Visibility 串行替代 |
| spreader.{h,cpp} | .sp 读取：256 字节头校验 + 按 subint fseek 读头/flags/weights/spectra（线性 FFT 序） | 布局见 data-spec 5.3；对应 fxcorr-f 的 FEngineWriter |
| xmac.{h,cpp} | XMAC 批循环 + baselineweight 累加 + uvshiftAndAverage | core.cpp:867-982（删 pulsar/phased array）、:1005-1052、:1431-1938（单相位中心、单线程：无 rotator、无锁、无 decorr 段）；vis2 逐块 `vectorConj_cf32` 后 `vectorAddProduct_cf32`（等价 getConjugatedFreqs） |
| integrate.{h,cpp} | 单 Visibility（numvis=1）：addData 满 intTime → writedata → increment；autocorr.bin 逐批次累加进 results 自相关段/acweight 段 | fxmanager.cpp:168-185（构造+polnames）、:114-133（todiskbuffer 预算）；core.cpp:1260-1370（autocorr 累加，无 zoom 假设） |

## 关键实现要点（易错，改前必读）

- **results 三层结构**：subintresults（coreresultlength，每 subint 清零）→ XMAC/uvshift/自相关累加 → `Visibility::addData` 加进长积分。offset 体系（threadresult*/coreresult*）全部沿用 configuration 预算，**resultindex 累加顺序必须与 populateResultLengths 一致**（f→x→baseline→pol）。
- **uvshiftAndAverage 简化版**：nsoffset/nswidth 参数保留但单相位中心下不用（rotator/decorr 全跳过）；频谱平均段照 core.cpp:1829-1853（virtualplacement/bin 平均，非 d260 老版）。
- **自相关批次节奏**：autocorr.bin 每 subint 存 ac_batches=ceil(bps/maxacblocks) 条记录（f 侧每批次 averageFrequency+zero），x 侧必须**全部读入逐条累加**，与 mpifxcorr 的 averageAndSendAutocorrs 节奏一致——否则 weight 与数值都对不上。
- **weight 语义链**：.sp weights = 每 FFT 块 dataWeight（槽式回填，见 fxcorr-f CLAUDE.md）→ baselineweight = Σ weight1×weight2 → floatresults（bweightoffset×2 处）；autocorr weight = 各批次 getWeight 累加 → acweightoffset×2 处。writedata 内部除以 fftsperintegration 归一。
- **V1 启动边界检查**（main.cpp）：单 scan、单相位中心、maxproducts≤2、intTime 为 subintNS 整数倍、无 pulsar/phased array；batch 起点 subint 边界校验（容差 1µs，同 fxcorr-f）。
- **executeseconds 语义**（Visibility::writedata 停写判定）：executeseconds 以 **scan 起点**为基准（mpifxcorr EXECUTE TIME 语义），batch 起点偏移 initsec 时须 `executeseconds = batch时长 + initsec + 1`，否则 batch 起点非 scan 起点的 batch 全部静默不写盘（2026-09-12 修复，batch 起点 scan 起点时退化为原语义、对拍回归 6/6）。
- **mpifxcorr mux 滞后**：对拍时 mpifxcorr 数据后段会有确定性的 invalid 边界 subint（vdifmux 流式管线滞后，两次运行可复现），fxcorr 无此滞后——对拍 batch 取数据完整覆盖段（见 impl-plan 2.3 实施记录）。

## V1 边界

- 输入仅 fxcorr-f 的 .sp（无 zoom band 切片）；多相位中心/脉冲星/STA/PCAL 文件生成均不做（V2）。
- 无流式：整 batch 逐 subint 顺序读 .sp（每 subint 各站各 band 一次 fseek），谱数据按 subint 常驻（blockspersend×nchan cf32/站/band）。

## 构建与注册

- 模板照 fxcorr-f：configure.ac（PKG_CHECK_MODULES: fxcorrcommon + fftw3f）/ Makefile.am / src/Makefile.am。
- install-difx 注册 4 处（components、setNormalComponentsFalse、apptargets dompicxx=True、--doonly 帮助）。

## 测试

- 资产与对拍工具在 `fxcorr/test/`：cmp_swin.py（SWIN 逐记录比较，验收标准 2）。
- 测试机 /root/fxcortest/：f 侧产物 fengine/58948_25200/ → `fxcorr-x 58948_25200` → config/test.difx/DIFX_*.s0000.b0000；对拍 mpifxcorr 用 EXECUTE TIME 截断到完整积分段（test-mpi2.input，EXECUTE TIME=2）。
- 已验证：2 站 4 秒实验 4 subint（2 积分）SWIN 与 mpifxcorr 逐记录全等（6/6，可见度 <1e-6、weight 逐位一致）；difx2fits 出 FITS 成功；多 batch 第 2 个 batch（起点 = scan 起点 + 1.024s，test-sim 配置 8 subints）4 积分 12 条记录全链路跑通。
