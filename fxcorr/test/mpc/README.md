# 多相位中心（P4b）检验

本目录是多相位中心特性的检验资产与验证记录（algo-plan P4b）。设计见 `algo-plan.md` P4b 节；实现易错点见 `applications/fxcorr-x/CLAUDE.md` 关键实现要点。

## 资产

- `test-mpc.v2d`：test.v2d 基础上追加 `SOURCE TEST { addPhaseCentre = name@TEST2/RA@01h00m05.0s/Dec@-80d00'00.0" }`——vex2difx 原生多相位中心链路（corrparams.cpp `addPhaseCentre`），difxcalc 自动为相位中心 TEST2 生成完整延迟/UVW 多项式（.im 的 SRC 2）。TEST2 在指向中心 TEST（01h00m00.0s, -80d）东侧 5s RA（~13" 角距），T1-T2 差分延迟 ~8ps（.im 中 TEST2 与 TEST 的 DELAY 常数项差：ANT0 1.132586 µs、ANT1 1.132578 µs）、rotator 相位 ~0.1 rad（2.4GHz），非零且可测。

**`.im` 的 SRC 索引语义**（model.cpp readScanData + difxcalc d_out.f 约定）：SRC 0 = 指向中心、SRC 1..N = 各相位中心（.im 里"SCAN 0 PHS CTR 0"可能就指向中心本身，如本配置 PHS CTR 0 = TEST、PHS CTR 1 = TEST2）；延迟多项式行按此对齐，diff 验证时勿比错 SRC 行。

用法：复制成 config/test.v2d 后复用 make_testdata.sh 全流程（test.vex 用根目录共享件；vex2difx/difxcalc 只在 test.input/test.im 不存在时运行，故 config 资产先就位即可）。

## 检验步骤（测试机可一键复现）

```bash
cd /root/fxcortest && rm -rf mpc && mkdir mpc && cd mpc && mkdir config
cp /root/fxcorr/fxcorr/test/test.vex config/
cp /root/fxcorr/fxcorr/test/mpc/test-mpc.v2d config/test.v2d
bash /root/fxcorr/fxcorr/make_testdata.sh mpc 1.5     # 出 config/test.input/.calc/.im（多源）
cd mpc
# 检查多源 .im：NUM PHASE CENTRES = 2，IM 段每样本有 SRC 0（TEST）与 SRC 1（TEST2）多项式
grep -A2 "NUM PHASE CENTRES" config/test.im | head -5
# mpifxcorr 基准用 EXECUTE TIME=2 截断变体（.input 行是 "EXECUTE TIME (SEC):"，sed 模式须带 (SEC)）
sed 's/^EXECUTE TIME (SEC):.*/EXECUTE TIME (SEC): 2/' config/test.input > config/test-mpi2.input
# fxcorr 链路
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200'
mkdir /tmp/mpc_fxcorr && mv config/test.difx/DIFX_58948_025200.s*.b0000 /tmp/mpc_fxcorr/
# mpifxcorr 基准
bash -c 'source /root/fxcorr/setup.bash && export LD_LIBRARY_PATH=/usr/local/difx/lib && \
  mpirun --allow-run-as-root -np 4 /usr/local/difx/bin/mpifxcorr config/test-mpi2.input'
# 对拍：每相位中心一套 .s%04d 文件，逐文件比较（头含 sourceindex/UVW/weight）
for s in 0000 0001; do
  python3 /root/fxcorr/fxcorr/test/cmp_swin.py \
    /tmp/mpc_fxcorr/DIFX_58948_025200.s$s.b0000 config/test.difx/DIFX_58948_025200.s$s.b0000 4096
done
# 单源回归：config/test.v2d 换回根目录 test.v2d、rm -rf fengine config/test.difx，
# 重跑 f/x 与 mpifxcorr（test-mpi2.input），cmp_swin.py ... 4096 对拍 6/6
```

## 验收判据

- 多源对拍 ALL OK：源 0（.s0000）6 条 + 源 1（.s0001）2 条（自相关只写指向中心源文件，visibility.cpp:894-898 上游语义），头字段（bl/mjd/sec/sourceindex/frq/pol/pbin/weight/uvw）与可见度 max rel ≤1e-6；
- 源间应有真实差异（rotator/decorr/UVW 生效证明）：.s0001 vs .s0000 的 sourceindex/uvw/可见度/weight 均不同（本配置 vis[0] rel ~1.5e-02）；
- 单源回归 6/6（退化路径与 P4b 前逐位一致）。

## 验证记录（2026-09-13）

- 环境：/root/fxcortest/mpc/，test-mpc.v2d 全链路（.im：NUM PHS CTRS 2 = TEST + TEST2，SRC 2 的 DELAY/UVW 独立），FXSIM_NOISE=0 造数，EXECUTE TIME (SEC)=2 截断基准。
- **多源对拍 8/8 记录全等**：mpifxcorr 基准 vs fxcorr，.s0000 6/6（互相关 1 + 自相关 2 站 × 2 积分）、.s0001 2/2（互相关 × 2 积分；自相关按上游语义只写指向中心源文件），可见度 max rel 0.00e+00（cmp_swin.py 逐字节头+可见度全等）。
- **源间差异确认**（rotator/decorr 非平凡）：.s0001 vs .s0000 —— src 0→1、uvw 差 ~0.006（米级，13" 角距）、可见度 rel 1.5e-02（~8ps 差分延迟的 rotator 相位）、weight 0.989 vs 1.0（decorr 衰减）。
- **单源回归 6/6**：config 换回根目录 test.v2d 重新 vex2difx/difxcalc（单源 .im），f/x 与 mpifxcorr 全等（退化路径不漂移）。
