# input/ —— `.input` 解析与 ds 组划分的对照判据

> `fxcorr/test/` 下的一个特性目录：**ds 组划分有两处实现，这里判它们不漂移**。
> 对照的是 `fxcorr/fxinput.py`（编排侧）与 `applications/fxcorr-x/src/main.cpp` 的
> `deriveDsGroups`（计算侧）。

## 为什么要有它

**ds 组 = 跨站、含全部极化的一批 datastream，覆盖同一个频段组**（规则权威是
`data-spec.md` 第 8 节；为什么不能按 ds 序号或 freq 条目配对，那里有实测表）。
它同时是两件事的粒度：

- **编排侧**（`fxinput.py`）：分几片、每片是哪些 ds——也就是 P3 的并行调度单位与
  目标机 tmpfs 的容量账（`v7-plan.md` §10.1 的第 1 条约束）；
- **计算侧**（`fxcorr-x`）：分片模式真正按它打 ds 掩码、决定哪些 baseline 算得出来。

两处是**同一条规则的两份实现，中间没有编译器兜底**。漂移的症状比 `roots.sh` vs
`FxcorrPath` 那一对更险：根错了程序会报"roots 不一致"，而**组划分错了没有任何症状**——
脚本按 4 组调度、程序按 5 组算，或组数对得上而成员对不上，于是每片漏掉或多算几个 ds，
**产物看起来完全正常**，只是记录少了一部分。

## 用法

```bash
./run_consistency.sh <workdir> [batch_id ...]
```

- `workdir` 需含 `batches/*.json` 与 `config/`（`.input` 由 batch.json 的 `config_file` 指出）。
  不给 `batch_id` 时对该 workdir 下全部 batch 判。
- 需要 `fxcorr-x` 在 `PATH` 里（或已 `source setup.bash`）。
- **不需要 raw / fengine 数据**——C++ 侧真值出口在读任何数据之前就退出了。

## 判据

| # | 判据 |
|---|---|
| 1 | 每个 batch 两侧的组划分**逐组逐成员**相同（组序也相同） |
| 自检 | 把 python 一侧人为改坏（临时副本，不动仓库文件），判据**必须报红** |

自检是这套判据成立的前提：没有它，"全绿"可能只是判据没有区分力。

## 真值出口：`FXCORR_X_GROUPS_ONLY`

`fxcorr-x` 的这个环境变量让它在分片模式下**只打印全部 ds 组划分就退出**——不读数据、
不写盘，输出与运行期那行 `fxcorr-x: shard mode, ds group G of N = {...}` 同前缀（差一个
` -> path`），所以同一条 `sed` 也读得懂正常跑批时的日志。

它**刻意跳过了两道 workdir 状态检查**（根一致性、SWIN 互斥），这是实测逼出来的：它只读
`.input` 与 `batch.json`，不碰任何根、不写 SWIN，而对照测试恰恰常跑在**已经跑过**的
workdir 上——第一版把它放在两道检查之后，20 个 batch 里有 2 个被拦（见下）。

## 验证记录（2026-09-29，目标集群 f12r4n02）

**判据在 20/20 个 batch 上通过**（`/work2/cstu0036/fxcorr/wd`，4 站 × 8 ds 的 t25362
参数配置）：两侧都给出 **4 个 ds 组**，成员与组序逐一相同。自检也按预期报红。
这是 python 侧实现（从 `run_batch.sh` 的内嵌段重写为 `fxinput.py`）与 C++ 侧的**第一次
对拍**，一次通过——但**不能因此认为判据多余**：它恰恰是"以后改动不会悄悄漂移"的保障。

**踩到的两个坑**（都是出口位置，已修）：

1. **SWIN 互斥检查**：出口原先在分片分支内，而 `if(!merge)` 的"本 batch 已经写过"检查
   在它之前——跑过一次的 batch 全部被拦（`already holds 1024 record(s) ...`）。
   **2026-09-30 根因已修**：分片模式本就不该做这道检查（`data-spec` 5.9"两者都不查"），
   条件改为 `!merge && !sharded`。出口前移**仍然保留**——它要跳过的还有根一致性检查，
   而那一道对"不读数据、不写盘"的出口同样没有意义。
2. **根一致性检查**：把它提到 SWIN 检查之前后，`00000002` 仍被拦——
   `meta/roots/00000002.json` 里留着上一次容器实验的 `FXCORR_FENGINE_ROOT=/dev/shm/fxcorr`，
   而当前解析出的是 `<workdir>/fengine`。这道检查在 **config 构建之前**，出口没法再往前
   挪（它要用 config 算分组），所以改成由出口**跳过**它。

**两处实现的等价性另有一条独立证据**：抽出 `fxinput.py` 时做过一次"逐字等价"验证——
从 git 取出抽出前的内嵌 python 段，与新的 `fxinput.py prepare` 在同一 workdir 上跑同一
batch，输出（`CFGIN`/`NGRP`/`OUTDIR` 三行 + 32 行站表）**逐字相同**（`00000001` /
`00000007` / `00000011` 三个 batch）。复现方法：`git show <抽出前的 commit>:fxcorr/run_batch.sh`
里 `<<'PYEOF'` 与 `PYEOF` 之间的内容即旧版。

## 未覆盖

- **只有单一 `.input` 的 workdir 验证不了变体**：多 ds 站、拆带多组等样本的生成器在
  `test/multids/`（`gen_multids_input.py` / `gen_splitbands_input.py`），**尚未接入本脚本**。
  接入前应按 `test/multids/README.md` 手工确认过一次，否则"20/20 通过"只说明这一种拓扑
  没漂移。
- **全单元素组的 `.input`**（每个 ds 各自成组）上自检会误报：改坏方式是"一条 baseline 都
  不合并"，而那种拓扑本来就不合并。测试机上的常规配置不属此列。
