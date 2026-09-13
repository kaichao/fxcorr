#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/model.h>
#include <fxcorrcommon/architecture.h>

#include "spreader.h"
#include "xmac.h"
#include "integrate.h"

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
	if(argc < 2)
	{
		cerr << "usage: fxcorr-x <batch_id> [workdir]" << endl
		     << "  env: FXCORR_WORKDIR (default .), overridden by the workdir argument" << endl;
		return EXIT_FAILURE;
	}
	string batchid = argv[1];
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;
	if(argc > 2)
		workdir = argv[2];	// argument takes precedence over the environment

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
	string inputfile, difxdir;
	if(!extractJsonDouble(json, "start_mjd", &startmjd) ||
	   !extractJsonInt(json, "n_subints", &nsubints) ||
	   !extractJsonString(json, "config_file", &inputfile))
	{
		cerr << "fxcorr-x: batch.json missing required fields" << endl;
		return EXIT_FAILURE;
	}
	if(!extractJsonString(json, "difx_dir", &difxdir))
		difxdir = "vis/" + batchid + ".difx";

	// parse .input (non-MPI constructor)
	Configuration config((workdir + "/" + inputfile).c_str(), 0);
	Model *model = config.getModel();

	// V1 restrictions (impl-plan 1 / data-spec 12)
	if(model->getNumScans() != 1)
	{
		cerr << "fxcorr-x: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)" << endl;
		return EXIT_FAILURE;
	}
	int scan = 0;
	int configindex = config.getScanConfigIndex(scan);
	if(configindex < 0)
	{
		cerr << "fxcorr-x: no configuration for scan 0" << endl;
		return EXIT_FAILURE;
	}
	if(config.pulsarBinOn(configindex))
	{
		cerr << "fxcorr-x: pulsar binning is not supported in V1" << endl;
		return EXIT_FAILURE;
	}
	if(config.phasedArrayOn(configindex))
	{
		cerr << "fxcorr-x: phased arrays are not supported in V1" << endl;
		return EXIT_FAILURE;
	}
	if(config.getMaxProducts() > 2)
	{
		cerr << "fxcorr-x: cross-polar autocorrelations (maxproducts > 2) are not supported in V1" << endl;
		return EXIT_FAILURE;
	}
	if(model->getNumPhaseCentres(scan) > 1)
	{
		cerr << "fxcorr-x: multi phase centre uvshifting is not supported in V1" << endl;
		return EXIT_FAILURE;
	}

	int subintns = config.getSubintNS(configindex);
	int blockspersend = config.getBlocksPerSend(configindex);
	int numbufferedffts = config.getNumBufferedFFTs(configindex);

	// intTime must be an integer multiple of subintns, so that Visibility's
	// dump grid coincides with the subint grid (offsetnsperintegration == 0)
	{
		long long inttimens = (long long)(config.getIntTime(configindex)*1.0e9);
		if(inttimens % (long long)subintns != 0)
		{
			cerr << "fxcorr-x: intTime " << config.getIntTime(configindex) << " s is not an integer multiple of subintNS " << subintns << " ns (required in V1)" << endl;
			return EXIT_FAILURE;
		}
	}

	// autocorr.bin carries maxacblocks-batch records per subint; the batch
	// size is read from the file header by Integrator::addAutocorrs
	double blockns = (double)subintns/(double)blockspersend;

	// open one SpReader per (datastream, recorded band)
	int numdatastreams = config.getNumDataStreams();
	vector<vector<SpReader *> > readers(numdatastreams);
	vector<string> autocorrFiles(numdatastreams);
	for(int ds=0;ds<numdatastreams;ds++)
	{
		string station = config.getDStationName(configindex, ds);
		string sdir = workdir + "/fengine/" + batchid + "/" + station;
		int nrecordedbands = config.getDNumRecordedBands(configindex, ds);
		readers[ds].resize(nrecordedbands);
		for(int band=0;band<nrecordedbands;band++)
		{
			char filename[32];
			snprintf(filename, sizeof(filename), "band_%02d.sp", band);
			readers[ds][band] = new SpReader(sdir + "/" + filename);
			if(!readers[ds][band]->ok())
			{
				cerr << "fxcorr-x: cannot read " << sdir << "/" << filename << endl;
				return EXIT_FAILURE;
			}
			int freqindex = config.getDRecordedFreqIndex(configindex, ds, band);
			if(readers[ds][band]->numChannels() != config.getFNumChannels(freqindex) ||
			   readers[ds][band]->blocksPerSend() != (u32)blockspersend ||
			   readers[ds][band]->numBufferedFFTs() != (u32)numbufferedffts ||
			   readers[ds][band]->subintNS() != (u32)subintns ||
			   readers[ds][band]->numSubints() < (u32)nsubints)
			{
				cerr << "fxcorr-x: " << sdir << "/" << filename << " header does not match the config (nchan " << readers[ds][band]->numChannels() << " vs " << config.getFNumChannels(freqindex) << ", bps " << readers[ds][band]->blocksPerSend() << " vs " << blockspersend << ", nsub " << readers[ds][band]->numSubints() << " vs " << nsubints << ")" << endl;
				return EXIT_FAILURE;
			}
		}
		autocorrFiles[ds] = sdir + "/autocorr.bin";
	}

	XmacEngine xmac(&config, configindex);

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
			cerr << "fxcorr-x: batch start " << startmjd << " is not on a subint boundary ("
			     << batchstartns % subintns << " ns into a " << subintns << " ns subint)" << endl;
			return EXIT_FAILURE;
		}
	}

	// Visibility start time: batch start relative to the scan start
	double reld = batchstartjob - (double)scanstartsec;
	if(reld < 0.0)
	{
		cerr << "fxcorr-x: batch starts before the scan start" << endl;
		return EXIT_FAILURE;
	}
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
	Integrator integrator(&config, configindex, workdir + "/" + difxdir, executeseconds, scan, initsec, initns);

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
		for(int ds=0;ds<numdatastreams;ds++)
		{
			int nrecordedbands = config.getDNumRecordedBands(configindex, ds);
			for(int band=0;band<nrecordedbands;band++)
			{
				int rsec, rns, rscan;
				if(!readers[ds][band]->readSubint(s, rscan, rsec, rns))
				{
					cerr << "fxcorr-x: failed to read subint " << s << " of station " << config.getDStationName(configindex, ds) << " band " << band << endl;
					return EXIT_FAILURE;
				}
				if(rscan != scan || rsec != expectedsec || rns != expectedns)
				{
					cerr << "fxcorr-x: subint " << s << " of station " << config.getDStationName(configindex, ds) << " band " << band << " has time " << rscan << "/" << rsec << "/" << rns << ", expected " << scan << "/" << expectedsec << "/" << expectedns << endl;
					return EXIT_FAILURE;
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

			xmac.xmacBatch(fftloop, readers);
			xmac.accumulateWeights(fftloop, readers);
			xcblockcount += numffts;
			if(xcblockcount == maxxcblocks)
			{
				double nsoffset = (xcshiftcount*maxxcblocks + ((double)maxxcblocks)/2.0)*blockns;
				xmac.uvshiftAndAverage(nsoffset, maxxcblocks*blockns, subintresults);
				xcblockcount = 0;
				xcshiftcount++;
			}
		}
		if(xcblockcount != 0)
		{
			double nsoffset = (xcshiftcount*maxxcblocks + ((double)xcblockcount)/2.0)*blockns;
			xmac.uvshiftAndAverage(nsoffset, xcblockcount*blockns, subintresults);
		}

		// baseline weights -> floatresults section (core.cpp:1065-1107, no locks)
		xmac.copyBaselineWeights((f32*)subintresults);

		// autocorrelations come from autocorr.bin (one record per subint)
		integrator.addAutocorrs(s, autocorrFiles, subintresults);

		if(integrator.addSubint(subintresults))
			integrationswritten++;
	}

	cout << "fxcorr-x: batch " << batchid << " complete, " << nsubints << " subints, " << integrationswritten << " integrations written" << endl;

	for(int ds=0;ds<numdatastreams;ds++)
		for(size_t band=0;band<readers[ds].size();band++)
			delete readers[ds][band];
	vectorFree(subintresults);

	return EXIT_SUCCESS;
}
