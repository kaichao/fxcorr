#ifndef DATAREADER_H
#define DATAREADER_H

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "corrections.h"

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/model.h>
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/datamuxer.h>
#include <mark5access/mark5bfix.h>
#include <mark5access/mark5bfile.h>
#include <mark5access/mark5_stream.h>

/**
 * @class DataReader
 * @brief Sequential, MPI-free reader of local raw files for one datastream.
 *
 * V1 (see fxcorr/v1-plan.md 2.2): local VDIF files as produced by datasim;
 * single scan per file, single mux thread.  Coarse delay and frame-aligned
 * byte offsets are ported from DataStream::calculateControlParams
 * (datastream.cpp:381-394, 516-573) and VDIFDataStream::calculateControlParams
 * (vdiffile.cpp:417-444); unpacking itself stays in Mk5Mode::unpack.
 *
 * P10 (algo-plan.md): format dispatch widened to the full file-based set --
 * multi-thread VDIF corner-turn (INTERLACEDVDIF, VDIFMuxer), Mark5B
 * (summarizemark5bfile + mark5bfix, upstream Mark5BDataStream path), the
 * mark5access generic stream set (MKIV/VLBA/VLBN/KVN5B/CODIF, upstream
 * Mk5DataStream path) and the LBA family (ASCII header + raw payload,
 * upstream base-DataStream path).  K5VSSP/K5VSSP32 stay rejected (not
 * implemented upstream either: mark5access K5 is "Not Yet Implemented" and
 * genMk5FormatName has no K5 branch).
 */
class DataReader {
public:
	// batchstartsec/batchstartns: absolute (seconds, ns) of the batch start --
	// the raw file holds this batch's data starting at that time (data-spec 5.2
	// file-per-batch layout), so all byte offsets are relative to it
	DataReader(Configuration *config, int configindex, int dsindex, Model *model,
	           long long batchstartsec, int batchstartns);
	~DataReader();

	inline int getBlocksPerSend() const { return blockspersend; }
	inline int getSendBytes() const { return sendbytes; }

	/**
	 * Reads the bytes for one subint, starting at the frame-aligned,
	 * delay-corrected position.  Returns bytes read; 0 means the subint is
	 * invalid (*sec is set to Mode::INVALID_SUBINT in that case).
	 * *sec/*ns receive the (absolute seconds, ns) of the data block start,
	 * in the same convention DataStream fills its controlbuffer (datastream.cpp:515).
	 * For corner-turned (muxed) VDIF the returned bytes are the multiplexed
	 * output (upstream: bufferinfo[].validbytes = datamuxer->multiplex()).
	 */
	int readSubint(int scan, int offsetsec, int offsetns, u8 *buffer, int bufsize, int *sec, int *ns);

	/** Fills per-FFT-block valid flags (one bit each) from the bytes actually read. */
	void fillValidFlags(s32 *flags, int validbytes) const;

	/** File offset of the last readSubint call, relative to the batch start
	 * (the frame-aligned, delay-corrected position). */
	long long getLastFileOffset() const { return lastfileoffset; }

	/** Whether the last readSubint handed over a contiguous run of file bytes.
	 * False once the window had to bridge a recording interruption: the buffer
	 * is then a frame grid on the time axis -- filler dropped, gap slots left
	 * empty -- and not something a byte-wise consumer can walk (B2,
	 * reader-model.md 4.8). */
	inline bool lastReadContiguous() const { return lastcontiguous; }

private:
	// reader kind, decided by the datastream format (algo-plan.md P10)
	enum readerkind { KIND_VDIF, KIND_MUXEDVDIF, KIND_MARK5B, KIND_MK5STREAM, KIND_LBA };

	// computes frame-aligned file offset and data start time for a subint
	bool locate(int scan, int offsetsec, int offsetns, int *sec, int *ns, long long *fileoffset);
	void openFile(int fileindex);

