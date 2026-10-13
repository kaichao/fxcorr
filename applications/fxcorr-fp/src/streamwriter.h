#ifndef STREAMWRITER_H
#define STREAMWRITER_H

#include <cstdio>
#include <string>

#include <fxcorrcommon/architecture.h>	// u8 / s32 / u32

/**
 * @class StreamWriter
 * @brief Writes fp's intermediate data stream (stream.bin) for one datastream
 *        of a batch -- the v9 chain's replacement for the fp32 spectra the old
 *        chain wrote (fxcorr/v9-plan.md section 4).
 *
 * The content is the per-subint product of DataReader::readSubint: the
 * time-slot-ordered data bytes (filler dropped, gap slots left empty), the
 * valid flags and both time stamps (the reader's sec/ns and the loop's
 * offsets).  One fixed-length record per subint in subint order; fxcorr-xp
 * reads them back in sequence and replays setValidFlags/setData/setOffsets
 * with no re-alignment of its own.
 *
 * One instance per datastream: the file is opened and its header written on
 * construction, and every subint is appended by writeSubint().
 */
class StreamWriter {
public:
	/**
	 * @param outdir  fengine/<batch_id>/<g>/<station>/ds_<N>/ (must exist;
	 *                same layout as before, data-spec 5.3)
	 * @param batchid  batch id, recorded in the header (xp cross-checks it)
	 * @param station  station name, recorded in the header
	 * @param dsindex  station-local datastream index (the ds_<N> number)
	 * @param subintns  subint length in ns (header; xp cross-checks it)
	 * @param nsubints  total subints expected in the batch (header)
	 * @param sendbytes  bytes per subint record data area (header)
	 * @param blockspersend  FFT blocks per subint (sizes the flags area)
	 * @param numbufferedffts  FFTs buffered per fftloop (header, informational)
	 * @param framebytes, framespersec  frame geometry (header, diagnostics)
	 */
	StreamWriter(const std::string &outdir, const std::string &batchid,
	             const std::string &station, int dsindex, int subintns,
	             int nsubints, int sendbytes, int blockspersend,
	             int numbufferedffts, int framebytes, int framespersec);
	~StreamWriter();

	/**
	 * Appends one subint record.  validbytes is DataReader::readSubint's
	 * return value verbatim (0 = invalid subint, written with status 1);
	 * sec/ns are the reader's outputs and offsetsec/offsetns the loop's, both
	 * stored exactly as issued to Mode::setData/setOffsets.  validflags is
	 * flagwords s32 words, laid out as fillValidFlags() produced them.  The
	 * data area is padded with zeros past validbytes so every record keeps
	 * one length.
	 */
	void writeSubint(int validbytes, int scan, int sec, int ns,
	                 int offsetsec, int offsetns, const s32 *validflags,
	                 const u8 *data);

	/**
	 * Flushes and closes the file, verifying that the writes actually
	 * landed.  Returns false if they did not, in which case the stream is
	 * incomplete and the caller must exit non-zero -- a full filesystem has
	 * to fail here, while this process still knows which file it was writing
	 * (v7-plan 17.2).
	 */
	bool finalise();

private:
	std::string path;
	FILE *file;
	int flagwords;
	int sendbytes;
	bool finalised;
};

#endif
