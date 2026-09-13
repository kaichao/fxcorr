#ifndef FENGINEWRITER_H
#define FENGINEWRITER_H

#include <cstdio>
#include <string>
#include <vector>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>

/**
 * @class FEngineWriter
 * @brief Writes station-based products (.sp / pcal.bin / autocorr.bin) for one
 *        datastream of a batch, following fxcorr/data-spec.md 5.3.
 *
 * One instance per datastream.  All files are opened and headers written on
 * construction; each subint is appended by the write* methods.
 */
class FEngineWriter {
public:
	/**
	 * @param outdir  fengine/<batch_id>/<station>/ (must exist)
	 * @param config  parsed .input
	 * @param configindex   configuration index for this datastream
	 * @param dsindex  datastream (configdatastream) index
	 * @param nsubints  total subints expected in the batch
	 * @param acblocks  FFTs per autocorrelation averaging batch (maxacblocks,
	 *                  same formula as core.cpp:778-783); autocorr.bin holds
	 *                  one record per such batch
	 */
	FEngineWriter(const std::string &outdir, Configuration *config, int configindex, int dsindex, int nsubints, int acblocks);
	~FEngineWriter();

	/** Writes the per-subint block header (scan/sec/ns, flags, weights) of every band_XX.sp. */
	void writeSubintHeader(int scan, int sec, int ns, Mode *mode, const s32 *validflags);

	/** Writes one fftloop worth of spectra (the buffered slots just processed) and collects the per-slot data weights. */
	void writeSpectra(int fftloop, Mode *mode);

	/** Rewrites the weights placeholder of the current subint with the collected per-FFT weights. */
	void flushWeights();

	/** Finalises and writes one subint of phasecal tones (pcal.bin); no-op if no phasecal configured. */
	void writePcal(Mode *mode);

	/**
	 * Averages in frequency and writes one autocorrelation averaging batch
	 * (autocorr.bin).  The caller must zeroAutocorrelations() afterwards,
	 * exactly like core.cpp:994-1003.
	 *
	 * P9: when datastreamsaveraged is set, the caller has already averaged
	 * the spectra in frequency (for an STA dump above the post-average
	 * channel count, core.cpp:1181-1187) and the average step here must be
	 * skipped, like core.cpp:1263-1268.
	 */
	void writeAutocorrelationBatch(Mode *mode, bool datastreamsaveraged = false);

private:
	void writeSpHeader(int band);
	void writePcalHeader();
	void writeAutocorrHeader();

	Configuration *config;
	int configindex;
	int dsindex;
	int nsubints;

	int nrecordedbands;
	int ntotalbands;	// recorded + zoom (autocorr.bin covers all of them)
	int blockspersend;
	int flagwords;
	int acblocks;		// FFTs per autocorrelation averaging batch
	std::vector<int> acbandnchan;	// per total band, autocorr width after averageFrequency()
	bool haspcal;
	bool hascrosspol;	// WRITE AUTOCORRS && maxproducts>2: autocorr.bin also carries
				// the cross-polar section (core.cpp:1288 condition)

	std::vector<FILE *> spfiles;
	std::vector<std::string> spnames;
	std::vector<f32 *> weightaccum;	// per band, blockspersend per-FFT weights
	std::vector<long long> weightoffsets;	// per band, file offset of the weights section
	FILE *pcalfile;
	FILE *autocorrfile;
};

#endif
