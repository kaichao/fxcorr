#ifndef SPREADER_H
#define SPREADER_H

#include <cstdio>
#include <string>

#include <fxcorrcommon/architecture.h>	// cf32, f32, s32, u32

/**
 * @class SpReader
 * @brief Reader for one band_XX.sp file produced by fxcorr-f
 *        (layout: fxcorr/data-spec.md 5.3).
 *
 * One instance per (datastream, recorded band).  Subints are read on demand
 * into internal buffers; spectra are in linear FFT order (the fftloop-batched
 * write order of FEngineWriter concatenates to global FFT index order).
 */
class SpReader {
public:
	SpReader(const std::string &path);
	~SpReader();

	/** True if the header was read and validated. */
	bool ok() const { return ok_; }

	// header fields
	int numChannels() const { return nchan_; }
	u32 numSubints() const { return nsub_; }
	u32 subintNS() const { return subns_; }
	u32 blocksPerSend() const { return bps_; }
	u32 numBufferedFFTs() const { return nbf_; }
	u32 flagWords() const { return flagwords_; }
	int bandIndex() const { return bandindex_; }
	char pol() const { return pol_; }

	/**
	 * Reads subint s into the internal buffers.
	 * @return false on EOF / short read (i.e. beyond the data written).
	 */
	bool readSubint(int s, int &scan, int &sec, int &ns);

	/** Valid flags of the last-read subint (flagwords s32 words). */
	const s32 *flags() const { return flagsbuf_; }

	/** Per-FFT-block data weights of the last-read subint (blockspersend f32). */
	const f32 *weights() const { return wbuf_; }

	/** Spectra of the last-read subint, linear FFT order (blockspersend*numChannels cf32). */
	const cf32 *spectra() const { return specbuf_; }

private:
	FILE *file_;
	bool ok_;

	int bandindex_;
	char pol_;
	int nchan_;
	u32 nsub_, subns_, bps_, nbf_, flagwords_;
	long long subintbytes_;	// bytes of one subint record (header section + spectra)

	s32 scan_, sec_, ns_;
	s32 *flagsbuf_;
	f32 *wbuf_;
	cf32 *specbuf_;
};

#endif
