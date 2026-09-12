#ifndef FXSIM_VDIFWRITER_H
#define FXSIM_VDIFWRITER_H

#include <cstdio>
#include <string>

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

private:
	FILE *f;
	long long startsec;
	long long framestart;
	long long ratehz;
	int nsampframe;          // samples per band per frame
	int framespersecond;
	int log2nchan;
	int framelength8;
	long long nframes;
};

#endif
