# 归档：band 并行与 ds×block 并行（2026-11-08 ~ 11-10）

> **归档说明（2026-11-15）**：历史记录，未被采用。当前口径是**单任务**
> （一个 (batch, ds 组)）。band 并行实测劣化 4.5×（413 s vs 91.9 s）；
> ds×block 并行未完成逐字节验证，默认关闭。保留仅为避免重复探索。

## 2026-11-08 xpp 分支与 band 并行

### 1. xpp 架子

- `applications/fxcorr-xpp` = `fxcorr-xp` 逐字复制（仅改名）；`install-difx` 4 处
  注册、`fxcorr/docker/Dockerfile` COPY 行已加；镜像重建后容器内与 xp 并存。
- **镜像内 xpp 默认路径与 xp 逐字节一致**：2 站 / 1.024 s / batch 00000001、
  OMP=8，md5 均 `82eb2c23…`（281,088 B），wall 20.87 s vs 20.91 s。

### 2. band 并行实现

- 落点 `libraries/fxcorrcommon`：`Mode::setBandParallel(n)`（默认关闭，其他工具
  行为/内存不变），每线程私有 `BandScratch`（旋转/frac/FFT 工作区/FFT plan）。
  zoom、linear2circular、单 band 数据流自动禁用。
- `fxcorr-xpp` 用环境变量 `FXCORR_XPP_BAND_THREADS=N` 启用；band 模式下 station
  的 ds 循环串行、band 循环作为顶层并行（`main.cpp` 已改为 lambda 双分支）。
- **修复的构建坑**：`fxcorrcommon/configure.ac` 的 `AX_OPENMP` 原先在
  `AC_LANG(C++)` 之前，只设置 `OPENMP_CFLAGS`，库从未带 `-fopenmp` 编译
  （`libraries/CLAUDE.md` 有记录）。
- 并行机制迭代：OpenMP 内层 `parallel for`（每 block 一次）在 GCC libgomp 下
  嵌套死锁/极慢；外层改 static 仍死锁；最终改为**持久线程池 `BandPool`**
  （每 Mode 一份 worker），运行时无嵌套 region。

### 3. 验证状态与局限

- 正确性：串行 vs band=8 的完整产物已对齐，SWIN md5 均 `82eb2c23…`（281,088 B）。
- 性能：band=8 墙钟 **413 s**（user 645 s、sys 695 s），比同批串行 **91.9 s** 慢
  4.5×。任务粒度是"每 block 每 band"，2 站 1.024 s 下约十万级任务提交，
  同步成本完全压过计算。band 路径保留为开关，不作为优化方向。
- 后续方向：block 并行（每线程独立 Mode 状态）或归约段并行化；XMAC 展平并行
  也在待办列表。

## 2026-11-08~10 方案 A：ds×block 组合并行（实验性，验证中）

### 1. 结果回顾与一次验证事故

- band 并行（任务 = 每 block 每 band）实测比串行慢 4.5×（413 s vs 91.9 s），
  每 block 一次提交的同步成本完全压过计算；保留为开关，不再作为优化方向。
- 块并行先后试过三种形态：
  1. 每 ds 一个 OpenMP 并行区、每 fftloop 归约（J1 B8 约 76 s，不能与 ds 并行叠加）；
  2. 自管 `SegmentPool`（std::thread）支持 ds×block 组合，条件变量唤醒在本机
     约 35 ms/次，wall 反而劣化；改自旋池后又引入确定性结果偏差；
  3. 大粒度（当前）：`Mode` 的频谱槽扩到整个 subint（槽号 = 全局块索引，
     `Mode::setSubloopCapacity`），每 subint 只处理一次、归约一次，去掉每
     fftloop 的同步。
- **验证事故（2026-11-10）**：`fxcorr/Makefile` 的 `make sync` 使用
  `rsync -az`，按大小+时间戳判断；本地与远端 mtime 相同时跳过传输。一段时间内
  远端 `main.cpp`/`mode.cpp` 一直是旧版本，多轮“回退/逐字节验证”实际都在旧
  二进制上执行（SWIN md5 恒定不变即为信号）。**此前记录的“块并行逐字节一致”
  结论不可靠，已作废**。`make sync` 已改为 `rsync -azc`（按内容比较），关键
  文件已强制同步。教训：同步后必须用 `md5sum` 核对源文件，不能只看 make 成功。

### 2. 当前实现（大粒度，默认关闭）

- `FXCORR_XPP_BLOCK_THREADS=N`（N>1）：
  - 每 ds 构造 N-1 个 Mode 副本；
  - 主 Mode 与副本的 `subloopCapacity` 扩到 `blockspersend`，`process(i, i)`
    把块 i 写进槽 i，不再需要 `blockSlotBase` 映射；
  - 每 subint 一次 `SegmentPool` 调度：每段处理 `blockspersend/N` 个连续块，
    段>0 把谱/数据权重拷回主 Mode；随后一次按块序归约（ac/weight/pcal/
    kurtosis）、一次 `collectBand(fftloop, true)`、一次 ac 批次；
  - ds 由 `OMP_NUM_THREADS` 并行，池任务不嵌套 OpenMP。
- 限制：`maxacblocks >= blockspersend`（否则回退串行）、无 zoom、pcal 提取器
  需支持原始累加器、`perbandweights` 未启用。
- 改动文件：`libraries/fxcorrcommon/src/mode.{h,cpp}`（`fftslots` /
  `setSubloopCapacity`）、`applications/fxcorr-xpp/src/{main,memreader}.*`。

### 3. 验证状态

- 大粒度版已编译通过（fxcorrcommon + xpp）。
- **尚未完成逐字节验证**：容器镜像内的 fxcorrcommon 缺少新符号，正在重建镜像；
  重建后需核对 batch 1/2 的 SWIN、`pcal.bin`、`PCAL_*` 与 xp 逐字节一致，并测
  OMP=1/16 × BLOCK=2/4/8。
- 在验证通过前，不要用块模式产出正式结果；xp 仍是唯一稳定参照。

