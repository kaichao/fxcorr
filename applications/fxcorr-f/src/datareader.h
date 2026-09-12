#ifndef DATAREADER_H
#define DATAREADER_H

#include <fstream>
#include <string>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/model.h>
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/architecture.h>

/**
 * @class DataReader
 * @brief Sequential, MPI-free reader of local raw files for one datastream.
 *
 * V1 scope (see fxcorr/impl-plan.md 2.2): local VDIF files as produced by
 * datasim; single scan per file, single mux thread, no fanout.  Coarse delay
 * and frame-aligned byte offsets are ported from DataStream::calculateControlParams
 * (datastream.cpp:381-394, 516-573) and VDIFDataStream::calculateControlParams
 * (vdiffile.cpp:417-444); unpacking itself stays in Mk5Mode::unpack.
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
	 */
	int readSubint(int scan, int offsetsec, int offsetns, u8 *buffer, int bufsize, int *sec, int *ns);

	/** Fills per-FFT-block valid flags (one bit each) from the bytes actually read. */
	void fillValidFlags(s32 *flags, int validbytes) const;

private:
	// computes frame-aligned file offset and data start time for a subint
	bool locate(int scan, int offsetsec, int offsetns, int *sec, int *ns, long long *fileoffset);
	void openFile(int fileindex);

	Configuration *config;
	Model *model;
	int configindex;
	int dsindex;

	// per-datastream frame parameters (V1: VDIF, one mux thread)
	int framebytes;
	int payloadbytes;
	double framespersecond;
	int sendbytes;
	int blockspersend;
	long long intclockseconds;

	// file state
	int numfiles;
	std::string *datafilenames;
	int currentfile;
	std::ifstream input;
	long long currentscanstartsec;	// absolute second of the scan this file holds
	int currentscan;
	long long batchstartabsns;	// absolute ns of the batch start (file origin)
};

#endif
