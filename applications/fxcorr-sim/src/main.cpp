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

#include "signalgen.h"
#include "vdifwriter.h"

using namespace std;

// Minimal batch.json field extraction (fixed V1 format, data-spec 5.3 D9);
// same helpers as fxcorr-f/src/main.cpp.
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

// VDIF epoch 32 = 2000-01-01, MJD 51544
#define VDIF_EPOCH_MJD 51544.0

int main(int argc, char **argv)
{
	if(argc < 3)
	{
		cerr << "usage: fxcorr-sim <batch_id> <station> [workdir] [tone_mhz ...]" << endl
		     << "  one tone value applies to all bands; nbands values apply band by band" << endl
		     << "  env: FXSIM_NOISE (default 0.02, 0 disables noise), FXSIM_SEED (default fixed)," << endl
		     << "       FXCORR_WORKDIR (default .), overridden by the workdir argument" << endl;
		return EXIT_FAILURE;
	}
	string batchid = argv[1];
	string station = argv[2];
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;
	if(argc > 3)
		workdir = argv[3];	// argument takes precedence over the environment

	// optional baseband tone frequencies in MHz
	vector<double> tonemhzarg;
	for(int a = 4; a < argc; a++)
		tonemhzarg.push_back(atof(argv[a]));

	double noisesigma = 0.02;
	if(const char *ns = getenv("FXSIM_NOISE"))
		noisesigma = atof(ns);
	unsigned long seed = 20260912UL;
	if(const char *sd = getenv("FXSIM_SEED"))
		seed = strtoul(sd, 0, 10);

	// batch.json is pre-written by make_testdata.sh (batches/<batch_id>.json)
	string batchjsonpath = workdir + "/batches/" + batchid + ".json";
	ifstream jf(batchjsonpath.c_str());
	if(!jf.is_open())
	{
		cerr << "fxcorr-sim: cannot open " << batchjsonpath << endl;
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
		cerr << "fxcorr-sim: batch.json missing required fields" << endl;
		return EXIT_FAILURE;
	}

	// parse .input (non-MPI constructor), same as fxcorr-f
	Configuration config((workdir + "/" + inputfile).c_str(), 0);
	if(config.getNumDataStreams() == 0)
	{
		cerr << "fxcorr-sim: failed to parse " << inputfile << endl;
		return EXIT_FAILURE;
	}
	Model *model = config.getModel();

	// V1 restriction: single-scan experiments only
	if(model->getNumScans() != 1)
	{
		cerr << "fxcorr-sim: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)" << endl;
		return EXIT_FAILURE;
	}

	// find this station's datastream index
	int dsindex = -1;
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		if(config.getDStationName(0, d) == station)
		{
			dsindex = d;
			break;
		}
	}
	if(dsindex < 0)
	{
		cerr << "fxcorr-sim: station " << station << " not found in .input" << endl;
		return EXIT_FAILURE;
	}

	// V1: real (baseband) sampling and one shared sample rate across bands
	if(config.getDSampling(0, dsindex) == Configuration::COMPLEX)
	{
		cerr << "fxcorr-sim: V1 supports real (baseband) sampling only" << endl;
		return EXIT_FAILURE;
	}
	// band count, not distinct-frequency count: dual-pol setups record two
	// bands (R/L) at the same frequency, which numrecordedfreqs would count once
	int nbands = config.getDNumRecordedBands(0, dsindex);
	if(nbands < 1)
	{
		cerr << "fxcorr-sim: station " << station << " has no recorded bands" << endl;
		return EXIT_FAILURE;
	}
	// per-band RF passband (for pcal baseband conversion); the FREQ table
	// stores MHz (BW (MHZ) parsed with atof), convert to Hz like everything
	// else here
	vector<double> bandwidhz;
	for(int b = 0; b < nbands; b++)
	{
		int fq = config.getDRecordedFreqIndex(0, dsindex, b);
		bandwidhz.push_back(config.getFreqTableBandwidth(fq) * 1.0e6);
	}

	// V1: 2-bit samples (4 samples per byte, per band); the .input field
	// counts all bands, so num/denom = nbands/4
	if(config.getDBytesPerSampleNum(0, dsindex) * 4 !=
	   config.getDBytesPerSampleDenom(0, dsindex) * nbands)
	{
		cerr << "fxcorr-sim: V1 supports 2-bit samples only" << endl;
		return EXIT_FAILURE;
	}

	// sample rate comes from the VDIF frame structure in .input, not from the
	// band bandwidth (a 4 MHz band is recorded at 8 Ms/s); same derivation as
	// fxcorr-f datareader.cpp
	int bytesperbandframe = config.getFramePayloadBytes(0, dsindex);
	int framespersecond = config.getFramesPerSecond(0, dsindex);
	if(bytesperbandframe <= 0 || framespersecond <= 0)
	{
		cerr << "fxcorr-sim: cannot derive frame structure from .input" << endl;
		return EXIT_FAILURE;
	}
	long long nsampframe = (long long)bytesperbandframe * 4;
	long long ratehz = nsampframe * (long long)framespersecond;
	long long framens = 1000000000LL / (long long)framespersecond;
	if(framens <= 0 || framens * framespersecond != 1000000000LL)
	{
		cerr << "fxcorr-sim: frame duration is not an integer number of ns at " << ratehz << " Hz" << endl;
		return EXIT_FAILURE;
	}

	// batch start must lie on a subint boundary (data-spec section 12);
	// tolerance absorbs the f64 representation error of start_mjd (~1 us)
	int subintns = config.getSubintNS(0);
	long long durationns = (long long)nsubints * (long long)subintns;
	{
		double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds() / 86400.0;
		long long batchstartns = (long long)floor((startmjd - jobstart) * 86400.0 * 1.0e9 + 0.5);
		if(batchstartns % subintns > 1000 && (subintns - batchstartns % subintns) > 1000)
		{
			cerr << "fxcorr-sim: batch start " << startmjd << " is not on a subint boundary ("
			     << batchstartns % subintns << " ns into a " << subintns << " ns subint)" << endl;
			return EXIT_FAILURE;
		}
	}

	// VDIF frame numbers wrap at each whole second (0..fps-1); the batch may
	// start at any frame boundary within a second.  Global frame numbering
	// (data-spec 5.2, distributed-generation requirement) holds as long as
	// the start lies on a frame boundary and the duration is an integer
	// number of frames (checked below).  The tolerance absorbs the f64
	// representation error of start_mjd (same reasoning as the subint check
	// above, ~100s of ns at these epoch values).
	long long startns = (long long)((startmjd - VDIF_EPOCH_MJD) * 86400.0 * 1.0e9 + 0.5);
	long long startsec = startns / 1000000000LL;
	long long secns = startns - startsec * 1000000000LL;
	{
		// a start within 1 us of a whole second snaps to the whole second,
		// keeping frame numbering from 0 there (the historical behaviour the
		// byte-comparison tests rely on); anything else must be on a frame
		// boundary
		if(secns < 1000)
			secns = 0;
		else if(1000000000LL - secns < 1000)
		{
			secns = 0;
			startsec += 1;
		}
		long long rem = secns % framens;
		if(rem > 1000 && framens - rem > 1000)
		{
			cerr << "fxcorr-sim: batch start is not on a frame boundary" << endl;
			return EXIT_FAILURE;
		}
		secns -= rem;                      // snap to the frame boundary
	}
	long long framestart = secns / framens;
	// The batch duration need not be an integer number of frames: the file
	// runs to the next frame boundary and fxcorr-f reads only the batch span.
	// Frame-aligned subints (128 ms = 32 frames) are required for multi-batch
	// starts to land on frame boundaries, but they trip a vdifmux frame-number
	// parsing bug in mpifxcorr (frame # bit 7 treated as signed, ~0.5 s of
	// every 256 frames misread; see impl-plan 2.4), so comparison runs use the
	// test-input config (0.524288 s subint = 131.072 frames, non-aligned).
	long long nframestotal = (durationns + framens - 1) / framens;

	// tones: 0 values = no tone, 1 value = all bands, nbands values = per band
	vector<double> tonehz(nbands, 0.0);
	if(tonemhzarg.size() == 1)
	{
		for(int b = 0; b < nbands; b++)
			tonehz[b] = tonemhzarg[0] * 1.0e6;
	}
	else if(tonemhzarg.size() == (size_t)nbands)
	{
		for(int b = 0; b < nbands; b++)
			tonehz[b] = tonemhzarg[b] * 1.0e6;
	}
	else if(tonemhzarg.size() > 0)
	{
		cerr << "fxcorr-sim: tone count must be 0, 1 or the band count (" << nbands << "), got " << tonemhzarg.size() << endl;
		return EXIT_FAILURE;
	}

	// pcal tones from the .input PHASE CAL config, converted to baseband
	// frequencies (the same grid configuration.cpp counts when it reads .input)
	vector<vector<double> > pcalhz(nbands);
	if(config.getDPhaseCalIntervalHz(0, dsindex) > 0)
	{
		for(int b = 0; b < nbands; b++)
		{
			int fq = config.getDRecordedFreqIndex(0, dsindex, b);
			double bandedge = config.getFreqTableFreq(fq) * 1.0e6;   // MHz -> Hz
			bool lsb = config.getFreqTableLowerSideband(fq);
			int nt = config.getDRecordedFreqNumPCalTones(0, dsindex, b);
			for(int k = 0; k < nt; k++)
			{
				double rf = config.getDRecordedFreqPCalToneFreqHz(0, dsindex, b, k);
				double base = lsb ? (bandedge - rf) : (rf - bandedge);
				if(base >= 0.0 && base < bandwidhz[b])
					pcalhz[b].push_back(base);
			}
		}
	}

	// output raw/<station>/<station>_<batch_id>.vdif (data-spec 5.2)
	string outdir = workdir + "/raw/" + station;
	string outpath = outdir + "/" + station + "_" + batchid + ".vdif";
	string mkdircommand = "mkdir -p " + outdir;
	if(system(mkdircommand.c_str()) != 0)
	{
		cerr << "fxcorr-sim: cannot create " << outdir << endl;
		return EXIT_FAILURE;
	}

	SignalGen gen;
	vector<double> ratehzvec(nbands, (double)ratehz);
	gen.init(ratehzvec, tonehz, pcalhz, noisesigma, seed);
	VDIFWriter writer(outpath, startsec, framestart, ratehz, nbands, bytesperbandframe);
	if(!writer.isOpen())
		return EXIT_FAILURE;

	int payloadbytes = bytesperbandframe * nbands;
	unsigned char *payload = new unsigned char[payloadbytes];
	for(long long n = 0; n < nframestotal; n++)
	{
		memset(payload, 0, (size_t)payloadbytes);
		gen.fillFramePayload(payload, payloadbytes, nbands);
		if(!writer.writeFrame(payload, payloadbytes))
		{
			cerr << "fxcorr-sim: write failed at frame " << n << endl;
			delete [] payload;
			return EXIT_FAILURE;
		}
	}
	delete [] payload;

	cerr << "wrote " << outpath << ": " << nframestotal << " frames, "
	     << nframestotal * (32 + payloadbytes) << " bytes" << endl;
	return EXIT_SUCCESS;
}
