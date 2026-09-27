#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/model.h>
#include <fxcorrcommon/polyco.h>
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/difxmonitor.h>
#include <fxcorrcommon/fxcorrpath.h>

#include "spreader.h"
#include "xmac.h"
#include "integrate.h"
#include "beamengine.h"
#include "log.h"
#include "ompcompat.h"

using namespace std;

// experiment name for DifxMessage identification, same as mpifxcorr's
// generateIdentifier (mpifxcorr.cpp:217-239): .input basename without the
// ".input" suffix
static string jobIdentifier(const string &inputfile)
{
	size_t slash = inputfile.find_last_of('/');
	string base = (slash == string::npos) ? inputfile : inputfile.substr(slash+1);
	size_t dot = base.find(".input");
	return (dot == string::npos) ? base : base.substr(0, dot);
}

// unified error exit: report to stderr, then send Alert + Aborting status
// (algo-plan.md P1, upstream alert.cpp:54 / fxmanager.cpp ABORTING)
static int fail(DifxMonitor &monitor, const string &msg)
{
	cerr << msg << endl;
	monitor.alert(msg, DIFX_ALERT_LEVEL_ERROR);
	monitor.status(DIFX_STATE_ABORTING, msg, 0.0, 0, 0, 0.0, 0.0);
	return EXIT_FAILURE;
}

// Minimal batch.json field extraction (fixed V1 format, data-spec 5.3 D9).
static bool extractJsonDouble(const string &json, const string &key, double *value)
{
	size_t pos = json.find("\"" + key + "\"");
	if(pos == string::npos)
		return false;
	pos = json.find(':', pos);
	if(pos == string::npos)
		return false;
	char *end;
	*value = strtod(json.c_str() + pos + 1, &end);
	return (end != json.c_str() + pos + 1);
}

static bool extractJsonInt(const string &json, const string &key, int *value)
{
	double d;
	if(!extractJsonDouble(json, key, &d))
		return false;
	*value = (int)(d + 0.5);
	return true;
}

static bool extractJsonString(const string &json, const string &key, string *value)
{
	size_t pos = json.find("\"" + key + "\"");
	if(pos == string::npos)
		return false;
	pos = json.find(':', pos);
	if(pos == string::npos)
		return false;
	size_t start = json.find('"', pos);
	size_t end = (start == string::npos) ? string::npos : json.find('"', start + 1);
	if(start == string::npos || end == string::npos)
		return false;
	*value = json.substr(start + 1, end - start - 1);
	return true;
}

// ---------------------------------------------------------------------------
// 分片模式（v5-plan.md P6 / data-spec 5.9）

// ds 组推导：把 datastream 按 BASELINE TABLE 的**连通分量**分组。每条 baseline
// 条目绑定一对具体的 ds，这两个覆盖同一频段组；把全部条目并查集合并后，连通
// 分量就是"覆盖同一频段组的一组 ds"（t25362 实测 4 组，每组 4 个 = 2 站 x 2 极化）。
//
// **不能按 ds 序号或 freq 条目配对**：同频段的 X/Y 是两条不同的 freq 条目
// （按 freq 聚类会把一对极化拆开），而 t25362 的第 2 条 baseline 是 (ds1,ds8)
// 而非 (ds1,ds9)——按序号配对同样会配错。机理与实测表见 data-spec 第 8 节。
//
// 合并时小的根胜出，于是代表元 = 组内最小 ds 序；收集时按 root 升序扫，组的
// 顺序就是"最小 ds 序"的顺序（即频段升序）。
static vector<vector<int> > deriveDsGroups(Configuration &config, int configindex)
{
	const int nds = config.getNumDataStreams();
	vector<int> parent(nds);
	for(int i = 0; i < nds; i++)
		parent[i] = i;
	for(int b = 0; b < config.getNumBaselines(); b++)
	{
		int a = config.getBOrderedDataStream1Index(configindex, b);
		int c = config.getBOrderedDataStream2Index(configindex, b);
		while(parent[a] != a) a = parent[a] = parent[parent[a]];
		while(parent[c] != c) c = parent[c] = parent[parent[c]];
		if(a < c)
			parent[c] = a;		// smaller index wins: representative = min
		else if(c < a)
			parent[a] = c;
	}
	vector<vector<int> > groups;
	vector<char> used(nds, 0);
	for(int root = 0; root < nds; root++)
	{
		if(used[root])
			continue;
		vector<int> grp;
		for(int i = root; i < nds; i++)
		{
			int r = i;
			while(parent[r] != r) r = parent[r];
			if(r == root)
			{
				grp.push_back(i);
				used[i] = 1;
			}
		}
		groups.push_back(grp);
	}
	return groups;
}

// 一条分片记录：74 字节 SWIN 头 + cf32 数据，逐字节搬运。归并 key 用**整数
// 纳秒**（由 mjd 与秒偏移推算），不用浮点秒——浮点相等比较不可靠（data-spec 5.9）。
struct PartRecord
{
	long long key;
	int baseline, freqindex, pulsarbin;
	int autocorr;		// 自相关记录（写盘时排在同周期的基线记录之后）
	string bytes;
};

