#ifndef ACBATCH_H
#define ACBATCH_H

#include <vector>

#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>

#include "integrate.h"	// AutocorrBatch

/**
 * @class AcBatchCollector
 * @brief One datastream's autocorrelation-batch collector for fxcorr-xp --
 *        the in-memory form of fxcorr-f's
 *        FEngineWriter::writeAutocorrelationBatch.
 *
 * The station stage calls collect() at every maxacblocks boundary of the
 * subint (and once for the trailing partial batch); Integrator::addAutocorrs
 * accumulates the returned records.  The channel counts and the
 * zoom-to-parent weight mapping are precomputed exactly like
 * FEngineWriter's constructor, and collect() follows the same steps as
 * writeAutocorrelationBatch: averageFrequency(), then the parallel section
 * and -- when WRITE AUTOCORRS && maxproducts > 2 -- the cross-pol section,
 * both walked in total-band order with zoom band weights taken from the
 * parent recorded band (core.cpp:1324-1339).
 */
class AcBatchCollector {
public:
	AcBatchCollector(Configuration *config, int configindex, int dsindex);

	/** Averages the spectra in frequency and copies out one ac batch record. */
	AutocorrBatch collect(Mode *mode);

private:
	Configuration *config;
	int configindex;
	int dsindex;
	int nrecordedbands;
	int ntotalbands;
	bool hascrosspol;
	std::vector<int> acbandnchan;	// per total band, nchan/chanstoavg
	std::vector<int> parentband;	// per total band: the recorded band whose weight applies
};

#endif
