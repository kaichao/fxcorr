#include "spreader.h"

#include <cstring>
#include <cstdlib>
#include <iostream>

using namespace std;

// .sp header field offsets (fxcorr/data-spec.md 5.3; written by FEngineWriter::writeSpHeader)
static const int OFF_MAGIC = 0;		// 6 bytes "FXCSP\0"
static const int OFF_VERSION = 6;	// u32
static const int OFF_BANDINDEX = 10;	// u32
static const int OFF_POL = 14;		// 1 byte
static const int OFF_NCHAN = 16;	// u32
static const int OFF_BANDWIDTH = 20;	// f64 (Hz)
static const int OFF_BANDEDGE = 28;	// f64 (Hz)
static const int OFF_LSB = 36;		// u32
static const int OFF_COMPLEX = 40;	// u32
static const int OFF_NSUB = 44;		// u32
static const int OFF_SUBNS = 48;	// u32
static const int OFF_BPS = 52;		// u32
static const int OFF_NBF = 56;		// u32
static const int OFF_FLAGWORDS = 60;	// u32
static const int HEADER_BYTES = 256;

static u32 readU32(const char *buf, int off)
{
	u32 v;
	memcpy(&v, buf + off, 4);
	return v;
}

SpReader::SpReader(const string &path) :
	file_(0), ok_(false), bandindex_(0), pol_(0), nchan_(0),
	nsub_(0), subns_(0), bps_(0), nbf_(0), flagwords_(0), subintbytes_(0),
	scan_(0), sec_(0), ns_(0), flagsbuf_(0), wbuf_(0), specbuf_(0)
{
	file_ = fopen(path.c_str(), "rb");
	if(file_ == NULL)
	{
		cerr << "SpReader: cannot open " << path << endl;
		return;
	}

	char header[HEADER_BYTES];
	if(fread(header, 1, HEADER_BYTES, file_) != HEADER_BYTES)
	{
		cerr << "SpReader: short header in " << path << endl;
		return;
	}
	if(memcmp(header + OFF_MAGIC, "FXCSP\0", 6) != 0 || readU32(header, OFF_VERSION) != 1)
	{
		cerr << "SpReader: bad magic or version in " << path << endl;
		return;
	}

	bandindex_ = (int)readU32(header, OFF_BANDINDEX);
	pol_ = header[OFF_POL];
	nchan_ = (int)readU32(header, OFF_NCHAN);
	nsub_ = readU32(header, OFF_NSUB);
	subns_ = readU32(header, OFF_SUBNS);
	bps_ = readU32(header, OFF_BPS);
	nbf_ = readU32(header, OFF_NBF);
	flagwords_ = readU32(header, OFF_FLAGWORDS);

	// per subint: i32 scan/sec/ns + flags + weights + spectra
	subintbytes_ = 12 + (long long)flagwords_*4 + (long long)bps_*4 + (long long)bps_*nchan_*8;

	flagsbuf_ = new s32[flagwords_];
	wbuf_ = new f32[bps_];
	specbuf_ = new cf32[(long long)bps_*nchan_];

	ok_ = true;
}

SpReader::~SpReader()
{
	if(file_ != NULL)
		fclose(file_);
	delete [] flagsbuf_;
	delete [] wbuf_;
	delete [] specbuf_;
}

bool SpReader::readSubint(int s, int &scan, int &sec, int &ns)
{
	scan = sec = ns = -1;
	if(!ok_ || s < 0 || (u32)s >= nsub_)
		return false;

	if(fseeko(file_, HEADER_BYTES + (long long)s*subintbytes_, SEEK_SET) != 0)
		return false;

	if(fread(&scan_, 4, 1, file_) != 1 || fread(&sec_, 4, 1, file_) != 1 || fread(&ns_, 4, 1, file_) != 1)
		return false;
	if(fread(flagsbuf_, 4, flagwords_, file_) != flagwords_)
		return false;
	if(fread(wbuf_, 4, bps_, file_) != bps_)
		return false;
	if(fread(specbuf_, sizeof(cf32), (long long)bps_*nchan_, file_) != (size_t)((long long)bps_*nchan_))
		return false;

	scan = scan_;
	sec = sec_;
	ns = ns_;
	return true;
}
