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
		cerr << "usage: fxcorr-f <batch_id> <station> [workdir]" << endl;
		return EXIT_FAILURE;
	}
	string batchid = argv[1];
	string station = argv[2];
	string workdir = (argc > 3) ? argv[3] : ".";

	// batch.json is pre-written by run_batch.sh (fengine/<batch_id>/batch.json)
	string batchjsonpath = workdir + "/fengine/" + batchid + "/batch.json";
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
	DataReader reader(&config, 0, dsindex, model);

	string outdir = workdir + "/fengine/" + batchid + "/" + station;
	string mkdircommand = "mkdir -p " + outdir;
	if(system(mkdircommand.c_str()) != 0)
	{
		cerr << "fxcorr-f: cannot create " << outdir << endl;
		return EXIT_FAILURE;
	}
	FEngineWriter writer(outdir, &config, 0, dsindex, nsubints);

	int sendbytes = reader.getSendBytes();
	u8 *databuf = new u8[sendbytes];
	int blockspersend = reader.getBlocksPerSend();
	int flagwords = (blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT;
	s32 *validflags = new s32[flagwords];

	bool haspcal = (config.getDPhaseCalIntervalHz(0, dsindex) > 0);
	int subintns = config.getSubintNS(0);

	// batch start expressed as job-relative seconds, then per-subint offsets
	int scan = 0;
	long long scanstartsec = (long long)model->getScanStartSec(scan, config.getStartMJD(), config.getStartSeconds());
	double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds()/86400.0;
	double batchstartjob = (startmjd - jobstart)*86400.0;

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
		int numbufferedffts = config.getNumBufferedFFTs(0);
		int fftloops = (blockspersend + numbufferedffts - 1)/numbufferedffts;
		for(int fftloop=0;fftloop<fftloops;fftloop++)
		{
			for(int b=0;b<numbufferedffts;b++)
			{
				int i = fftloop*numbufferedffts + b;
				if(i >= blockspersend)
					break;
				mode->process(i, b);
			}
			writer.writeSpectra(fftloop, mode);
		}

		writer.writePcal(mode);
		writer.writeAutocorrelation(mode);
	}

	delete [] databuf;
	delete [] validflags;
	delete mode;

	return EXIT_SUCCESS;
}
