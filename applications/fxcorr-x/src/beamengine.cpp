#include "beamengine.h"

#include <cstring>
#include <iostream>
#include <utility>
#include <sys/stat.h>
#include <sys/types.h>

using namespace std;

// beam.bin header field offsets, fixed 256-byte header like band_XX.sp
// (fxcorr/data-spec.md, P8; no struct fwrite - padding-free by design)
static const int OFF_MAGIC = 0;		// 6 bytes "FXCBM\0"
static const int OFF_VERSION = 6;	// u32 = 1
static const int OFF_NSUB = 10;		// u32 n_subints
static const int OFF_NACCS = 14;	// u32 acc windows per subint
static const int OFF_ACNS = 18;		// u32 ACC TIME (NS)
static const int OFF_NSEGS = 22;	// u32 number of (freq, pol) output segments
static const int HEADER_BYTES = 256;

BeamEngine::BeamEngine(Configuration *conf, int cindex, const string &workdir,
		       const string &batchid, int nsubints,
		       const vector<vector<SpReader *> > &readers) :
	config(conf), configindex(cindex), freqtablelength(config->getFreqTableLength()),
	readers(readers),
	segs(), outlength(0), accum(0), file(0), ok_(false),
	numaccs(0), accfftloops(0), numbufferedffts(0), blockspersend(0)
{
	int subintns = config->getSubintNS(configindex);
	int accns = config->getFPhasedArrayAccumulationNS(configindex);
	numbufferedffts = config->getNumBufferedFFTs(configindex);
	blockspersend = config->getBlocksPerSend(configindex);
	double blockns = (double)subintns/(double)blockspersend;
	int accffts = (int)(accns/blockns + 0.5);
	numaccs = subintns/accns;
	accfftloops = accffts/numbufferedffts;

	int numdatastreams = config->getNumDataStreams();

	// build the output segment table in upstream order (per freq table
	// entry, per papol) and the (datastream, band) contributor map
	// (recorded bands first, zoom bands as fallback - core.cpp:822-851)
	for(int f=0;f<freqtablelength;f++)
	{
		int npols = config->getFPhasedArrayNumPols(configindex, f);
		for(int p=0;p<npols;p++)
		{
			BeamSeg seg;
			seg.freqindex = f;
			seg.pol = config->getFPhaseArrayPol(configindex, f, p);
			seg.nchan = config->getFNumChannels(f);	// not configindex (upstream bug, algo-plan P8)
			for(int ds=0;ds<numdatastreams;ds++)
			{
				int nrecorded = config->getDNumRecordedBands(configindex, ds);
				int ntotal = config->getDNumTotalBands(configindex, ds);
				int match = -1;
				for(int band=0;band<nrecorded;band++)
				{
					if(config->getDRecordedFreqIndex(configindex, ds, band) == f &&
					   config->getDRecordedBandPol(configindex, ds, band) == seg.pol)
					{
						match = band;
						break;
					}
				}
				if(match < 0)
				{
					for(int band=nrecorded;band<ntotal;band++)
					{
						int localzoom = band-nrecorded;
						if(config->getDZoomFreqIndex(configindex, ds, localzoom) == f &&
						   config->getDZoomBandPol(configindex, ds, localzoom) == seg.pol)
						{
							match = band;
							break;
						}
					}
				}
				if(match >= 0)
					seg.sources.push_back(pair<int,int>(ds, match));
			}
			outlength += seg.nchan;
			segs.push_back(seg);
		}
	}

	accum = vectorAlloc_cf32(outlength);

	// open beam/<batch_id>/beam.bin and write the header
	string dir = workdir + "/beam/" + batchid;
	mkdir(workdir.c_str(), 0755);
	mkdir((workdir + "/beam").c_str(), 0755);
	mkdir(dir.c_str(), 0755);
	string path = dir + "/beam.bin";
	file = fopen(path.c_str(), "wb");
	if(file == NULL)
	{
		cerr << "fxcorr-x: cannot create " << path << endl;
		return;
	}
	char header[HEADER_BYTES];
	memset(header, 0, sizeof(header));
	memcpy(header + OFF_MAGIC, "FXCBM", 6);
	u32 version = 1;
	memcpy(header + OFF_VERSION, &version, 4);
	u32 hnsub = nsubints;
	memcpy(header + OFF_NSUB, &hnsub, 4);
	u32 hnaccs = numaccs;
	memcpy(header + OFF_NACCS, &hnaccs, 4);
	u32 haccns = accns;
	memcpy(header + OFF_ACNS, &haccns, 4);
	u32 hnsegs = segs.size();
	memcpy(header + OFF_NSEGS, &hnsegs, 4);
	if(fwrite(header, 1, HEADER_BYTES, file) != HEADER_BYTES)
	{
		cerr << "fxcorr-x: cannot write " << path << " header" << endl;
		return;
	}
	// per-segment table: freq index, pol, nchan (9 bytes per segment)
	for(size_t i=0;i<segs.size();i++)
	{
		u32 freqindex = segs[i].freqindex;
		u32 nchan = segs[i].nchan;
		char pol = segs[i].pol;
		if(fwrite(&freqindex, 4, 1, file) != 1 ||
		   fwrite(&pol, 1, 1, file) != 1 ||
		   fwrite(&nchan, 4, 1, file) != 1)
		{
			cerr << "fxcorr-x: cannot write " << path << " segment table" << endl;
			return;
		}
	}
	ok_ = true;
}

