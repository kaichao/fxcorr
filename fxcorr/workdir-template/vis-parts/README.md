# vis-parts/ —— 分片局部记录（D16）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 5.9；分组实测见 `fxcorr/data-volume.md` §7.3。

| 属性 | |
|---|---|
| **根变量** | **无**——恒在 `$FXCORR_WORKDIR/vis-parts`，但**必须全局可见** |
| **生产者** | `fxcorr-x` 的分片任务（一个 `(batch, ds 组)` 一份） |
| **消费者** | `fxcorr-x merge`（batch 级，读 `ds*.part`）；`merge --experiment`（实验级，读 `merged.part`） |
| **生命周期** | 实验级 merge 写出 SWIN 后即删 |
| **量级** | ≈ SWIN（KB 级 / batch） |
| **共享 / 本地** | **必须全局共享**（见下） |

## 目录结构

```
vis-parts/
└── 00000001/                 # batch_id
    ├── ds0.part              # 一个 ds 组的产出（跨站、含全极化）
    ├── ds1.part
    ├── ...
    └── merged.part           # batch 级归并产物（实验级 merge 的输入）
```

`.part` **就是 SWIN 记录流原样**——74 字节记录头 + `cf32` 频谱，与 `DIFX_*`
逐字节同构（记录头的字段表见 `../vis/README.md`）。`merge` 只搬运、不解释语义
（记录长度不在头里，由 `freqindex` 查 `.input` 的 FREQ 表得到）。

**命名即隔离**：`.part` 的 glob 是 `ds*.part`，与 `merged.part` 不冲突。

## ⚠ 为什么必须与 vis/ 分开

`difx2fits` 只 glob `OUTPUT FILENAME` 目录下**以 `DIFX` 开头**的文件
（`fitsUV.c:82-98`）。`.part` 既不放进 `vis/`、也不用 `DIFX` 前缀——**两重隔离**，
杜绝被误当正式 SWIN 读入。

`ds<G>.part` 这个命名是 fxcorr 自定的：**DiFX 生态里不存在"可见度分片"这个概念**，
没有既有约定要遵守，只需避免与 `DIFX*` 撞名。

## ⚠ 为什么不给它一个独立根

它与 `config/` `batches/` `meta/` 同类：**量小、必须全局一致可见、独立重定向
只有坏处**。多节点下的可见性靠 **`FXCORR_WORKDIR` 指向共享存储**实现——
由此推出一条部署硬约束：

> **`FXCORR_WORKDIR`（含 `config/` `batches/` `meta/` `vis-parts/`）必须落在
> 全局共享存储上**；本地根只有 `RAW` / `FENGINE` 两个（`SIM_COMMON` 已随 V6 S2.5 取消）。

放本地盘的代价：实验级 merge 要读**跨数百个节点的全部 batch 产物**，就得由
编排层逐节点搬运——而 24 h 观测总共才十几 GB（84,375 batch × 0.14 MB ≈ 11.8 GB），
搬它没有意义。

## ds 组怎么分（硬约束）

从 **`.input` 的 BASELINE TABLE** 推导——"覆盖同一频段组的那些 baseline 条目
所涉及的 ds 集合"，**不是按 ds 序号猜**：

- 每条 baseline 条目绑定一对**具体**的 ds、只出一个极化产品（极化维度被展开
  进 baseline 编号），所以**按序号配对会配错**；
- 只含单极化的分片会丢掉该频段的 `RL`/`LR`/`LL`；
- t25362 实测：4 组、每组 4 个 ds（2 站 × 2 极化）；4 站配置：4 组、每组 8 个。

同一 batch 内数据齐备（f 本就覆盖该时段全部站的全部 ds），所以只要分组正确，
该频段声明的 baseline 都能算出。

## 缺片处理

**两个层级各查各的**，判据不同但逃生口同一个（`FXCORR_X_MERGE_FORCE=1`）：

- **batch 级**（`merge <batch_id>`，现状）：启动时先核对本 batch 的 ds 组是否集齐，
  **缺则报错退出、不写任何东西**。强制写出时缺失组对应的频段静默缺段，只在 stderr 留痕。
- **实验级**（`merge --experiment`）：核对**本实验的全部 batch** 是否都已有
  `merged.part`，判据是 `batches/*.json`（**不是** `meta/batches.index`——那是 append-only
  的完成流水，会让 `failed` 与未调度的 batch 静默消失），见 `data-spec` 5.9 末条第 2 条。

默认严格是刻意的：分片缺失通常意味着任务失败或未调度，静默缺段比报错危险得多。

## 相关

- 规范：`fxcorr/data-spec.md` 5.9
- 分组依据与方案：`fxcorr/data-volume.md` §7.3、§7.5
- 两级 merge 与清理时机：`fxcorr/v6-plan.md` S4.1、S4.2
