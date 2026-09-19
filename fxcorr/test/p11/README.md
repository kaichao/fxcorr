# P11 reader 语义检验（valid flag 跨段续接、延迟中途重对齐）

> **验证记录（冻结）**——本目录记录 P11 的检验资产与验证过程，**截止 2026-09-14，此后未再更新**。
> 脚本与其判据仍是当前的；本文记的是当时的验证结论。读取路径的后续改进见 `reader-model.md` 与 `v4-plan.md`。

algo-plan.md P11。验证目标：datareader 的 delay 重对齐语义（修正起点早于数据起点时的跳块 + count 块 invalid + 时间重算）与 mpifxcorr 逐位对拍；delay=0 回归（cmp5 6/6）保持全等。

## 资产

| 文件 | 作用 |
|---|---|
| `gen_test_p11.py` | 从 test.vex/test.v2d 生成 `test-delay.vex`/`test-delay.v2d`：TEST2 挪到 T1 对跖点（基线 ~12744km → 几何 delay 最大 ~42.5ms），第一个 subint 的延迟修正起点必落在数据起点之前，触发上游 count>0 跳块语义（datastream.cpp:538-574）。delay 在 2.097s 观测内变化仅 ~6µs → tosubtract 补偿恒 round 0，跳块语义独立可验、补偿项走代码对照（见 algo-plan P11 验证方法） |

## 检验步骤（测试机 /root/fxcortest/p11/）

```bash
cd /root/fxcortest/p11
source /root/fxcorr/setup.bash
export LD_LIBRARY_PATH=/usr/local/difx/lib

# 0. 基础配置 + delay 变体
mkdir -p config
cp /root/fxcorr/fxcorr/test/test.vex /root/fxcorr/fxcorr/test/test.v2d config/
python3 /root/fxcorr/fxcorr/test/p11/gen_test_p11.py config/test.vex config/test.v2d config/
cp config/test-delay.vex .   # 实测坑：vex2difx 从进程 cwd 找 vex= 文件（非 v2d 所在目录），不在 cwd 即报 cannot open
vex2difx config/test-delay.v2d
difxcalc config/test-delay.calc
# delay 非零检查：.im 的 DELAY 多项式常数项应为 ms 级（实测 ANT0 ~11.2ms、ANT1 ~-11.2ms）
grep -m1 "DELAY" config/test-delay.im

# 1. 造数据（VDIF 单线程，DATA TABLE 文件名 TEST1.vdif/TEST2-usb.vdif 由 v2d 定）
python3 /root/fxcorr/fxcorr/test/gen_test_vdif.py TEST1.vdif 4 1.5 8
python3 /root/fxcorr/fxcorr/test/gen_test_vdif.py TEST2-usb.vdif 4 1.0 8

# 2. batch.json（start_mjd 精确 repr 58948.291666666664，n_subints=4）

# 3. fxcorr 侧（.input 用 config/test-delay.input，OUTPUT FILENAME 默认 config/test-delay.difx）
fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200
mv config/test-delay.difx fxcorr-delay.difx

# 4. mpifxcorr 基准（EXECUTE TIME 截断到 2 个积分 + OUTPUT 指回）
sed -e 's/EXECUTE TIME (SEC): 1200/EXECUTE TIME (SEC): 2/' -e 's|OUTPUT FILENAME: *.*|OUTPUT FILENAME: bench.difx|' config/test-delay.input > config/test-delay-mpi.input
mpirun --allow-run-as-root -np 4 mpifxcorr config/test-delay-mpi.input

# 5. 对拍（第一 subint 触发 count>0；vdifmux 滞后，取完整覆盖段）
python3 /root/fxcorr/fxcorr/test/cmp_swin.py fxcorr-delay.difx bench.difx 4096

# 6. delay=0 回归：cmp5 目录 SWIN 对拍 6/6 不变
```

对拍注意（继承 p10 经验）：mpifxcorr 拒绝覆盖已有 SWIN 输出目录；vdifmux 滞后（数据后段 subint 无效，对拍取完整覆盖段）；batch.json 的 start_mjd 必须精确 repr；install-difx 重建 fxcorr-f 若报 ipp 探测失败（测试机 /opt/intel 已删），加 `--noipp`（ipp.pc 与 fxcorr-f 无关）。

## 验收判据

1. delay≠0 配置 fxcorr vs mpifxcorr SWIN 对拍全等（cmp_swin.py 全记录通过；第一个 subint 触发跳块，其余 count=0）；
2. delay=0 回归（cmp5）6/6 全等；
3. tosubtract 公式/quirk 与上游逐行对照记录写入 algo-plan P11 实施记录（对拍数据 delay 变化 ~6µs，补偿恒 0，不覆盖补偿分支）。

## 验证记录（2026-09-14）

- **delay≠0 对拍 PASS**（判据 1）：vex2difx + difxcalc 全链路，`.im` DELAY 实测 ANT0 ≈ +11.2ms / ANT1 ≈ −11.2ms（一阶项 −0.23µs/s，2.097s 内变化 ~0.48µs → tosubtract 恒 round 0）。fxcorr vs mpifxcorr SWIN 6/6 全等。**跳块确认触发**：两边 weight 分布一致——第一个 subint 的 T1 相关记录（bl 258/257）weight 0.989258（修正起点早于数据起点 → 跳块 + 前段块 invalid），T2 相关记录（bl 514）与后续 subint 全部 1.0（负 delay 不触发）。
- **delay=0 回归 PASS**（判据 2）：cmp5 目录 6/6 全等。
- **补偿项**（判据 3）：tosubtract 公式/quirk 与上游逐行对照记录见 algo-plan P11 实施记录；对拍数据补偿恒 0，补偿分支走代码对照。
- **踩坑**：① mpifxcorr 基准的 OUTPUT FILENAME sed 替换须带 4 空格（.input 列对齐，漏掉时输出目录被读成 `ch.difx`——值从第 17 列起截断）；② vex2difx 从进程 cwd 找 `vex=` 文件（非 v2d 目录），已写入上方步骤；③ gen_test_p11.py 初版 delay 打印单位错（多除 1000）。
