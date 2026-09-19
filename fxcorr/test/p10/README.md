# P10 输入格式检验（Mark5B / LBA / 多线程 VDIF corner-turn / mark5access 通用集）

> **验证记录（冻结）**——本目录记录 P10 的检验资产与验证过程，**截止 2026-09-14，此后未再更新**。
> 脚本与其判据仍是当前的；本文记的是当时的验证结论。

algo-plan.md P10。验证策略（2026-09-13 用户定）：**Mark5B、多线程 VDIF、LBA 逐位对拍**；MKIV/VLBA/VLBN/KVN5B/CODIF 通性验证（无基准数据，代码对照审查兜底）；K5VSSP/K5VSSP32 上游不可用不迁移（mark5access K5 "Not Yet Implemented"、genMk5FormatName 无 K5 分支）。

## 资产

| 文件 | 作用 |
|---|---|
| `gen_test_mk5b.py` | Mk5B 数据生成器（10016 字节帧：16 头 + 10000 payload；帧头照 mark5access 读端 m5bfile.c/mark5_format_mark5b.c 布局，非官方 spec：sync ED DE AD AB、15 位帧号、BCD 世纪天数+秒；payload 2bit 低位先连续流） |
| `gen_test_lba.py` | LBASTD 数据生成器（16 字节老式 ASCII 头 `YYYYMMDD:HHMMSS\n` + raw 2bit 采样**低位先**连续流——与 VDIF/Mk5B 相同，照 LBAMode lookup mode.cpp 构造器：shift=0 起取 u16 低 2bit；2026-09-14 修正原高位先错误，见验证记录） |
| `gen_test_p10.py` | .input 变体生成（DATA FORMAT/FRAME SIZE/DATA TABLE 文件名重写）：test-mk5b.input（Mark5B/10016）、test-lba.input（LBASTD/320004096 哨兵——抄 vex2difx 对 LBA 的生成值，见 tests/ATNF v521c）、test-ivdif.input（INTERLACEDVDIF:0:1 线程后缀，configuration.cpp:1416-1423 语法） |

## 检验步骤（测试机 /root/fxcortest/p10/）

```bash
cd /root/fxcortest/p10
source /root/fxcorr/setup.bash
export LD_LIBRARY_PATH=/usr/local/difx/lib

# 0. 基础配置（2 站 T1/T2，2020y100d07h00m00s 起，tone T1=1.5MHz T2=1.0MHz）
mkdir -p config
cp /root/fxcortest/p8reg/config/test.vex /root/fxcortest/p8reg/config/test.v2d config/
vex2difx config/test.v2d
difxcalc config/test.calc
python3 /root/fxcorr/fxcorr/test/p10/gen_test_p10.py config/test.input config/

# 1. Mark5B 数据 + 对拍
python3 /root/fxcorr/fxcorr/test/p10/gen_test_mk5b.py TEST1.m5b 4 1.5 8
python3 /root/fxcorr/fxcorr/test/p10/gen_test_mk5b.py TEST2-usb.m5b 4 1.0 8
# 帧合法性检查（生成器帧头如有误，m5bstate 会报异常）
m5bstate TEST1.m5b
# batch.json（start_mjd 精确 repr 58948.291666666664，n_subints=4）
# fxcorr 侧
fxcorr-f 58948_25200 T1 && fxcorr-f 58948_25200 T2 && fxcorr-x 58948_25200
mv config/test.difx fxcorr-mk5b.difx
# mpifxcorr 基准（EXECUTE TIME 截断到 2 个积分 + OUTPUT 指回）
sed -e 's/EXECUTE TIME (SEC): 1200/EXECUTE TIME (SEC): 2/' -e 's|OUTPUT FILENAME: *.*|OUTPUT FILENAME: bench.difx|' config/test-mk5b.input > config/test-mk5b-mpi.input
mpirun --allow-run-as-root -np 4 mpifxcorr config/test-mk5b-mpi.input
python3 /root/fxcorr/fxcorr/test/cmp_swin.py fxcorr-mk5b.difx bench.difx 4096

# 2. 多线程 VDIF（corner-turn）+ 对拍
fakemultiVDIF TEST1.vdif TEST1.ivdif 128
fakemultiVDIF TEST2-usb.vdif TEST2-usb.ivdif 128
# batch.json 同 1；.input 用 config/test-ivdif.input（EXECUTE TIME/OUTPUT 同 1 的 sed）
# fxcorr 侧 + mpifxcorr 基准 + cmp_swin.py 同 1

# 3. LBA + 对拍（mpifxcorr 基准走基类 DataStream raw 路径）
python3 /root/fxcorr/fxcorr/test/p10/gen_test_lba.py TEST1.lba 4 1.5 8
python3 /root/fxcorr/fxcorr/test/p10/gen_test_lba.py TEST2-usb.lba 4 1.0 8
# .input 用 config/test-lba.input；fxcorr + mpifxcorr 基准 + cmp_swin.py 同 1
# 若 mpifxcorr 基类路径自身不可用 → 降级 fxcorr 自洽验证（tone 落位/谱形），结论记入本文

# 4. 回归：现 VDIF 单线程配置 SWIN 对拍 6/6（p8reg 流程）
```

