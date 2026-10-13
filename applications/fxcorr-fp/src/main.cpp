#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>	// getpid (timing event lines)

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/difxmonitor.h>
#include <fxcorrcommon/switchedpower.h>
#include <fxcorrcommon/fxcorrpath.h>

#include "datareader.h"
#include "streamwriter.h"
#include "log.h"

using namespace std;

// ---- stage timing (FXCORR_TIMING; measurement only, no behaviour change) ----
// Three switches, all off by default (every nowsec() call is then skipped, so
// an instrumented build does the same work as an uninstrumented one):
//
//   FXCORR_TIMING=1            enable; one aggregate TIMING line at exit
//   FXCORR_TIMING_FILE=<path>  send the output to <path> (appended) instead
//                              of stderr; setting it alone also enables
//   FXCORR_TIMING_TRACE=1      also write one TSV event line per completed
//                              stage (enables the timer too):
//     <epoch sec>\t<prog>\t<pid>\t<thread>\t<file:line>\t<label>\t<sec>
//
// The aggregate line is the production form (per-stage totals); the event
// lines are for debugging and detailed optimisation.  This tool is serial,
// so the thread column stays 0 (the seven-column format is shared with
// fxcorr-f).
static bool g_timing = false;
static bool g_timing_trace = false;
static bool g_timing_brief = false;
static FILE *g_timing_out = 0;
static double g_tt_read, g_tt_swp, g_tt_write, g_tt_start;
static double g_tt_init, g_tt_fin, g_tt_del;

static inline double nowsec()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec*1e-9;
}

// wall clock for the event timestamps: they must be comparable across
// processes, so epoch seconds, not the monotonic clock the durations use
static inline double epochsec()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec*1e-9;
}

// ---------------------------------------------------------------- timing tiers
// Three tiers share the seven STAGE_LAP call sites; only the output path
// differs (tier comments at the initialisation below):
//   aggregate -- one summary line at exit, unchanged
//   brief     -- one TSV row per probe at exit (the table below drives it)
//   trace     -- every lap buffered in memory, written once at exit
struct TimingEvent
{
	double ts;		// epoch seconds at the lap
	double dur;		// seconds since the timer's previous lap
	int tid;		// always 0 (serial tool; column kept for the TSV format)
	int line;
	const char *file;	// basename of the lap site
	const char *label;
};
static vector<TimingEvent> g_trace_buf;

struct TimingProbe
{
	const char *label;
	const char *file;
	int line;
	double *total;		// the matching g_tt_* accumulator
	double first_ts;	// epoch seconds of the first lap (0 = not seen)
};
// pipeline order: init -> read -> swp -> write -> fin -> del
static TimingProbe g_probes[] = {
	{"init",    "main.cpp", 604, &g_tt_init,  0},
	{"read",    "main.cpp", 625, &g_tt_read,  0},
	{"swp",     "main.cpp", 680, &g_tt_swp,   0},
	{"write",   "main.cpp", 684, &g_tt_write, 0},
	{"fin",     "main.cpp", 713, &g_tt_fin,   0},
	{"del",     "main.cpp", 702, &g_tt_del,   0},
	{0, 0, 0, 0, 0}
};

static void timing_event(const char *label, const char *file, int line,
                         double dur, double ts)
{
	const char *base = strrchr(file, '/');
	base = (base != 0) ? base+1 : file;
	if(g_timing_trace)
	{
		TimingEvent ev;
		ev.ts = ts;
		ev.dur = dur;
		ev.tid = 0;
		ev.line = line;
		ev.file = base;
		ev.label = label;
		g_trace_buf.push_back(ev);
	}
	else if(g_timing_brief)
	{
		for(TimingProbe *p = g_probes; p->label != 0; p++)
			if(strcmp(p->label, label) == 0)
			{
				if(p->first_ts == 0.0)
					p->first_ts = ts;
				break;
			}
	}
}

