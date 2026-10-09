# raw/ —— 原始基带数据（D7）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 第 5.2 节；读模型的完整分析见 `fxcorr/reader-model.md`。

| 属性 | |
|---|---|
| **根变量** | `FXCORR_RAW_ROOT`（默认 `$FXCORR_WORKDIR/raw`） |
| **生产者** | 记录节点（真实观测）/ `fxcorr-sim station`（仿真） |
| **消费者** | `fxcorr-f`（任务参数定位：headers `real_path` > 命名规则推导，见下） |
| **生命周期** | 该站该 ds 的 f 任务完成后可删；**真实观测不可再生** |
| **量级** | TB 级（1 小时 4 站） |
| **共享 / 本地** | **本地即可**——同 batch 同节点 |

## 目录结构

```
raw/
├── BA/
│   ├── BA_00000001_ds0.vdif      # 多 ds 站：每 ds 一个文件
│   ├── BA_00000001_ds1.vdif
│   └── ...
├── S6/
│   └── ...
└── ...
```

命名：`<station>/<station>_<batch_id>[_ds<N>].vdif`。
**`_ds<N>` 后缀只在多 datastream 站出现**——单 ds 站是
`<station>_<batch_id>.vdif`，与加多 ds 支持之前的命名逐字相同。

## 数据定位（f 怎么找到数据）

`fxcorr-f` 定位数据**不经过** `.input` 的 DATA TABLE（2026-10-02 定案并实施，见
`fxcorr/v8-plan.md` §2），两级：

1. **任务 headers 的 `real_path`**（可选）：数据文件路径，**逗号分隔列表**（容一个 ds
   多段文件）；相对路径按 `FXCORR_RAW_ROOT` 解析、绝对路径原样——调试直读共享存储
   （路径不满足上面的命名规则）时用它；
2. **无 `real_path` → 命名规则推导**：
   `<RAW_ROOT>/<station>/<station>_<batch_id>[_ds<N>].vdif`
   （先试带 `_ds<N>` 后缀、再试无后缀）——仿真/本地化数据的正常形态，
   **数据落规范路径即可**。

```
raw/BA/BA_00000002_ds0.vdif     ← 数据本体（sim 产物，规范布局）
f 任务 00000002-BA-0（无 real_path）→ 按命名规则推导命中上面这个文件
```

**DATA TABLE 软链（现状：为 mpifxcorr 保留，2026-10-02 定）**：fxcorr 链不再需要
"把 DATA TABLE 的文件名链到真实文件"这一层——多 batch 并行时软链是共享可变状态
（同一个文件名要指向不同 batch 的文件，重指即串），新机制下每个任务自带定位依据
（见上）。`make_testdata.sh` **仍会生成**这层软链（`raw/<DATA TABLE 名>` → 最后
batch 的 VDIF）：唯一消费者是 **mpifxcorr 基准**（`run_bench.sh`——mpifxcorr 按
`.input` 的 FILE 行打开数据，多 batch 下靠软链定位 batch）；fxcorr 三工具与 difx2fits
都不使用（后者已实测不依赖 FILE 行，`v8-plan.md` §2.5-1）。**多文件**（一个 ds 跨多个
VDIF 文件）只能走 `real_path` 列表。

## `<N>` 的口径（三处必须一致）

`_ds<N>` 的 `N` 是**站内 datastream 序号**（0-based，按 `.input` DATASTREAM 表里
该站出现的次序）。三处同口径，不一致就会读错文件：

| 处 | 形态 |
|---|---|
| 本目录文件名 | `BA_00000001_ds0.vdif` |
| `fengine/<batch>/<g>/<station>/` | `ds_0/` |
| `fxcorr-f` 的命令行参数 | `<ds_index>` |

## 文件里的时间语义（三条容易踩的）

- **文件起点不要求等于 batch 起点**：真实观测中各记录系统的帧计数器不同相，
  文件可以从某一秒的中途开始；起点之前的部分判为无效。文件中间缺帧
  （含"直接缺帧"与"filler 帧占位"两种形态）同样受支持。
- **帧头秒字段是当日秒**（对 86400 取模、不带日期），帧号是秒内序号
  （对帧率取模，每秒回绕）。文件起点由首帧的 (秒, 帧号) 唯一确定。
- **标了 VDIF invalid 位的帧按"在时间轴上在位、数据不可用"处理**——它照常占
  一个时间槽（帧号参与连续性判断），只是对应的块在 f 侧被标无效；这与
  "占字节不占时间轴"的 filler 是两回事。

## 相关

- 规范：`fxcorr/data-spec.md` 5.2（含 5.2.1 根解析规则）
- 缺陷根因与诊断判据：`fxcorr/reader-model.md`
- f 侧目录级实现要点：`applications/fxcorr-f/CLAUDE.md`