对拍注意（继承 p8reg/sta 经验）：mpifxcorr 拒绝覆盖已有 SWIN 输出目录；vdifmux 滞后（数据后段 subint 无效，对拍取完整覆盖段）；batch.json 的 start_mjd 必须精确 repr。

## 验收判据

1. Mark5B 对拍逐位全等（cmp_swin.py 全记录通过）；
2. 多线程 VDIF 对拍逐位全等；
3. LBA 对拍逐位全等（否则自洽验证 + 结论记录）；
4. MKIV/VLBA/VLBN/KVN5B/CODIF：无基准数据，以构造路径/报错路径 + 代码对照审查为准（记录于 algo-plan.md 实施记录）；
5. 现 VDIF 回归 6/6 全等。

## 验证记录（2026-09-14）

- **Mark5B 对拍 PASS**（判据 1）：fxcorr vs mpifxcorr，SWIN 6/6 逐记录全等；fixmark5b 验证生成帧合法（80 valid frames、start 25200s）。
- **多线程 VDIF 对拍 PASS**（判据 2）：README 步骤 2 的 `fakemultiVDIF` 不可用（复制语义非 fanout 拆分），改用自写 `gen_test_ivdif.py`（t0=偶采样、t1=奇采样、帧号秒内取模）；fxcorr vs mpifxcorr SWIN 6/6 全等，payload 与 vdifio vdifmux 逐字节一致验证过。
- **LBA 对拍降级 → 自洽验证 PASS**（判据 3 预案）：mpifxcorr 读 LBA+FILE 走基类 DataStream 路径，4 进程 100% CPU 失控死循环（上游陈年 bug，基准不可用）→ 降级 fxcorr 自洽验证（`verify_lba.py`）：T1/T2 autocorr 峰位 1536/1024（1.5/1.0MHz）、SWIN 12 记录 weight 0.82-1.0，ALL PASS。
- **五格式审查**（判据 4）：MKIV/VLBA/VLBN/KVN5B/CODIF 无基准数据，以代码对照审查为验收依据（构造分派与上游一致、mark5access 同库同 formatname、报错路径完整、读入架构与 MARK5B 路径同构），结论记录于 algo-plan.md P10 实施记录。
- **VDIF 单线程回归 PASS**（判据 5）：cmp5 目录标准 VDIF 配置 SWIN 6/6 全等。

### 问题排查记录（P10 全程，按类别）

#### 1. 代码修复类（fxcorrcommon/fxcorr-f 源码改动）

**① datamuxer.cpp 输出 EDV4 头三坑**（多线程 VDIF 对拍时暴露，ASAN 定位）

