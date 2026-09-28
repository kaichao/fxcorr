# multids —— 多 datastream 检验资产

**性质**：验证记录（对应 v5-plan.md P6「多 datastream 生成」，2026-09-27 建，写完不再更新；
重跑回归以脚本为准）。

真实观测里每天线有多个 datastream（t25362 每站 8 个 = 4 频段组 × 2 极化，见
`data-volume.md` §7.3），而树里的测试资产**全是单 ds 配置**（`data-volume.md` §3 的参数表里
"每站 datastream 数 = 1"）。本目录提供把单 ds 的 `.input` 展开成多 ds 的工具，供
fxcorr-sim 的多 ds 生成、fxcorr-f/x 的多 ds 任务，以及后续的 `ds_group` 分片验证使用。

## 资产

| 文件 | 作用 |
|---|---|
| `gen_multids_input.py` | 把单 datastream 的 `.input` 展开成"每天线多 datastream"，改 DATASTREAM / BASELINE / DATA 三张表与两处计数 |
| `gen_splitbands_input.py` | 把"每站一个 ds 含多个 band"拆成"每个 band 一个 ds"——baseline 只连同一频段的两个站，于是自然形成**按频段分组的多个 ds 组**（test2b：2 组）。用来验分片的跨分片归并（单组验不到） |

```bash
python3 gen_multids_input.py <src.input> <dst.input> [ds_per_station]   # 默认 2
```

展开规则：每个原有的 datastream 复制 N 份，`TELESCOPE INDEX` 不变、`REC BAND k POL`
在 R/L 之间交替（同 t25362 的 ds 对：同一频段、两个极化）；BASELINE TABLE 取原条目
ds 展开集的笛卡尔积；DATA TABLE 的每个 ds 一行、文件名加 `_ds<N>`。FREQ 表不动
（同一频段的两个极化共用一个 freq 条目是合法的——极化记在 DATASTREAM 的 REC BAND POL，
不在 FREQ 表里）。

## 四个必须知道的坑（前两个在 `.input` 解析器里，后两个在拆分算法里）

1. **值必须从第 20 列开始**。`configuration.cpp:3585` 的 `getinputkeyval` 按
   `key->substr(DEFAULT_KEY_LENGTH)` 取值（`DEFAULT_KEY_LENGTH = 20`），行短于 20 字符
   时 `substr` 抛 `std::out_of_range`——不是解析报错，是**进程直接崩**（`terminate called
   after throwing an instance of 'std::out_of_range'`）。脚本的 `kv()` 就是为此而写。
2. **空行也会崩**。`getinputkeyval` 只跳 `#` 开头的注释行，空行会走到同一个 `substr(20)`
   上。段与段之间的空行是安全的（读到的 key 不匹配就继续读下一行），但**块内不能有空行**
   ——脚本的 `clean()` 负责在复制块时剔除首尾空行。
3. **拆 band 时帧长是 `32 + (原帧长-32)/band数`**（只均分 payload，VDIF 头是每帧一份）。
   连头一起均分会得到 8016 = 32 + 7984（7984 = 16×499），帧复样本带上质因子 499，与公共
   信号块大小（2 的幂 × 5 的幂）永不整除，fxcorr-sim 报 "block is not a whole number of
   frames"；正确值是 8032 = 32 + 8000。
4. **`D/STREAM A BAND <n>:` 的 `<n>` 是 polproduct 序号**（每条 baseline 内从 0 开始），
   **不是 baseline 号**——mpifxcorr 与 fxcorrcommon 都按 `getinputline(..., "D/STREAM A BAND ", k)`
   定位（k 是 polproduct 循环变量，`mpifxcorr/src/configuration.cpp:1089`、
   `libraries/fxcorrcommon/src/configuration.cpp:1016`）。**写错的话 mpifxcorr 直接报错退出**
   （`We thought we were reading something starting with 'D/STREAM A BAND 0', when we actually
   got 'D/STREAM A BAND 1'`），fxcorr 侧则因为值恰好都是 0 而看不出问题——2026-09-27 就是
   这样踩到的。生成器现在把编号恒置 0（只支持每条 baseline 一个 polproduct 的配置）。
   同一 baseline 的多个 freq slot 会重复出现同名字段，只能按顺序解析。
   另外，拆分后 band 索引的**值**要归零（新 ds 只含一个 band），保留原值（1）会让程序越界。

### 多组配置（验分片归并）

```bash
python3 gen_splitbands_input.py <workdir>/config/test2b.input /tmp/test2b-split.input
```
拆完是 4 个 ds（每站 2 个：200MHz 与 205MHz）、**2 条 baseline**（各连同一频段的两个站）→ 2 个 ds 组。

**注意**：拆 band 后 `INT TIME` 与 `SUBINT` 不变，但 batch 的 `n_subints` 要凑满一个积分（test2b 是 1.31072/0.262144 = **5** 个 subint），否则 fxcorr-x 报 "0 integrations written"——积分周期没走完。

## 检验步骤（测试机，可复现）

