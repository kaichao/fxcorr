#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT
#include <fxcorrcommon/architecture.h>

#include "datareader.h"
#include "fenginewriter.h"
#include "pcaltextwriter.h"

using namespace std;

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
	{
		cerr << "fxcorr-f: cannot create " << outdir << endl;
		return EXIT_FAILURE;
	}
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
		{
			cerr << "fxcorr-f: cannot create " << pcaldir << endl;
			return EXIT_FAILURE;
		}
		pcaltext = new PcalTextWriter(pcaldir, &config, 0, dsindex);
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
			cerr << "fxcorr-f: batch start " << startmjd << " is not on a subint boundary ("
			     << batchstartns % subintns << " ns into a " << subintns << " ns subint)" << endl;
			return EXIT_FAILURE;
		}
	}

	// intTime boundaries for the PCAL text file: count subints per intTime
	// (run_batch.sh validates intTime is a subint multiple and the batch
	// length is an intTime multiple, so boundaries never split a batch)
	double inttime = config.getIntTime(0);
	long long subintsperint = (long long)(inttime*1.0e9/(double)subintns + 0.5);
	if(subintsperint < 1)
	{
		cerr << "fxcorr-f: intTime " << inttime << " s is smaller than one subint (" << subintns << " ns)" << endl;
		return EXIT_FAILURE;
	}
	long long batchstartabsns = (long long)batchstartsec*1000000000LL + (long long)batchstartns;

	for(int s=0;s<nsubints;s++)
	{
		double subintstart = batchstartjob + (double)s*(double)subintns/1.0e9;
		double reld = subintstart - (double)scanstartsec;
		if(reld < 0.0)
		{
			cerr << "fxcorr-f: subint " << s << " starts before the scan start" << endl;
			return EXIT_FAILURE;
		}
		int offsetsec = (int)floor(reld);
		int offsetns = (int)((reld - (double)offsetsec)*1.0e9 + 0.5);

		// station-based processing of one subint (core.cpp:694-801, V1 single-threaded)
		int datasec = 0, datans = 0;
		int bytes = reader.readSubint(scan, offsetsec, offsetns, databuf, sendbytes, &datasec, &datans);

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
		int acblockcount = 0;
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
				writer.writeAutocorrelationBatch(mode);
				mode->zeroAutocorrelations();
				acblockcount = 0;
			}
		}
		if(acblockcount != 0)
		{
			if(haspcal)
				pcaltext->accumulateWeight(mode);	// before zeroAutocorrelations clears weights
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
		{
			cerr << "fxcorr-f: batch " << batchid << " ends with unflushed pcal tones (batch length is not an intTime multiple)" << endl;
			return EXIT_FAILURE;
		}
		delete pcaltext;
	}

	delete [] databuf;
	delete [] validflags;
	delete mode;

	return EXIT_SUCCESS;
}
