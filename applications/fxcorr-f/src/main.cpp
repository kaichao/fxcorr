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
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/difxmonitor.h>
#include <fxcorrcommon/switchedpower.h>
#include <fxcorrcommon/fxcorrpath.h>

#include "datareader.h"
#include "fenginewriter.h"
#include "pcaltextwriter.h"
#include "log.h"
#include "ompcompat.h"

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
// lines are for debugging and detailed optimisation.  Workers lap from inside
// the parallel region, so event writes are serialised by a critical section.
static bool g_timing = false;
static bool g_timing_trace = false;
static bool g_timing_brief = false;
static FILE *g_timing_out = 0;
static double g_tt_read, g_tt_swp, g_tt_setup, g_tt_hdr, g_tt_proc, g_tt_reduce,
              g_tt_copy, g_tt_write, g_tt_ac, g_tt_subtail, g_tt_start;
static double g_tt_init, g_tt_repl, g_tt_blk, g_tt_fin, g_tt_del;

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
// Three tiers share the 23 STAGE_LAP call sites; only the output path differs
// (tier comments at the initialisation below):
//   aggregate -- one summary line at exit, unchanged
//   brief     -- one TSV row per probe at exit (the table below drives it)
//   trace     -- every lap buffered in memory, written once at exit
struct TimingEvent
{
	double ts;		// epoch seconds at the lap
	double dur;		// seconds since the timer's previous lap
	int tid;
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
// order matches the pipeline diagrams in app-fxcorr docs/timing/
static TimingProbe g_probes[] = {
	{"init",    "main.cpp", 860, &g_tt_init,    0},
	{"repl",    "main.cpp", 738, &g_tt_repl,    0},
	{"read",    "main.cpp", 879, &g_tt_read,    0},
	{"swp",     "main.cpp", 934, &g_tt_swp,     0},
	{"setup",   "main.cpp", 958, &g_tt_setup,   0},
	{"hdr",     "main.cpp", 960, &g_tt_hdr,     0},
	{"blk",     "main.cpp", 995, &g_tt_blk,     0},
	{"proc",    "main.cpp", 1019, &g_tt_proc,    0},
	{"reduce",  "main.cpp", 1089, &g_tt_reduce,  0},
	{"copy",    "main.cpp", 1130, &g_tt_copy,    0},
	{"write",   "main.cpp", 1142, &g_tt_write,   0},
	{"ac",      "main.cpp", 1113, &g_tt_ac,      0},
	{"subtail", "main.cpp", 1242, &g_tt_subtail, 0},
	{"fin",     "main.cpp", 1283, &g_tt_fin,     0},
	{"del",     "main.cpp", 1270, &g_tt_del,     0},
	{0, 0, 0, 0, 0}
};

static void timing_event(const char *label, const char *file, int line,
                         double dur, double ts)
{
	const char *base = strrchr(file, '/');
	base = (base != 0) ? base+1 : file;
	int thread = omp_get_thread_num();
	#pragma omp critical(fxcorr_timing)
	{
		if(g_timing_trace)
		{
			TimingEvent ev;
			ev.ts = ts;
			ev.dur = dur;
			ev.tid = thread;
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
			fprintf(g_timing_out, "%.6f\tfxcorr-f\t%d\t%d\t%s:%d\t%s\t%.6f\n",
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
			fprintf(g_timing_out, "%.6f\tfxcorr-f\t%d\t%d\t%s:%d\t%s\t%.6f\n",
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

// BINARY_STA records, one per recorded band, per autocorrelation batch
// (algo-plan.md P1; core.cpp averageAndSendAutocorrs 1195-1253, V1
// single-thread form).  nsoffsetns/nswidthns describe the current ac batch
// (centre offset and width in ns).
// P9: datastreamsaveraged means the caller already ran averageFrequency()
// for this batch (core.cpp:1181-1187); the renormalisation and the channel
// count then refer to the averaged spectrum (core.cpp:1249-1254).
static void sendSTA(DifxMonitor *monitor, Configuration &config, int configindex, int dsindex,
                    Mode *mode, int scan, long long scanstartsec, int subintsec, int subintns,
                    double nsoffsetns, double nswidthns, bool datastreamsaveraged, const string &jobname)
{
	// core.cpp:1191-1192: no STA at all when one record does not fit the MTU
	int recordsize = sizeof(DifxMessageSTARecord) + sizeof(f32)*config.getSTADumpChannels();
	if(recordsize > config.getMTU())
		return;

	int nrecordedbands = config.getDNumRecordedBands(configindex, dsindex);
	for(int band=0;band<nrecordedbands;band++)
	{
		int freqindex = config.getDRecordedFreqIndex(configindex, dsindex, band);
		int freqchannels = config.getFNumChannels(freqindex);
		f32 weight = mode->getWeight(false, band);

		// minimum weight gate, core.cpp:1218-1220 (dodgy packet protection)
		double stasamples = 0.001*nswidthns*2*config.getFreqTableBandwidth(freqindex);
		if(weight < 0.333*stasamples/(2*freqchannels))
			continue;

		// core.cpp:1224: energy -> power normalisation
		f32 renormvalue = 1.0f/(2*freqchannels*weight);
		// core.cpp:1227-1232: on an averaged spectrum channels are summed
		// below, not averaged, so correct both factors
		if(datastreamsaveraged)
		{
			renormvalue /= config.getFChannelsToAverage(freqindex);
			freqchannels /= config.getFChannelsToAverage(freqindex);
		}

		// core.cpp:1234-1237: fold to the requested STA dump resolution
		int nchan = config.getSTADumpChannels();
		if(freqchannels < nchan)
			nchan = freqchannels;
		int chans_to_avg = freqchannels/nchan;

		cf32 *acdata = mode->getAutocorrelation(false, band);
		DifxMessageSTARecord *record = (DifxMessageSTARecord *)malloc(recordsize);
		memset(record, 0, recordsize);

		record->messageType = STA_AUTOCORRELATION;
		record->dsindex = dsindex;
		record->coreindex = 0;
		record->threadindex = 0;
		snprintf(record->identifier, DIFX_MESSAGE_PARAM_LENGTH, "%s",
		         jobname.substr(0, DIFX_MESSAGE_PARAM_LENGTH-1).c_str());
		record->nChan = nchan;
		record->scan = scan;
		// core.cpp:1208-1214: sec is day-of-observation seconds (the code,
		// not the "since scan start" comment, is authoritative)
		record->sec = (int)scanstartsec + subintsec;
		record->ns = subintns + (int)nsoffsetns;
		if(record->ns >= 1000000000)
		{
			record->ns -= 1000000000;
			record->sec++;
		}
		record->nswidth = (int)nswidthns;
		record->bandindex = band;
		// sum (not average) of chans_to_avg adjacent real parts, core.cpp:1244-1247.
		// Upstream indexes acdata as f32* with stride 2 (= cf32 stride 1); the
		// P1 port kept the factor 2 with a cf32* pointer and folded every
		// second chans_to_avg block instead of adjacent channels (fixed P9,
		// caught by the averaged-branch STA bitwise comparison).
		for(int k=0;k<nchan;k++)
		{
			record->data[k] = acdata[k*chans_to_avg].re;
			for(int l=1;l<chans_to_avg;l++)
				record->data[k] += acdata[k*chans_to_avg+l].re;
		}
		// vectorMulC_f32_I(renormvalue, ...): energy -> power
		for(int k=0;k<nchan;k++)
			record->data[k] *= renormvalue;

		monitor->staSend(record, sizeof(DifxMessageSTARecord) + sizeof(f32)*nchan);
		free(record);
	}
}

// BINARY_STA records with messageType STA_KURTOSIS, one per recorded band,
// per subint (algo-plan.md P9; core.cpp averageAndSendKurtosis 1378-1434,
// V1 single-thread form).  The mode has accumulated s1/s2 in process(); the
// excess-kurtosis statistic is folded and copied out by
// calculateAndAverageKurtosis.  nsoffsetns/nswidthns describe the whole
// subint (centre offset and width); unlike the autocorrelation STA there is
// no weight gate and no renormalisation (core.cpp:1420-1431).
static void sendKurtosis(DifxMonitor *monitor, Configuration &config, int configindex, int dsindex,
                         Mode *mode, int scan, long long scanstartsec, int subintsec, int subintns,
                         double nsoffsetns, double nswidthns, int numblocks, const string &jobname)
{
	bool valid = mode->calculateAndAverageKurtosis(numblocks, config.getSTADumpChannels());
	if(!valid)
		return;

	int nrecordedbands = config.getDNumRecordedBands(configindex, dsindex);
	for(int band=0;band<nrecordedbands;band++)
	{
		int freqindex = config.getDRecordedFreqIndex(configindex, dsindex, band);
		int freqchannels = config.getFNumChannels(freqindex);
		int nchan = config.getSTADumpChannels();
		if(freqchannels < nchan)
			nchan = freqchannels;

		int recordsize = sizeof(DifxMessageSTARecord) + sizeof(f32)*nchan;
		DifxMessageSTARecord *record = (DifxMessageSTARecord *)malloc(recordsize);
		memset(record, 0, recordsize);

		record->messageType = STA_KURTOSIS;
		record->dsindex = dsindex;
		record->coreindex = 0;
		record->threadindex = 0;
		snprintf(record->identifier, DIFX_MESSAGE_PARAM_LENGTH, "%s",
		         jobname.substr(0, DIFX_MESSAGE_PARAM_LENGTH-1).c_str());
		record->nChan = nchan;
		record->scan = scan;
		record->sec = (int)scanstartsec + subintsec;
		record->ns = subintns + (int)nsoffsetns;
		if(record->ns >= 1000000000)
		{
			record->ns -= 1000000000;
			record->sec++;
		}
		record->nswidth = (int)nswidthns;
		record->bandindex = band;
		f32 *sk = mode->getKurtosis(band);
		for(int k=0;k<nchan;k++)
			record->data[k] = sk[k];

		monitor->staSend(record, recordsize);
		free(record);
	}
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
//      data-spec 5.2).  The same rule lives in fxinput.py's
//      station_out_path, fxcorr-sim's stationOutPath and the app-fxcorr
//      router's fxin.StationOutPath -- four places that change together
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

	cerr << "fxcorr-f: no data file for station " << station << " batch " << batchid
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
		cerr << "usage: fxcorr-f <batch_id> <station> [workdir] [ds_index]" << endl
		     << "  env: FXCORR_WORKDIR (default .), overridden by the workdir argument" << endl
		     << "       FXCORR_REAL_PATH: data file path(s), comma-separated"
		     << " (default: raw-root layout rule)" << endl;
		return EXIT_FAILURE;
	}
	// P3 (algo-plan.md): thread count from OMP_NUM_THREADS; unset = serial
	// (V2 behaviour unchanged).  Must run before any OpenMP parallel region.
	{
		const char *env = getenv("OMP_NUM_THREADS");
		if(env == 0 || env[0] == '\0')
			omp_set_num_threads(1);
	}
	// stage timing, three tiers (app-fxcorr docs/timing/design.md):
	//   1 aggregate: FXCORR_TIMING=1 -- one summary line, unchanged (output
	//     file via FXCORR_TIMING_FILE)
	//   2 brief: FXCORR_TIMING_BRIEF=1|<path> -- one TSV row per probe at
	//     exit, first timestamp + accumulated total (<=20 rows; 1 = stderr,
	//     a path = that file)
	//   3 trace: FXCORR_TIMING_TRACE=<path>|1 -- every lap buffered in memory
	//     and written once at exit; 1 = <workdir>/fxcorr-f-<batch>.timing.tsv
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
				cerr << "fxcorr-f: cannot open timing file " << outpath << ", using stderr" << endl;
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
		string tp = workdir + "/fxcorr-f-" + batchid + ".timing.tsv";
		FILE *f = fopen(tp.c_str(), "a");
		if(f != 0)
			g_timing_out = f;
		else
			cerr << "fxcorr-f: cannot open timing file " << tp << ", using stderr" << endl;
	}
	// hand the resolved workdir to the shared root resolver (V5 P5): the
	// library opens .input/.calc/.im paths through it, so every relative path
	// in the config set lands on the same roots this tool uses below
	FxcorrPath::init(workdir);
	workdir = FxcorrPath::workdir();
	FxcorrPath::print("fxcorr-f");
	// the roots THIS tool touches: raw (FILE lines), fengine (its output) and
	// vis (it writes the PCAL / SWITCHEDPOWER text files into the SWIN dir)
	static const FxcorrPath::Root usedroots[] = {
		FxcorrPath::ROOT_RAW, FxcorrPath::ROOT_FENGINE, FxcorrPath::ROOT_VIS };
	if(!FxcorrPath::checkRoots(batchid, usedroots, 3, "fxcorr-f"))
		return EXIT_FAILURE;
	// multi-datastream stations (real observations: one datastream per
	// recording thread): ds_index selects the station's n-th datastream
	// (0-based, default 0; fengine output goes to <ds-group>/<station>/
	// ds_<ds_index>/ as of the 2026-10-09 per-group layout)
	int dsarg = 0;
	if(argc > 4)
		dsarg = atoi(argv[4]);
	if(dsarg < 0)
	{
		cerr << "fxcorr-f: ds_index must be >= 0" << endl;
		return EXIT_FAILURE;
	}

	// batch.json is pre-written by run_batch.sh (batches/<batch_id>.json)
	string batchjsonpath = workdir + "/batches/" + batchid + ".json";
	ifstream jf(batchjsonpath.c_str());
	if(!jf.is_open())
	{
		cerr << "fxcorr-f: cannot open " << batchjsonpath << endl;
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
		cerr << "fxcorr-f: batch.json missing required fields" << endl;
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
		cerr << "fxcorr-f: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)" << endl;
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
		cerr << "fxcorr-f: station " << station << " ds_index " << dsarg << " not found in .input" << endl;
		return EXIT_FAILURE;
	}

	// ds 组（fengine 按组布局，data-spec 5.3，2026-10-09）：组 = BASELINE 表
	// 连通分量（与 fxcorr-x 的分片划分同一实现——fxcorrcommon 的 deriveDsGroups）；
	// 输出路径 <batch>/<组>/<station>/ds_<N>/ 使组级 purge 可整目录删除。
	vector<vector<int> > dsgroups = deriveDsGroups(config, 0);
	int dsgroupof = groupOfDs(dsgroups, dsindex);
	if(dsgroupof < 0)
	{
		cerr << "fxcorr-f: ds " << dsindex << " not in any ds group" << endl;
		return EXIT_FAILURE;
	}

	Mode *mode = config.getMode(0, dsindex);
	if(!mode->initialisedOK())
	{
		cerr << "fxcorr-f: mode initialisation failed for station " << station << endl;
		return EXIT_FAILURE;
	}

	// P1: DifxMessage status emission (algo-plan.md).  f plays the
	// datastream/core role: mpiId = dsindex+1, RUNNING is left to
	// fxcorr-x (manager role); progress is reported via diagnostics.
	// container mode logs to meta/difxmsg/<exp>_<batch>_<station>.*
	string expname = jobIdentifier(inputfile);
	string containerprefix;
	if(const char *runmode = getenv("FXCORR_RUN_MODE"))
	{
		if(strcmp(runmode, "container") == 0)
			containerprefix = workdir + "/meta/difxmsg/" + expname + "_" + batchid + "_" + station;
	}
	bool dosta = false;
	if(const char *staenv = getenv("FXCORR_STA"))
		dosta = (strcmp(staenv, "1") == 0);
	// P9: kurtosis STA (algo-plan.md); upstream enables it at runtime via a
	// difxmessage parameter ("dumpkurtosis=true"), fxcorr via environment.
	bool dokurtosis = false;
	if(const char *kurtosisenv = getenv("FXCORR_KURTOSIS"))
		dokurtosis = (strcmp(kurtosisenv, "1") == 0);
	if(containerprefix.size() > 0)
	{
		if(system(("mkdir -p " + workdir + "/meta/difxmsg").c_str()) != 0)
			cerr << "fxcorr-f: cannot create " << workdir << "/meta/difxmsg" << endl;
	}
	DifxMonitor monitor(dsindex+1, expname, inputfile, containerprefix);
	// AC_INIT version of fxcorr-f
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

	// P9: Mode::process accumulates s1/s2 per FFT block only when this is
	// set (mode.cpp:1236, upstream core.cpp:702)
	mode->setDumpKurtosis(dokurtosis);

	// P3 (algo-plan.md): replica Modes for block-parallel processing.  Each
	// replica shares the read-only config/model and owns its work buffers;
	// the per-subint setup calls and the per-fftloop reduction below keep the
	// replicas consistent with the primary Mode (bit-identical results).
	int nthreads = omp_get_max_threads();
	// P3 (pcal): the parallel pcal reduction needs the extractors' raw
	// accumulators (PCal::getAccumSize()); an extractor that cannot expose
	// them forces the serial path -- a slow correct run beats a fast wrong one
	if(nthreads > 1 && (config.getDPhaseCalIntervalHz(0, dsindex) > 0) && !mode->blockPcalCapable())
	{
		cerr << "fxcorr-f: pcal extractor does not expose its raw accumulator; falling back to OMP_NUM_THREADS=1" << endl;
		nthreads = 1;
	}
	vector<Mode *> replicas;
	StageTimer tr;
	for(int t=1;t<nthreads;t++)
	{
		Mode *m = config.getMode(0, dsindex);
		if(!m->initialisedOK())
		{
			cerr << "fxcorr-f: replica mode initialisation failed for station " << station << endl;
			return EXIT_FAILURE;
		}
		m->setDumpKurtosis(dokurtosis);
		replicas.push_back(m);
	}
	g_tt_repl += STAGE_LAP(tr, "repl");

	// P3: per-block accumulators on every Mode, the primary included (it is
	// worker 0).  Each block's autocorrelation and weight land in their own
	// slot, and the reduction below adds the slots up in block order -- the
	// serial accumulation sequence -- instead of summing per-thread segment
	// totals, which reorders the floating point additions.  Cost: ~21 MB per
	// Mode for a 1310-block, 8-band, 128-channel, dual-polarisation config.
	// the per-block slots are the base of the reduction below, so they are
	// allocated at every thread count (the serial run reduces its own block
	// slots back in block order -- numerically identical to the direct
	// accumulation, and it keeps the reduce code on one path).  Each Mode
	// only gets the slots of the stretch its thread will process -- the
	// per-thread share of the subint plus slack for the per-fftloop rounding
	// (setBlockSlotBase below maps blocks onto slots).  The allocation itself
	// runs inside the parallel region, one thread per Mode, so its page-fault
	// cost (0.8 s serially at 32000 blocks) lands on the team too.
	int slotcount;
	{
		int bps = config.getBlocksPerSend(0);
		int nbuf = config.getNumBufferedFFTs(0);
		slotcount = (bps + nthreads - 1)/nthreads + (bps + nbuf - 1)/nbuf + 8;
	}

	// autocorrelation averaging batch, same formula as core.cpp:769-783
	int numbufferedffts = config.getNumBufferedFFTs(0);
	double blockns = (double)config.getSubintNS(0)/(double)reader.getBlocksPerSend();
	int maxacblocks = (int)(model->getMaxNSBetweenACAvg(0)/blockns);
	maxacblocks -= maxacblocks%numbufferedffts;
	if(maxacblocks == 0)
	{
		maxacblocks = numbufferedffts;
		cerr << "fxcorr-f: requested autocorrelation shift/average time of " << model->getMaxNSBetweenACAvg(0) << " ns cannot be met with " << numbufferedffts << " FFTs being buffered; the time resolution which will be attained is " << maxacblocks*blockns << " ns" << endl;
	}

	// fengine layout (data-spec 5.3, 2026-10-09): <batch>/<ds-group>/
	// <station>/ds_<N>/ — the group layer is the lifecycle unit (per-group
	// purge removes one directory tree; fxcorr-x reads the same layout).
	// ds_N keeps multi-datastream stations (one stream per recording
	// thread) from colliding; N = station-local index.
	string outdir = FxcorrPath::root(FxcorrPath::ROOT_FENGINE) + "/" + batchid + "/" + to_string(dsgroupof) + "/" + station + "/ds_" + to_string(dsarg);
	string mkdircommand = "mkdir -p " + outdir;
	if(system(mkdircommand.c_str()) != 0)
		return fail(monitor, "fxcorr-f: cannot create " + outdir);
	FEngineWriter writer(outdir, &config, 0, dsindex, nsubints, maxacblocks);

	bool haspcal = (config.getDPhaseCalIntervalHz(0, dsindex) > 0);
	int subintns = config.getSubintNS(0);

	// experiment-level PCAL text file lives next to the SWIN output
	// (algo-plan.md P0); f runs first and creates the .difx directory.
	// Use OUTPUT FILENAME as-is (like fxcorr-x): difxcalc writes it as an
	// absolute path, run_bench.sh rewrites it relative to the workdir cwd.
	PcalTextWriter *pcaltext = 0;
	if(haspcal)
	{
		string pcaldir = config.getOutputFilename();
		if(system(("mkdir -p " + pcaldir).c_str()) != 0)
			return fail(monitor, "fxcorr-f: cannot create " + pcaldir);
		pcaltext = new PcalTextWriter(pcaldir, &config, 0, dsindex);
	}

	// experiment-level SWITCHEDPOWER text file (algo-plan.md P6): TCAL
	// FREQUENCY > 0 enables the per-second switched power statistics, written
	// next to the SWIN output like the PCAL text file.  frequency = 0 (the
	// default) keeps this path entirely off.
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
			return fail(monitor, "fxcorr-f: cannot create " + spdir);
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

	int sendbytes = reader.getSendBytes();
	u8 *databuf = new u8[sendbytes];
	int blockspersend = reader.getBlocksPerSend();
	int flagwords = (blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT;
	s32 *validflags = new s32[flagwords];

	// data-spec section 12: the batch start must lie on a subint boundary;
	// tolerance absorbs the f64 representation error of start_mjd (~1 us)
	{
		long long batchstartns = (long long)floor(batchstartjob*1.0e9 + 0.5);
		if(batchstartns % subintns > 1000 && (subintns - batchstartns % subintns) > 1000)
		{
			ostringstream oss;
			oss << "fxcorr-f: batch start " << startmjd << " is not on a subint boundary ("
			    << batchstartns % subintns << " ns into a " << subintns << " ns subint)";
			return fail(monitor, oss.str());
		}
	}

	// intTime boundaries for the PCAL text file: count subints per intTime
	// (run_batch.sh validates intTime is a subint multiple and the batch
	// length is an intTime multiple, so boundaries never split a batch)
	double inttime = config.getIntTime(0);
	long long subintsperint = (long long)(inttime*1.0e9/(double)subintns + 0.5);
	if(subintsperint < 1)
	{
		ostringstream oss;
		oss << "fxcorr-f: intTime " << inttime << " s is smaller than one subint (" << subintns << " ns)";
		return fail(monitor, oss.str());
	}
	long long batchstartabsns = (long long)batchstartsec*1000000000LL + (long long)batchstartns;
	g_tt_init += STAGE_LAP(ti, "init");

	for(int s=0;s<nsubints;s++)
	{
		double subintstart = batchstartjob + (double)s*(double)subintns/1.0e9;
		double reld = subintstart - (double)scanstartsec;
		if(reld < 0.0)
		{
			ostringstream oss;
			oss << "fxcorr-f: subint " << s << " starts before the scan start";
			return fail(monitor, oss.str());
		}
		int offsetsec = (int)floor(reld);
		int offsetns = (int)((reld - (double)offsetsec)*1.0e9 + 0.5);

		// station-based processing of one subint (core.cpp:694-801, V1 single-threaded)
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

		mode->zeroAutocorrelations();
		if(dokurtosis)
			mode->zeroKurtosis();	// core.cpp:703-704
		reader.fillValidFlags(validflags, bytes);
		mode->setValidFlags(validflags);
		mode->setData(databuf, bytes, scan, datasec, datans);
		mode->setOffsets(scan, offsetsec, offsetns);
		if(haspcal)
			mode->resetpcal();
		// P3: the subint setup applies to every replica as well
		for(Mode *m : replicas)
		{
			m->zeroAutocorrelations();
			if(dokurtosis)
				m->zeroKurtosis();
			m->setValidFlags(validflags);
			m->setData(databuf, bytes, scan, datasec, datans);
			m->setOffsets(scan, offsetsec, offsetns);
			if(haspcal)
				m->resetpcal();
		}

		g_tt_setup += STAGE_LAP(st, "setup");
		writer.writeSubintHeader(scan, offsetsec, offsetns, mode, validflags);
		g_tt_hdr += STAGE_LAP(st, "hdr");

		// same loop structure as core.cpp:786-801: buffered slot reuse over fftloops
		int fftloops = (blockspersend + numbufferedffts - 1)/numbufferedffts;
		int acblockcount = 0, acshiftcount = 0;

		// P3 (algo-plan.md): one parallel region per subint (not per fftloop,
		// so team fork/join happens once per subint and fftloops are
		// separated by plain barriers).  Each thread processes contiguous
		// block segments (t*numffts/nthreads) with its own Mode, so per-block
		// computation is bit-identical to the serial form; thread 0 then
		// reduces the replicas block-ordered (accumulation sequence equals
		// the serial one) and writes the batch.  acblockcount/acshiftcount
		// are only touched by thread 0 and read after the region.
		int nrecordedbands = config.getDNumRecordedBands(0, dsindex);
		// per-thread slot bases for the block-ordered reduction: thread 0
		// advances them after each fftloop; the workers mirror that in their
		// own localbase (see setBlockSlotBase below)
		vector<int> slotbase(nthreads, 0);
		#pragma omp parallel num_threads(nthreads)
		{
			int t = omp_get_thread_num();
			Mode *m = (t == 0) ? mode : replicas[t-1];
			// per-thread stage accumulators (each stage is timed around its own
			// body, so barrier waits are not attributed to any stage)
			double t_proc_l = 0.0, t_reduce_l = 0.0, t_copy_l = 0.0, t_write_l = 0.0, t_ac_l = 0.0;
			int localbase = 0;	// slots this thread has used so far

			// allocate this Mode's per-block slots (idempotent, the first
			// subint does the work).  The barrier publishes every Mode's
			// slots to the cross-Mode reduction below before it reads them.
			double t_blk_l;
			{
				StageTimer ta;
				m->enableBlockAccumulators(slotcount);
				t_blk_l = STAGE_LAP(ta, "blk");
			}
			#pragma omp atomic
			g_tt_blk += t_blk_l;
			#pragma omp barrier

			for(int fftloop=0;fftloop<fftloops;fftloop++)
			{
				int numffts = blockspersend - fftloop*numbufferedffts;
				if(numffts > numbufferedffts)
					numffts = numbufferedffts;

				int bstart = (int)((long long)t*numffts/nthreads);
				int bend = (int)((long long)(t+1)*numffts/nthreads);
				// this thread's slice of the fftloop maps onto its own slots
				// starting at localbase (slot = global block index - base)
				m->setBlockSlotBase(fftloop*numbufferedffts + bstart - localbase);
				{
					StageTimer ts;
					for(int b=bstart;b<bend;b++)
					{
						int i = fftloop*numbufferedffts + b;
						m->process(i, b);
					}
					t_proc_l += STAGE_LAP(ts, "proc");
				}
				localbase += bend - bstart;
				#pragma omp barrier

				// P3: the block-ordered reduction is split over the whole team
				// by (crosspol, band) pair -- each pair's accumulation
				// sequence over the blocks is independent, so the split keeps
				// every one of them in block order and the result stays
				// bit-identical.  Summing per-thread segment totals instead
				// would put a rounding between the segments and shift the low
				// bits of every value (that was the old, broken form).  The
				// workers join in here rather than idling at the barrier.
				{
					StageTimer ts;
					int crosspols = mode->writeCrossAutoCorrs() ? 2 : 1;
					int npairs = crosspols*nrecordedbands;
					for(int pair=t;pair<npairs;pair+=nthreads)
					{
						int cp = pair/nrecordedbands;
						int j = pair%nrecordedbands;
						int freqindex = config.getDRecordedFreqIndex(0, dsindex, j);
						int nchan = config.getFNumChannels(freqindex);
						cf32 *dst = mode->getAutocorrelation(cp, j);
						// weight: fold the blocks into the value already in
						// the primary Mode (read-modify-write; this pair has
						// exactly one owner), matching the serial add sequence
						f32 w = mode->getWeightReduced(cp, j);
						for(int wt=0;wt<nthreads;wt++)
						{
							Mode *wm = (wt == 0) ? mode : replicas[wt-1];
							int bstart = (int)((long long)wt*numffts/nthreads);
							int bend = (int)((long long)(wt+1)*numffts/nthreads);
							for(int b=bstart;b<bend;b++)
							{
								int slot = slotbase[wt] + (b - bstart);
								const cf32 *src = wm->getBlockAc(cp, slot, j);
								for(int k=0;k<nchan;k++)
								{
									dst[k].re += src[k].re;
									dst[k].im += src[k].im;
								}
								w += wm->getBlockWeight(cp, slot, j);
							}
						}
						mode->setWeightReduced(cp, j, w);
					}

					// pcal: same split, one band per thread (the parked
					// accumulators are band-indexed; a band's blocks stay in
					// block order -- see pcal.h)
					if(haspcal)
					{
						for(int j=t;j<nrecordedbands;j+=nthreads)
						{
							for(int wt=0;wt<nthreads;wt++)
							{
								Mode *wm = (wt == 0) ? mode : replicas[wt-1];
								int bstart = (int)((long long)wt*numffts/nthreads);
								int bend = (int)((long long)(wt+1)*numffts/nthreads);
								for(int b=bstart;b<bend;b++)
								{
									int slot = slotbase[wt] + (b - bstart);
									uint64_t samples = 0;
									const f32 *accum = wm->getBlockPcal(j, slot, &samples);
									mode->addPcalBlock(j, accum, samples);
								}
							}
						}
					}
					t_reduce_l += STAGE_LAP(ts, "reduce");
				}
				if(t == 0)
				{
					StageTimer ts;
					// kurtosis is not per-block yet (mode.h), so it keeps the
					// old segment-wise reduction -- and it stays copies-only,
					// because the primary already accumulated its own share
					// inside process().  Left with thread 0: it is off by
					// default, and the large folds above already ran on the
					// whole team.
					if(dokurtosis)
					{
						for(size_t rt=0;rt<replicas.size();rt++)
						{
							Mode *rm = replicas[rt];
							for(int j=0;j<nrecordedbands;j++)
							{
								int freqindex = config.getDRecordedFreqIndex(0, dsindex, j);
								int nchan = config.getFNumChannels(freqindex);
								mode->addKurtosisProducts(j, rm->getKurtosisProducts1(j), rm->getKurtosisProducts2(j), nchan);
							}
						}
					}
					t_ac_l += STAGE_LAP(ts, "ac");
				}
				else
				{
					StageTimer ts;
					// copy this worker's output slots (spectra + per-slot
					// weights) into the primary Mode's buffered slots
					for(int b=bstart;b<bend;b++)
					{
						for(int j=0;j<nrecordedbands;j++)
						{
							int freqindex = config.getDRecordedFreqIndex(0, dsindex, j);
							int nchan = config.getFNumChannels(freqindex);
							memcpy(mode->getFreqsWrite(j, b), m->getFreqs(j, b), sizeof(cf32)*nchan);
							mode->setDataWeight(j, b, m->getDataWeight(j, b));
						}
					}
					t_copy_l += STAGE_LAP(ts, "copy");
				}
				#pragma omp barrier

				// bands are the natural split for the write as well: every
				// band owns its .sp stream and its weightaccum row, and the
				// data it drains (the primary's slots) is complete -- both
				// the reduction and the slot copies ended at the barrier above
				{
					StageTimer ts;
					for(int j=t;j<nrecordedbands;j+=nthreads)
						writer.writeSpectraBand(j, fftloop, mode);
					t_write_l += STAGE_LAP(ts, "write");
				}

				if(t == 0)
				{
					StageTimer ts;
					// autocorrelation averaging batches, core.cpp:993-1003
					acblockcount += numffts;
					if(acblockcount == maxacblocks)
					{
						if(haspcal)
							pcaltext->accumulateWeight(mode);	// before zeroAutocorrelations clears weights
						// P9: STA may demand the post-average spectrum (core.cpp:1181-1187):
						// average before the dump, let the writer skip its own average
						bool datastreamsaveraged = false;
						if(dosta && config.getMinPostAvFreqChannels(0) >= config.getSTADumpChannels())
						{
							mode->averageFrequency();
							datastreamsaveraged = true;
						}
						if(dosta)
							sendSTA(&monitor, config, 0, dsindex, mode, scan, scanstartsec, offsetsec, offsetns,
							        (acshiftcount*maxacblocks + maxacblocks/2.0)*blockns, maxacblocks*blockns, datastreamsaveraged, config.getJobName());
						writer.writeAutocorrelationBatch(mode, datastreamsaveraged);
						mode->zeroAutocorrelations();
						acblockcount = 0;
						acshiftcount++;
					}
					// advance the per-thread slot bases for the next fftloop
					// (the workers' localbase runs the same recurrence; the
					// barrier above guarantees every thread's reduction is
					// done reading them)
					for(int wt=0;wt<nthreads;wt++)
						slotbase[wt] += (int)((long long)(wt+1)*numffts/nthreads)
						              - (int)((long long)wt*numffts/nthreads);
					t_ac_l += STAGE_LAP(ts, "ac");
				}
				else
				{
					StageTimer ts;
					// the replica's accumulation window is one fftloop: zero it
					// (after the reduction above) so the next reduction adds
					// only this worker's new blocks
					m->zeroAutocorrelations();
					if(dokurtosis)
						m->zeroKurtosis();
					if(haspcal)
						m->resetpcal();
					t_copy_l += STAGE_LAP(ts, "copy");
				}
				#pragma omp barrier
			}
			#pragma omp atomic
			g_tt_proc += t_proc_l;
			#pragma omp atomic
			g_tt_reduce += t_reduce_l;
			#pragma omp atomic
			g_tt_copy += t_copy_l;
			#pragma omp atomic
			g_tt_write += t_write_l;
			#pragma omp atomic
			g_tt_ac += t_ac_l;
		}
		StageTimer st2;
		if(acblockcount != 0)
		{
			if(haspcal)
				pcaltext->accumulateWeight(mode);	// before zeroAutocorrelations clears weights
			// P9: same pre-STA averaging as the full batch above
			bool datastreamsaveraged = false;
			if(dosta && config.getMinPostAvFreqChannels(0) >= config.getSTADumpChannels())
			{
				mode->averageFrequency();
				datastreamsaveraged = true;
			}
			if(dosta)
				sendSTA(&monitor, config, 0, dsindex, mode, scan, scanstartsec, offsetsec, offsetns,
				        (acshiftcount*maxacblocks + acblockcount/2.0)*blockns, acblockcount*blockns, datastreamsaveraged, config.getJobName());
			writer.writeAutocorrelationBatch(mode, datastreamsaveraged);
			mode->zeroAutocorrelations();
		}

		writer.writePcal(mode);
		if(haspcal)
		{
			pcaltext->accumulate(mode);
			if(((long long)(s+1)) % subintsperint == 0)
			{
				// start of the intTime that just completed
				long long totalns = batchstartabsns + (long long)(s+1-subintsperint)*(long long)subintns;
				pcaltext->flush(totalns/1000000000LL, (int)(totalns%1000000000LL));
			}
		}
		writer.flushWeights();

		// P9: kurtosis covers the whole subint, not an ac batch
		// (core.cpp:1062-1063; single-thread numblocks = blockspersend)
		if(dokurtosis)
			sendKurtosis(&monitor, config, 0, dsindex, mode, scan, scanstartsec, offsetsec, offsetns,
			             (blockspersend/2.0)*blockns, (double)blockspersend*blockns, blockspersend, config.getJobName());
		g_tt_subtail += STAGE_LAP(st2, "subtail");
	}

	if(haspcal)
	{
		if(pcaltext->hasUnflushed())
			return fail(monitor, "fxcorr-f: batch " + batchid + " ends with unflushed pcal tones (batch length is not an intTime multiple)");
		delete pcaltext;
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
		delete mode;
		for(Mode *m : replicas)
			delete m;
		g_tt_del += STAGE_LAP(td, "del");
	}

	// Products must be on disk before this task calls itself done: a full
	// filesystem has to fail HERE, while this process still knows which file
	// it was writing, rather than surface later as fxcorr-x reading a
	// truncated band_XX.sp (v7-plan 17.2).  Under the P3 layout fengine
	// lives on tmpfs, whose capacity is a hard wall, so this is the expected
	// failure mode, not a rare one.
	{
		StageTimer tf;
		if(!writer.finalise())
			return fail(monitor, "fxcorr-f: station products for batch " + batchid + " are incomplete (see above)");
		g_tt_fin += STAGE_LAP(tf, "fin");
	}

	// upstream ending sequence (fxmanager.cpp terminate/DONE): Ending then Done
	monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
	monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);

	if(g_timing)
	{
		if(!g_timing_trace && !g_timing_brief)
		{
			// parallel stages are sums over threads (CPU-seconds; divide by
			// nthreads for the per-thread view), the rest are wall seconds of
			// the single serial stream
			double total = nowsec() - g_tt_start;
			fprintf(g_timing_out,
			        "TIMING fxcorr-f batch=%s station=%s ds=%d nthreads=%d nsubints=%d"
			        " total=%g init=%g repl=%g blk=%g read=%g swp=%g setup=%g hdr=%g"
			        " proc=%g reduce=%g copy=%g write=%g ac=%g subtail=%g fin=%g del=%g\n",
			        batchid.c_str(), station.c_str(), dsarg, nthreads, nsubints, total,
			        g_tt_init, g_tt_repl, g_tt_blk, g_tt_read, g_tt_swp, g_tt_setup,
			        g_tt_hdr, g_tt_proc, g_tt_reduce, g_tt_copy, g_tt_write, g_tt_ac,
			        g_tt_subtail, g_tt_fin, g_tt_del);
			fflush(g_timing_out);
		}
		timing_finish();	// brief rows / trace buffer
	}

	return EXIT_SUCCESS;
}
