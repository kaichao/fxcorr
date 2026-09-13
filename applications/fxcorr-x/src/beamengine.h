#ifndef BEAMENGINE_H
#define BEAMENGINE_H

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <fxcorrcommon/architecture.h>	// cf32, f32, s32, u32
#include <fxcorrcommon/configuration.h>

#include "spreader.h"

/**
 * @class BeamEngine
 * @brief Phased array beam forming (P8): frequency-domain weighted sum of
 *        all stations' spectra into one beam spectrum per (freq, pol),
 *        written to beam/<batch_id>/beam.bin (layout: algo-plan.md P8 and
 *        fxcorr/data-spec.md).
 *
 * Port of core.cpp:818-865 (the phased array branch of processdata) with
 * the f-side Mode objects replaced by .sp spectra read through the SpReader
 * views built in main.cpp (recorded + zoom).  Upstream output (padomain /
 * paoutputformat / DIFX / VDIF / TIMESERIES) has no consumer and is not
 * ported; the beam file format is defined by fxcorr (see data-spec.md).
 *
 * Semantics kept from upstream:
 * - per (freq, papol): sum over datastreams of DWeight[freq][ds] *
 *   spectrum, accumulate (no normalisation), no valid-flag check (invalid
 *   FFT blocks arrive as all-zero spectra from fxcorr-f);
 * - one record per ACC TIME window; the window is an integer number of
 *   fftloop batches (Configuration guarantees accffts % numbufferedffts
 *   == 0 and subintns % paaccumulationns == 0);
 * - channel count is getFNumChannels(f) - note upstream core.cpp:821
 *   passes the config index instead of the freq index (only correct for
 *   single-freq configs where both are 0); fxcorr uses the correct one.
 */
class BeamEngine {
public:
	/**
	 * @param config     parsed .input
	 * @param configindex scan's config index
	 * @param workdir    workdir root; beam file goes to workdir/beam/<batchid>/beam.bin
	 * @param batchid    batch id (output directory name)
	 * @param nsubints   subints of this batch (written to the header)
	 * @param readers    per (datastream, total band) SpReader views, already
	 *                   opened and header-validated by main.cpp
	 */
	BeamEngine(Configuration *config, int configindex, const std::string &workdir,
		   const std::string &batchid, int nsubints,
		   const std::vector<std::vector<SpReader *> > &readers);
	~BeamEngine();

	/**
	 * Beam-form one subint: per ACC TIME window accumulate the weighted
	 * spectra of all stations and append one record to beam.bin.
	 * scan/sec/ns is the subint start in the scan-relative time base
	 * (same values as the .sp record of this subint); window timestamps
	 * are derived from it.
	 */
	void processSubint(int s, int scan, int sec, int ns);

	/** True if the output file was created and the header written. */
	bool ok() const { return ok_; }

private:
	// one (freq, pol) output segment
	struct BeamSeg {
		int freqindex;
		char pol;
		int nchan;				// getFNumChannels(freqindex)
		std::vector<std::pair<int, int> > sources;	// (datastream, total band) contributors
	};

	Configuration *config;
	int configindex;
	int freqtablelength;
	const std::vector<std::vector<SpReader *> > &readers;
	std::vector<BeamSeg> segs;		// output order: per freq, per papol
	long long outlength;			// total cf32 per acc record
	cf32 *accum;				// outlength cf32 accumulation buffer
	FILE *file;
	bool ok_;

	int numaccs;				// subintns / paaccumulationns
	int accfftloops;			// FFT loops (numbufferedffts batches) per acc window
	int numbufferedffts;
	int blockspersend;
};

#endif
