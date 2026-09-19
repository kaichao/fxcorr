# P9 检验资产：Kurtosis STA + STA 频域平均分支

> **验证记录（冻结）**——本目录记录 P9 的检验资产与验证过程，**截止 2026-09-13，此后未再更新**。
> 脚本与其判据仍是当前的；本文记的是当时的验证结论。

## 背景

上游 mpifxcorr 的 STA 监控（`averageAndSendAutocorrs`，core.cpp:1166-1265）与 Kurtosis
STA（`averageAndSendKurtosis`，core.cpp:1378-1434）由 difxmessage 控制消息**运行时开启**
（`dumpsta=true` / `dumpkurtosis=true` / `stachannels=N`，mpifxcorr.cpp:88-97，独立消息
线程阻塞接收）。fxcorr 用环境变量：`FXCORR_STA=1`（P1 已做，自相关 STA）与
`FXCORR_KURTOSIS=1`（P9，峰度 STA）。上游无命令行开关，因此基准侧必须用
`sta_ctrl send` 发控制消息。

P9 补的另一个分支：`minpostavfreqchannels >= STADumpChannels` 时（默认 4096 ≥ 32 恒真），
上游在 STA 发送**前**先 `averageFrequency()`（core.cpp:1181-1187），STA 的 renorm 与
通道数按平均后频谱修正（:1249-1254），autocorr.bin 落盘跳过重复平均（:1263-1268）。
CHANS TO AVG=1 时该分支与旧实现数值等价（平均是 no-op），测不出差异——本资产用
`CHANS TO AVG 4` 变体做真实考验。

## 资产

| 文件 | 作用 |
|---|---|
| `sta_ctrl.c` | C 工具（链 libdifxmessage，`cc -O2 -o sta_ctrl sta_ctrl.c -ldifxmessage`）：`send <name> <value> [mpiDest]` 发控制消息（默认 ALLMPIFXCORR）；`recv <outfile> [maxmsgs]` 加入 BINARY_STA 组播（DIFX_BINARY_GROUP/PORT）把原始 record 流抓进文件（2 秒静默窗口结束），与 fxcorr-f container 模式 `.sta` 落盘同布局 |
| `gen_test_sta.py` | 从 test.input 生成 `test-sta.input`（CHANS TO AVG 0: 1→4，激活平均分支）与 `test-stampi.input`（再截 EXECUTE TIME=2 供 mpifxcorr 基准） |
| `cmp_sta.py` | 逐记录比较两个原始流（72 字节头 + nChan×f32），按 (messageType, dsindex, bandindex, scan, sec, ns, nswidth) 分组匹配，nChan/identifier/data 逐位比对（coreindex/threadindex 两边均为 0，不比对） |

## 检验步骤（测试机 /root/fxcortest/sta/）

```bash
# 0) 准备工作目录（raw 数据软链 p8reg 的 fxcorr-sim 产物，对拍段 4 subints）
mkdir -p /root/fxcortest/sta && cd /root/fxcortest/sta
mkdir -p config batches
cp /root/fxcortest/p8reg/config/test.input config/
python3 /root/fxcorr/fxcorr/test/sta/gen_test_sta.py config/test.input
# OUTPUT FILENAME 指本目录 + 截断变体（gen_test_sta.py 的 -stampi 变体沿用旧路径，须重截）
sed -i 's|/root/fxcortest/p8reg/config/test.difx|/root/fxcortest/sta/config/test.difx|' config/test.input config/test-sta.input
sed 's/EXECUTE TIME (SEC): 1200/EXECUTE TIME (SEC): 2/' config/test-sta.input > config/test-stampi.input
ln -s ../p8reg/raw raw
ln -s raw/T1/T1_58948_25200.vdif TEST1.vdif
ln -s raw/T2/T2_58948_25200.vdif TEST2-usb.vdif
python3 - <<'EOF'   # batch.json：对拍段 4 subints、config 指 sta 变体
import json
d = json.load(open('/root/fxcortest/p8reg/batches/58948_25200.json'))
d['config_file'] = 'config/test-sta.input'
d['n_subints'] = 4
d['duration_sec'] = 4 * d['subint_ns'] / 1e9
json.dump(d, open('batches/58948_25200.json', 'w'), indent=4)
EOF
cc -O2 -I/usr/local/difx/include -o sta_ctrl /root/fxcorr/fxcorr/test/sta/sta_ctrl.c -L/usr/local/difx/lib -ldifxmessage

# 1) 基准：mpifxcorr + 控制消息 + 抓包（后台）
source /root/fxcorr/setup.bash; export LD_LIBRARY_PATH=/usr/local/difx/lib
rm -rf config/test.difx    # mpifxcorr 拒绝覆盖已有 SWIN
./sta_ctrl recv bench.sta & RECV=$!
sleep 0.3
mpirun --allow-run-as-root -np 4 mpifxcorr config/test-stampi.input & MPI=$!
for i in $(seq 40); do ./sta_ctrl send dumpkurtosis true >/dev/null; ./sta_ctrl send dumpsta true >/dev/null; sleep 0.2; done
wait $MPI; wait $RECV     # recv 在 2 秒静默后自行退出
# 控制消息每 0.2s 连发 40 轮：mpifxcorr 的命令线程在 .input 读完后才 spawn，
# 之前发的必丢（组播 fire-and-forget）；实测该密度下 dump 从第 2 个 subint 起
# 生效，首 subint 无基准记录——对拍时两边统一用 min_absns=524288000 过滤
# （见 cmp_sta.py 第 3 参数）

# 2) fxcorr：container 模式落盘 .sta（与组播同字节），两站各一个进程
rm -rf fengine meta
FXCORR_STA=1 FXCORR_KURTOSIS=1 FXCORR_RUN_MODE=container fxcorr-f 58948_25200 T1
FXCORR_STA=1 FXCORR_KURTOSIS=1 FXCORR_RUN_MODE=container fxcorr-f 58948_25200 T2
cat meta/difxmsg/test-sta_58948_25200_T1.sta meta/difxmsg/test-sta_58948_25200_T2.sta > fx.sta
# （.sta 文件名前缀 = .input basename：test-sta；默认配置轮为 test）

# 3) 对拍（min_absns 过滤基准缺失的首 subint）
python3 /root/fxcorr/fxcorr/test/sta/cmp_sta.py bench.sta fx.sta 524288000

# 4) 默认 CHANS TO AVG=1 轮：batch.json config_file 改 config/test.input、
#    基准用 config/test-mpi2.input（test.input 的 EXECUTE TIME=2 截断），其余同上
```

