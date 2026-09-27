# raw/ —— 原始基带数据（D7）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 第 5.2 节；读模型的完整分析见 `fxcorr/reader-model.md`。

| 属性 | |
|---|---|
| **根变量** | `FXCORR_RAW_ROOT`（默认 `$FXCORR_WORKDIR/raw`） |
| **生产者** | 记录节点（真实观测）/ `fxcorr-sim station`（仿真） |
| **消费者** | `fxcorr-f`（按 `.input` 的 DATA TABLE 读） |
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
├── BA_ds0.vdif                   # ← DATA TABLE 软链（见下）
└── S6_ds0.vdif
```

命名：`<station>/<station>_<batch_id>[_ds<N>].vdif`。
**`_ds<N>` 后缀只在多 datastream 站出现**——单 ds 站是
`<station>_<batch_id>.vdif`，与加多 ds 支持之前的命名逐字相同。

## DATA TABLE 软链（仿真的关键机制）

`.input` 的 DATA TABLE 写的是 **filelist 里的那个路径**（通常是相对裸名，
如 `BA_ds0.vdif`）。`fxcorr/make_testdata.sh` 在该路径下建软链，指向 fxcorr-sim 实际
生成的 `raw/<station>/<station>_<batch_id>_ds<N>.vdif`：

```
raw/BA_ds0.vdif  →  raw/BA/BA_00000001_ds0.vdif
```

绕这一道是因为**命名口径不同**：DATA TABLE 每个 ds 只有一个固定名字，
而仿真的文件按 batch 命名。`fxcorr/run_batch.sh` 每跑一个 batch 就把软链重指到
该 batch 的 VDIF（`fxcorr/make_testdata.sh` 多 batch 时软链停在最后一个 batch）。

**真实观测不软链**——FILE 行直接就是数据文件路径（绝对路径或直接可见）。
多文件（一个 ds 跨多个 VDIF 文件）也是真实观测的常态。

## `<N>` 的口径（三处必须一致）

`_ds<N>` 的 `N` 是**站内 datastream 序号**（0-based，按 `.input` DATASTREAM 表里
该站出现的次序）。三处同口径，不一致就会读错文件：

| 处 | 形态 |
|---|---|
| 本目录文件名 | `BA_00000001_ds0.vdif` |
| `fengine/<batch>/<station>/` | `ds_0/` |
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