- 现象一：weight 全乱。vdifio vdifmux 输出帧带 EDV4 扩展头（eversion=4 + syncword 0xACABFEED + validitymask=1）；fxcorr 输出不带 EDV4 时，mark5access 的 blank_vdif_EDV4 对 eversion=0 帧直接跳过 blank，但 invalid 数组残留未初始化垃圾 → 后续按垃圾 invalid 算 weight。
- 现象二：SEGV（fftloop 27 fwrite 的 memmove dst=NULL，ASAN 定位为堆破坏）。原因：用 vdif_edv4_header 结构体写 EDV4，其 u64 validitymask 字段的 8 字节对齐 padding 越出帧头、破坏了相邻堆数据。
- 现象三：SEGV（format_vdif.c:8542）。原因：EDV4 的 nchan 字段设成 numthreads（2），blank_vdif_EDV4 按 1<<nchan 遍历 per-channel 数组（invalid/unpacked 只按 nrecordedbands 分配）→ 越界。
- 现象四：FPE 除零（format_vdif.c:8538）。原因：validitymask 设为 3，而 goodMask=1<<nchan=1，d=nChan/masklength 除零。
- 根因：fxcorr 的 muxed 输出必须与 vdifio vdifmux 的布局逐位一致，而 blank_vdif_EDV4 的实现假设 EDV4 各字段互恰（validitymask==goodMask、nchan 与 per-channel 数组一致）。
- 修复：裸 word 写 `w[4]=(numthreads<<16)|(4<<24); w[5]=0xACABFEED; w[6]=1; w[7]=0`，nchan 字段保持输入值 0。

**② mode.cpp unpacked 数组分配 +8**（fxcorr 崩溃，上游侥幸存活）

- 现象：fxcorr 跑 VDIF 时崩溃（ASAN 报越界写）。
- 根因：Mk5Mode::unpack 里 mark5access 解包写 samplestounpack = unpacksamples + granularity（+4）；上游分配恰好 unpacksamples，每 FFT 越界 16 字节，mpifxcorr 靠堆布局侥幸从未触发；fxcorr 的堆布局不同直接崩。
- 修复：`vectorAlloc_f32(unpacksamples + 8)`（带注释说明 +8 来源）。

**③ mk5mode.cpp invalid 数组按线程数分配**（多线程 VDIF）

- 现象：INTERLACEDVDIF 对拍时 blank 越界。
- 根因：invalid 数组按 nrecordedbands 分配，blank_vdif_EDV4 按 masklength（=mux 线程数）遍历 → 越界。
- 修复：INTERLACEDVDIF 时 `new int[config->getDNumMuxThreads(...)]`。

#### 2. 生成器/测试资产类（fxcorr/test/p10/*.py）

**④ VDIF 帧头 word3 位布局**（多线程暴露）

- 现象：mpifxcorr 报 "frame rate = 62 per thread"、vdifmux 判 0 帧有效 → 基准全 0。
- 根因：vdifio/mark5access 的真实 word3 布局 = stationid [15:0]、threadid [25:16]、nbits-1 [30:26]、iscomplex [31]。旧生成器 `(0<<16)|(0<<6)|(2<<1)` 把 thread 放 bit6、nbits 放 bit1。单线程 thread=0 且 nbits 不被 vdifmux 检查 → 从未暴露。
- 修复：`(station<<0)|(thread<<16)|((nbits-1)<<26)`。对照依据：demorest/vdifio cornerturners.c 与 mark5access format_vdif.c。

**⑤ LBA 2bit 位序 = 低位先**（本 README 资产表已改；P10 耗时最长的排查）

