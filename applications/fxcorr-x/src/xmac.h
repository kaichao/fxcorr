#ifndef XMAC_H
#define XMAC_H

#include <vector>

#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/model.h>

#include "spreader.h"

/**
 * @class XmacEngine
 * @brief Baseline-based cross multiplication and uvshift/averaging for one
 *        subint, extracted from Core::processdata() (core.cpp:814-1052,
 *        1431-1938) with the following removals: MPI/pthread, pulsar
 *        binning, phased array, thread splitting.  Multi phase centre
 *        rotation and decorrelation are supported (P4b, core.cpp:1636-1725,
 *        1746-1815, 1877-1923).
 *
 * Data source is .sp files (per-station per-band spectra + weights) instead
 * of Mode objects; vis2 is conjugated on the fly (equivalent of
 * Mode::getConjugatedFreqs()).
 */
class XmacEngine {
public:
	XmacEngine(Configuration *config, int configindex, Model *model, int scan);
	~XmacEngine();

	/** Zero threadcrosscorrs, baselineweights and baselineshiftdecorrs for a new subint (core.cpp:722-759). */
	void zeroSubint();

	/**
	 * One fftloop batch of cross multiplication into threadcrosscorrs
	 * (core.cpp:867-982, non-pulsar branch).
	 * readers[ds][band] are positioned at the current subint.
	 */
	void xmacBatch(int fftloop, const std::vector<std::vector<SpReader *> > &readers);

	/** Accumulate this batch's baseline weights (core.cpp:1005-1052). */
	void accumulateWeights(int fftloop, const std::vector<std::vector<SpReader *> > &readers);

	/**
	 * Average threadcrosscorrs (with spectral averaging and multi phase
	 * centre rotation if configured) into subintresults, then zero
	 * threadcrosscorrs (core.cpp:1431-1603, single thread path).
	 * offsetsec is the subint start in scan-relative seconds (upstream
	 * offsets[1] + offsets[2]/1e9), nsoffset the mid-XMAC-window offset in ns.
	 */
	void uvshiftAndAverage(double offsetsec, double nsoffset, double nswidth, cf32 *subintresults);

	/** Copy accumulated baseline weights (and shift decorrs) into the floatresults section (core.cpp:1070-1107, no locks). */
	void copyBaselineWeights(f32 *floatresults);

private:
	void uvshiftAndAverageBaselineFreq(double offsetsec, double nsoffset, double nswidth, int freqindex, int baseline, cf32 *subintresults);

	Configuration *config;
	Model *model;
	int scan;
	int configindex;
	int numphasecentres;
	int freqtablelength, numbaselines, numdatastreams;
	int numBufferedFFTs, blockspersend;
	int xmacstridelength;
	long long threadresultlength;
	cf32 *threadcrosscorrs;
	f32 ***baselineweight;	// [freqtablelength][numbaselines][numpolproducts]
	f32 ***baselineshiftdecorr; // [freqtablelength][numbaselines][numphasecentres], only if >1 phase centres
	cf32 *conjbuf;		// scratch for vis2 conjugation
	// multi phase centre scratchspace (core.cpp:416-433)
	int maxchan, maxrotatestrideplussteplength;
	f64 *chanfreqs;
	cf32 *rotator;
	cf32 *rotated;
	f32 *argument;
	int shifterrorcount;
};

#endif
