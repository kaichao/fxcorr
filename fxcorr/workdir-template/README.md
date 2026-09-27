# fxcorr workdir 模板

**最后更新**：2026-09-27（前处理已在 `ssh difx` 上验证）

一个 fxcorr workdir 的**实物骨架**——十个目录都建了出来（`config/` 有配置实物，
其余各一份 README 说明"里面有什么"），**不含任何仿真或观测数据**。三个用途，
一份资产：

1. **`fxcorr/v6-plan.md` S0 的起点**（单 batch 端到端实测的两次跑）；
2. **S3 规模验证的基准**（4 站仿真）；
3. **`fxcorr/data-spec.md` 布局的可执行样本**——布局此前只有文字描述，
   仓库里没有一份能看到的实物（全在测试机上）。

运行时数据一律不进 git；各目录的 README 是**读物**（`gen_vex.py --install` 只装
`config/`），真实 workdir 的目录骨架由 `fxcorr/roots.sh` 的 `fxcorr_mkroots` 建。

## 目录

| 路径 | 内容 |
|---|---|
| `gen_vex.py` | 生成器：base → 4 站 → mini 变体；也负责装进 workdir（见「用法」） |
| `config/t25362-base.{vex,v2d}` | **t25362 真实观测的原配置**（2 站），生成器的输入，逐字取自 `ssh difx` |
| `config/t25362-4st.{vex,v2d}` | 4 站、**完整跨度**——S0 第②次跑、S3 基准 |
| `config/t25362-4st-mini.{vex,v2d}` | 4 站、**跨度压到连续**——S0 第①次跑 |
| `config/data/filelist_*.t[1-8]` | 32 个 filelist（4 站 × 8 ds），`gen_vex.py` 生成 |
| `<目录>/README.md` | 十个目录各一份，讲"里面有什么"——见下一节 |

## 各目录的说明

模板把 workdir 的**全部目录**都建了出来，每个目录里一份 README，讲"这里面有什么"：

| 目录 | 一句话 | README 的重点 |
|---|---|---|
| `config/` | 配置与模型（**有实物**：三份配置 + 32 个 filelist） | 前处理链、filelist 的作用、`.input` 里三个决定运行时的字段 |
| `batches/` | 批量元数据（D9） | batch.json 全字段、`stations`/`baselines` 是**逐条展开**而非去重 |
| `raw/` | 原始基带（D7） | 命名、DATA TABLE 软链、`_ds<N>` 的三处同口径、时间语义三坑 |
| `fengine/` | F 频域谱（D8） | 三层编号各回答什么、为什么必须落 tmpfs |
| `vis/` | SWIN 可见度（D10） | 追加顺序为什么不能乱、两个被并发写的文本文件 |
| `vis-parts/` | 分片局部记录（D16） | 为什么与 vis/ 隔离、为什么不新增根、ds 组怎么分 |
| `beam/` | 波束输出（D14，相位阵） | 与互相关模式互斥 |
| `product/` | FITS 产品（D11/D12） | 实验级操作、产物名不受 fxcorr 控制 |
| `meta/` | 索引与根记录 | `batches.index` 与 `roots/*.json` 的格式 |

**除 `config/` 外都是空的**——运行时数据不进 git（空目录也不行，所以每个里面
只有一份 README）。这些 README 是**读物**：`gen_vex.py --install` 只装
`config/`，不装它们；workdir 的目录骨架由 `fxcorr/roots.sh` 的 `fxcorr_mkroots` 建。

**与 `fxcorr/data-spec.md` 的分工**：那边是**规范**（格式定义、约束、跨目录规则），
这边是**实物与逐目录说明**。「这个目录长什么样、里面有哪些文件」看这里，
「为什么必须这样」看那边。

## 三个配置的对照

| | base | 4st | 4st-mini |
|---|---|---|---|
| 站 | BA, S6 | BA, **BX**, S6, **SX** | 同 4st |
| datastream | 16（每站 8） | 32 | 32 |
| 唯一频率 | 32 | 32 | 32 |
| 频率范围（MHz） | 2936.40–9976.40 | 同 base | **2936.40–3928.40** |
| **跨度** | **7072 MHz** | 7072 MHz | **1024 MHz** |
| `chan_def` 数 = recorded band | 64 | 64 | 64 |
| 通道带宽 / `NUM CHANNELS` | 32 MHz / 128 | 同 | 同 |
| `INT TIME` / `SUBINT` | 1.024 s / 5.12 ms | 同 | 同 |
| 前处理产物 | `T25362_1.input` | `t25362-4st_1.input` | `t25362-4st-mini_1.input` |

**跨度的口径是 `max(freq + bw) − min(freq)`**——fxcorr-sim 的 `deriveGrid` 定义
（`applications/fxcorr-sim/src/commonsignal.cpp`），**不是** `max(freq) − min(freq)`
（那给出 7040 / 992）。带宽那一项不能漏：**网格分辨率**由它决定（`deriveGrid` 的
候选筛选与它绑定），进而决定每帧占几个 slice。

