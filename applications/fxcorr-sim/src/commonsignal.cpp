#include "commonsignal.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <fxcorrcommon/configuration.h>

using namespace std;

namespace CommonSignal
{

// ---------------------------------------------------------------------------
// grid derivation

static bool isInteger(double x)
{
	return fabs(x - rint(x)) < 1.0e-9;
}

// Whole-experiment band layout: per-station band start frequency and
// bandwidth in MHz.  Every band must land on the grid, so the frequency
// differences and bandwidths of ALL bands (not just band 0 like datasim)
// enter the GCD; this catches multi-band layouts datasim's getSpecRes misses.
static bool collectBands(Configuration &config,
                         vector<double> *freqs, vector<double> *bws,
                         vector<long long> *vpsperband)
{
	if(config.getNumDataStreams() == 0)
		return false;
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		int nbands = config.getDNumRecordedBands(0, d);
		if(nbands < 1)
		{
			cerr << "fxcorr-sim: datastream " << config.getDStationName(0, d)
			     << " has no recorded bands" << endl;
			return false;
		}
		// complex baseband samples per band per frame: main.cpp's vpsamps
		// (the frame payload is the total across bands, so divide first)
		long long vps = (long long)config.getFramePayloadBytes(0, d) / nbands * 2;
		for(int b = 0; b < nbands; b++)
		{
			int fq = config.getDRecordedFreqIndex(0, d, b);
			freqs->push_back(config.getFreqTableFreq(fq));
			bws->push_back(config.getFreqTableBandwidth(fq));
			vpsperband->push_back(vps);
		}
	}
	return true;
}

