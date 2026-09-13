# zoom band（P4a）检验

本目录是 zoom band 特性的检验资产与验证记录（algo-plan P4a，2026-09-13 实施完成）。设计见 `algo-plan.md` P4a 节；实现易错点见 `applications/fxcorr-x/CLAUDE.md` 关键实现要点。

## 资产

- `gen_test_zoom.py`：从无 zoom 的 .input 生成 test-zoom.input（父 band 上追加 zoom freq 定义 + DATASTREAM ZOOM 段 + BASELINE 第二 freq）+ EXECUTE TIME=2 截断变体 test-zoom-mpi2.input（mpifxcorr 基准用）。用法：`gen_test_zoom.py <input_file> [lowedge_mhz] [bw_mhz] [nchan]`。注意 BASELINE 段 `D/STREAM A/B BAND` 行的 key 序号是 pol product 序号不是 freq 序号（写错静默解析为 0）。
- `cmp_swin_zoom.py`：按 SWIN 头 freqindex 分拆多 nchan 记录的逐记录比较（zoom 配置的 SWIN 把主带与 zoom 写在同一文件、各 freq nchan 不同，cmp_swin.py 的固定 nchan 不适用）。用法：`cmp_swin_zoom.py <a> <b> <freq0=nchan0,freq1=nchan1,...>`。

## 检验步骤（2026-09-13 验证过，测试机可一键复现）

```bash
cd /root/fxcortest && rm -rf zoom && mkdir zoom
bash /root/fxcorr/fxcorr/make_testdata.sh zoom 1.5     # 单 band 只能 1 个 tone（tone 落 zoom 中心）
cd zoom
python3 /root/fxcorr/fxcorr/test/zoom/gen_test_zoom.py config/test.input
#   -> config/test-zoom.input（父 4MHz/4096ch + zoom 201.5MHz 1MHz/1024ch）
#   -> config/test-zoom-mpi2.input（EXECUTE TIME=2 截断，mpifxcorr 基准用）
python3 - <<'EOF'
import json
j = json.load(open('batches/58948_25200.json'))
j['config_file'] = 'config/test-zoom.input'
json.dump(j, open('batches/58948_25200.json','w'), indent=2)
EOF
# fxcorr 链路
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200'
mv config/test.difx/DIFX_58948_025200.s0000.b0000 /tmp/zoom_fxcorr.s0000.b0000
# mpifxcorr 基准
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  mpirun --allow-run-as-root -np 4 /usr/local/difx/bin/mpifxcorr config/test-zoom-mpi2.input'
# 对拍（按 freqindex 分拆：主带 4096ch、zoom 1024ch）
python3 /root/fxcorr/fxcorr/test/zoom/cmp_swin_zoom.py \
  /tmp/zoom_fxcorr.s0000.b0000 config/test.difx/DIFX_58948_025200.s0000.b0000 0=4096,1=1024
# 无 zoom 回归：batch.json config_file 改回 config/test.input，rm -rf fengine config/test.difx，
# 重跑 f/x 与 mpifxcorr（test-mpi2.input），cmp_swin.py ... 4096 对拍 6/6
```

## 验收判据

- zoom 对拍 12/12 记录 ALL OK：主带 6 + zoom 6，头字段（bl/mjd/sec/src/frq/pol/pbin/weight/uvw）与可见度 max rel ≤1e-6；
- 无 zoom 回归 6/6（cmp_swin.py）。

## 验证记录（2026-09-13）

- **zoom 对拍**（/root/fxcortest/zoom/，test-zoom.input：父 band 4MHz/4096ch + zoom 201.5MHz 起 1MHz/1024ch，tone 1.5MHz 落 zoom 中心）：mpifxcorr 基准 vs fxcorr **12/12 记录全等**（freq 0 主带 6 条 + freq 1 zoom 6 条 = 2 积分 × (互相关 1 + 自相关 2 站×2 band)；可见度 max rel 0.00e+00）。
- **无 zoom 回归**：原 test.input 配置对拍 6/6 全等。
- zoom 的 XMAC/uvshift/accumulateWeights 均零改动（config 表驱动 + recordbandindex 父映射现成）；f 侧 Mode 零改动（getMode 工厂传 numzoombands）。
