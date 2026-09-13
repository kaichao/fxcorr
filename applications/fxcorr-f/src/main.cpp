#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/difxmonitor.h>
#include <fxcorrcommon/switchedpower.h>

#include "datareader.h"
#include "fenginewriter.h"
#include "pcaltextwriter.h"

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

// BINARY_STA records, one per recorded band, per autocorrelation batch
// (algo-plan.md P1; core.cpp averageAndSendAutocorrs 1195-1253, V1
// single-thread non-averaged branch).  nsoffsetns/nswidthns describe the
// current ac batch (centre offset and width in ns).
static void sendSTA(DifxMonitor *monitor, Configuration &config, int configindex, int dsindex,
                    Mode *mode, int scan, long long scanstartsec, int subintsec, int subintns,
                    double nsoffsetns, double nswidthns, const string &jobname)
{
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

		int nchan = config.getSTADumpChannels();
		if(freqchannels < nchan)
			nchan = freqchannels;
		int chans_to_avg = freqchannels/nchan;
		f32 renormvalue = 1.0f/(2*freqchannels*weight);

		cf32 *acdata = mode->getAutocorrelation(false, band);
		int recordsize = sizeof(DifxMessageSTARecord) + sizeof(f32)*nchan;
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
		// sum (not average) of chans_to_avg adjacent real parts, core.cpp:1244-1247
		for(int k=0;k<nchan;k++)
		{
			record->data[k] = acdata[2*k*chans_to_avg].re;
			for(int l=1;l<chans_to_avg;l++)
				record->data[k] += acdata[2*(k*chans_to_avg+l)].re;
		}
		// vectorMulC_f32_I(renormvalue, ...): energy -> power
		for(int k=0;k<nchan;k++)
			record->data[k] *= renormvalue;

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

int main(int argc, char **argv)
{
	if(argc < 3)
	{
		cerr << "usage: fxcorr-f <batch_id> <station> [workdir]" << endl
		     << "  env: FXCORR_WORKDIR (default .), overridden by the workdir argument" << endl;
		return EXIT_FAILURE;
	}
	string batchid = argv[1];
	string station = argv[2];
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;
	if(argc > 3)
		workdir = argv[3];	// argument takes precedence over the environment

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

	// parse .input (non-MPI constructor)
	Configuration config((workdir + "/" + inputfile).c_str(), 0);
	Model *model = config.getModel();

	// V1 restriction: single-scan experiments only
	if(model->getNumScans() != 1)
	{
		cerr << "fxcorr-f: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)" << endl;
		return EXIT_FAILURE;
	}

	// find this station's datastream index
	int dsindex = -1;
	for(int d=0;d<config.getNumDataStreams();d++)
	{
		if(config.getDStationName(0, d) == station)
		{
			dsindex = d;
			break;
		}
	}
	if(dsindex < 0)
	{
		cerr << "fxcorr-f: station " << station << " not found in .input" << endl;
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

	DataReader reader(&config, 0, dsindex, model, batchstartsec, batchstartns);

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

	string outdir = workdir + "/fengine/" + batchid + "/" + station;
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
		int datasec = 0, datans = 0;
		int bytes = reader.readSubint(scan, offsetsec, offsetns, databuf, sendbytes, &datasec, &datans);

		// P6: accumulate subint bytes into datasegment-sized blocks and feed
		// the switched power detector (vdiffile.cpp:945-964).  Subint reads
		// carry frame-aligned guard overlap (sendbytes > one subint of frames),
		// so skip the bytes already covered by the previous subint to keep the
		// block frame-continuous -- upstream feeds the continuous vdifmux
		// stream, which has no overlap.
		if(switchedpower && bytes > 0)
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

		// P1: input datarate diagnostics, upstream datastream.cpp:631-634
		monitor.diagnosticDataConsumed(bytes);
		monitor.diagnosticInputDatarate((double)bytes / ((double)subintns/1.0e9));

		mode->zeroAutocorrelations();
		reader.fillValidFlags(validflags, bytes);
		mode->setValidFlags(validflags);
		mode->setData(databuf, bytes, scan, datasec, datans);
		mode->setOffsets(scan, offsetsec, offsetns);
		if(haspcal)
			mode->resetpcal();

		writer.writeSubintHeader(scan, offsetsec, offsetns, mode, validflags);

		// same loop structure as core.cpp:786-801: buffered slot reuse over fftloops
		int fftloops = (blockspersend + numbufferedffts - 1)/numbufferedffts;
		int acblockcount = 0, acshiftcount = 0;
		for(int fftloop=0;fftloop<fftloops;fftloop++)
		{
			int numffts = blockspersend - fftloop*numbufferedffts;
			if(numffts > numbufferedffts)
				numffts = numbufferedffts;

			for(int b=0;b<numbufferedffts;b++)
			{
				int i = fftloop*numbufferedffts + b;
				if(i >= blockspersend)
					break;
				mode->process(i, b);
			}
			writer.writeSpectra(fftloop, mode);

			// autocorrelation averaging batches, core.cpp:993-1003
			acblockcount += numffts;
			if(acblockcount == maxacblocks)
			{
				if(haspcal)
					pcaltext->accumulateWeight(mode);	// before zeroAutocorrelations clears weights
				if(dosta)
					sendSTA(&monitor, config, 0, dsindex, mode, scan, scanstartsec, offsetsec, offsetns,
					        (acshiftcount*maxacblocks + maxacblocks/2.0)*blockns, maxacblocks*blockns, config.getJobName());
				writer.writeAutocorrelationBatch(mode);
				mode->zeroAutocorrelations();
				acblockcount = 0;
				acshiftcount++;
			}
		}
		if(acblockcount != 0)
		{
			if(haspcal)
				pcaltext->accumulateWeight(mode);	// before zeroAutocorrelations clears weights
			if(dosta)
				sendSTA(&monitor, config, 0, dsindex, mode, scan, scanstartsec, offsetsec, offsetns,
				        (acshiftcount*maxacblocks + acblockcount/2.0)*blockns, acblockcount*blockns, config.getJobName());
			writer.writeAutocorrelationBatch(mode);
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

	delete [] databuf;
	delete [] validflags;
	delete mode;

	// upstream ending sequence (fxmanager.cpp terminate/DONE): Ending then Done
	monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
	monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);

	return EXIT_SUCCESS;
}
