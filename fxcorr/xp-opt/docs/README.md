# 文档索引

| 文件 | 内容 |
|---|---|
| [`environment.md`](environment.md) | difx 机器、数据资产、镜像、实验 workdir、单任务测量口径、已知坑 |
| [`plan.md`](plan.md) | 单任务优化目标、边界、阶段（Step 1–5）、判据 |
| [`optimizations.md`](optimizations.md) | **优化项清单与效果**（相对单线程 xp），含已采用/试过回滚/未采用 |
| [`parameters.md`](parameters.md) | **单任务参数表**：P、绑核、环境变量、两套环境的推荐配置与分阶段预期 |
| [`log.md`](log.md) | 当前口径的运行台账：Step 1–5 与相关分析 |
| [`archive/`](archive/) | 历史口径与负结果（band/block 并行、全批扫描、旧线程扫描），只作对照 |

规则：

- 新实验先写进 `plan.md` 的阶段或仓库外临时区的短说明，跑完把过程与结论追加到
  `log.md`。
- 历史口径的内容不再更新，只归档到 `archive/`。
