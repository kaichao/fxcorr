# fxcorr 改造工作区

本目录是 fxcorr 改造的工作区：文档 + bash 编排脚本（**脚本直接放本目录，与 README / data-spec 平级，不再设 scripts/ 子目录**）。算法实现见 `applications/fxcorr-f`、`applications/fxcorr-x`，共享代码见 `libraries/fxcorrcommon`。

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

## 本目录脚本（规划）

| 脚本 | 作用 |
|---|---|
| `run_batch.sh` | 对单个 batch_id：依次调用各站 fxcorr-f，再调用 fxcorr-x |
| `watch_and_dispatch.sh` | 长驻/轮询 raw/，发现齐套时间窗后生成 batch_id 并调用 run_batch.sh |

调用示例（接口以实际实现为准）：

```bash
./fxcorr/run_batch.sh 60512_45000 STA1,STA2,STA3
```

## 相关指引

- 拆分缝隙（core.cpp processdata）与可复用清单：`mpifxcorr/CLAUDE.md`
- 依赖库与 fxcorrcommon 样板：`libraries/CLAUDE.md`
- 构建注册（install-difx 4 处）：根 `CLAUDE.md`