基准与 fxcorr 的消息数须一致：STA_AUTOCORRELATION = 每 ac 批次 × band × ds，
STA_KURTOSIS = 每 subint × band × ds（4 subints × 2 站 = 8 条）。mpifxcorr 的 vdifmux
流式滞后只在数据后段出现，截断段（前 2 秒）无 invalid subint，两边 weight gate 结果一致。

## 验收判据

1. `cmp_sta.py` PASS：全部记录逐位全等（missing/extra/mismatched 均为 0），两种
   messageType 都出现且条数相等；
2. 默认 CHANS TO AVG=1 配置（test.input + test-mpi2.input，batch.json 指 config/test.input）
   同样 PASS（平均分支等价性回归）;
3. 无 STA 开关回归：p8reg 原配置跑 f+x，SWIN 与基准 6/6 全等；
4. CHANS TO AVG 4 变体下 autocorr.bin 记录完整（f 只出 fengine 产物，x 不跑该变体）。

## 验证记录（2026-09-13）

- **两轮对拍全部逐位 PASS**：CHANS TO AVG 1（默认，test.input）与 CHANS TO AVG 4
  （test-sta.input，真平均分支）各 318 条（autocorr 312 + kurtosis 6，过滤首 subint），
  missing/extra/mismatched 均为 0。
- 无开关回归：p8reg 全链路（f+x vs mpifxcorr 基准）SWIN 对拍 6/6 全等（取前 6 条，
  fxcorr batch 8 subints 4 积分 vs 基准 EXECUTE TIME=2 的 2 积分）。
- `FXCORR_STA=1` 时 autocorr.bin 与无开关逐字节一致（md5），datastreamsaveraged 提前
  平均不改变写盘数据。
- **过程中修掉 P1 遗留 bug（stride 错 2 倍）**：sendSTA 的 fold 循环
  `acdata[2*k*chans_to_avg].re` 直接照抄上游 f32* 的 ×2 stride——上游 acdata 是
  `(f32*)getAutocorrelation()`（f32 stride 2 = cf32 stride 1），fxcorr 用 cf32* 指针
  时 ×2 导致每隔 chans_to_avg 取一块而非相邻通道折叠，STA 谱形完全错误。P1 时
  STA 对拍未覆盖 data 数值（或当时无真平均分支对拍），P9 的 CHANS TO AVG 4 逐位
  对拍暴露。修复为 `acdata[k*chans_to_avg].re`。
- **控制消息时序**：mpifxcorr 的命令线程在 Configuration 读完 .input 后才 spawn，
  启动前发的消息必丢；且 4 次×1s 连发仍只能从第 3 个 subint 起生效。检验用
  每 0.2s×40 轮连发（约覆盖启动 + 全部 subints），dumpsta/dumpkurtosis 均从第 2 个
  subint 起生效；首 subint 无基准记录，两边用 `min_absns=524288000`（subint 起点
  sec×1e9+ns 的绝对纳秒）过滤后对齐——这是组播 fire-and-forget 的固有行为，
  不是 fxcorr 问题。
- sta_ctrl 编译须带 `-I/usr/local/difx/include -L/usr/local/difx/lib`；运行须
  `LD_LIBRARY_PATH=/usr/local/difx/lib`（libdifxmessage.so.0 不在默认路径）。
- 基准重跑前须 `rm -rf config/test.difx`（mpifxcorr 拒绝覆盖已有 SWIN），且
  test.input/test-sta.input 的 OUTPUT FILENAME 须 sed 指本目录（默认指向来源目录）。
