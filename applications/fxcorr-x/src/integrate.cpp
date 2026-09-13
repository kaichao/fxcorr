#include "integrate.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>

#include <fxcorrcommon/visibility.h>
#include <fxcorrcommon/difxmonitor.h>

using namespace std;

static const string CIRCULAR_POL_NAMES[4] = {"RR", "LL", "RL", "LR"};
static const string LL_CIRCULAR_POL_NAMES[4] = {"LL", "RR", "LR", "RL"};
static const string LINEAR_POL_NAMES[4] = {"XX", "YY", "XY", "YX"};

// autocorr.bin header layout (FEngineWriter::writeAutocorrHeader):
// "FXCAC\0" + u32 version + u32 nsub + u32 nbands + per band (u32 bandindex, u32 nchan)

Integrator::Integrator(Configuration *conf, int cindex, const string &difxdir, int eseconds,
	int scan, int startsec, int startns, DifxMonitor *monitor) :
	config(conf), configindex(cindex), vis_(0), todiskbuffer_(0), monitor_(monitor)
{
	// todiskbuffer sizing follows fxmanager.cpp:114-133
	int resultlength = config->getMaxCoreResultLength();
	int todiskbufferlen = resultlength*8;
	for(int i=0;i<config->getNumConfigs();i++)
	{
		int confresultbytes = config->getCoreResultLength(i)*8;
		int minchans = 999999;
		for(int j=0;j<config->getFreqTableLength();j++)
		{
			if(config->isFrequencyOutput(i, j) && config->getFNumChannels(j)/config->getFChannelsToAverage(j) < minchans)
				minchans = config->getFNumChannels(j)/config->getFChannelsToAverage(j);
		}
		double headerbloatfactor = 1.0 + ((double)(Visibility::HEADER_BYTES))/(minchans*8);
		if(confresultbytes*headerbloatfactor > todiskbufferlen)
			todiskbufferlen = int(1.02*confresultbytes*headerbloatfactor); //a little extra margin to be sure
	}

	todiskbuffer_ = (char*)vectorAlloc_u8(todiskbufferlen);
	if(!todiskbuffer_)
	{
		cerr << "Failed to allocate " << todiskbufferlen << " bytes for the SWIN output buffer" << endl;
		exit(EXIT_FAILURE);
	}

	// writeSWIN appends into the .input OUTPUT FILENAME directory, so it must
	// exist beforehand (difx_dir in batch.json is metadata only)
	if(system(("mkdir -p " + config->getOutputFilename()).c_str()) != 0)
	{
		cerr << "Failed to create output directory " << config->getOutputFilename() << endl;
		exit(EXIT_FAILURE);
	}

	// polarisation names as in fxmanager.cpp:171-174
	const string *polnames;
	if(config->circularPolarisations())
		polnames = ((config->getMaxProducts() == 1)&&(config->getDRecordedBandPol(0, 0, 0) == 'L'))?LL_CIRCULAR_POL_NAMES:CIRCULAR_POL_NAMES;
	else
		polnames = LINEAR_POL_NAMES;

	vis_ = new Visibility(config, 0, 1, todiskbuffer_, todiskbufferlen, eseconds, scan, startsec, startns, polnames);
	if(!vis_->configuredOK())
	{
		cerr << "Visibility configuration failed" << endl;
		exit(EXIT_FAILURE);
	}
}

Integrator::~Integrator()
{
	delete vis_;
	vectorFree(todiskbuffer_);
}

bool Integrator::addSubint(cf32 *subintresults)
{
	bool done = vis_->addData(subintresults);
	if(done)
	{
		vis_->writedata();
		// RUNNING before increment(), which zeroes floatresults
		// (same order as FxManager::loopwrite: writedata, multicastweights)
		if(monitor_)
			sendRunning();
		vis_->increment();
	}
	return done;
}