BeamEngine::~BeamEngine()
{
	if(file != NULL)
	{
		// records are fixed-size, append-only: n_subints backfill not needed
		fflush(file);
		fclose(file);
	}
	if(accum != NULL)
		vectorFree(accum);
}

void BeamEngine::processSubint(int s, int scan, int sec, int ns)
{
	if(!ok_)
		return;

	vectorZero_cf32(accum, outlength);

	int fftloops = (blockspersend + numbufferedffts - 1)/numbufferedffts;
	for(int a=0;a<numaccs;a++)
	{
		// one acc window: accfftloops fftloop batches (Configuration
		// guarantees the acc time is an integer multiple of numbufferedffts)
		for(int l=0;l<accfftloops && a*accfftloops + l < fftloops;l++)
		{
			int fftloop = a*accfftloops + l;
			int numffts = blockspersend - fftloop*numbufferedffts;
			if(numffts > numbufferedffts)
				numffts = numbufferedffts;
			for(int subloop=0;subloop<numffts;subloop++)
			{
				cf32 *dest = accum;
				for(size_t i=0;i<segs.size();i++)
				{
					const BeamSeg &seg = segs[i];
					// beam[f][p] += DWeight[f][ds] * spectrum_ds (core.cpp:850-861)
					for(size_t c=0;c<seg.sources.size();c++)
					{
						int ds = seg.sources[c].first;
						int band = seg.sources[c].second;
						const cf32 *spec = readers[ds][band]->spectra() + (fftloop*numbufferedffts + subloop)*seg.nchan;
						f32 weight = (f32)config->getFPhasedArrayDWeight(configindex, seg.freqindex, ds);
						for(int ch=0;ch<seg.nchan;ch++)
						{
							dest[ch].re += spec[ch].re*weight;
							dest[ch].im += spec[ch].im*weight;
						}
					}
					dest += seg.nchan;
				}
			}
		}
		// window start = subint start + a*accns (scan-relative time base)
		long long windowns = (long long)ns + (long long)a*config->getFPhasedArrayAccumulationNS(configindex);
		int wsec = sec + (int)(windowns/1000000000);
		int wns = (int)(windowns%1000000000);
		s32 hsec = wsec, hns = wns, hscan = scan;
		if(fwrite(&hscan, 4, 1, file) != 1 ||
		   fwrite(&hsec, 4, 1, file) != 1 ||
		   fwrite(&hns, 4, 1, file) != 1 ||
		   fwrite(accum, sizeof(cf32)*outlength, 1, file) != 1)
		{
			cerr << "fxcorr-x: cannot write beam.bin record" << endl;
			ok_ = false;
			return;
		}
		vectorZero_cf32(accum, outlength);
	}
}
