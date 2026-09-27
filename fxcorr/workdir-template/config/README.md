# config/ —— 配置与模型（D1/D2/D3/D4/D6）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 第 5.1 节；模板里这三份配置见 `../README.md`。

| 属性 | |
|---|---|
| **根变量** | 无——恒在 `$FXCORR_WORKDIR/config` |
| **生产者** | 观测准备：`vex2difx` + `difxcalc`（经 `fxcorr/wrap_*.sh`） |
| **消费者** | 全部三个工具（f / x / sim 都读 `.input`），以及 difx2fits |
| **生命周期** | **长期**——**实验级**（不是 batch 级），整个实验共用 |
| **量级** | 几十到几百 KB |

## 目录结构

```
config/
├── test.v2d                # vex2difx 的输入：扫描 / 站 / datastream 规划
├── test.vex                # VEX：站坐标、频段、时钟、EOP、扫描
├── test.input              # vex2difx 产物：DiFX 配置（三工具的主输入）
├── test.calc               # vex2difx 产物：difxcalc 的输入
├── test.im                 # difxcalc 产物：几何模型（delay / UVW）
└── data/                   # 真实观测的 filelist（每 ds 一个）
    ├── filelist_ba_t1      # → 对应 .v2d 的 DATASTREAM BA1
    └── ...
```

**文件名不是约定死的**——`.input` 的名字由 `.v2d` 的名字派生。但
`fxcorr/make_testdata.sh` 与两个 `wrap_*.sh` 认死 `test.` 这个前缀（四处），
所以模板的配置装进 workdir 时要改名（`gen_vex.py --install` 会做）。

## 前处理链

```
.v2d ──vex2difx──> .input + .calc ──difxcalc──> .im
```

两个封装脚本各管一件事（`fxcorr/wrap_vex2difx.sh` / `fxcorr/wrap_difxcalc.sh`）：

1. **在 `config/` 目录内调用**——`vex2difx` 的 `vex =` 路径与它的产物都相对
   **cwd**（不是相对 `.v2d` 所在目录），换目录就跑不起来；
2. **把产物里的绝对路径规范化回相对**（`CALC FILENAME` / `OUTPUT FILENAME` /
   `.im` 的 `IM`/`FLAG FILENAME`）——配置目录一搬就断。幂等，重复跑不会二次改写。

## filelist（真实观测）

真实观测的 `.v2d` 用 `filelist = data/filelist_<station>_t<N>` 指向一个文本文件，
每行一条：

```
<数据文件路径> <起始 MJD> <结束 MJD>
```

**vex2difx 只看时间范围**、不检查文件是否存在，并把这个路径**抄进 `.input` 的
DATA TABLE**（`FILE <n>/0:`）——那里的路径就是 fxcorr-f 要读的路径，所以
filelist 写相对名还是绝对路径，直接决定运行时从哪里取数（`../raw/README.md`）。

## `.input` 里三个决定运行时的字段

| 字段 | 决定 |
|---|---|
| `START MJD` / `START SECONDS` | batch 时间轴的起点（`fxcorr/make_testdata.sh` 据此写 `batch.json`） |
| `INT TIME (SEC)` / `SUBINT NANOSECONDS` | 积分与 subint 粒度；batch 时长须是 `INT TIME` 的整数倍 |
| `OUTPUT FILENAME` | SWIN 落点的**目录名**（由 `FXCORR_VIS_ROOT` 拼根），也是 `PCAL_*`/`SWITCHEDPOWER_*` 的落点 |

`ACTIVE BASELINES` 的条目数 = 站对数 × 频段组数 × 极化组合数
（t25362 2 站 = 16；4 站 = 96）——**ds 分组**与**极化产品**都从这一节推导。

## 相关

- 规范：`fxcorr/data-spec.md` 5.1（配置与模型）、第 8 节（通道/频率/偏振映射）
- 根解析：同文档 5.2.1；脚本侧 `fxcorr/roots.sh`
