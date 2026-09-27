# product/ —— 最终科学产品（D11 / D12）

> `workdir-template/` 的一部分：说明 workdir 里这个目录的**实物形态**。
> 规范条文见 `fxcorr/data-spec.md` 5.6；命令行见 `fxcorr/usage.md`。

| 属性 | |
|---|---|
| **根变量** | `FXCORR_PRODUCT_ROOT`（默认 `$FXCORR_WORKDIR/product`） |
| **生产者** | `fxcorr/wrap_difx2fits.sh`（封装 difx2fits / difx2mark4） |
| **消费者** | 下游科学分析 |
| **生命周期** | **长期保留** |
| **量级** | 与 `vis/` 同量级（difx2fits 是复制 + 表头） |
| **共享 / 本地** | 可本地生成，再由编排层迁移到全局 |

## 目录结构

```
product/
├── T25362.FITS                  # 产物名由 difx2fits 自己定，见下
├── T25362.0.bin0.source0.FITS   # 分源/分 bin 时的变体
└── mark4/                       # D12（用 difx2mark4 时）
    └── ...
```

**产物名不受 fxcorr 控制**——difx2fits 按传入的 `<base>` 自定规则生成
（`<base>` 或 `<base>.0.bin*.source*.FITS`）。`<exp>_<batch_id>.FITS` 那种
按 batch 命名的形态是**推荐命名**（`data-spec` 第 10 节），不是现状。

## 实验级操作，不进 run_batch.sh

**difx2fits 是实验级操作**——它读整个 `<exp>.difx/` 目录的全部 SWIN 文件，
不是一个 batch 的产物。所以它**不在 `fxcorr/run_batch.sh` 里**，由编排层在该实验
**全部 batch 跑完后调一次**（与实验级 `merge` 同一时机，见 `fxcorr/v6-plan.md` S4.1）。

`wrap_difx2fits.sh [workdir] <base>` 做的适配（difx2fits 不认识 fxcorr 的根变量，
且要求 `<base>.difx` / `<base>.input` / `<base>.calc` 三者同名同目录、按 cwd
解析，又**没有 `-o` 选项**）：

1. 把三件套复制/软链组装到 `$FXCORR_PRODUCT_ROOT` 下；
2. `cd` 过去单参数调用（产物只能落运行目录）；
3. 跑完清掉临时件，只留 `.FITS`。

## 相关

- 规范：`fxcorr/data-spec.md` 5.6
- 实验级 merge 与 difx2fits 的时机：`fxcorr/v6-plan.md` S4.1