// write the tier's product: brief = one row per probe (first timestamp +
// accumulated total), trace = the whole buffer.  Called on the normal exit
// path and from fail(), so an aborted task still leaves its events behind.
static void timing_finish(void)
{
	static bool done = false;
	if(done || !g_timing)
		return;
	done = true;
	if(g_timing_trace)
	{
		for(size_t i=0;i<g_trace_buf.size();i++)
		{
			const TimingEvent &e = g_trace_buf[i];
			fprintf(g_timing_out, "%.6f\tfxcorr-fp\t%d\t%d\t%s:%d\t%s\t%.6f\n",
			        e.ts, (int)getpid(), e.tid, e.file, e.line, e.label, e.dur);
		}
		g_trace_buf.clear();
	}
	else if(g_timing_brief)
	{
		for(TimingProbe *p = g_probes; p->label != 0; p++)
		{
			if(p->first_ts == 0.0)
				continue;	// probe never reached (e.g. aborted early)
			fprintf(g_timing_out, "%.6f\tfxcorr-fp\t%d\t%d\t%s:%d\t%s\t%.6f\n",
			        p->first_ts, (int)getpid(), 0, p->file, p->line, p->label,
			        *p->total);
		}
	}
	fflush(g_timing_out);
	if(g_timing_out != stderr)
	{
		fclose(g_timing_out);
		g_timing_out = stderr;	// harmless default if anything runs on
	}
}

// STAGE_LAP(timer, label): the one lap form -- returns the seconds since the
// timer's construction (or previous lap); with tracing on it also writes one
// event line, and label + __FILE__:__LINE__ name the step in both outputs
#define STAGE_LAP(timer, label) (timer).lap(label, __FILE__, __LINE__)

struct StageTimer
{
	double t0;
	StageTimer() : t0(g_timing ? nowsec() : 0.0) {}
	double lap(const char *label, const char *file, int line)
	{
		if(!g_timing)
			return 0.0;
		double t = nowsec();
		double d = t - t0;
		t0 = t;
		if(g_timing_trace || g_timing_brief)
			timing_event(label, file, line, d, epochsec());
		return d;
	}
};

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
	timing_finish();	// brief rows / trace buffer survive an aborted task
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

// Data-file localization (v8-plan.md 2.2, 2026-10-02): the .input FILE lines
// are retired (a shared mutable name space cannot serve several batches at
// once); the files are located in two states instead --
//
//   1. FXCORR_REAL_PATH -- set by the module runner from the task header
//      `real_path` (debugging: read straight from the shared storage):
//      comma-separated list, each item resolved against the RAW root when
//      relative, used as-is when absolute (FxcorrPath::under, the same rule
//      Configuration applies to FILE lines);
//   2. otherwise the layout rule
//      <RAW_ROOT>/<station>/<station>_<batch>[_ds<N>].vdif -- the `_ds<N>`
//      suffix first (multi-datastream stations), then no suffix (single,
//      data-spec 5.2).  fxcorr-sim's stationOutPath and fxcorr-f's copy of
//      this function carry the same rule; the places change together
//      (v8-plan.md 2.5-2).
//
// Returns an empty list when nothing is found (the error has been printed).
static vector<string> resolveDataFiles(const string &station, const string &batchid, int dsarg)
{
	const string rawroot = FxcorrPath::root(FxcorrPath::ROOT_RAW);

	if(const char *env = getenv("FXCORR_REAL_PATH"))
	{
		string list = env;
		vector<string> files;
		size_t pos = 0;
		while(pos <= list.size())
		{
			size_t comma = list.find(',', pos);
			string item = (comma == string::npos) ? list.substr(pos)
			                                      : list.substr(pos, comma - pos);
			if(item.size() > 0)
				files.push_back(FxcorrPath::under(rawroot, item));
			if(comma == string::npos)
				break;
			pos = comma + 1;
		}
		if(files.size() > 0)
			return files;
	}

	char suffix[32];
	snprintf(suffix, sizeof(suffix), "_ds%d", dsarg);
	const string candidates[2] = {
		rawroot + "/" + station + "/" + station + "_" + batchid + suffix + ".vdif",
		rawroot + "/" + station + "/" + station + "_" + batchid + ".vdif" };
	struct stat st;
	for(int i=0;i<2;i++)
	{
		if(stat(candidates[i].c_str(), &st) == 0 && S_ISREG(st.st_mode))
			return vector<string>(1, candidates[i]);
	}

	cerr << "fxcorr-fp: no data file for station " << station << " batch " << batchid
	     << " ds " << dsarg << endl
	     << "  tried " << candidates[0] << endl
	     << "        " << candidates[1] << endl
	     << "  or set FXCORR_REAL_PATH to the file path(s), comma-separated" << endl;
	return vector<string>();
}

