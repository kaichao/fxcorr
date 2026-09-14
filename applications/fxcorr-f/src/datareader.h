#ifndef DATAREADER_H
#define DATAREADER_H

#include <fstream>
#include <string>

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
 * V1 (see fxcorr/impl-plan.md 2.2): local VDIF files as produced by datasim;
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

private:
	// reader kind, decided by the datastream format (algo-plan.md P10)
	enum readerkind { KIND_VDIF, KIND_MUXEDVDIF, KIND_MARK5B, KIND_MK5STREAM, KIND_LBA };

	// computes frame-aligned file offset and data start time for a subint
	bool locate(int scan, int offsetsec, int offsetns, int *sec, int *ns, long long *fileoffset);
	void openFile(int fileindex);

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
};

#endif