// 排序键刻意**不含极化对**，并用 stable_sort：写盘顺序是
// baseline -> freq -> 相位中心 -> bin -> polproduct k，而记录头里只有极化对、
// 没有 k（RR/LL/RL/LR 的字典序不等于 k 的顺序）。stable_sort 保持同一
// (周期, baseline, freq, bin) 内的原有相对顺序，于是极化顺序天然正确；不同
// baseline 的记录不会撞这个键（它们来自不同的分片），跨分片归并仍然有序。
//
// autocorr 排在基线之后：writeSWIN 先写全部基线、再写自相关段，而自相关的
// baselinenumber 是 257*(天线序+1)，与基线的编号不在同一个序列上。用
// "非零且能被 257 整除"识别它是个启发式——万一某条基线的编号恰好撞上，
// 只会改变这一周期内的记录顺序（集合与时间单调性不受影响，difx2fits 也不
// 关心周期内顺序）。
static bool partRecordLess(const PartRecord &a, const PartRecord &b)
{
	if(a.key != b.key) return a.key < b.key;
	if(a.autocorr != b.autocorr) return a.autocorr < b.autocorr;
	if(a.baseline != b.baseline) return a.baseline < b.baseline;
	if(a.freqindex != b.freqindex) return a.freqindex < b.freqindex;
	return a.pulsarbin < b.pulsarbin;
}

// 读一个 .part 的全部记录。检查 sync word 与记录长度，任何不一致都报错——
// 分片文件写坏时宁可停下来，也不要写出一份看着正常、实则残缺的 SWIN。
static bool readPart(const string &path, Configuration &config, vector<PartRecord> *out,
                     std::ostringstream *err)
{
	ifstream in(path.c_str(), ios::binary);
	if(!in.is_open())
	{
		*err << "cannot open " << path;
		return false;
	}
	long long nrec = 0;
	while(true)
	{
		char head[74];
		in.read(head, sizeof(head));
		if(in.gcount() == 0)
			break;			// clean EOF
		if(in.gcount() != (std::streamsize)sizeof(head))
		{
			*err << path << ": truncated record header after " << nrec << " record(s)";
			return false;
		}
		unsigned int sync;
		memcpy(&sync, head, 4);
		if(sync != Visibility::SYNC_WORD)
		{
			*err << path << ": bad sync word 0x" << hex << sync << dec << " at record " << nrec;
			return false;
		}
		PartRecord rec;
		int mjd, freqindex, baselinenum;
		double seconds;
		memcpy(&baselinenum, head + 8, 4);
		memcpy(&mjd, head + 12, 4);
		memcpy(&seconds, head + 16, 8);
		memcpy(&freqindex, head + 32, 4);
		memcpy(&rec.pulsarbin, head + 38, 4);
		rec.baseline = baselinenum;
		rec.freqindex = freqindex;
		rec.autocorr = (baselinenum != 0 && baselinenum % 257 == 0) ? 1 : 0;
		rec.key = (long long)mjd * 86400LL * 1000000000LL +
		          (long long)floor(seconds * 1.0e9 + 0.5);
		// 每条记录的数据长度由它的 freqindex 决定：SWIN 头里没有 length 字段
		// （记录是定长头 + nchan 个 cf32），所以要读 .input 才知道边界
		int nchan = config.getFNumChannels(freqindex);
		int chansavg = config.getFChannelsToAverage(freqindex);
		if(nchan <= 0 || chansavg <= 0 || nchan % chansavg != 0)
		{
			*err << path << ": freqindex " << freqindex << " has a bad channel count ("
			     << nchan << "/" << chansavg << ")";
			return false;
		}
		size_t nbytes = (size_t)(nchan / chansavg) * sizeof(cf32);
		rec.bytes.assign(head, sizeof(head));
		rec.bytes.resize(sizeof(head) + nbytes);
		in.read(&rec.bytes[sizeof(head)], nbytes);
		if(in.gcount() != (std::streamsize)nbytes)
		{
			*err << path << ": truncated data in record " << nrec << " (wanted " << nbytes
			     << " bytes, got " << in.gcount() << ")";
			return false;
		}
		out->push_back(rec);
		nrec++;
	}
	return true;
}

// 本 batch 的时间范围在目标 SWIN 里是否已有记录。分片与全量两种模式对同一
// batch **互斥**（data-spec 5.9）：分片任务不写 SWIN 而 merge 会追加，混跑会
// 写出重复记录并破坏 SWIN 的时间单调——difx2fits 顺序读记录、时间回退的被静默
// 丢弃（fitsUV.c:1227 的 RecordIsOld、fitsUV.c:1868 只打一行计数）。这里把那条
// 文档约束落成程序内检查，与 mpifxcorr 拒绝覆盖已有 SWIN 的行为一致。
//
// 只读 74 字节记录头，数据用 seekg 跳过：对一个 GB 级的 SWIN 也是秒级。
// 记录长度由 freqindex 查 .input 得到（头里没有 length 字段），与 readPart 同。
static bool swinHasBatchRange(const string &path, Configuration &config,
                              double batchstartmjd, double durationsec,
                              long long *nrecords)
{
	*nrecords = 0;
	ifstream in(path.c_str(), ios::binary);
	if(!in.is_open())
		return false;			// 还没有这个文件 = 没写过
	const double lo = batchstartmjd * 86400.0;
	const double hi = lo + durationsec;
	while(true)
	{
		char head[74];
		in.read(head, sizeof(head));
		if(in.gcount() != (std::streamsize)sizeof(head))
			break;
		unsigned int sync;
		int mjd, freqindex;
		double sec;
		memcpy(&sync, head, 4);
		if(sync != Visibility::SYNC_WORD)
			break;			// 不是记录流，不猜
		memcpy(&mjd, head + 12, 4);
		memcpy(&sec, head + 16, 8);
		memcpy(&freqindex, head + 32, 4);
		double t = (double)mjd * 86400.0 + sec;
		if(t >= lo - 1e-6 && t < hi + 1e-6)
			(*nrecords)++;
		int nchan = config.getFNumChannels(freqindex);
		int chansavg = config.getFChannelsToAverage(freqindex);
		if(nchan <= 0 || chansavg <= 0)
			break;
		in.seekg((std::streamoff)((size_t)(nchan / chansavg) * sizeof(cf32)), ios::cur);
	}
	return *nrecords > 0;
}