// P1: RUNNING status, byte-identical semantics to
// Visibility::multicastweights (visibility.cpp:1100-1146)
void Integrator::sendRunning()
{
	int numdatastreams = config->getNumDataStreams();
	float *weight = new float[numdatastreams];
	int freqindex, weightcount;

	// per-station weights, averaged over recorded bands (only used
	// frequencies); the acweight section is indexed with the same
	// resultindex walk as visibility.cpp:445-460
	for(int i=0;i<numdatastreams;i++)
	{
		const int n = config->getDNumTotalBands(configindex, i);

		weight[i] = 0.0;
		weightcount = 0;
		if(n > 0)
		{
			int resultindex = config->getCoreResultACWeightOffset(configindex, i)*2;
			for(int j=0;j<n;j++)
			{
				freqindex = config->getDTotalFreqIndex(configindex, i, j);
				if(config->isFrequencyUsed(configindex, freqindex) || config->isEquivalentFrequencyUsed(configindex, freqindex))
				{
					// f32 truncation at storage, like upstream
					// autocorrweights (visibility.cpp:455)
					float acw = (float)(vis_->floatresults[resultindex]/vis_->fftsperintegration);
					weight[i] += acw;
					resultindex++;
					weightcount++;
				}
			}
			if(weightcount > 0)
				weight[i] /= weightcount;
		}
	}

	// integration centre time, visibility.cpp:1119-1132 (day-of-observation
	// decomposition; the integer/remainder order matters for f64 precision)
	int intsec = vis_->experseconds +
	             (int)config->getModel()->getScanStartSec(vis_->currentscan, vis_->expermjd, vis_->experseconds) +
	             vis_->currentstartseconds;
	int dumpmjd = vis_->expermjd + intsec/86400;
	double dumpseconds = double(intsec%86400) + ((double)vis_->currentstartns)/1000000000.0 +
	                     config->getIntTime(configindex)/2.0;
	if(dumpseconds > 86400.0)
	{
		dumpmjd++;
		dumpseconds -= 86400.0;
	}
	double mjd = dumpmjd + dumpseconds/86400.0;

	monitor_->status(DIFX_STATE_RUNNING, "", mjd, numdatastreams, weight,
	                 vis_->expermjd + vis_->experseconds/86400.0,
	                 vis_->expermjd + (vis_->experseconds + vis_->executeseconds)/86400.0);

	delete [] weight;
}

