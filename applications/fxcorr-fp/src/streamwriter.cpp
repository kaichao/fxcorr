#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT

#include "streamwriter.h"

using namespace std;

// stream.bin layout (fxcorr/v9-plan.md section 4).  Both structures are
// written in host byte order -- the whole fxcorr chain runs on little-endian
// x86-64 / aarch64, the same assumption data-spec 5.3 makes for the .sp
// files.  The static asserts pin the on-disk sizes against padding changes.
struct StreamHeader {
	char magic[8];		// "FXCORR9S"
	u32 version;		// 1
	char batchid[32];
	char station[16];
	u32 dsindex;		// station-local datastream index (ds_<N>)
	u32 subintns;
	u32 blockspersend;
	u32 numbufferedffts;
	u32 nsubints;
	u32 sendbytes;		// bytes per subint record data area
	u32 framebytes;
	u32 framespersec;
};
static_assert(sizeof(StreamHeader) == 92, "stream.bin header must stay 92 bytes");

struct StreamRecordHead {
	u32 status;		// 0 = ok, 1 = invalid subint (validbytes == 0)
	u32 validbytes;		// DataReader::readSubint's return value
	u32 scan;
	s32 sec;		// reader's data seconds (Mode::INVALID_SUBINT if status 1)
	u32 ns;
	u32 offsetsec;		// the offsets issued to Mode::setOffsets
	u32 offsetns;
};
static_assert(sizeof(StreamRecordHead) == 28, "stream.bin record head must stay 28 bytes");

StreamWriter::StreamWriter(const string &outdir, const string &batchid,
                           const string &station, int dsindex, int subintns,
                           int nsubints, int sendbytes, int blockspersend,
                           int numbufferedffts, int framebytes, int framespersec)
	: path(outdir + "/stream.bin"),
	  file(0),
	  flagwords((blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT),
	  sendbytes(sendbytes),
	  finalised(false)
{
	file = fopen(path.c_str(), "wb");
	if(file == 0)
	{
		cerr << "StreamWriter: cannot open " << path << " for writing" << endl;
		exit(EXIT_FAILURE);
	}

	StreamHeader h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "FXCORR9S", 8);
	h.version = 1;
	snprintf(h.batchid, sizeof(h.batchid), "%s", batchid.c_str());
	snprintf(h.station, sizeof(h.station), "%s", station.c_str());
	h.dsindex = (u32)dsindex;
	h.subintns = (u32)subintns;
	h.blockspersend = (u32)blockspersend;
	h.numbufferedffts = (u32)numbufferedffts;
	h.nsubints = (u32)nsubints;
	h.sendbytes = (u32)sendbytes;
	h.framebytes = (u32)framebytes;
	h.framespersec = (u32)framespersec;
	if(fwrite(&h, sizeof(h), 1, file) != 1)
	{
		cerr << "StreamWriter: error writing header of " << path << endl;
		exit(EXIT_FAILURE);
	}
}

StreamWriter::~StreamWriter()
{
	// silent: finalise() is the one that must notice a failed write, the
	// destructor has no way to report (v7-plan 17.2)
	if(file != 0)
		fclose(file);
}

void StreamWriter::writeSubint(int validbytes, int scan, int sec, int ns,
                               int offsetsec, int offsetns, const s32 *validflags,
                               const u8 *data)
{
	if(finalised || file == 0)
		return;
	if(validbytes < 0 || validbytes > sendbytes)
	{
		cerr << "StreamWriter: invalid byte count " << validbytes
		     << " (sendbytes " << sendbytes << ")" << endl;
		exit(EXIT_FAILURE);
	}

	// status mirrors the reader's contract: zero bytes read means the subint
	// is invalid (DataReader::readSubint).  Everything else is stored
	// verbatim, so xp issues exactly the calls fxcorr-f issued.
	StreamRecordHead rh;
	rh.status = (validbytes == 0) ? 1 : 0;
	rh.validbytes = (u32)validbytes;
	rh.scan = (u32)scan;
	rh.sec = (s32)sec;
	rh.ns = (u32)ns;
	rh.offsetsec = (u32)offsetsec;
	rh.offsetns = (u32)offsetns;

	fwrite(&rh, sizeof(rh), 1, file);
	fwrite(validflags, sizeof(s32), flagwords, file);
	if(validbytes > 0)
		fwrite(data, 1, validbytes, file);
	// pad the record tail with zeros: the bytes past validbytes carry no
	// meaning (xp replays setData with validbytes), the padding just keeps
	// the record length fixed
	size_t remain = (size_t)(sendbytes - validbytes);
	static u8 zeros[65536] = {0};
	while(remain > 0)
	{
		size_t take = (remain < sizeof(zeros)) ? remain : sizeof(zeros);
		fwrite(zeros, 1, take, file);
		remain -= take;
	}
}

bool StreamWriter::finalise()
{
	if(finalised)
		return true;
	finalised = true;

	bool ok = true;
	if(file != 0)
	{
		if(fflush(file) != 0 || ferror(file))
		{
			cerr << "StreamWriter: write to " << path << " failed: " << strerror(errno) << endl;
			ok = false;
		}
		if(fclose(file) != 0)
		{
			cerr << "StreamWriter: close of " << path << " failed: " << strerror(errno) << endl;
			ok = false;
		}
		file = 0;
	}
	if(!ok)
		cerr << "StreamWriter: " << path << " is INCOMPLETE (filesystem full?)" << endl;
	return ok;
}
