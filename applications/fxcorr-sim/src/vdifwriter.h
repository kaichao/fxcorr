#ifndef FXSIM_VDIFWRITER_H
#define FXSIM_VDIFWRITER_H

#include <cstdio>
#include <string>
#include <vector>

// Sequential VDIF frame writer.  Frame headers follow gen_test_vdif.py word
// for word (epoch 32, VDIF version 1, 2-bit real, thread 0, station 0); the
// only extension is the nchan field (log2 of the band count) in word3.
//
// Timestamps and frame numbers are derived from the batch start and increment
// monotonically: sec = startsec + (framestart + n) * nsampframe / rate,
// frame = (framestart + n) % (rate / nsampframe).  Frame numbers wrap at each
// whole second, so framestart is the in-second frame offset of the batch
// start.  The constructor rejects band counts that are not powers of two
// (VDIF nchan field) and batches whose duration is not an integer number of
// frames (the global-continuity check for distributed generation: a split
// point that breaks frame numbering would corrupt the VDIF stream).
class VDIFWriter
{
public:
	// outpath: full output file path
	// startsec: whole seconds since the VDIF epoch (2000.0) at the batch start
	// framestart: in-second frame offset of the batch start (0..fps-1)
	// ratehz: per-band sample rate in Hz (all bands share it)
	// nbands: number of bands per frame (1, 2, 4, 8, 16 or 32)
	// bytesperbandframe: payload bytes per band per frame (8000 for 2-bit)
	VDIFWriter(const std::string &outpath, long long startsec, long long framestart,
	           long long ratehz, int nbands, int bytesperbandframe);
	~VDIFWriter();

	bool isOpen() const { return f != 0; }

	// Write one frame (header + payload).  payloadbytes must equal
	// bytesperbandframe * nbands.
	bool writeFrame(const unsigned char *payload, int payloadbytes);

	long long getFrameCount() const { return nframes; }

	// Recording interruption (t25362 regression).  At `atsec` seconds into the
	// batch, `missingframes` frame numbers are lost.  fillerframes=0 writes
	// nothing for them, so the file comes out shorter than the time axis (the
	// "plain missing frame" form fxcorr-f's gap correction undoes).
	// fillerframes>0 instead writes that many all-zero-header frames in their
	// place: bytes but no time slot, the form the recorder left in t25362's BA
	// ds_2 and the one fillershiftbytes undoes.
	//
	// The two counts are independent, and t25362 needs them to be: its BA ds_2
	// runs hold 81..508 filler frames while only 10..63 frame numbers are lost
	// (the recorder wrote zeros for the whole outage but its frame counter only
	// advanced by the data that would have been produced).  Making the filler
	// run as long as the gap -- the only form FXSIM_GAPS could express before
	// 2026-09-18 -- cannot reproduce what the datareader gets wrong there.
	//
	// Call before the first writeFrame; positions are taken in increasing
	// order (they are sorted on insertion).
	void addGap(double atsec, long long missingframes, long long fillerframes);

	// The recorder started `frames` frames after the batch start, so the file
	// begins that far into the batch: its first frame carries the batch start's
	// time plus `frames`, and the file is that much shorter than the batch.  It
	// is the third form real observations show (t25362's BA starts 79.3 ms, or
	// 1269 frames, late) and the one fxcorr-f's anchorbytes -- "bytes from the
	// file's first frame back to the batch start", negative here -- undoes.
	// Without it the A-class defects (reader-model.md 4.1) have no synthetic
	// asset to regress against.  Call before the first writeFrame.
	void setFileStartOffset(long long frames) { filestartoffset = frames; }

private:
	struct Gap
	{
		long long atframe;	// first frame number lost, relative to the batch start
		long long missing;	// how many frame numbers
		long long fillerframes;	// all-zero-header frames occupying the file (0 = none)
	};
	std::vector<Gap> gaps;
	size_t gapnext;

	FILE *f;
	long long startsec;
	long long framestart;
	long long ratehz;
	int nsampframe;          // samples per band per frame
	int framespersecond;
	int log2nchan;
	int framelength8;
	long long nframes;
	long long filestartoffset;	// frames the file starts after the batch start
};

#endif
