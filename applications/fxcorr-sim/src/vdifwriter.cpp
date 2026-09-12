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

	unsigned int header[8];
	header[0] = (unsigned int)(sec & 0x3FFFFFFF) | (1u << 30);
	header[1] = (unsigned int)((sec >> 30) & 0x3FFFFFFF);
	header[2] = (32u << 24) | (unsigned int)(frame & 0xFFFFFF);
	header[3] = (1u << 30) | ((unsigned int)log2nchan << 24) | (unsigned int)framelength8;
	header[4] = (2u << 26) | (0u << 24);
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