bool deriveGrid(Configuration &config, Grid *grid, int specresfac)
{
	if(specresfac < 1)
	{
		cerr << "fxcorr-sim: FXSIM_SPECRES must be a positive integer" << endl;
		return false;
	}
	vector<double> freqs, bws;
	vector<long long> vpsperband;
	if(!collectBands(config, &freqs, &bws, &vpsperband))
		return false;

	// Spectrum resolution: datasim's getSpecRes walks 0.5 MHz down to
	// 1/1024 MHz and takes the first candidate that every frequency difference
	// and every bandwidth is a multiple of ("0.5 MHz is clean enough" being its
	// upper bound).  Two differences here: the accumulator is reset per
	// candidate (datasim keeps a sticky false and so rejects any layout that is
	// not 0.5 MHz clean), and the walk continues *above* 0.5 MHz when the whole
	// first half fails - see the frame condition below.
	//
	// Every band must also come out as a whole number of slices per frame.
	// The frame structure is the real observation's, not ours to choose, and
	// blksize = bandwidth / specres has to divide vpsamps, the band's complex
	// samples per frame (signalgen.cpp rejects the layout otherwise).  t25362
	// is why the second half is needed at all: vpsamps = 2000 = 16 x 125, whose
	// largest power-of-two factor is 16, so every candidate at or below 0.5 MHz
	// (blksize >= 64) fails and 2 MHz (blksize 16) is the only answer.
	//
	// Order is 0.5 MHz first (datasim's choice), then finer, then coarser: a
	// finer grid only costs computation, while a coarser one starts merging
	// channels - a specres wider than the per-channel bandwidth makes
	// neighbouring channels share a grid point and come out perfectly
	// correlated - so finer has to be tried before coarser.
	double specres = 0.0;
	for(int step = 0; step < 16 && specres == 0.0; step++)
	{
		double cand;
		if(step < 10)
			cand = 1.0 / pow(2.0, step + 1);	// 0.5 .. 1/1024 MHz
		else
			cand = pow(2.0, step - 10);		// 1, 2, 4, ... MHz
		bool isgcd = true;
		for(size_t i = 0; i < freqs.size() && isgcd; i++)
			for(size_t j = i + 1; j < freqs.size(); j++)
				isgcd = isgcd && isInteger(fabs(freqs[i] - freqs[j]) / cand);
		for(size_t i = 0; i < bws.size() && isgcd; i++)
			isgcd = isgcd && isInteger(bws[i] / cand);
		// frame compatibility (blksize >= 4 is also the cut that ends the
		// coarser half of the walk)
		for(size_t i = 0; i < bws.size() && isgcd; i++)
		{
			double blksize = bws[i] / cand;
			long long n = (long long)rint(blksize);
			isgcd = isgcd && isInteger(blksize) && blksize >= 4.0 &&
			        n <= vpsperband[i] && vpsperband[i] % n == 0;
		}
		if(isgcd)
			specres = cand;
	}
	if(specres == 0.0)
	{
		cerr << "fxcorr-sim: cannot find a spectrum resolution: band frequencies "
		        "and bandwidths must be multiples of a 1/2^n MHz grid, and the "
		        "resolution must split every band into whole slices per frame" << endl;
		return false;
	}

	// FXSIM_SPECRES: divide the GCD grid by the scaling factor (datasim.cpp
	// "specRes /= setupinfo.specres"); every consistency check below then
	// runs on the scaled grid
	specres /= (double)specresfac;

	// coverage span: min band start to max band top edge across ALL bands of
	// ALL datastreams (datasim getMaxChanFreq uses band-0 bandwidth x band
	// count, i.e. assumes each datastream's bands are contiguous; layouts
	// with gaps like 200 + 205 MHz silently overrun its common signal, so
	// cover the full extent instead - slices in a gap are generated but
	// never read by any station)
	double minstartfreq = freqs[0];
	double maxtopfreq = freqs[0] + bws[0];
	for(size_t i = 0; i < freqs.size(); i++)
	{
		if(freqs[i] < minstartfreq)
			minstartfreq = freqs[i];
		if(freqs[i] + bws[i] > maxtopfreq)
			maxtopfreq = freqs[i] + bws[i];
	}
	double maxchanfreq = maxtopfreq - minstartfreq;

	if(!isInteger(maxchanfreq / specres))
	{
		cerr << "fxcorr-sim: coverage span " << maxchanfreq
		     << " MHz is not an integer multiple of the spectrum resolution "
		     << specres << " MHz" << endl;
		return false;
	}
	grid->specresmhz = specres;
	grid->numsamps = (int)rint(maxchanfreq / specres);
	grid->minstartfreqmhz = minstartfreq;
	grid->stimeus = 1.0 / specres;
	// Block length: D15's 0.5 s block-file granularity kept as the default;
	// FXSIM_BLOCK_US overrides it in microseconds.  It caps a station task's
	// peak memory - the block buffer and every per-band baseband are
	// blksize x slicesperblock - and does not change the output: the PRNG
	// stream runs continuously across blocks and each frame's window stays
	// reachable through the rolling buffers, so any block length holding a
	// whole number of frames reproduces the data byte for byte (a length that
	// does not is rejected at processing time, see signalgen.cpp).
	double blockus = 500000.0;
	if(const char *bu = getenv("FXSIM_BLOCK_US"))
	{
		blockus = atof(bu);
		if(!(blockus > 0.0))
		{
			cerr << "fxcorr-sim: FXSIM_BLOCK_US must be a positive number of "
			        "microseconds" << endl;
			return false;
		}
	}
	grid->slicesperblock = (long long)rint(blockus / grid->stimeus);
	if(grid->slicesperblock < 1)
	{
		cerr << "fxcorr-sim: block length " << blockus << " us is shorter than "
		        "one slice (" << grid->stimeus << " us)" << endl;
		return false;
	}

	// every band must be a contiguous cut of the slice
	for(size_t i = 0; i < freqs.size(); i++)
	{
		double startidx = (freqs[i] - minstartfreq) / specres;
		double blksize = bws[i] / specres;
		if(!isInteger(startidx) || !isInteger(blksize) || blksize < 4.0 ||
		   (long long)rint(startidx + blksize) > (long long)grid->numsamps)
		{
			cerr << "fxcorr-sim: band at " << freqs[i] << " MHz (bw " << bws[i]
			     << " MHz) does not fit the spectrum grid" << endl;
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// slice generation

// Spectral line filter (datasim gengaussianfilter): amplitude sqrt(amp), the
// line centred at (freq - grid origin) / specres grid points, rms in grid
// points, re and im carrying the same value.
static bool buildLineFilter(const Grid &grid, const LineSpec &line,
                            vector<float> *linefilter)
{
	linefilter->clear();
	if(line.freqmhz <= 0.0)
		return true;
	double freqidx = (line.freqmhz - grid.minstartfreqmhz) / grid.specresmhz;
	if(line.amp <= 0.0 || line.rms <= 0.0)
	{
		cerr << "fxcorr-sim: FXSIM_LINE amp and rms must be positive" << endl;
		return false;
	}
	if(freqidx < 0.0 || freqidx >= (double)grid.numsamps)
	{
		cerr << "fxcorr-sim: spectral line at " << line.freqmhz
		     << " MHz is outside the common signal band ("
		     << grid.minstartfreqmhz << " .. "
		     << grid.minstartfreqmhz + (double)grid.numsamps * grid.specresmhz
		     << " MHz)" << endl;
		return false;
	}
	double amplitude = sqrt(line.amp);
	linefilter->resize((size_t)grid.numsamps);
	for(int i = 0; i < grid.numsamps; i++)
	{
		double delta = (double)i - freqidx;
		(*linefilter)[(size_t)i] = (float)(amplitude *
			exp(-M_PI * M_PI * delta * delta / (2.0 * line.rms * line.rms)));
	}
	return true;
}

// One block of the common signal: gencplx semantics (independent real/imag
// Gaussians, STDEV 1, slice-major order, frequency points ascending inside a
// slice) followed by the optional per-slice line multiply.  The engine and the
// distribution are passed in rather than built here on purpose: std::
// normal_distribution caches the second value of each Box-Muller pair, so
// rebuilding it per block would shift every later sample.  This is the only
// place the PRNG advances -- which is what made the old file path and the
// streaming path bit-identical, and is why the S2.5 acceptance test could be
// "the raw output must match byte for byte".
static void fillSliceBlock(const Grid &grid, long long slices,
                           mt19937 &engine, normal_distribution<double> &gauss,
                           const vector<float> &linefilter, float *block)
{
	long long nfloats = slices * 2LL * grid.numsamps;
	for(long long i = 0; i < nfloats; i++)
		block[(size_t)i] = (float)gauss(engine);
	if(linefilter.empty())
		return;
	for(long long t = 0; t < slices; t++)
	{
		long long base = t * 2LL * grid.numsamps;
		for(int i = 0; i < grid.numsamps; i++)
		{
			block[(size_t)(base + 2LL * i)] *= linefilter[(size_t)i];
			block[(size_t)(base + 2LL * i + 1)] *= linefilter[(size_t)i];
		}
	}
}

// ---------------------------------------------------------------------------
// small JSON text helpers
//
// These were Reader's static methods while there was a common/ signal to read;
// after plan E the only JSON left to parse is batch.json, so they are plain
// functions here (main.cpp calls them).

bool jsonDouble(const string &json, const string &key, double *value)
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

bool jsonInt(const string &json, const string &key, int *value)
{
	double d;
	if(!jsonDouble(json, key, &d))
		return false;
	*value = (int)(d + 0.5);
	return true;
}

// 64-bit variant: block_bytes passed INT_MAX once the grid got wide -- t25362
// at 2 MHz resolution asked for 512 points x 1e6 slices x 8 bytes per block.
// Both variants go through the double parser, which is exact well past any
// value a batch-level file holds.
bool jsonInt(const string &json, const string &key, long long *value)
{
	double d;
	if(!jsonDouble(json, key, &d))
		return false;
	*value = (long long)(d + 0.5);
	return true;
}

bool jsonString(const string &json, const string &key, string *value)
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

SliceStream::SliceStream()
	: seed_(0), totalslices_(0), nblocks_(0), blockfloats_(0)
{
}

bool SliceStream::init(const Grid &grid, long long totalslices, unsigned long seed,
                       const LineSpec &line)
{
	if(totalslices <= 0 || grid.numsamps <= 0 || grid.slicesperblock <= 0)
	{
		cerr << "fxcorr-sim: invalid common signal extent" << endl;
		return false;
	}
	if(!buildLineFilter(grid, line, &linefilter_))
		return false;
	grid_ = grid;
	seed_ = seed;
	totalslices_ = totalslices;
	blockfloats_ = (long long)grid.numsamps * 2 * grid.slicesperblock;
	nblocks_ = (totalslices + grid.slicesperblock - 1) / grid.slicesperblock;
	// same reseed as generate(): mt19937::seed() and the mt19937(seed)
	// constructor produce the same stream
	engine_.seed(seed);
	gauss_.reset();
	return true;
}

long long SliceStream::fillBlock(long long n, float *buf)
{
	if(n < 0 || n >= nblocks_)
		return -1;
	long long slices = grid_.slicesperblock;
	if(n == nblocks_ - 1)
		slices = totalslices_ - n * grid_.slicesperblock;
	fillSliceBlock(grid_, slices, engine_, gauss_, linefilter_, buf);
	return slices * 2LL * grid_.numsamps;
}

} // namespace CommonSignal