- 现象：LBA 自洽验证时 T1（tone 1.5MHz）autocorr 主峰 1024（=1.0MHz 位置），期望 1536；T2（1.0MHz）峰 1024"恰好正确"（位序反转对 fs/8 频率不变，纯属巧合，极具迷惑性）。
- 定位过程（五级 dump 逐层排除，全部由环境变量触发、已清理）：
  1. **Goertzel 直测数据文件**：TEST1.lba payload 在 1.5MHz 功率 1e8、1.0MHz 近零 → 数据文件本身正确，怀疑 fxcorr 链路。
  2. **分 subint 分析 autocorr**：8 个 subint 峰位恒定 1024 无漂移 → 非 seek 漂移/累积问题，第一 FFT 块就错（FXCORR_ACDUMP 证实第一自相关谱即有 1024 主峰 + 512 次峰 + 1536 微弱）。
  3. **FXCORR_RAWDUMP**：fxcorr 读入字节与生成器 payload 逐字节一致（1048576 字节全等）→ 读入/seek 正确。
  4. **FXCORR_FFTINDUMP**：FFT 输入谱 1.0/2.0MHz 等强（1.64e6）、0.5 次之、1.5 微弱 → 解包后的序列错（输入应为纯 1.5MHz）。
  5. **FXCORR_LOOKUPDUMP + FXCORR_UNPACKDUMP**：lookup 表行（u16=0..15）与构造代码完全自洽（shift=0 起取 u16 低 2bit = LSB-first）；unpacked 码序列 [3,3,1,1,3,1,1,2,...] 与 data 字节 bd 5f 97 f5、packed[0]=0x975F（unpackstartsamples=436 处）逐位自洽 → **lookup、unpack、读入全部正确，唯一不一致在"生成器打包序 vs lookup 位序"**。
- 根因：LBAMode lookup 构造器 `(i >> shift) & 0x03` 从 shift=0 起、u16 小端 → **每字节低 2bit 是第一采样**（LSB-first，与 VDIF/Mk5B 相同）。4MHz 带宽配置 samplesperblock = int(recordedbw×2/blockclock) = int(4×2/8) = 1，lookup 走 outputshift=0 分支（64MHz 的 samplesperblock=4 才有 outputshift 重排）。生成器按 MSB-first 打包（`q << (6-2i)`）→ 解包 = 字节内时间反转 → tone 被 fs/8 载波调制：1.5MHz → 1MHz 主峰 + 2MHz 谐波 + 0.5MHz 次峰。
- 教训：README 资产表最初写"LBA 高位先与 VDIF 相反"是**错的**——当时按 samplesperblock==4 分支的 outputshift 重排推导，未注意 4MHz 配置走 samplesperblock=1 分支。判断位序要看对应带宽的实际分支。
- 修复：`q << (2 * (i % 4))`。

**⑥ fakemultiVDIF 不可用于对拍**（自写 gen_test_ivdif.py 替代）

- 现象：fakemultiVDIF 生成 2 线程数据，fxcorr/mpifxcorr 均 0 记录输出；128/16/8/4 Mbps 参数全部试过。
- 根因：① 复制语义——两线程含相同帧内容，不是 fanout 拆分（t0=偶采样、t1=奇采样）；② Mbps 参数按"每线程"数据率编帧号（2 线程 4MHz 2bit 传 8 → 125fps），语义混乱。
- 修复：自写 gen_test_ivdif.py（fanout 2 线程：`g = n*nthreads*nsamp_per_frame + t + j*nthreads`，帧号秒内取模 `frame = n % int(fps)`）。

**⑦ 其他生成器坑**：

- **.input 列对齐**：getinputkeyval 按固定第 20 列读值；sed 替换 OUTPUT FILENAME 时值前须 4 空格（`OUTPUT FILENAME:    bench.difx`），单空格从第 17 列起导致读成 "ch-..."。
- **Mk5B 生成器 day 溢出**：BCD 世纪天数曾写 `day_of_century + sec//86400`（把 2000 纪元天数也加进去）→ struct.error；应为 `day_of_century + (sec - start_sec)//86400`。
- **LBA 头格式**：曾写 17 字符带冒号 → datareader 报 "no TIME line"；正确为 15 字符 `YYYYMMDD:HHMMSS`（时分秒无冒号）+ 换行 = 16 字节。
- **格式名大小写**：.input 解析要求 "MARK5B" 全大写（gen_test_p10.py 曾写 "Mark5B"）。

#### 3. muxed VDIF 路径类（datareader KIND_MUXEDVDIF）

**⑧ demux 槽位顺序**：getCurrentDemuxBuffer 在 resetcounters 之前调用 → 读槽 1 解槽 0，输出错乱；修复：resetcounters 移到读 demuxbuf 之前。