// merge 子命令：读 vis-parts/<batch_id>/ 下的全部分片，按积分周期归并，追加写出
// 该 batch 的 SWIN 记录。**SWIN 只能有一个写入者**：difx2fits 顺序读记录并按天线
// 检查时间单调（fitsUV.c:1227 的 RecordIsOld），时间回退的记录被静默丢弃、只在
// 结尾打一行计数（fitsUV.c:1868，不报错）——分片任务各自追加同一个文件必然时间
// 回退，所以写出集中在这里。分析与方案见 data-volume.md §7.5。
static int doMerge(Configuration &config, const string &workdir, const string &batchid,
                   DifxMonitor &monitor)
{
	int scan = 0;
	int configindex = config.getScanConfigIndex(scan);
	if(configindex < 0)
		return fail(monitor, "fxcorr-x merge: no configuration for scan 0");

	const string partdir = workdir + "/vis-parts/" + batchid;
	// ① 本 batch 应有几片：ds 组的个数（从 .input 的 baseline 表推导）
	vector<vector<int> > groups = deriveDsGroups(config, configindex);
	vector<char> haveshard(groups.size(), 0);
	vector<PartRecord> records;
	std::ostringstream err;

	for(size_t g = 0; g < groups.size(); g++)
	{
		char name[64];
		snprintf(name, sizeof(name), "/ds%d.part", (int)g);
		string path = partdir + name;
		ifstream probe(path.c_str());
		if(!probe.is_open())
			continue;			// 缺片，下面统一报
		probe.close();
		haveshard[g] = 1;
		if(!readPart(path, config, &records, &err))
			return fail(monitor, "fxcorr-x merge: " + err.str());
	}

	// ② 缺片：默认报错退出、不写任何东西；FXCORR_X_MERGE_FORCE=1 时写出已到齐
	//    的部分并在 stderr 列明缺了哪些组（data-spec 5.9 的硬约束 + 逃生口）
	bool forced = false;
	if(const char *f = getenv("FXCORR_X_MERGE_FORCE"))
		forced = (strcmp(f, "1") == 0);
	string missing;
	for(size_t g = 0; g < groups.size(); g++)
	{
		if(!haveshard[g])
		{
			if(missing.empty())
				missing = "ds" + std::to_string((long long)g) + ".part";
			else
				missing += ", ds" + std::to_string((long long)g) + ".part";
		}
	}
	if(!missing.empty() && !forced)
	{
		return fail(monitor, "fxcorr-x merge: batch " + batchid + " is missing shard(s) "
		            + missing + " of " + std::to_string((long long)groups.size())
		            + "; nothing was written (set FXCORR_X_MERGE_FORCE=1 to write the "
		              "shards that are present, leaving those frequency bands empty)");
	}
	if(!missing.empty())
	{
		cerr << "fxcorr-x merge: WARNING: writing a partial merge, shard(s) " << missing
		     << " of " << groups.size() << " are missing - the frequency bands they cover "
		        "will have no records in this batch (" << partdir << ")" << endl;
	}
	if(records.empty())
	{
		cerr << "fxcorr-x merge: no records in " << partdir << ", nothing written" << endl;
		return EXIT_SUCCESS;
	}

	// ③ 归并：按积分周期排序（同一周期内的顺序不影响 difx2fits 的读取，
	//    它按记录逐条查 baseline 并对时间做单调检查）
	stable_sort(records.begin(), records.end(), partRecordLess);
	long long lastkey = -1;
	long long ntimeback = 0;
	for(size_t i = 0; i < records.size(); i++)
	{
		if(records[i].key < lastkey)
			ntimeback++;
		lastkey = records[i].key;
	}
	if(ntimeback > 0)
		cerr << "fxcorr-x merge: WARNING: " << ntimeback
		     << " record(s) out of time order after sorting (please report)" << endl;

	// ④ 追加写出：文件名与不分片模式完全一致（experiment 起点决定，与记录时间无关）
	//    输出目录可能还不存在（分片任务不写 SWIN，没人建过它）——与全量模式一样
	//    自己建
	string outdir = config.getOutputFilename();
	if(system(("mkdir -p '" + outdir + "'").c_str()) != 0)
		return fail(monitor, "fxcorr-x merge: cannot create " + outdir);
	char filename[4096];
	snprintf(filename, sizeof(filename), "%s/DIFX_%05d_%06d.s0000.b0000",
	         config.getOutputFilename().c_str(), config.getStartMJD(), config.getStartSeconds());
	ofstream out(filename, ios::app|ios::binary);
	if(!out.is_open())
		return fail(monitor, string("fxcorr-x merge: cannot open ") + filename + " for append");
	for(size_t i = 0; i < records.size(); i++)
		out.write(records[i].bytes.data(), records[i].bytes.size());
	out.close();
	if(!out)
		return fail(monitor, string("fxcorr-x merge: error writing ") + filename);

	FXLOG(FXLOG_INFO) << "fxcorr-x merge: batch " << batchid << ", " << groups.size() << " shard(s), "
	                  << records.size() << " records -> " << filename << endl;
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		cerr << "usage:\n"
		     << "  fxcorr-x <batch_id> [workdir]\n"
		     << "      correlate every datastream of the batch, write SWIN\n"
		     << "  fxcorr-x <batch_id> [workdir] <ds_group>\n"
		     << "      shard mode: correlate only that ds group, write\n"
		     << "      vis-parts/<batch_id>/ds<G>.part instead of SWIN\n"
		     << "  fxcorr-x merge <batch_id> [workdir]\n"
		     << "      merge the batch's shards into SWIN (the only SWIN writer)\n"
		     << "  env: FXCORR_WORKDIR (default .), overridden by the workdir argument;\n"
		     << "      FXCORR_X_MERGE_FORCE=1 writes a partial merge instead of failing\n"
		     << "      when a shard is missing" << endl;
		return EXIT_FAILURE;
	}
	// P3 (algo-plan.md): thread count from OMP_NUM_THREADS; unset = serial
	// (V2 behaviour unchanged).  Must run before any OpenMP parallel region.
	{
		const char *env = getenv("OMP_NUM_THREADS");
		if(env == 0 || env[0] == '\0')
			omp_set_num_threads(1);
	}
	// Three call forms (usage above).  ds_group sits after workdir, same
	// position as fxcorr-f's ds_index, so giving it requires giving workdir.
	bool merge = false;
	int dsgroup = -1;
	string batchid;
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;
	if(strcmp(argv[1], "merge") == 0)
	{
		if(argc < 3)
		{
			cerr << "fxcorr-x merge: missing batch_id" << endl;
			return EXIT_FAILURE;
		}
		merge = true;
		batchid = argv[2];
		if(argc > 3)
			workdir = argv[3];	// argument takes precedence over the environment
	}
	else
	{
		batchid = argv[1];
		if(argc > 2)
			workdir = argv[2];	// argument takes precedence over the environment
		if(argc > 3)
		{
			char *end;
			long v = strtol(argv[3], &end, 10);
			if(end == argv[3] || *end != '\0' || v < 0)
			{
				cerr << "fxcorr-x: bad ds_group '" << argv[3]
				     << "' (a non-negative integer expected)" << endl;
				return EXIT_FAILURE;
			}
			dsgroup = (int)v;
		}
	}
	bool sharded = (dsgroup >= 0);
	// hand the resolved workdir to the shared root resolver (V5 P5): the
	// library opens .input/.calc/.im paths through it, so every relative path
	// in the config set lands on the same roots this tool uses below
	FxcorrPath::init(workdir);
	workdir = FxcorrPath::workdir();
	FxcorrPath::print("fxcorr-x");
	// the roots each form touches: full correlation reads fengine and writes
	// vis; a shard only reads fengine (its output is vis-parts/, which has no
	// root - it always lives under the workdir); merge only writes vis
	static const FxcorrPath::Root usedroots_all[] = {
		FxcorrPath::ROOT_FENGINE, FxcorrPath::ROOT_VIS };
	static const FxcorrPath::Root usedroots_shard[] = {
		FxcorrPath::ROOT_FENGINE };
	static const FxcorrPath::Root usedroots_merge[] = {
		FxcorrPath::ROOT_VIS };
	if(merge)
	{
		if(!FxcorrPath::checkRoots(batchid, usedroots_merge, 1, "fxcorr-x"))
			return EXIT_FAILURE;
	}
	else if(sharded)
	{
		if(!FxcorrPath::checkRoots(batchid, usedroots_shard, 1, "fxcorr-x"))
			return EXIT_FAILURE;
	}
	else if(!FxcorrPath::checkRoots(batchid, usedroots_all, 2, "fxcorr-x"))
	{
		return EXIT_FAILURE;
	}

	// batch.json is pre-written by run_batch.sh (batches/<batch_id>.json)
	string batchjsonpath = workdir + "/batches/" + batchid + ".json";
	ifstream jf(batchjsonpath.c_str());
	if(!jf.is_open())
	{
		cerr << "fxcorr-x: cannot open " << batchjsonpath << endl;
		return EXIT_FAILURE;
	}
	stringstream jss;
	jss << jf.rdbuf();
	string json = jss.str();
	jf.close();

	double startmjd = 0.0;
	int nsubints = 0;
	string inputfile;
	if(!extractJsonDouble(json, "start_mjd", &startmjd) ||
	   !extractJsonInt(json, "n_subints", &nsubints) ||
	   !extractJsonString(json, "config_file", &inputfile))
	{
		cerr << "fxcorr-x: batch.json missing required fields" << endl;
		return EXIT_FAILURE;
	}

	// FXCORR_LOGLEVEL has to be in force before the configuration is loaded:
	// fxcorrcommon reports the whole parsing pass through the Alert streams
	// (see log.h), which is most of what the tool otherwise prints.
	fxApplyAlertLevel();

	// parse .input (non-MPI constructor)
	Configuration config((workdir + "/" + inputfile).c_str(), 0);
	Model *model = config.getModel();

	// P1: DifxMessage status emission (algo-plan.md).  x plays the manager
	// role: mpiId = 0, RUNNING per written integration (via Integrator).
	// container mode logs to meta/difxmsg/<exp>_<batch>.xml
	string expname = jobIdentifier(inputfile);
	string containerprefix;
	if(const char *runmode = getenv("FXCORR_RUN_MODE"))
	{
		if(strcmp(runmode, "container") == 0)
			containerprefix = workdir + "/meta/difxmsg/" + expname + "_" + batchid;
	}
	if(containerprefix.size() > 0)
	{
		if(system(("mkdir -p " + workdir + "/meta/difxmsg").c_str()) != 0)
			cerr << "fxcorr-x: cannot create " << workdir << "/meta/difxmsg" << endl;
	}
	DifxMonitor monitor(0, expname, inputfile, containerprefix);

	// V1 restrictions (v1-plan 1 / data-spec 12)
	if(model->getNumScans() != 1)
	{
		ostringstream oss;
		oss << "fxcorr-x: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)";
		return fail(monitor, oss.str());
	}
	int scan = 0;
	int configindex = config.getScanConfigIndex(scan);
	if(configindex < 0)
		return fail(monitor, "fxcorr-x: no configuration for scan 0");
	// P7: cross-polar autocorrelations (maxproducts > 2) are supported; the
	// autocorr.bin cross-pol section flag decides whether the extra records
	// are read (Integrator::addAutocorrs).  P8: phased arrays are supported
	// (beam forming early-return branch below, no SWIN output).

	// AC_INIT version of fxcorr-x
	monitor.status(DIFX_STATE_STARTING, "Version 0.1.0", 0.0, 0, 0, 0.0, 0.0);

	int subintns = config.getSubintNS(configindex);
	int blockspersend = config.getBlocksPerSend(configindex);
	int numbufferedffts = config.getNumBufferedFFTs(configindex);

	// intTime must be an integer multiple of subintns, so that Visibility's
	// dump grid coincides with the subint grid (offsetnsperintegration == 0).
	// Phased array batches write no SWIN and need no intTime grid (P8).
	if(!config.phasedArrayOn(configindex))
	{
		long long inttimens = (long long)(config.getIntTime(configindex)*1.0e9);
		if(inttimens % (long long)subintns != 0)
		{
			ostringstream oss;
			oss << "fxcorr-x: intTime " << config.getIntTime(configindex) << " s is not an integer multiple of subintNS " << subintns << " ns (required in V1)";
			return fail(monitor, oss.str());
		}
	}

	// 互斥检查（三种模式共用）：本 batch 的时间范围若已经在 SWIN 里，说明它已经
	// 被另一次运行写过——全量模式、或者 merge。再写一遍就会追加重复记录、破坏
	// 时间单调。**分片任务本身不写 SWIN，所以正常重跑分片不会被拦**；会被拦的
	// 是"merge 之后又跑全量/又 merge"，那正是要挡的情况。
	{
		char swinpath[4096];
		snprintf(swinpath, sizeof(swinpath), "%s/DIFX_%05d_%06d.s0000.b0000",
		         config.getOutputFilename().c_str(), config.getStartMJD(), config.getStartSeconds());
		double duration = (double)nsubints * (double)config.getSubintNS(configindex) / 1.0e9;
		long long nconflict = 0;
		bool allow = false;
		if(const char *v = getenv("FXCORR_X_SWIN_CONFLICT"))
			allow = (strcmp(v, "allow") == 0);
		if(swinHasBatchRange(swinpath, config, startmjd, duration, &nconflict) && !allow)
		{
			ostringstream oss;
			oss << "fxcorr-x: " << swinpath << " already holds " << nconflict
			    << " record(s) inside this batch's time range (start_mjd " << startmjd
			    << ", duration " << duration << " s): this batch has been written already, "
			       "by a full (non-sharded) run or by a merge.  Writing it again would "
			       "append duplicate records and break the monotonic time order difx2fits "
			       "relies on (it silently drops out-of-time records).  Remove or rename "
			       "the SWIN file - or set FXCORR_X_SWIN_CONFLICT=allow - if you really "
			       "mean to redo this batch.";
			return fail(monitor, oss.str());
		}
	}

	// merge 分支：读分片、归并、写 SWIN（本工具唯一写 SWIN 的路径；
	// 分片的输出是 D16，见 data-spec 5.9）
	if(merge)
	{
		monitor.status(DIFX_STATE_STARTING, "Version 0.1.0", 0.0, 0, 0, 0.0, 0.0);
		int rc = doMerge(config, workdir, batchid, monitor);
		monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
		if(rc == EXIT_SUCCESS)
			monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);
		return rc;
	}

	// ---- 分片模式：只处理一个 ds 组，输出 D16 而不是 SWIN --------------------
	// 一组 = 跨站、含全部极化的一批 datastream，它们覆盖同一个频段组；成员从
	// .input 的 BASELINE TABLE 推导（连通分量），不是按 ds 序号或 freq 条目。
	// 全量模式（不传 ds_group）下两个掩码都是空的，所有循环与改动前一致。
	vector<char> dsactive(config.getNumDataStreams(), 1);
	vector<char> blactive(config.getNumBaselines(), 1);
	string partpath;
	if(sharded)
	{
		// D16 必须是"SWIN 记录流原样"（merge 只做搬运）。多相位中心与
		// pulsar binning 会走 flushBuffersToDisk 的多文件分支，分片会丢数据，
		// 这两种配置直接挡掉——分片的目标场景是常规观测
		int nphasecentres = model->getNumPhaseCentres(scan);
		if(nphasecentres != 1)
			return fail(monitor, "fxcorr-x: shard mode supports a single phase centre only (got "
			            + std::to_string((long long)nphasecentres) + ")");
		if(config.pulsarBinOn(configindex))
			return fail(monitor, "fxcorr-x: shard mode does not support pulsar binning");
		if(config.phasedArrayOn(configindex))
			return fail(monitor, "fxcorr-x: shard mode does not apply to phased array configurations");

		vector<vector<int> > groups = deriveDsGroups(config, configindex);
		if(dsgroup >= (int)groups.size())
			return fail(monitor, "fxcorr-x: ds_group " + std::to_string((long long)dsgroup)
			            + " is out of range - this batch has "
			            + std::to_string((long long)groups.size()) + " ds group(s)");
		for(int i = 0; i < config.getNumDataStreams(); i++)
			dsactive[i] = 0;
		for(size_t k = 0; k < groups[dsgroup].size(); k++)
			dsactive[groups[dsgroup][k]] = 1;
		// 一条 baseline 只有在它声明的两个 ds 都在场时才算得出来
		for(int b = 0; b < config.getNumBaselines(); b++)
		{
			int d1 = config.getBOrderedDataStream1Index(configindex, b);
			int d2 = config.getBOrderedDataStream2Index(configindex, b);
			blactive[b] = (dsactive[d1] && dsactive[d2]) ? 1 : 0;
		}
		partpath = workdir + "/vis-parts/" + batchid + "/ds"
		           + std::to_string((long long)dsgroup) + ".part";
		string mkdircommand = "mkdir -p " + workdir + "/vis-parts/" + batchid;
		if(system(mkdircommand.c_str()) != 0)
			return fail(monitor, "fxcorr-x: cannot create " + workdir + "/vis-parts/" + batchid);
		ostringstream oss;
		oss << "fxcorr-x: shard mode, ds group " << dsgroup << " of " << groups.size() << " = {";
		for(size_t k = 0; k < groups[dsgroup].size(); k++)
			oss << (k ? "," : "") << groups[dsgroup][k];
		oss << "} -> " << partpath;
		FXLOG(FXLOG_INFO) << oss.str() << endl;
	}

	// autocorr.bin carries maxacblocks-batch records per subint; the batch
	// size is read from the file header by Integrator::addAutocorrs
	double blockns = (double)subintns/(double)blockspersend;

	// open one SpReader per (datastream, band) in datastream-total band order:
	// recorded bands read their band_XX.sp whole, zoom bands are a channel-slice
	// view of the parent recorded band's .sp (P4a, data-spec 5.3)
	int numdatastreams = config.getNumDataStreams();
	vector<vector<SpReader *> > readers(numdatastreams);
	vector<string> autocorrFiles(numdatastreams);
	for(int ds=0;ds<numdatastreams;ds++)
	{
		if(!dsactive[ds])
			continue;	// shard mode: not ours - leave readers[ds] empty
		string station = config.getDStationName(configindex, ds);
		// fengine layout: ds_<N>/ subdirectory, N = station-local datastream
		// index (data-spec 5.3; multi-datastream stations from fxcorr-f's
		// ds_index parameter)
		int dswithinstation = 0;
		for(int d=0;d<ds;d++)
		{
			if(config.getDStationName(configindex, d) == station)
				dswithinstation++;
		}
		string sdir = FxcorrPath::root(FxcorrPath::ROOT_FENGINE) + "/" + batchid + "/" + station + "/ds_" + to_string(dswithinstation);
		int nrecordedbands = config.getDNumRecordedBands(configindex, ds);
		int ntotalbands = config.getDNumTotalBands(configindex, ds);
		readers[ds].resize(ntotalbands);
		for(int band=0;band<ntotalbands;band++)
		{
			char filename[32];
			int freqindex;
			if(band < nrecordedbands)
			{
				snprintf(filename, sizeof(filename), "band_%02d.sp", band);
				freqindex = config.getDRecordedFreqIndex(configindex, ds, band);
				readers[ds][band] = new SpReader(sdir + "/" + filename);
			}
			else
			{
				// zoom band: parent recorded band .sp with a channel slice
				int localzoom = config.getDLocalZoomFreqIndex(configindex, ds, band-nrecordedbands);
				int parentfreqindex = config.getDZoomFreqParentFreqIndex(configindex, ds, localzoom);
				int parentband = -1;
				for(int l=0;l<nrecordedbands;l++)
				{
					if(config.getDLocalRecordedFreqIndex(configindex, ds, l) == parentfreqindex &&
					   config.getDZoomBandPol(configindex, ds, band-nrecordedbands) == config.getDRecordedBandPol(configindex, ds, l))
					{
						parentband = l;
						break;
					}
				}
				if(parentband < 0)
					return fail(monitor, "fxcorr-x: no parent band for zoom band " + to_string(band) + " of station " + station);
				snprintf(filename, sizeof(filename), "band_%02d.sp", parentband);
				freqindex = config.getDZoomFreqIndex(configindex, ds, localzoom);
				readers[ds][band] = new SpReader(sdir + "/" + filename,
					config.getDZoomFreqChannelOffset(configindex, ds, localzoom),
					config.getFNumChannels(freqindex));
			}
			if(!readers[ds][band]->ok())
				return fail(monitor, "fxcorr-x: cannot read " + sdir + "/" + filename);
			if(readers[ds][band]->numChannels() != config.getFNumChannels(freqindex) ||
			   readers[ds][band]->blocksPerSend() != (u32)blockspersend ||
			   readers[ds][band]->numBufferedFFTs() != (u32)numbufferedffts ||
			   readers[ds][band]->subintNS() != (u32)subintns ||
			   readers[ds][band]->numSubints() < (u32)nsubints)
			{
				ostringstream oss;
				oss << "fxcorr-x: " << sdir << "/" << filename << " header does not match the config (nchan " << readers[ds][band]->numChannels() << " vs " << config.getFNumChannels(freqindex) << ", bps " << readers[ds][band]->blocksPerSend() << " vs " << blockspersend << ", nsub " << readers[ds][band]->numSubints() << " vs " << nsubints << ")";
				return fail(monitor, oss.str());
			}
		}
		autocorrFiles[ds] = sdir + "/autocorr.bin";
	}

	XmacEngine xmac(&config, configindex, model, scan);

	// batch start expressed as job-relative seconds (same as fxcorr-f main)
	long long scanstartsec = (long long)model->getScanStartSec(scan, config.getStartMJD(), config.getStartSeconds());
	double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds()/86400.0;
	double batchstartjob = (startmjd - jobstart)*86400.0;

	// data-spec section 12: the batch start must lie on a subint boundary;
	// tolerance absorbs the f64 representation error of start_mjd (~1 us)
	{
		long long batchstartns = (long long)floor(batchstartjob*1.0e9 + 0.5);
		if(batchstartns % subintns > 1000 && (subintns - batchstartns % subintns) > 1000)
		{
			ostringstream oss;
			oss << "fxcorr-x: batch start " << startmjd << " is not on a subint boundary ("
			    << batchstartns % subintns << " ns into a " << subintns << " ns subint)";
			return fail(monitor, oss.str());
		}
	}

	// P8: phased array beam forming (algo-plan.md).  No cross-correlation/SWIN
	// output: upstream has no consumer for the beam data (core.cpp:818-865
	// fills threadcrosscorrs in a baseline-free layout that
	// uvshiftAndAverage cannot consume), so beam.bin is defined by fxcorr
	// (data-spec.md).  Station products (.sp/autocorr.bin) still come from
	// fxcorr-f as usual.  The beam branch needs no intTime grid.
	if(config.phasedArrayOn(configindex))
	{
		// beam/ has no separate root: like config/ and meta/ it always lives
		// under the workdir (v5-plan.md Q17)
		BeamEngine beam(&config, configindex, workdir, batchid, nsubints, readers);
		if(!beam.ok())
			return fail(monitor, "fxcorr-x: cannot initialise beam output");
		for(int s=0;s<nsubints;s++)
		{
			// read this subint from every station, checking the time stamps
			// (same time formula as the XMAC branch below)
			double subintstart = batchstartjob + (double)s*(double)subintns/1.0e9;
			double sreld = subintstart - (double)scanstartsec;
			int expectedsec = (int)floor(sreld);
			int expectedns = (int)((sreld - (double)expectedsec)*1.0e9 + 0.5);
			for(int ds=0;ds<numdatastreams;ds++)
			{
				for(size_t band=0;band<readers[ds].size();band++)
				{
					int rsec, rns, rscan;
					if(!readers[ds][band]->readSubint(s, rscan, rsec, rns))
					{
						ostringstream oss;
						oss << "fxcorr-x: failed to read subint " << s << " of station " << config.getDStationName(configindex, ds) << " band " << band;
						return fail(monitor, oss.str());
					}
					if(rscan != scan || rsec != expectedsec || rns != expectedns)
					{
						ostringstream oss;
						oss << "fxcorr-x: subint " << s << " of station " << config.getDStationName(configindex, ds) << " band " << band << " has time " << rscan << "/" << rsec << "/" << rns << ", expected " << scan << "/" << expectedsec << "/" << expectedns;
						return fail(monitor, oss.str());
					}
				}
			}
			beam.processSubint(s, scan, expectedsec, expectedns);
		}
		FXLOG(FXLOG_INFO) << "fxcorr-x: batch " << batchid << " complete, " << nsubints << " subints of beam output written" << endl;
		for(int ds=0;ds<numdatastreams;ds++)
			for(size_t band=0;band<readers[ds].size();band++)
				delete readers[ds][band];
		// upstream ending sequence (fxmanager.cpp terminate/DONE)
		monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
		monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);
		return EXIT_SUCCESS;
	}

	// Visibility start time: batch start relative to the scan start
	double reld = batchstartjob - (double)scanstartsec;
	if(reld < 0.0)
		return fail(monitor, "fxcorr-x: batch starts before the scan start");
	int initsec = (int)floor(reld);
	int initns = (int)((reld - (double)initsec)*1.0e9 + 0.5);
	if(initns >= 1000000000)
	{
		initsec++;
		initns -= 1000000000;
	}

	// Visibility::writedata stops when currentstartseconds + scanstartsec >=
	// executeseconds, i.e. executeseconds is measured from the scan start
	// (mpifxcorr EXECUTE TIME semantics); a batch starting initsec into the
	// scan must shift the limit by that offset (+1 so the last integration
	// always clears it)
	int executeseconds = (int)((double)nsubints*(double)subintns/1.0e9 + 0.5) + initsec + 1;
	Integrator integrator(&config, configindex, executeseconds, scan, initsec, initns, &monitor);

	// 分片模式：把"哪些 baseline / datastream 在场"交给两个引擎，并把记录
	// 写到 D16 而不是 SWIN（全量模式下这三个 setter 都不调，行为与改动前一致）
	if(sharded)
	{
		xmac.setActiveBaselines(blactive);
		integrator.visibility()->setActiveBaselines(blactive);
		integrator.visibility()->setActiveDatastreams(dsactive);
		integrator.visibility()->setOutputPath(partpath);
	}

	int coreresultlength = config.getCoreResultLength(configindex);
	cf32 *subintresults = vectorAlloc_cf32(coreresultlength);

	// per-subint loop (Core::processdata + FxManager::addData merged)
	double maxNSBetweenXCAvg = model->getMaxNSBetweenXCAvg(scan);
	int maxxcblocks = (int)(maxNSBetweenXCAvg/blockns);
	maxxcblocks -= maxxcblocks%numbufferedffts;
	if(maxxcblocks == 0)
	{
		maxxcblocks = numbufferedffts;
		cerr << "fxcorr-x: requested cross-correlation shift/average time of " << maxNSBetweenXCAvg << " ns cannot be met with " << numbufferedffts << " FFTs being buffered; the time resolution which will be attained is " << maxxcblocks*blockns << " ns" << endl;
	}

	int fftloops = (blockspersend + numbufferedffts - 1)/numbufferedffts;
	int integrationswritten = 0;
	for(int s=0;s<nsubints;s++)
	{
		// read this subint from every station, checking the time stamps
		// (same time formula as fxcorr-f main, so sec/ns must match exactly)
		double subintstart = batchstartjob + (double)s*(double)subintns/1.0e9;
		double sreld = subintstart - (double)scanstartsec;
		int expectedsec = (int)floor(sreld);
		int expectedns = (int)((sreld - (double)expectedsec)*1.0e9 + 0.5);
		// scan-relative seconds of the subint start, same time base as the
		// upstream delay interpolator (offsets[1] + offsets[2]/1e9)
		double offsetsec = (double)expectedsec + (double)expectedns/1.0e9;

		// pulsar: pick the polyco covering this subint and set its time
		// (core.cpp:496-508, day-of-year seconds time base - NOT the same as
		// offsetsec, which is scan-relative and used by the delay model)
		Polyco *currentpolyco = 0;
		if(config.pulsarBinOn(configindex))
		{
			double sec = double(config.getStartSeconds() + scanstartsec + expectedsec) + ((double)expectedns)/1000000000.0;
			currentpolyco = Polyco::getCurrentPolyco(configindex, config.getStartMJD(), sec/86400.0, config.getPolycos(configindex), config.getNumPolycos(configindex), false);
			if(currentpolyco == NULL)
			{
				ostringstream oss;
				oss << "fxcorr-x: could not locate a polyco to cover time " << config.getStartMJD() + sec/86400.0;
				return fail(monitor, oss.str());
			}
			currentpolyco->setTime(config.getStartMJD(), sec/86400.0);
		}
		for(int ds=0;ds<numdatastreams;ds++)
		{
			// every band view (recorded + zoom) must read its subint; zoom
			// views share the parent .sp so their timestamps are identical
			for(size_t band=0;band<readers[ds].size();band++)
			{
				int rsec, rns, rscan;
				if(!readers[ds][band]->readSubint(s, rscan, rsec, rns))
				{
					ostringstream oss;
					oss << "fxcorr-x: failed to read subint " << s << " of station " << config.getDStationName(configindex, ds) << " band " << band;
					return fail(monitor, oss.str());
				}
				if(rscan != scan || rsec != expectedsec || rns != expectedns)
				{
					ostringstream oss;
					oss << "fxcorr-x: subint " << s << " of station " << config.getDStationName(configindex, ds) << " band " << band << " has time " << rscan << "/" << rsec << "/" << rns << ", expected " << scan << "/" << expectedsec << "/" << expectedns;
					return fail(monitor, oss.str());
				}
			}
		}

		// zero the per-subint accumulation (threadcrosscorrs, baselineweights, results)
		xmac.zeroSubint();
		vectorZero_cf32(subintresults, coreresultlength);

		// XMAC + uvshift/average, batched exactly like core.cpp:786-1057
		int xcblockcount = 0, xcshiftcount = 0;
		for(int fftloop=0;fftloop<fftloops;fftloop++)
		{
			int numffts = blockspersend - fftloop*numbufferedffts;
			if(numffts > numbufferedffts)
				numffts = numbufferedffts;

			xmac.xmacBatch(fftloop, readers, currentpolyco);
			xmac.accumulateWeights(fftloop, readers);
			xcblockcount += numffts;
			if(xcblockcount == maxxcblocks)
			{
				double nsoffset = (xcshiftcount*maxxcblocks + ((double)maxxcblocks)/2.0)*blockns;
				xmac.uvshiftAndAverage(offsetsec, nsoffset, maxxcblocks*blockns, currentpolyco, subintresults);
				xcblockcount = 0;
				xcshiftcount++;
			}
		}
		if(xcblockcount != 0)
		{
			double nsoffset = (xcshiftcount*maxxcblocks + ((double)xcblockcount)/2.0)*blockns;
			xmac.uvshiftAndAverage(offsetsec, nsoffset, xcblockcount*blockns, currentpolyco, subintresults);
		}

		// baseline weights -> floatresults section (core.cpp:1065-1107, no locks)
		xmac.copyBaselineWeights((f32*)subintresults);

		// autocorrelations come from autocorr.bin (one record per subint)
		integrator.addAutocorrs(s, autocorrFiles, subintresults);

		if(integrator.addSubint(subintresults))
			integrationswritten++;
	}

	FXLOG(FXLOG_INFO) << "fxcorr-x: batch " << batchid << " complete, " << nsubints << " subints, " << integrationswritten << " integrations written" << endl;

	for(int ds=0;ds<numdatastreams;ds++)
		for(size_t band=0;band<readers[ds].size();band++)
			delete readers[ds][band];
	vectorFree(subintresults);

	// upstream ending sequence (fxmanager.cpp terminate/DONE): Ending then Done
	monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
	monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);

	return EXIT_SUCCESS;
}