**mini 跨度**把 32 个唯一频率重排到 2936.40 起的连续网格（间隔 = 32 MHz 带宽），
`$IF` 的 `if_freq` 不动——实测两个变体都能过 `vex2difx` + `difxcalc`。这一条是
`fxcorr/data-volume.md` §6 的**杠杆 1**（降公共信号的覆盖跨度——V6 S2.5 之后它不再落盘，但各站
仍要按跨度跑一遍生成器，所以这条杠杆依然成立），S0 第①次跑用它先暴露流程问题。

## 4 站的造法与其后果

`$SITE` / `$ANTENNA` **复制自 BA / S6 并改名**（`BX` 复制 `BA`、`SX` 复制 `S6`），
**`site_position` 逐字照抄**——delay 结构已知、可控。由此带两条要知道的后果：

- **BA–BX、S6–SX 两条基线的几何 delay 与钟差恒为 0**（坐标与 `clock_early` 都相同）。
  它们仍会正常出现在 SWIN 里，只是没有条纹；要非零 delay 就改 `$SITE` 的
  `site_position`（生成器里改一处即可）。
- **`difxcalc` 对新站名报 `No ocean pole tide loading coefficients for BX/SX`**——
  海潮负荷表按站名查，新名字查不到。只是警告，两站坐标本就取自已有的站。

`$SITE`/`$ANTENNA` 用**新名字**而不是让两站共用 BA 的定义，是因为同名 site 被两站共用时，
`vex2difx` 若按 site 名索引会把两站当成同一个。

## 用法

```bash
# ① 装进一个 workdir（现有脚本认死 config/test.{vex,v2d} 这个名字，见下）
./gen_vex.py --install /data/scalebox/s0run/mini t25362-4st-mini
./gen_vex.py --install /data/scalebox/s0run/full t25362-4st

# ② 造数 + 跑批（在 workdir 内；见 fxcorr/CLAUDE.md 的脚本表）
BATCH_NSUBINTS=200 ./fxcorr/make_testdata.sh .     # 200 × 5.12 ms = 1.024 s
./fxcorr/run_batch.sh <batch_id> .
```

`--install` 之所以要改名，是因为 `fxcorr/make_testdata.sh` 与 `fxcorr/wrap_vex2difx.sh` /
`fxcorr/wrap_difxcalc.sh` 认死 `test.` 这个前缀（约四处）——变体名只活在模板里。要改成可配的
前缀是另一件事，S0 不依赖它。

**`BATCH_NSUBINTS=200` 是 S0 的关键参数**：默认值 4 只给 20.48 ms，而 t25362 的
`SUBINT NANOSECONDS` 是 5.12 ms——要 1.024 s 的 batch（与 `fxcorr/data-volume.md` §4.1 的
推算表同参）就得 200 个 subint。

## 实测记录（2026-09-27，`ssh difx:/data/scalebox/s0probe/`）

前处理链 `vex2difx` → `difxcalc` 对两个变体各跑一遍，结果：

| 检查项 | 4st | 4st-mini | 期望 |
|---|---|---|---|
| `TELESCOPE INDEX` 数 | 32 | 32 | 4 站 × 8 ds ✓ |
| `TELESCOPE NAME` | BA, BX, S6, SX | 同 | ✓ |
| `ACTIVE BASELINES` 数 | 96 | 96 | 6 站对 × 4 频段组 × 4 极化 ✓ |
| `FILE` 数 | 32 | 32 | ✓，且是**相对名**（`BA_ds0.vdif`）✓ |
| `INT TIME` / `SUBINT` | 1.024 / 5.12 ms | 同 | ✓ |
| `DATA FRAME SIZE` / `NUM CHANNELS` | 8032 / 128 | 同 | ✓ |
| `.im` 生成 | 20935 B | 20935 B | ✓ |

两条**无害警告**（不影响 S0 的体量实测）：

1. `Warning: fewer than nDatastream+1 machines specified in .v2d file`——`machines`
   行写了 32 个（与 datastream 数相同），而 vex2difx 想要 +1。**base 同样是 16/16，
   是既有形态**；fxcorr 不用 vex2difx 的机器分配，故不动。
2. `No ocean pole tide loading coefficients for BX/SX`（见上）。

**`FILE` 行的顺序**（= datastream 序 = `$STATION` 序，不是 v2d 里 `DATASTREAM` 行的
顺序）：`0-7` BA、`8-15` BX、`16-23` S6、`24-31` SX。`fxcorr/make_testdata.sh` 与 `fxcorr/run_batch.sh`
都按 `.input` 的顺序解析，所以两处一致即可——**v2d 里 BX/SX 的块追加在末尾、而 vex 里
插在各自模板站之后**，这个差异是有意的（站序跟着 `$STATION`）。

## 与文档的对应

| 文档 | 关系 |
|---|---|
| `fxcorr/v6-plan.md` | **S0** 用本目录的两个变体跑两次；S3 用 4st |
| `fxcorr/data-volume.md` | §4.1/§4.2 的推算表按"4 站 t25362 参数"，本目录是它的配置来源；§6 杠杆 1 = mini 变体 |
| `fxcorr/data-spec.md` | 本目录的 `config/` 是布局里那一份的可执行样本；filelist 的路径形态决定 `.input` 的 `DATA TABLE`（见 `data-spec` 5.2） |
| `fxcorr/CLAUDE.md` | 脚本表的 `fxcorr/make_testdata.sh` / `fxcorr/run_batch.sh` 条目 |