int main(int argc, char **argv)
{
	if(argc < 3)
	{
		cerr << "usage: fxcorr-fp <batch_id> <station> [workdir] [ds_index]" << endl
		     << "  env: FXCORR_WORKDIR (default .), overridden by the workdir argument" << endl
		     << "       FXCORR_REAL_PATH: data file path(s), comma-separated"
		     << " (default: raw-root layout rule)" << endl;
		return EXIT_FAILURE;
	}
	// stage timing, three tiers:
	//   1 aggregate: FXCORR_TIMING=1 -- one summary line, unchanged (output
	//     file via FXCORR_TIMING_FILE)
	//   2 brief: FXCORR_TIMING_BRIEF=1|<path> -- one TSV row per probe at
	//     exit, first timestamp + accumulated total (<=20 rows; 1 = stderr,
	//     a path = that file)
	//   3 trace: FXCORR_TIMING_TRACE=<path>|1 -- every lap buffered in memory
	//     and written once at exit; 1 = <workdir>/fxcorr-fp-<batch>.timing.tsv
	// Trace wins over brief wins over aggregate when several are set.
	{
		const char *timingenv = getenv("FXCORR_TIMING");
		const char *fileenv = getenv("FXCORR_TIMING_FILE");
		const char *traceenv = getenv("FXCORR_TIMING_TRACE");
		const char *briefenv = getenv("FXCORR_TIMING_BRIEF");
		bool trace_path = (traceenv != 0 && strchr(traceenv, '/') != 0);
		bool brief_path = (briefenv != 0 && strchr(briefenv, '/') != 0);
		g_timing_trace = (traceenv != 0
		                  && (strcmp(traceenv, "1") == 0 || trace_path));
		g_timing_brief = (briefenv != 0
		                  && (strcmp(briefenv, "1") == 0 || brief_path))
		              && !g_timing_trace;
		g_timing = (timingenv != 0 && strcmp(timingenv, "1") == 0)
		        || (fileenv != 0 && fileenv[0] != '\0')
		        || g_timing_trace || g_timing_brief;
		g_timing_out = stderr;
		const char *outpath = 0;
		if(g_timing_trace && trace_path)
			outpath = traceenv;
		else if(g_timing_brief && brief_path)
			outpath = briefenv;
		else if(fileenv != 0 && fileenv[0] != '\0')
			outpath = fileenv;
		if(g_timing && outpath != 0)
		{
			FILE *f = fopen(outpath, "a");
			if(f == 0)
				cerr << "fxcorr-fp: cannot open timing file " << outpath << ", using stderr" << endl;
			else
				g_timing_out = f;
		}
		if(g_timing)
			g_tt_start = nowsec();
	}
	string batchid = argv[1];
	string station = argv[2];
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;
	if(argc > 3)
		workdir = argv[3];	// argument takes precedence over the environment

	// FXCORR_TIMING_TRACE=1 without a path: name the trace dump after the
	// workdir and batch, which are only known here
	if(g_timing_trace && g_timing_out == stderr)
	{
		string tp = workdir + "/fxcorr-fp-" + batchid + ".timing.tsv";
		FILE *f = fopen(tp.c_str(), "a");
		if(f != 0)
			g_timing_out = f;
		else
			cerr << "fxcorr-fp: cannot open timing file " << tp << ", using stderr" << endl;
	}
	// hand the resolved workdir to the shared root resolver (V5 P5): the
	// library opens .input/.calc/.im paths through it, so every relative path
	// in the config set lands on the same roots this tool uses below
	FxcorrPath::init(workdir);
	workdir = FxcorrPath::workdir();
	FxcorrPath::print("fxcorr-fp");
	// the roots THIS tool touches: raw (input data), fengine (its stream.bin)
	// and vis (it writes the SWITCHEDPOWER text file into the SWIN dir)
	static const FxcorrPath::Root usedroots[] = {
		FxcorrPath::ROOT_RAW, FxcorrPath::ROOT_FENGINE, FxcorrPath::ROOT_VIS };
	if(!FxcorrPath::checkRoots(batchid, usedroots, 3, "fxcorr-fp"))
		return EXIT_FAILURE;
	// multi-datastream stations (real observations: one datastream per
	// recording thread): ds_index selects the station's n-th datastream
	// (0-based, default 0; the stream goes to <ds-group>/<station>/
	// ds_<ds_index>/ as of the 2026-10-09 per-group layout)
	int dsarg = 0;
	if(argc > 4)
		dsarg = atoi(argv[4]);
	if(dsarg < 0)
	{
		cerr << "fxcorr-fp: ds_index must be >= 0" << endl;
		return EXIT_FAILURE;
	}

	// batch.json is pre-written by run_batch.sh (batches/<batch_id>.json)
	string batchjsonpath = workdir + "/batches/" + batchid + ".json";
	ifstream jf(batchjsonpath.c_str());
	if(!jf.is_open())
	{
		cerr << "fxcorr-fp: cannot open " << batchjsonpath << endl;
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
		cerr << "fxcorr-fp: batch.json missing required fields" << endl;
		return EXIT_FAILURE;
	}

	// FXCORR_LOGLEVEL has to be in force before the configuration is loaded:
	// fxcorrcommon reports the whole parsing pass through the Alert streams
	// (see log.h), which is most of what the tool otherwise prints.
	fxApplyAlertLevel();
	StageTimer ti;

	// parse .input (non-MPI constructor)
	Configuration config((workdir + "/" + inputfile).c_str(), 0);
	Model *model = config.getModel();

	// V1 restriction: single-scan experiments only
	if(model->getNumScans() != 1)
	{
		cerr << "fxcorr-fp: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)" << endl;
		return EXIT_FAILURE;
	}

	// find this station's datastream indices (multi-datastream stations have
	// several entries with the same TELESCOPE name); dsarg picks the n-th
	int dsindex = -1;
	{
		int count = 0;
		for(int d=0;d<config.getNumDataStreams();d++)
		{
			if(config.getDStationName(0, d) == station)
			{
				if(count == dsarg)
				{
					dsindex = d;
					break;
				}
				count++;
			}
		}
	}
	if(dsindex < 0)
	{
		cerr << "fxcorr-fp: station " << station << " ds_index " << dsarg << " not found in .input" << endl;
		return EXIT_FAILURE;
	}

	// ds 组（fengine 按组布局，data-spec 5.3，2026-10-09）：组 = BASELINE 表
	// 连通分量（与 fxcorr-x 的分片划分同一实现——fxcorrcommon 的 deriveDsGroups）；
	// 输出路径 <batch>/<组>/<station>/ds_<N>/ 使组级 purge 可整目录删除。
	vector<vector<int> > dsgroups = deriveDsGroups(config, 0);
	int dsgroupof = groupOfDs(dsgroups, dsindex);
	if(dsgroupof < 0)
	{
		cerr << "fxcorr-fp: ds " << dsindex << " not in any ds group" << endl;
		return EXIT_FAILURE;
	}

	// P1: DifxMessage status emission (algo-plan.md).  fp plays the
	// datastream role: mpiId = dsindex+1, RUNNING is left to the correlator
	// tool (manager role); progress is reported via diagnostics.
	// container mode logs to meta/difxmsg/<exp>_<batch>_<station>.*
	string expname = jobIdentifier(inputfile);
	string containerprefix;
	if(const char *runmode = getenv("FXCORR_RUN_MODE"))
	{
		if(strcmp(runmode, "container") == 0)
			containerprefix = workdir + "/meta/difxmsg/" + expname + "_" + batchid + "_" + station;
	}
	// STA records (autocorrelation and kurtosis dumps) move with the station
	// stack to fxcorr-xp in the v9 chain -- this tool holds no Mode and no
	// spectra, so a set switch is reported rather than silently ignored.
	if(const char *staenv = getenv("FXCORR_STA"))
	{
		if(strcmp(staenv, "1") == 0)
			cerr << "fxcorr-fp: FXCORR_STA=1 is ignored here: STA records are dumped by fxcorr-xp" << endl;
	}
	if(const char *kurtosisenv = getenv("FXCORR_KURTOSIS"))
	{
		if(strcmp(kurtosisenv, "1") == 0)
			cerr << "fxcorr-fp: FXCORR_KURTOSIS=1 is ignored here: kurtosis records are dumped by fxcorr-xp" << endl;
	}
	if(containerprefix.size() > 0)
	{
		if(system(("mkdir -p " + workdir + "/meta/difxmsg").c_str()) != 0)
			cerr << "fxcorr-fp: cannot create " << workdir << "/meta/difxmsg" << endl;
	}
	DifxMonitor monitor(dsindex+1, expname, inputfile, containerprefix);
	// AC_INIT version of fxcorr-fp
	monitor.status(DIFX_STATE_STARTING, "Version 0.1.0", 0.0, 0, 0, 0.0, 0.0);

	// batch start expressed as job-relative seconds; the raw file holds this
	// batch's data starting at that time (data-spec 5.2 file-per-batch), so
	// DataReader byte offsets are relative to the batch start
	int scan = 0;
	long long scanstartsec = (long long)model->getScanStartSec(scan, config.getStartMJD(), config.getStartSeconds());
	double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds()/86400.0;
	double batchstartjob = (startmjd - jobstart)*86400.0;
	long long batchstartsec = (long long)config.getStartSeconds() + (long long)floor(batchstartjob);
	int batchstartns = (int)((batchstartjob - (double)floor(batchstartjob))*1.0e9 + 0.5);
	if(batchstartns >= 1000000000)
	{
		batchstartsec++;
		batchstartns -= 1000000000;
	}

	// data files (v8-plan.md 2.2): an explicit list (FXCORR_REAL_PATH) wins
	// over the layout rule; the .input FILE lines are no longer consulted
	vector<string> datafiles = resolveDataFiles(station, batchid, dsarg);
	if(datafiles.empty())
		return EXIT_FAILURE;

	DataReader reader(&config, 0, dsindex, model, batchstartsec, batchstartns, &datafiles);

	int sendbytes = reader.getSendBytes();
	u8 *databuf = new u8[sendbytes];
	int blockspersend = reader.getBlocksPerSend();
	int flagwords = (blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT;
	s32 *validflags = new s32[flagwords];

	// fengine layout (data-spec 5.3, 2026-10-09): <batch>/<ds-group>/
	// <station>/ds_<N>/ — the group layer is the lifecycle unit (per-group
	// purge removes one directory tree; fxcorr-xp reads the same layout).
	// ds_N keeps multi-datastream stations (one stream per recording
	// thread) from colliding; N = station-local index.
	string outdir = FxcorrPath::root(FxcorrPath::ROOT_FENGINE) + "/" + batchid + "/" + to_string(dsgroupof) + "/" + station + "/ds_" + to_string(dsarg);
	string mkdircommand = "mkdir -p " + outdir;
	if(system(mkdircommand.c_str()) != 0)
		return fail(monitor, "fxcorr-fp: cannot create " + outdir);

	int subintns = config.getSubintNS(0);
	int numbufferedffts = config.getNumBufferedFFTs(0);
	StreamWriter writer(outdir, batchid, station, dsarg, subintns, nsubints,
	                    sendbytes, blockspersend, numbufferedffts,
	                    config.getFrameBytes(0, dsindex), config.getFramesPerSecond(0, dsindex));

	// experiment-level SWITCHEDPOWER text file (algo-plan.md P6): TCAL
	// FREQUENCY > 0 enables the per-second switched power statistics, written
	// next to the SWIN output.  frequency = 0 (the default) keeps this path
	// entirely off.
	SwitchedPower *switchedpower = 0;
	u8 *spblock = 0;
	int spreadbytes = 0;
	int spblockfill = 0;
	int switchedpowerincrement = 1;
	int spblockcount = 0;	// global block counter, upstream keeps one too
	long long prevfileoffset = -1;	// end of the last subint fed (batch-relative)
	if(config.getDSwitchedPowerFrequency(dsindex) > 0)
	{
		string spdir = config.getOutputFilename();
		if(system(("mkdir -p " + spdir).c_str()) != 0)
			return fail(monitor, "fxcorr-fp: cannot create " + spdir);
		switchedpower = new SwitchedPower(&config, 0, dsindex);

		// feed granularity, same as upstream (vdiffile.cpp:945-964): one block
		// per datasegment of readbytes = (databufferfactor/numdatasegments)
		// * maxdata bytes, frame aligned; the fraction of blocks actually fed
		// follows the data rate (vdiffile.cpp:405-415)
		spreadbytes = (config.getDDataBufferFactor()/config.getDNumDataSegments())*config.getMaxDataBytes(dsindex);
		spblock = new u8[spreadbytes];
		float datarate = (float)config.getFrameBytes(0, dsindex)*(float)config.getFramesPerSecond(0, dsindex)*8.0f/1.0e6f;
		if(datarate >= 512.0f)
			switchedpowerincrement = (int)(datarate/512.0f + 0.1f);
	}

	// data-spec section 12: the batch start must lie on a subint boundary;
	// tolerance absorbs the f64 representation error of start_mjd (~1 us)
	{
		long long batchstartns = (long long)floor(batchstartjob*1.0e9 + 0.5);
		if(batchstartns % subintns > 1000 && (subintns - batchstartns % subintns) > 1000)
		{
			ostringstream oss;
			oss << "fxcorr-fp: batch start " << startmjd << " is not on a subint boundary ("
			    << batchstartns % subintns << " ns into a " << subintns << " ns subint)";
			return fail(monitor, oss.str());
		}
	}
	g_tt_init += STAGE_LAP(ti, "init");

	for(int s=0;s<nsubints;s++)
	{
		double subintstart = batchstartjob + (double)s*(double)subintns/1.0e9;
		double reld = subintstart - (double)scanstartsec;
		if(reld < 0.0)
		{
			ostringstream oss;
			oss << "fxcorr-fp: subint " << s << " starts before the scan start";
			return fail(monitor, oss.str());
		}
		int offsetsec = (int)floor(reld);
		int offsetns = (int)((reld - (double)offsetsec)*1.0e9 + 0.5);

		// station read of one subint (the fp side of core.cpp:694-801: the
		// read, the switched-power feed and the bookkeeping are all this tool
		// does; the DSP happens in fxcorr-xp)
		StageTimer st;
		int datasec = 0, datans = 0;
		int bytes = reader.readSubint(scan, offsetsec, offsetns, databuf, sendbytes, &datasec, &datans);
		g_tt_read += STAGE_LAP(st, "read");

		// P6: accumulate subint bytes into datasegment-sized blocks and feed
		// the switched power detector (vdiffile.cpp:945-964).  Subint reads
		// carry frame-aligned guard overlap (sendbytes > one subint of frames),
		// so skip the bytes already covered by the previous subint to keep the
		// block frame-continuous -- upstream feeds the continuous vdifmux
		// stream, which has no overlap.
		// B2 (reader-model.md 4.8): once a subint has bridged a recording
		// interruption its buffer is a frame grid on the time axis -- filler
		// dropped, gap slots left empty -- and no longer a run of file bytes, so
		// the overlap bookkeeping below (and the feed's assumption of a
		// continuous stream) does not describe it.  Those subints are skipped
		// rather than fed bytes whose place in the stream is not known; the
		// next contiguous subint starts the block afresh.
		if(switchedpower && bytes > 0 && reader.lastReadContiguous())
		{
			long long curstart = reader.getLastFileOffset();
			long long skip = prevfileoffset - curstart;
			if(skip < 0 || skip > bytes)
				skip = 0;
			prevfileoffset = curstart + bytes;

			int remain = bytes - (int)skip;
			int off = (int)skip;
			while(remain > 0)
			{
				int take = spreadbytes - spblockfill;
				if(take > remain)
					take = remain;
				memcpy(spblock + spblockfill, databuf + off, take);
				spblockfill += take;
				off += take;
				remain -= take;
				if(spblockfill == spreadbytes)
				{
					++spblockcount;
					if(spblockcount % switchedpowerincrement == 0)
						switchedpower->feed(spblock, spblockfill);
					spblockfill = 0;
				}
			}
		}
		else if(switchedpower && bytes > 0)
		{
			FXLOG(FXLOG_VERBOSE) << "SWITCHEDPOWER subint " << s
			     << ": skipped (buffer is a time-slot grid, not a byte run)" << endl;
			// restart the block after the interruption: the next contiguous
			// subint sees a negative overlap and feeds from its own start
			prevfileoffset = reader.getLastFileOffset();
		}

		// P1: input datarate diagnostics, upstream datastream.cpp:631-634
		monitor.diagnosticDataConsumed(bytes);
		monitor.diagnosticInputDatarate((double)bytes / ((double)subintns/1.0e9));
		g_tt_swp += STAGE_LAP(st, "swp");

		reader.fillValidFlags(validflags, bytes);
		writer.writeSubint(bytes, scan, datasec, datans, offsetsec, offsetns, validflags, databuf);
		g_tt_write += STAGE_LAP(st, "write");
	}

	// P6: feed the final partial block (upstream feeds short trailing
	// segments the same way), then close, which flushes the last window
	if(switchedpower && spblockfill > 0)
	{
		++spblockcount;
		if(spblockcount % switchedpowerincrement == 0)
			switchedpower->feed(spblock, spblockfill);
	}
	delete switchedpower;	// dtor: close() -> flush() of the tail window
	delete [] spblock;

	{
		StageTimer td;
		delete [] databuf;
		delete [] validflags;
		g_tt_del += STAGE_LAP(td, "del");
	}

	// The stream must be on disk before this task calls itself done: a full
	// filesystem has to fail HERE, while this process still knows which file
	// it was writing, rather than surface later as fxcorr-xp reading a
	// truncated stream.bin (v7-plan 17.2).
	{
		StageTimer tf;
		if(!writer.finalise())
			return fail(monitor, "fxcorr-fp: stream for batch " + batchid + " station " + station + " ds " + to_string(dsarg) + " is incomplete (see above)");
		g_tt_fin += STAGE_LAP(tf, "fin");
	}

	// upstream ending sequence (fxmanager.cpp terminate/DONE): Ending then Done
	monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
	monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);

	if(g_timing)
	{
		if(!g_timing_trace && !g_timing_brief)
		{
			// serial tool: every stage is wall seconds of the one stream
			double total = nowsec() - g_tt_start;
			fprintf(g_timing_out,
			        "TIMING fxcorr-fp batch=%s station=%s ds=%d nsubints=%d"
			        " total=%g init=%g read=%g swp=%g write=%g fin=%g del=%g\n",
			        batchid.c_str(), station.c_str(), dsarg, nsubints, total,
			        g_tt_init, g_tt_read, g_tt_swp, g_tt_write, g_tt_fin, g_tt_del);
			fflush(g_timing_out);
		}
		timing_finish();	// brief rows / trace buffer
	}

	return EXIT_SUCCESS;
}