	// P12: frame-number continuity check on each freshly read buffer, in two
	// halves.  checkFrameContinuity scans the frames *inside* this buffer only
	// -- a step across a buffer boundary is normal alignment drift between the
	// subint length and whole frames, not a gap (synthetic data without any
	// gaps shows exactly that at the boundaries) -- and accumulates what the
	// read position has to be corrected by: the gaps themselves (the ledger's
	// gapShift) and the filler frames (its fillerFrames);
	// the frame-number jump across a run of filler frames is
	// what the real gaps look like once the filler is skipped.  shiftFrameGaps
	// then rebuilds the buffer's frame grid, so that buffer position i really
	// is the i-th frame of the subint: frames behind a gap move forward, filler
	// frames are dropped, and the slots left without data are recorded in
	// gapinvalid (post-shift frame indices) for fillValidFlags.  A buffer with
	// neither a gap nor filler is passed through untouched.
	//
	// VDIF only: the frame number lives in word 1 of the VDIF header in
	// vdifio's word layout; muxed VDIF output frames are synthesised by the
	// corner-turner and the other kinds have no such field.
	//
	// B2 (reader-model.md 4.8): the window is filled by time slots, not by input
	// bytes, so the bytes being *scanned* and the slots being *filled* are two
	// different numbers.  src/srcbytes is the stretch of file actually needed
	// for this subint -- the caller trims it to what fills `slots` slots, so a
	// filler run's tail does not count into this subint's books -- and dst is
	// the subint's frame grid, `slots` frames long.  Returns the number of slots
	// placed (the frame ranges that end up without data are in gapinvalid).
	int checkFrameContinuity(const u8 *src, int srcbytes, long long readoffset, u8 *dst, int slots);

	// Places each source frame where its frame number says it belongs: filler is
	// dropped (it has no time slot) and a jump in the frame number advances the
	// destination by the jump, leaving the slots in between as holes.  Returns
	// the number of slots placed.  With dryrun the walk happens without writing
	// dst or recording holes -- that is how the caller finds out how many input
	// frames this subint actually needs -- and *usedframesp receives the frames
	// consumed getting that far.  src and dst may be the same buffer (the
	// ordinary case), which is why the rebuild goes through gapbuffer.
	int shiftFrameGaps(const u8 *src, int srcframes, u8 *dst, int slots,
	                   int *usedframesp = 0, bool dryrun = false);
	// The reader's one stretch scan: everything between the last scan's
	// watermark and `upto` is read in blocks, its chain walked by layer 1 and
	// what it holds counted into the ledger (C3: the whole of it lives here --
	// how far the last scan got, reading the bytes, and the books).  Called
	// before a read and again -- as a no-op -- after it, so a read position
	// derived from corrections that the stretch itself produced is never used.
	// Returns the filler frames found.
	long long scanStretch(long long upto, long long *gapsp);

	// The read position the corrections make of locate()'s raw value (C3): pure
	// arithmetic on the ledger -- filler counted so far pushes it forward (bytes
	// but no slot), the gaps lying before this subint pull it back, and a query
	// landing inside a gap is moved to the far side of it.  What the stretch
	// behind the position holds is scanStretch's business, not this one's;
	// lastgapframes (reported by READPOS) is what the gaps contributed.
	long long correctedPosition(long long uncorrected);

	// file offset a subint is read from: correctedPosition() settled against the
	// stretches the read position walked over (see the definition).  Returns
	// false when the subint lies entirely before the file's first frame.
	bool settleReadPosition(long long uncorrected, int locatecount, long long *fileoffset);

	// Reads one subint's window from `fileoffset` and rebuilds its frame grid
	// (C3: the read half of readSubint).  Returns the bytes to hand over -- for
	// VDIF that is the slot grid, holes included -- or -1 when the position is
	// past the end of the file.  *contiguousp says whether what came back is a
	// run of file bytes at all (see the definition).
	int readWindow(long long fileoffset, int headskip, u8 *buffer, bool *contiguousp);

	// P12 diagnostics: the correction the last subint was read with, and the
	// slot shiftFrameGaps started it at (both reported by READPOS)
	long long lastgapframes;
	int lastshiftdst;
	long long lastuncorrected;	// position before the gap/filler corrections
	int lastsettlepasses;		// passes the pre-read stretch scan needed

	Configuration *config;
	Model *model;
	int configindex;
	int dsindex;
	readerkind kind;

	// per-datastream frame parameters; for muxed VDIF these are the
	// multiplexed values (vdiffile.cpp:396-401)
	int framebytes;
	int payloadbytes;
	double framespersecond;
	int sendbytes;
	int blockspersend;
	long long intclockseconds;
	int nummuxthreads;

	// per-sample timing and byte-grid parameters, common to all kinds
	// (datastream.cpp:750-761); bufferindex arithmetic and the P11 delay
	// realignment (algo-plan.md P11) work on these
	int bytespersamplenum;
	int bytespersampledenom;
	double sampletimens;		// ns per sample
	int blockbytes;			// bytes per FFT block
	int bytesbetweenintegerns;	// bytes per integer-ns boundary
	long long nsinc;		// upstream segment span in ns (early-bail bound)
	int lastcount;			// FFT blocks skipped by the last locate
					// realignment; fillValidFlags forces these invalid

