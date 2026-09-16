# P3 多线程（OpenMP）性能资产

P3 = V3 唯一实现项（f/x 进程内 OpenMP 并行，algo-plan P3 节）。本目录放性能剖析与加速比验证资产。

## 资产

| 文件 | 作用 |
|---|---|
| `gen_test_multist.py` | N 站 vex/v2d/threads 生成器（N=2 与 test.vex/test.v2d 等价：T1 基准坐标、LO 2000、帧长联动带宽）。输出命名对齐 make_testdata.sh 约定（test.vex/test.v2d/threads 预生成到 config/ 后，make_testdata.sh 见资产存在即跳过复制，前处理→sim→软链→batch.json 链直接可用） |

用法（测试机）：

```bash
mkdir -p p3pro/config
python3 gen_test_multist.py p3pro/config -n 4          # 4 站 / bw=4MHz / dur=1200s / nChan=4096
cd p3pro/config && vex2difx test.v2d && difxcalc test.calc
cd .. && BATCH_NSUBINTS=116 make_testdata.sh .          # 61s batch（116×0.524288s，intTime 整数倍）
```

放大场景（第 6 步加速比用）：`-n 8`（28 基线）+ v2d nChan 32768（XMAC 计算 38×、读盘不变 → 计算占比 ~90%）；数据生成 `-p 8` 并行。

## 验证记录

### 第 2 步：单线程基线 + 热点采样（2026-09-15，测试机 8 核）

基准场景：4 站 / 4MHz / nChan 4096 / 61s batch（116 subints、58 积分、6 基线）。GNU time 基线：

| 阶段 | wall | user | sys | 峰值内存 |
|---|---|---|---|---|
| fxcorr-f（每站） | 7.8s | 6.3s | 1.5s | 10MB |
| fxcorr-x | 7.2s | 5.1s | 2.0s | 72MB |

gdb 采样热点（sample.sh：attach + bt 统计；无 perf，测试机 gdb 可用）：

- **f 侧**（40 次×6 帧）：条纹旋转 apply/apply_dit 25、FFT（genFFT_CtoC_cf32+n1fv_16）14、fwrite 12、Mk5Mode::unpack 4 → **f 侧并行范围 = 块循环整体（条纹旋转 + FFT），不限于 FFT**
- **x 侧**（60 次×3 帧，带行号）：读盘 fread/read 26、XMAC（genericAddProduct_32fc/genericConj_32fc/xmacBatch 内循环）~34 → **x 侧 = 读 .sp 与 XMAC 大致对半**（4MHz/4096 配置）；nChan 32768 放大后 XMAC 占比 ~90%（Amdahl 上限 ~8×）

结论：f 侧 CPU 占 81%、x 侧 71%，并行空间存在；基线耗时（f 7.8s/站、x 7.2s）为第 6 步加速比分母。

### 第 4/5 步：P3 实现对拍（2026-09-15）

4 站 61s batch（58948_25200）逐位对拍（cmp_swin.py，SWIN 记录 580 条）：

| 场景 | f | x | 结果 |
|---|---|---|---|
| 串行（未设 OMP_NUM_THREADS，改造后） | 串行 | 串行 | 580/580 全等（vs 改造前 SWIN） |
| OMP_NUM_THREADS=4 | 4 线程 | 4 线程 | 580/580 逐位全等 |

性能（4MHz/4096 配置，I/O 主导）：x 侧 4/8 线程 1.76×/2.09×（Amdahl 上限 ~2×）；f 侧写盘 ~40% 串行，加速有限。

**放大场景加速比（8 站 28 基线 + nChan 32768、61s batch，/root/fxcortest/p3big，bench.sh 三组）**：

| 侧 | 串行 | OMP 4 | OMP 8 | 受限因素 |
|---|---|---|---|---|
| f（每站） | 8.8s | 4.1s（2.15×） | 4.0s（2.2×，见顶） | 读 122MB + 写 2GB .sp 串行 I/O |
| x | 30.0s | 13.3s（2.26×） | 10.3s（2.92×） | 读 7.8GB .sp 串行 + uvshift 串行段；user 含 spin 浪费（passive 不改善 wall） |

对拍：4/8 线程全链路 SWIN 2088/2088 逐位全等（vs 串行）；4 站配置 580/580 同证。进一步提速需异步写盘/预读（P3 范围外）。

### 坑

- `cmd1 && cmd2 & PID=$!` 的 `&` 把整条 && 链后台化，`$!` 是子 shell PID——采样脚本须把被测程序单独 `"$BIN" "$@" &`（sample.sh 已按此写）。
- **每 fftloop 一个并行区不可行**（f 侧）：fork/join 开销吃光收益（gdb 采样 gomp_barrier 主导、user 6×串行），须每 subint 一个并行区 + 区内 barrier。
- **AC_OPENMP([CXX]) 探测的是 C 编译器**（configure 输出 "gcc option to support OpenMP"、CXXFLAGS 无 -fopenmp），autoconf 须 `AC_LANG_PUSH([C++])` + `AC_OPENMP` + `AC_LANG_POP([C++])`。
- **SWIN 追加语义**：重跑 x 对拍前必须清 vis 输出目录（多次运行记录叠加：3 次运行 = 3×580 条，cmp 记录数对不上）。
