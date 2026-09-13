#include "vdifwriter.h"

#include <cstring>
#include <iostream>

using namespace std;

VDIFWriter::VDIFWriter(const string &outpath, long long startsec_, long long framestart_,
                       long long ratehz_, int nbands, int bytesperbandframe)
	: f(0), startsec(startsec_), framestart(framestart_), ratehz(ratehz_),
	  nsampframe(bytesperbandframe * 4), log2nchan(0), framelength8(0), nframes(0)
{
	// VDIF word3 nchan field is log2 of the channel (band) count
	if(nbands <= 0 || (nbands & (nbands - 1)) != 0 || nbands > 32)
	{
		cerr << "fxcorr-sim: band count must be a power of two <= 32 (got " << nbands << ")" << endl;
		return;
	}
	for(int t = nbands; t > 1; t >>= 1)
		log2nchan++;

	if(ratehz <= 0 || ratehz % nsampframe != 0)
	{
		cerr << "fxcorr-sim: sample rate " << ratehz << " Hz is not an integer multiple of "
		     << nsampframe << " samples per frame" << endl;
		return;
	}
	framespersecond = (int)(ratehz / nsampframe);
	framelength8 = (32 + bytesperbandframe * nbands) / 8;

	f = fopen(outpath.c_str(), "wb");
	if(!f)
		cerr << "fxcorr-sim: cannot create " << outpath << endl;
}

VDIFWriter::~VDIFWriter()
{
	if(f)
		fclose(f);
}

bool VDIFWriter::writeFrame(const unsigned char *payload, int payloadbytes)
{
	if(!f)
		return false;
	if(payloadbytes != framelength8 * 8 - 32)
	{
		cerr << "fxcorr-sim: payload size mismatch" << endl;
		return false;
	}

	// same header words as gen_test_vdif.py; frame number wraps at the
	// frames-per-second boundary, seconds advance every framespersecond frames
	long long sec = startsec + (framestart + nframes) * (long long)nsampframe / ratehz;
	int frame = (int)((framestart + nframes) % framespersecond);

	// header layout follows vdifio/mark5access (not the VDIF 2010 spec):
	// word0 [29:0] seconds (bit30 legacymode=0 -> 32-byte header, bit31 invalid=0),
	// word1 [29:24] ref epoch + [23:0] frame number, word2 [31:29] version + [23:0]
	// frame length in 8-byte units, word3 station/thread/nbits/iscomplex.
	// mark5access parses this layout (switched power path); the old "spec" layout
	// put the length in word3, which mark5access read as frame number * 8 and
	// blanked every frame after the first -- see fxcorr/test/tcal/README.md.
	unsigned int header[8];
	header[0] = (unsigned int)(sec & 0x3FFFFFFF);
	// epoch 0 = 2000.0, matching the word0 seconds counted from 2000.0
	// (mark5access mjdepochs[0] = 51544); see fxcorr/test/tcal/README.md
	header[1] = (0u << 24) | (unsigned int)(frame & 0xFFFFFF);
	header[2] = (1u << 29) | (unsigned int)framelength8;
	header[3] = (0u << 16) | (0u << 6) | (2u << 1) | 0u;
	header[4] = 0;
	header[5] = 0;
	header[6] = 0;
	header[7] = 0;

	if(fwrite(header, sizeof(header), 1, f) != 1)
		return false;
	if(fwrite(payload, 1, (size_t)payloadbytes, f) != (size_t)payloadbytes)
		return false;

	nframes++;
	return true;
}