	// file state
	int numfiles;
	std::string *datafilenames;
	int currentfile;
	std::ifstream input;
	long long currentscanstartsec;	// absolute second of the scan this file holds
	int currentscan;
	long long batchstartabsns;	// absolute ns of the batch start (file origin)
	long long lastfileoffset;	// file offset of the last readSubint (batch-relative)
	long long anchorbytes;		// bytes from the file's first frame/payload to the
					// batch start (upstream initialiseFile dataoffset)

	// muxed VDIF (KIND_MUXEDVDIF)
	DataMuxer *muxer;
	int inputframebytes;	// single-thread frame size: input file offsets and demux
				// reads use this, muxed output uses framebytes

	// Mark5B (KIND_MARK5B)
	struct mark5b_fix_statistics m5bstats;
	unsigned char *readbuffer;	// mark5bfix source buffer (sendbytes + 2 frames)
	int readbuffersize;

	// LBA family (KIND_LBA)
	long long headerbytes;		// bytes of ASCII header in front of the payload
	double bytesperns;		// payload byte rate (upstream base DataStream
					// bufferindex conversion; common to all kinds)

	// P12 step 1 diagnostics (see checkFrameContinuity); lifetime totals,
	// reported once in the destructor
	long long gapchecksubints;	// buffers scanned
	long long gapcheckframes;	// frames scanned
	long long gapcheckgaps;		// frame-number discontinuities inside a buffer
	long long gapcheckmissing;	// frames missing across those discontinuities
	long long gapcheckfillers;	// filler (header-less zero) frames passed over
	long long gapchecklastfr;	// last frame number seen in the previous buffer
	long long gapcheckcrossmin;	// smallest frame-number step across a buffer boundary
	long long gapcheckcrossmax;	// largest step across a buffer boundary
	long long gapcheckcrosscount;	// buffer boundaries seen
	long long gapcheckcrossprinted;	// anomalous boundaries reported so far
	long long gapchecklastoff;	// file offset the previous buffer was read from
	bool gapchecklastvalid;		// whether gapchecklastfr is set

	// P12 step 2a, restructured by C2: locate() maps time onto file bytes
	// linearly, which only holds while the file's frame sequence is one frame
	// per time slot.  The two kinds of damage that break it -- a gap making the
	// file *shorter* than the time axis, filler making it *longer* -- and the
	// books kept about them live in corrections.h, where they are recorded
	// against the time axis and queried by it (gapShift / fillerFrames;
	// readSubint applies them to the position the linear map produced).
	//
	// What stayed here is the scan's own progress: the frame-number chain and
	// the seam it left at the end of the last skipped-stretch scan.  Those are
	// questions about how far the scanning has got, not corrections, and the
	// I/O layer (C3) is where they belong.
	corrections::Ledger ledger;
	// the last stretch scan ended on a data frame, i.e. the frame-number chain
	// runs unbroken from the stretch into the buffer just read (see
	// scanStretch); consumed by checkFrameContinuity to seed its chain
	bool gapstretchseam;
	long long lastframesin;		// the frame index locate() mapped the last
					// subint onto: file position and time-axis slot
					// coincide only until a gap turns up
	int lastframens;		// in-second frame number of that slot, which
					// is what the buffer's first frame has to be
					// measured against (shiftFrameGaps)

	// P12 step 2b: holes left in the current buffer by shiftFrameGaps, as
	// post-shift frame ranges for fillValidFlags
	std::vector<std::pair<int,int> > gapinvalid;
	u8 *gapbuffer;			// scratch for the shifted buffer (sendbytes)

	// B2 (reader-model.md 4.8): input buffer for reads that have to bridge a
	// filler run.  The first attempt reads straight into the output buffer --
	// the no-interruption case, byte for byte what it always was -- and only
	// when that stretch cannot fill the subint's slots does the read move here
	// and grow, since how many bytes are needed is not known until the run has
	// been walked.
	u8 *inbuf;
	int inbufsize;
	// whether the last read was the whole of what the file holds there.  False
	// once filler bytes were dropped or gap slots left without data: the buffer
	// is then a frame grid on the time axis, not a run of file bytes, which is
	// what the switched-power feed assumes (P6).
	bool lastcontiguous;
};

#endif