```bash
# ① 展开 .input（用已有的单 ds 产物做源）
python3 gen_multids_input.py <workdir>/config/test.input /tmp/test-mds.input 2

# ② 造一个最小 workdir：config/ 三件套 + batch.json
mkdir -p /tmp/mds/config /tmp/mds/batches
cp /tmp/test-mds.input /tmp/mds/config/test.input
cp <workdir>/config/test.calc <workdir>/config/test.im /tmp/mds/config/
#    batch.json 取 start_mjd / n_subints / config_file 三个字段，config_file = config/test.input

# ③ 生成（默认模式 = common + 全部站的全部 ds）
fxcorr-sim <batch_id> /tmp/mds

# ④ 或走编排脚本的多 ds 路径（任务展开 + DATA TABLE 逐 ds 软链）
fxcorr/make_testdata.sh /tmp/mds
```

## 验收判据（2026-09-27 实测，全部通过）

| # | 判据 | 结果 |
|---|---|---|
| 1 | 每站每 ds **一个文件**、文件名含 ds 编号（`<st>_<bid>_ds<N>.vdif`） | 4 个文件（2 站 × 2 ds），525 帧 / 4216800 字节各一 |
| 2 | DATA TABLE 的每行**各软链到对应 ds** 的文件 | `raw/TEST1_ds0.vdif → raw/T1/T1_<bid>_ds0.vdif`（4 条全对） |
| 3 | 同站两个 ds **样本互不相关**（噪声种子含 ds 索引） | 有噪（默认 0.02）时 md5 两两不同；`FXSIM_NOISE=0` 时同站 ds0 与 ds1 **逐字节相同**（差异只来自噪声，帧头/时间轴/band 布局/延迟链完全一致） |
| 4 | **单 ds 配置的产物逐字节不变** | `T1_00000001.vdif` = `18dea81e…`、`T2_00000001.vdif` = `e936e88f…`、`T1_00000003.vdif` = `22c20050…`，与改动前 md5 完全相同 |
| 5 | **单 ds 与多 ds 生成在同一 ds 上逐位相同** | 多 ds 配置产出的 `T1_00000001_ds0.vdif` = `18dea81e…`、`T2_..._ds0.vdif` = `e936e88f…`——与判据 4 的单 ds 配置**同一 md5** |
| 6 | 多 ds 下重跑幂等 | 第二次调用逐文件 `skip existing`，编号复用 `00000001` |

**分片与归并（同一批资产，2026-09-27）**：

| # | 判据 | 结果 |
|---|---|---|
| 7 | **单组分片 = 不分片** | 4 ds / 1 组的配置下，`fxcorr-x <bid> . 0` 的 `ds0.part` 与 `fxcorr-x <bid> .` 的 SWIN **逐字节相同**（`bb972cc5…`）；`merge` 后仍相同 |
| 8 | **多组分片 + merge = 不分片** | test2b 拆 band 的 2 组配置：两个 `.part`（各 3 条记录）→ `merge` → 与不分片 SWIN **逐字节相同**（`ca153e52…`）。这一条覆盖**极化完整性**（每组只含一个频段，缺任何一组都会漏掉整段）与跨分片归并 |
| 9 | 真实 `.input` 的分组推导 | t25362（16 ds / 16 baseline）→ **4 组 = `{0,1,8,9} {2,3,10,11} {4,5,12,13} {6,7,14,15}`**，与 `data-spec` 第 8 节的实测分析逐字一致；越界组号报 `has 4 ds group(s)` |
| 10 | 缺片处理 | 删掉 `.part` 后 `merge` 报错、**不写任何东西**；`FXCORR_X_MERGE_FORCE=1` 时告警并写出已到齐的部分 |
| 11 | `run_batch.sh` 的分片路径 | `FXCORR_X_SHARD=1` 逐组跑 + 自动 `merge`，产物与基准逐字节相同 |
| 12 | **多 batch 下软链重指**（2026-09-28 由 V6 S3 发现并修复，**判据 11 是单 batch 所以没抓到**） | 判据 11 用的是**单 batch**，而单 batch 下 `make_testdata.sh` 建的软链本来就是对的——`run_batch.sh` 的重指即使用错也看不出来。多 batch 才现形：**重指的源路径必须带 `_ds<N>` 后缀**（多 ds 站），漏了它软链就停在**最后一个 batch** 上，f 按本 batch 的时间轴读**另一个 batch 的数据**，全程无报错，`fxcorr-x` 照报 "N integrations written" 而 SWIN 写下 **0 条记录**。复现：`-n 4` 生成后跑 `run_batch.sh 00000001`，`readlink raw/<st>_ds0.vdif` 应指向 `_00000001_`；修好后 4 个 batch 的 SWIN 是单 batch 的精确 4 倍（4497408 = 4 × 1124352） |

判据 5 是"分片不改变结果"的生成侧依据（v5-plan.md P6 的"生成链已是逐 band 隔离"那张表）：
同一个 ds 在单 ds 配置与多 ds 配置下**字节相同**，所以按 ds 拆开生成不会改变任何一个 ds 的内容。

## 相关

- 需求与改造清单：`fxcorr/v5-plan.md` P6「多 datastream 生成」
- 命令行：`fxcorr/usage.md` 的 fxcorr-sim 段（`ds_index` 参数）
- 实现要点与验证记录：`applications/fxcorr-sim/CLAUDE.md`「多 datastream」条与 `applications/fxcorr-x/CLAUDE.md`「分片实现要点」
- 数据规范：`fxcorr/data-spec.md` 5.9（D16 分片局部记录，2026-09-27 定稿）
- 数据规范：`fxcorr/data-spec.md` 5.2（`_ds<N>` 的站内序号口径）
