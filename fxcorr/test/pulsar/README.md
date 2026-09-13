# 脉冲星 binning（P4c）检验

本目录是脉冲星 binning 特性的检验资产与验证记录（algo-plan P4c）。设计见 `algo-plan.md` P4c 节；实现易错点见 `applications/fxcorr-x/CLAUDE.md` 关键实现要点。

## 资产

- `gen_test_pulsar.py`：从无 pulsar 的 .input 生成检验资产（用法 `gen_test_pulsar.py <input_file> [--scrunch] [--negative-weight]`）：
  - `config/test-pulsar.input`：PULSAR BINNING TRUE + PULSAR CONFIG FILE 注入（紧跟 PULSAR BINNING 行之后，.input 顺序解析 configuration.cpp:1292-1297）；`test-pulsar-scrunch.input` 同理（scrunch 变体）
  - `config/test-pulsar-mpi2.input`：EXECUTE TIME (SEC) = 2 截断（mpifxcorr 基准用）
  - `config/pulsar.cfg`：NUM POLYCO FILES 1 + POLYCO FILE + NUM PULSAR BINS 4 + SCRUNCH OUTPUT + BIN PHASE END/WEIGHT 各 4（--negative-weight 时 bin 1 权重 -1）
  - `config/test.polyco`：tempo 风格 polyco（tmid 硬编码测试 scan 起点 MJD 58948.2916667、timespan 120min、f0 = 2.0Hz 使 2.1s 数据扫过全 4 bin、系数全 0 = 恒频脉冲串、DM 0）

注意：polyco/pulsar config 路径按 **workdir cwd 相对**解析（同 DATA TABLE 软链语义），所有命令须在 workdir 内执行。

## 检验步骤（测试机可一键复现）

```bash
cd /root/fxcortest && rm -rf pulsar && mkdir pulsar
bash /root/fxcorr/fxcorr/make_testdata.sh pulsar 1.5
cd pulsar
python3 /root/fxcorr/fxcorr/test/pulsar/gen_test_pulsar.py config/test.input
python3 - <<'EOF'
import json
j = json.load(open('batches/58948_25200.json'))
j['config_file'] = 'config/test-pulsar.input'
json.dump(j, open('batches/58948_25200.json','w'), indent=2)
EOF
# fxcorr 链路
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200'
mkdir /tmp/pulsar_fxcorr && mv config/test.difx/DIFX_58948_025200.s*.b0* /tmp/pulsar_fxcorr/
# mpifxcorr 基准
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  mpirun --allow-run-as-root -np 4 /usr/local/difx/bin/mpifxcorr config/test-pulsar-mpi2.input'
# 对拍：非 scrunch 时每 bin 一套 .b%04d 文件（自相关固定写 .b0000，visibility.cpp:899）
for b in 0000 0001 0002 0003; do
  python3 /root/fxcorr/fxcorr/test/cmp_swin.py \
    /tmp/pulsar_fxcorr/DIFX_58948_025200.s0000.b$b config/test.difx/DIFX_58948_025200.s0000.b$b 4096
done
# scrunch 变体：gen_test_pulsar.py --scrunch 重生成，batch.json config_file 改
# test-pulsar-scrunch.input，rm -rf fengine config/test.difx，重跑两侧，单 .b0000 对拍 6 条
# 无 pulsar 回归：batch.json config_file 改回 config/test.input，重跑 f/x 与
# mpifxcorr（test-mpi2.input 或 EXECUTE TIME 截断），cmp_swin.py ... 4096 对拍 6/6
```

## 验收判据

- 非 scrunch 对拍 ALL OK：.b0000 6 条（互相关 bin0 2 + 自相关 4）+ .b0001-.b0003 各 2 条，共 14 条，头字段（含 PULSAR BIN）与可见度 max rel ≤1e-6；
- scrunch 对拍 ALL OK：单 .b0000 6 条；
- 无 pulsar 回归 6/6。

## 验证记录（2026-09-13）

- 环境：/root/fxcortest/pulsar/，FXSIM_NOISE=0 造数，gen_test_pulsar.py 资产（4 bins、f0=2Hz 恒频 polyco、EXECUTE TIME (SEC)=2 截断基准）。
- **非 scrunch 对拍 14/14 记录全等**：mpifxcorr 基准 vs fxcorr，.b0000 6/6（互相关 bin0 2 + 自相关 4，自相关固定写 .b0000）+ .b0001-.b0003 各 2/2，可见度 max rel 0.00e+00（cmp_swin.py 逐字节头+可见度全等）。
- **scrunch 对拍 6/6 全等**：单 .b0000（互相关 2 + 自相关 4）；**负权重变体（--negative-weight，bin1 权重 -1）对拍 6/6 全等**（剥离缓变信号语义一致）。
- **binning 生效确认**：.b0001 vs .b0002 的 pbin 头字段 1 vs 2、weight 0.953 vs 0.957、可见度 rel 4.1e-04（各 bin 收到不同相位段数据）。
- **无 pulsar 回归 6/6**：config 换回 test.input，f/x 与 mpifxcorr（test-mpi2.input）全等（退化路径不漂移）。
- **实施中实测的坑（已入 fxcorr-x CLAUDE.md）**：scrunch 的 pulsaraccumspace 必须在 uvshiftAndAverage 尾部清零（core.cpp:1565-1602），漏移植会导致 accumspace 跨 uvshift 窗口累积、可见度按积分序放大（首积分 1.5×、次积分 3.5×）。
