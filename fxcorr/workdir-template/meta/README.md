# meta/ —— 全局索引与根记录

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 5.7；根解析规则见同文档 5.2.1 与 `fxcorr/roots.sh`。

| 属性 | |
|---|---|
| **根变量** | **无**——恒在 `$FXCORR_WORKDIR/meta` |
| **生产者** | 编排脚本：`fxcorr/run_batch.sh`（索引、根记录）、`fxcorr/roots.sh`（根检查） |
| **消费者** | 编排层（scalebox） |
| **生命周期** | **长期** |
| **量级** | 小（每 batch 一行 + 一个 json） |
| **共享 / 本地** | **必须全局一致可见** |

## 目录结构

```
meta/
├── batches.index             # D13：batch 状态流水（append-only）
├── roots/
│   ├── 00000001.json         # 该 batch 的四个根快照
│   └── ...
└── difxmsg/                  # DifxMessage 落盘（container 模式；当前暂不启用）
    ├── <exp>_<batch>.xml
    └── <exp>_<batch>_<station>.xml
```

## `batches.index`（D13）

`fxcorr/run_batch.sh` 在 batch 成功时**追加**一行，append-only：

```
00000001,done,2026-09-08T12:35:12Z
00000002,running,2026-09-08T12:36:01Z
00000003,failed,2026-09-08T12:36:40Z
```

格式：`<batch_id>,<status>,<UTC 时间戳>`。合法状态只有
**`running` / `done` / `failed`**——没有 `pending`（那不是一个会被落盘的状态）。

**它是"哪些 batch 已完成"的流水记录**——但**不是**"这个实验应该有哪些 batch"的判据来源：
它 append-only、只在 `done` 时追加一行，拿它当应有集会让 `failed` 与尚未调度的 batch
**静默消失**。实验级 `merge` 的应有集判据是 `batches/*.json` 本身（2026-09-28 订正，
`data-spec` 5.9 末条第 2 条、`fxcorr/v6-plan.md` S4.1 实施细则）。

## `roots/<batch_id>.json`

该 batch **开跑时**的四个根快照。程序启动时会比对自己用到的根，脚本也会先做
实验级一致性检查——**同一实验中途换了根，产物会分裂在两处而没有任何报错**，
所以写前先查。

```json
{
  "workdir": "/data/scalebox/s0run",
  "raw": "/data/scalebox/s0run/raw",
  "fengine": "/dev/shm/fengine",
  "vis": "/data/scalebox/s0run/vis",
  "product": "/data/scalebox/s0run/product"
}
```

注意 `fengine` 可以指向 `/dev/shm` ——**根是路径，不要求都在 workdir 下**
（这正是 P5 根变量化要的形状）。

## `difxmsg/`（暂不启用）

仅 container 模式（`FXCORR_RUN_MODE=container`）产生：组播受限的容器内降级为
落盘，由编排层读取转发；每进程一个文件、启动时截断重写（重跑幂等）。

**当前状态：暂不启用**（2026-09-17 定）。host 模式默认不设
`DIFX_MESSAGE_GROUP`/`DIFX_MESSAGE_PORT`，每次发送都是静默 no-op，故本目录
不会产生；运维侧的全局视图由编排层承担。

## 未来：SQLite 索引（⚠ 未实施）

编排层计划把 batch 元数据另存一份 SQLite 表以支持结构化查询。

- **权威仍是 `batches/*.json`**（D9），sqlite 是**派生索引**、可随时重建——
  这条不能反：`test/` 下十余个回归流程靠**手改 `batch.json`** 跑；
- **单一写者**：只有编排层主路由写，计算节点只读或完全不碰。不能多进程写——
  sqlite 的 WAL 依赖共享内存，在网络文件系统上不可用。

## 相关

- 规范：`fxcorr/data-spec.md` 5.7
- 根变量与三档回退：同文档 5.2.1；脚本侧 `fxcorr/roots.sh`
- 一致性判据：`fxcorr/test/roots/run_consistency.sh`
