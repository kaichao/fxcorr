#include "vdifwriter.h"

#include <cstring>
#include <iostream>

using namespace std;

VDIFWriter::VDIFWriter(const string &outpath, long long startsec_, long long framestart_,
                       long long ratehz_, int nbands, int bytesperbandframe)
	: gapnext(0), f(0), startsec(startsec_), framestart(framestart_), ratehz(ratehz_),
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

void VDIFWriter::addGap(double atsec, long long missingframes, bool filler)
{
	Gap g;
	g.atframe = (long long)(atsec * framespersecond + 0.5);
	g.missing = missingframes;
	g.filler = filler;
	// keep the list ordered: writeFrame walks it once, front to back
	size_t at = gaps.size();
	while(at > 0 && gaps[at-1].atframe > g.atframe)
		at--;
	gaps.insert(gaps.begin() + at, g);
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

	// Recording interruptions, applied before the frame whose number they
	// precede.  A plain gap skips frame numbers without writing anything, so
	// the file ends up shorter than the time axis -- the form fxcorr-f's
	// gapshiftbytes undoes.  A filler run instead writes all-zero-header frames
	// in their place: bytes in the file but no slot on the time axis, the form
	// fillershiftbytes undoes (data-spec 5.2).  Either way the frame number
	// advances by what was really lost, which is what keeps a filled datastream
	// and a merely short one describing the same stretch of time.
	while(gapnext < gaps.size() && nframes >= gaps[gapnext].atframe)
	{
		const Gap &g = gaps[gapnext];
		if(g.filler)
		{
			int pbytes = framelength8*8 - 32;
			unsigned int zh[8] = {0, 0, 0, 0, 0, 0, 0, 0};
			unsigned char *zbuf = new unsigned char[(size_t)(pbytes > 0 ? pbytes : 1)];
			memset(zbuf, 0, (size_t)(pbytes > 0 ? pbytes : 1));
			bool ok = true;
			for(long long k=0; k<g.missing && ok; k++)
				ok = fwrite(zh, sizeof(zh), 1, f) == 1 &&
				     (pbytes <= 0 || fwrite(zbuf, 1, (size_t)pbytes, f) == (size_t)pbytes);
			delete [] zbuf;
			if(!ok)
				return false;
		}
		// Both forms lose these frame numbers: the interruption is real time
		// that no data covers, so the frame number either side of it must jump
		// by exactly what was lost (data-spec 5.2: "the frame-number jump
		// reflects only what was really lost, regardless of how many filler
		// frames the run holds").  The filler form additionally leaves that
		// many frames' worth of BYTES in the file -- bytes but no time slot,
		// which is what fillershiftbytes undoes.  Advancing the frame number
		// for filler too is what keeps a filled datastream and a merely short
		// one describing the same stretch of time.
		nframes += g.missing;
		gapnext++;
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