void Integrator::addAutocorrs(int subint, const vector<string> &autocorrFiles, cf32 *subintresults)
{
	f32 *floatresults = (f32*)subintresults;
	int numdatastreams = config->getNumDataStreams();

	for(int ds=0;ds<numdatastreams;ds++)
	{
		FILE *file = fopen(autocorrFiles[ds].c_str(), "rb");
		if(file == NULL)
		{
			cerr << "addAutocorrs: cannot open " << autocorrFiles[ds] << endl;
			continue;
		}

		// header: magic + version + nsub + acbatches + nbands
		//         [+ crosspol flag (version 2, P7)] + per-band (bandindex, nchan)
		char magic[6];
		u32 version, nsub, acbatches, nbands, crosspol = 0;
		if(fread(magic, 1, 6, file) != 6 || memcmp(magic, "FXCAC\0", 6) != 0 ||
		   fread(&version, 4, 1, file) != 1 || (version != 1 && version != 2) ||
		   fread(&nsub, 4, 1, file) != 1 ||
		   fread(&acbatches, 4, 1, file) != 1 ||
		   fread(&nbands, 4, 1, file) != 1 ||
		   (version == 2 && fread(&crosspol, 4, 1, file) != 1))
		{
			cerr << "addAutocorrs: bad header in " << autocorrFiles[ds] << endl;
			fclose(file);
			continue;
		}
		if((int)nbands != config->getDNumTotalBands(configindex, ds))
		{
			cerr << "addAutocorrs: " << autocorrFiles[ds] << " has " << nbands << " bands, config expects " << config->getDNumTotalBands(configindex, ds) << endl;
			fclose(file);
			continue;
		}

		// per-band channel counts; each ac batch record is sum over bands of
		// (nchan*8 + 4) bytes per section (parallel + optional cross-pol)
		int *bandnchan = new int[nbands];
		long long recordsize = 0;
		int maxnchan = 0;
		for(int k=0;k<(int)nbands;k++)
		{
			u32 bandindex, nchan;
			if(fread(&bandindex, 4, 1, file) != 1 || fread(&nchan, 4, 1, file) != 1)
			{
				cerr << "addAutocorrs: short header in " << autocorrFiles[ds] << endl;
				delete [] bandnchan;
				fclose(file);
				continue;
			}
			bandnchan[k] = (int)nchan;
			recordsize += (long long)nchan*8 + 4;
			if((int)nchan > maxnchan)
				maxnchan = (int)nchan;
		}
		recordsize *= (long long)(1 + crosspol);

		long long headeroffset = 6 + 4 + 4 + 4 + 4 + (long long)(version == 2 ? 4 : 0) + (long long)nbands*8;
		if(subint < 0 || (u32)subint >= nsub)
		{
			cerr << "addAutocorrs: subint " << subint << " out of range (nsub=" << nsub << ")" << endl;
			delete [] bandnchan;
			fclose(file);
			continue;
		}
		if(fseeko(file, headeroffset + (long long)subint*(long long)acbatches*recordsize, SEEK_SET) != 0)
		{
			cerr << "addAutocorrs: seek failed in " << autocorrFiles[ds] << endl;
			delete [] bandnchan;
			fclose(file);
			continue;
		}

		// core.cpp:1273-1302 / 1314-1339: every ac batch record of this subint
		// is accumulated, over total bands (recorded + zoom); zoom band weights
		// were already mapped to the parent recorded band by fxcorr-f.
		// P7: the cross-pol section follows the parallel one and continues the
		// same resultindex/weightindex walk (core.cpp:1288-1301 / 1342-1369
		// concatenates the sections in the results layout)
		cf32 *acbuf = new cf32[maxnchan];
		for(u32 rec=0;rec<acbatches;rec++)
		{
			int resultindex = config->getCoreResultAutocorrOffset(configindex, ds);
			int weightindex = config->getCoreResultACWeightOffset(configindex, ds)*2;
			auto readsection = [&]()
			{
				for(int k=0;k<(int)nbands;k++)
				{
					int freqindex = config->getDTotalFreqIndex(configindex, ds, k);
					int freqchannels = config->getFNumChannels(freqindex)/config->getFChannelsToAverage(freqindex);
					if((int)bandnchan[k] != freqchannels)
					{
						cerr << "addAutocorrs: " << autocorrFiles[ds] << " band " << k << " has " << bandnchan[k] << " channels, config expects " << freqchannels << endl;
						resultindex += freqchannels;
						weightindex++;
						continue;
					}

					if(fread(acbuf, sizeof(cf32), freqchannels, file) != (size_t)freqchannels)
					{
						cerr << "addAutocorrs: short record in " << autocorrFiles[ds] << endl;
						break;
					}
					f32 acweight = 0.0f;
					if(fread(&acweight, 4, 1, file) != 1)
					{
						cerr << "addAutocorrs: short weight in " << autocorrFiles[ds] << endl;
						break;
					}

					if(config->isFrequencyUsed(configindex, freqindex) || config->isEquivalentFrequencyUsed(configindex, freqindex))
					{
						vectorAdd_cf32_I(acbuf, &subintresults[resultindex], freqchannels);
						floatresults[weightindex] += acweight;
					}
					resultindex += freqchannels;
					weightindex++;
				}
			};
			readsection();
			if(crosspol)
				readsection();
		}
		delete [] acbuf;
		delete [] bandnchan;
		fclose(file);
	}
}
