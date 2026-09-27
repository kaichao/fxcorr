# batches/ —— 批量元数据（D9）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 第 5.3 节（字段）与第 6 节（`batch_id`）。

| 属性 | |
|---|---|
| **根变量** | 无——恒在 `$FXCORR_WORKDIR/batches` |
| **生产者** | 切批规划：`fxcorr/make_testdata.sh`（全字段一次写全） |
| **消费者** | 三工具（fxcorr-f / x / sim）与全部编排脚本 |
| **生命周期** | **长期**——batch 的元数据是权威记录，数据删了它也留 |
| **量级** | 约 1 KB / batch |

## 目录结构

```
batches/
├── 00000001.json
├── 00000002.json
└── ...
```

**文件名 = `<batch_id>.json`**。`batch_id` 是 **8 位零填充顺序号**（`00000001` 起），
由切批规划单点分配、workdir 内全局唯一，**名字不承载时间信息**——起点、时长、
频段全在 json 里。旧的时间编码格式（如 `60512_45000`）仍然可用：三工具不校验
格式，只当不透明字符串拼路径。

## 文件格式（全字段单文件）

```json
{
  "batch_id": "00000001",
  "start_mjd": 61037.28484954,
  "start_time": "2026-01-13T06:50:11",
  "duration_sec": 1.024,
  "stations": ["BA", "S6"],
  "baselines": ["BA-S6"],
  "config_file": "config/test.input",
  "calc_file": "config/test.calc",
  "im_file": "config/test.im",
  "seed": 20260912,
  "n_subints": 200,
  "subint_ns": 5120000,
  "integration_sec": 1.024,
  "n_channels": 128,
  "polarizations": ["XX", "XY", "YX", "YY"],
  "difx_dir": "vis/T25362.difx",
  "created_at": "2026-09-27T12:00:00Z",
  "status": "running",
  "fxcorr_f_version": "0.1.0",
  "fxcorr_x_version": "0.1.0"
}
```

（上面按 2 站举例，读起来短。4 站配置下 `stations` 是 32 项、`baselines` 96 项。）

字段全部由编排脚本**一次写全**，三工具只读。`status` 是唯一的可变字段
（`running` → `done` / `failed`），由 `fxcorr/run_batch.sh` 写回。

- **`stations` / `baselines` 是"逐条展开"的**：`fxcorr/make_testdata.sh` 按 `.input`
  的 TELESCOPE INDEX 与 D/STREAM A/B INDEX **逐条**生成，**可含重复**
  （t25362 是 16 / 16；4 站配置是 32 / 96）。手工构造的 batch.json（如
  `t25362work` 里那份）常写成**去重**形态，`fxcorr/run_batch.sh` 用 `set()` 比较，
  **两种都被接受**——但读的人要知道它未必去重；
- **`seed`（2026-09-27 V6 S2.5 新增）**：公共信号的 PRNG 种子，**同一 batch 的全部 station
  任务必须读到同一个值**——否则各站合成出的公共信号不同，**跨站相干静默消失**，事后没有
  程序能检测出来。它是 D15 取消后唯一遗留的"公共参数"：网格（`specRes` / `numSamps` /
  `minStartFreq`）与 batch 的 slice 总数都是 `.input` 的纯函数，各站就地重算、必然逐位相同，
  所以不存也不传。`fxcorr/make_testdata.sh` 从环境变量 `FXSIM_SEED` 取（缺省 `20260912`），
  规范见 `fxcorr/data-spec.md` 5.8；
  **旧的 batch.json 没有这个字段**（S2.5 之前生成的），`fxcorr-sim` 会以
  `batch.json missing required fields` 报错退出——补一行即可（值取生成该批数据时用的
  `FXSIM_SEED`，缺省 `20260912`），不必重跑 `make_testdata.sh`；
- `n_subints` × `subint_ns` = batch 时长，须是 `integration_sec` 的整数倍
  （`.input` 的 `INT TIME`），`fxcorr/run_batch.sh` 开跑前校验；
- `polarizations` 逐条 baseline 推导（A 侧 band 极化 × B 侧 band 极化），
  **字符透传不改写**——t25362 是 `X/Y`，测试资产是 `R/L`；
- `difx_dir` 照抄 `.input` 的 `OUTPUT FILENAME`，**没有程序把它当路径读**，
  实际落点由 `FXCORR_VIS_ROOT` 决定。

## ⚠ 不要在模板/示例里放 `.json`

`fxcorr/make_testdata.sh` 扫本目录的 `*.json` 来分配下一个 `batch_id`，并按
`(start_mjd, n_subints, subint_ns)` 复用已有编号。放一个示例 json 进去，
切批规划会把它当成真实 batch——所以要展示格式，就写进 README（像上面那样），
不要落成文件。

## 相关

- 批量状态流水：`meta/batches.index`（D13，batch `done` 时追加一行）
- 切批与重跑约束：`fxcorr/data-spec.md` 第 12 节