**⑨ 两坐标系混用**："Framebytes has changed" 报错、deinterlace 读垃圾。根因：fileoffset 按输出帧组（16032 字节）计算，但输入流按单线程帧（8032 字节）对齐。修复：locate 用 `framesin*nthreads*inputframebytes`，读入长度用 `muxer->getSegmentBytes()`。

#### 4. 环境/基准类

**⑩ mpifxcorr LBA 基准失控**：mpifxcorr 读 LBA+FILE 走基类 DataStream 路径，4 进程 100% CPU 失控死循环（上游陈年 bug 实锤）——杀掉进程，按验收判据 3 预案降级 fxcorr 自洽验证（verify_lba.py），结论记入 algo-plan.md P10 实施记录。

**⑪ p8reg 目录不可做 VDIF 回归**：p8reg 的 test.input 是 P8 相位阵变体（SUBINT 改成 numbufferedffts 整数倍），mpifxcorr 相位阵是死代码 → 基准 SWIN weight 全 0（stderr 还有 "databytesperpacket change from 8000 to -32" 帧错乱警告）。**标准 VDIF 回归目录是 cmp5**（batch n_subints=4、基准 bench/test.difx 6 条 weight 0.989-1.0）。

**⑫ 其他环境项**：

- 测试机遗留 8 个 mpifxcorr 进程（100% CPU 数百小时）→ kill -9 清理。
- 测试机 libasan 损坏（/usr/lib64/libasan.so.6.0.0 自引用 symlink 循环 → ld EMFILE "Too many open files"）→ `dnf install libasan` 修复；ASAN 构建可用（CXXFLAGS/LDFLAGS=-fsanitize=address）。Model 析构的 new[]/delete mismatch 与 28 处泄漏是上游既有问题（ASAN 下报、普通运行无害）。
- mpifxcorr 拒绝覆盖已有 SWIN 输出目录（对拍前须 rm -rf）；vdifmux 滞后使数据后段 subint 无效（对拍取完整覆盖段）。
- batch.json 的 start_mjd 须精确 repr（如 58948.291666666664），否则 fxcorr-f 对齐校验（1µs 容差）报错。
- install-difx 的 `--doonly` **只认最后一个**（多个 --doonly 只有最后生效），重建多组件须分别跑（fxcorrcommon 先、fxcorr-f 后，因为后者链接前者）。

#### 5. 排查方法论与文件格式速查

- **tone 峰位判据用 autocorr 而非 .sp**：.sp 谱经 fringe rotation 频移（tone 峰不在原通道），autocorr 是 |X|² 不受相位旋转影响，峰位 = tone 通道。LBA 排查初期看 .sp 峰位（T1 在低通道一堆峰、T2 在 1792 附近）是死胡同。
- **autocorr.bin 格式**：`FXCAC\0`（6B）+ u32 version=2/nsub/acb/nbands/crosspol + 每 band (u32 bandindex, u32 nchan)；数据 = 每 subint acb 条批次记录，每条 = 每 band nchan 个 cf32 + 每 band 1 个 f32 weight（crosspol=1 时平行段后接 crosspol 段）。
- **.sp 格式**：256 字节文件头（`FXCSP\0` + u32 version/bandindex/nchan/lsb/cx/nsub/subns/bps/nbf/fw + f64 bw/edge 等）+ 每 subint（12 字节 scan/sec/ns i32 + flagwords×4 有效位 + bps×4 weight 占位，subint 尾 fseek 回填）+ bps×nchan 个 cf32 谱。
- **SWIN 头 74 字节**：`<2I 2i d 3i 2s i d 3d`（sync 0xFF00FF00 起，见 cmp_swin.py HEADER）；记录长 74+nchan×8。
- **测试机无 numpy**：单频点检测用 Goertzel（纯 python 每频点 O(N)），比手写 FFT 快且够用。
- **五级 dump 分层排查法**（本 README ⑤ 的过程）：读入字节（RAWDUMP）→ lookup 表（LOOKUPDUMP）→ 解包序列（UNPACKDUMP）→ FFT 输入（FFTINDUMP）→ 自相关谱（ACDUMP），每级与"正确值"对比即可二分定位；dump 由环境变量触发、零成本，定位后已全部清理。
