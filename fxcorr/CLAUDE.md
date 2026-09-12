# fxcorr 改造工作区

本目录是 fxcorr 改造的工作区：文档 + bash 编排脚本（**脚本直接放本目录，与 README / data-spec 平级，不再设 scripts/ 子目录**）+ test/ 测试资产。算法实现见 `applications/fxcorr-f`（已建成，见其 CLAUDE.md）、`applications/fxcorr-x`（已建成，见其 CLAUDE.md），仿真数据生成器见 `applications/fxcorr-sim`（已建成，见其 CLAUDE.md），共享代码见 `libraries/fxcorrcommon`（已建成）。

## 文档分工

- **README.md**：需求（R1-R7）、总体架构、实现阶段（V1/V2/V3）、设计要点。改架构/需求时改它。
- **data-spec.md**：数据规范（版本 1.1）—— 目录布局、D1-D13 数据类型、模块 I/O、band_XX.sp / pcal.bin / autocorr.bin / SWIN 二进制格式、时间轴与通道/偏振映射、切批约束。**改数据接口/文件格式时必须先同步它**。
- **impl-plan.md**：V1 实施方案——组件源码清单、core.cpp 切分落点、datareader 改造、install-difx 注册、验收标准。改实施步骤时改它。

## 核心约定（源自 data-spec.md）

- **batch_id**：`60512_45000`（MJD+秒，推荐）或 `20260908_123000`（紧凑日期时间）；全局唯一，字符串排序即时间顺序；同一 batch 在 fengine/ 与 vis/ 用相同 batch_id。
- **目录**：`config/`（.vex/.v2d/.input/.calc/.im/.flag）、`raw/<station>/`（TB 级原始基带）、`fengine/<batch_id>/<station>/`（band_XX.sp 复数频谱 + pcal.bin + autocorr.bin，二进制布局见 data-spec 5.3）、`vis/<experiment>.difx/`（SWIN 文件集，跨 batch 追加）+ `vis/<batch_id>/batch.json`、`product/`、`meta/`、`work/`（临时）。均不进 git。
- **batch.json**：fengine/ 版字段 batch_id / start_mjd / start_time / duration_sec / stations / config_file / calc_file / im_file / n_subints / subint_ns / status / fxcorr_f_version；vis/ 版增加 baselines / integration_sec / n_channels / polarizations / difx_dir。
- **数据流**：vex2difx + difxcalc（实验级一次）→ fxcorr-f × 各站（D3+D4+D6+D7 → D8+D9）→ fxcorr-x（D3+D4+D6+D8+D9 → D10+D9）→ difx2fits / difx2mark4（按需）。
- **三个敲定决策**：UVW 由 fxcorr-x 读 .calc/.im 求值（D1）；可见度直出 SWIN、difx2fits 零改造（D2）；偏振是 band 属性、偏振组合在 x 侧按 BASELINE TABLE 选（D3）。V1 边界与实施步骤见 impl-plan.md。
- **SWIN 输出目录**：由 .input 的 OUTPUT FILENAME 决定（Visibility 写盘语义，difx2fits 零改造的前提），batch.json 的 difx_dir 仅为元数据。

## 本目录脚本（规划，未实现）

| 脚本 | 作用 |
|---|---|
| `make_testdata.sh` | 构建 data-spec 布局的标准测试数据（前处理 + 仿真 VDIF + 两版 batch.json），支持多 batch |
| `run_bench.sh` | difx 原命令基准：mpifxcorr 固化流程出基准 SWIN 供 cmp_swin.py 对拍 |
| `run_batch.sh` | fxcorr 流水线：校验对齐 → 写 batch.json → 逐站 fxcorr-f → fxcorr-x → 更新 meta/batches.index |

`watch_and_dispatch.sh` 已砍（V1 静态数据集无轮询场景）；流式监视与多节点调度 V2 由 scalebox 承担，容器化同列 V2（scalebox Module 需容器镜像）。

调用示例（接口以实际实现为准）：

```bash
./fxcorr/make_testdata.sh                 # 一次性构建标准测试数据
./fxcorr/run_bench.sh                     # 出基准 SWIN（对拍基准）
./fxcorr/run_batch.sh 60512_45000 STA1,STA2,STA3
```

## test/ 测试资产

| 文件 | 作用 |
|---|---|
| `test.vex` | 上游 `tests/Synthetic/test-usb.vex` 原版（2 站 T1/T2、单 band 4MHz USB、2bit、2020y100d07h00m00s） |
| `test.v2d` | 配套 vex2difx 配置（antennas=T1,T2，tInt=1，nChan=4096） |
| `gen_test_vdif.py` | 生成 2bit 单 band VDIF 测试数据（datasim 因上游 IPP 依赖无法 --noipp 构建，此脚本替代；**低位先打包**对齐 mark5access 位序；fxcorr-sim 的位序逐字节对拍参照，对拍已验证 BYTE-IDENTICAL；帧号公式已修为 `n % fps`（原 `n % 8000000 // 32000` 恒为 0，对拍时发现）） |
| `test2b.vex` / `test2b.v2d` | 2 band 测试配置（test.vex 加 205MHz 第 2 band），fxcorr-sim 多 band 验证资产 |
| `cmp_swin.py` | SWIN 逐记录比较（74 字节头 + 可见度复数），对拍工具（impl-plan 验收标准 2） |
| `make_testdata.sh` | 数据构建脚本（规划，见上方脚本表） |
| `testdata-min/` | 最小数据集（规划）：对拍最小子集 + sha256 入仓库，待 2 秒配置对拍实测干净后定 |

测试流程（测试机 /root/fxcortest/）：`vex2difx test.v2d` → `difxcalc test.calc`（出 .input/.calc/.im）→ 数据生成两种方式：**fxcorr-sim**（读 batch.json 生成 `raw/<station>/<station>_<batch_id>.vdif`，软链到 .input DATA TABLE 文件名）或 `gen_test_vdif.py TEST1.vdif 4 1.5 8`（对拍参照）→ 写 `fengine/<batch_id>/batch.json`（start_mjd 用精确 repr，否则 fxcorr-f 对齐校验报错）→ `fxcorr-f <batch_id> T1` / `T2` → 写 `vis/<batch_id>/batch.json`（x 版）→ `fxcorr-x <batch_id>` → `cmp_swin.py` 与 mpifxcorr 对拍。已验证（2026-09-12，fxcorr-sim 数据）：tone 峰落 1.5/1.0MHz 通道；SWIN 对拍 6/6 记录全等（可见度 <1e-6、weight 逐位一致）；difx2fits 出 FITS；pcal 链路 4 tones 检出；2 band（test2b）全链路跑通（mpifxcorr 读不了 2 band VDIF，2 band 对拍以物理验证为准，详见 applications/fxcorr-sim/CLAUDE.md）。对拍注意 mpifxcorr 的 mux 滞后（impl-plan 2.3 实施记录）。

## 相关指引

- 拆分缝隙（core.cpp processdata）与可复用清单：`mpifxcorr/CLAUDE.md`
- 依赖库与 fxcorrcommon 样板：`libraries/CLAUDE.md`
- 构建注册（install-difx 4 处）：根 `CLAUDE.md`
