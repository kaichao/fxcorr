#include "commonsignal.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <fxcorrcommon/configuration.h>

using namespace std;

namespace CommonSignal
{

// ---------------------------------------------------------------------------
// grid derivation

static bool isInteger(double x)
{
	return fabs(x - rint(x)) < 1.0e-9;
}

static string readAll(const string &path)
{
	ifstream f(path.c_str());
	if(!f.is_open())
		return "";
	stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Whole-experiment band layout: per-station band start frequency and
// bandwidth in MHz.  Every band must land on the grid, so the frequency
// differences and bandwidths of ALL bands (not just band 0 like datasim)
// enter the GCD; this catches multi-band layouts datasim's getSpecRes misses.
static bool collectBands(Configuration &config,
                         vector<double> *freqs, vector<double> *bws)
{
	if(config.getNumDataStreams() == 0)
		return false;
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		int nbands = config.getDNumRecordedBands(0, d);
		if(nbands < 1)
		{
			cerr << "fxcorr-sim: datastream " << config.getDStationName(0, d)
			     << " has no recorded bands" << endl;
			return false;
		}
		for(int b = 0; b < nbands; b++)
		{
			int fq = config.getDRecordedFreqIndex(0, d, b);
			freqs->push_back(config.getFreqTableFreq(fq));
			bws->push_back(config.getFreqTableBandwidth(fq));
		}
	}
	return true;
}

bool deriveGrid(Configuration &config, Grid *grid)
{
	vector<double> freqs, bws;
	if(!collectBands(config, &freqs, &bws))
		return false;

	// GCD of all pairwise frequency differences and all bandwidths; the
	// highest candidate is 0.5 MHz, halved down to 1/2^10 (datasim
	// getSpecRes, with the accumulator reset per candidate - datasim's
	// version keeps a sticky false and reports failure for any layout that
	// is not 0.5 MHz clean)
	double specres = 0.0;
	for(int cnt = 1; cnt <= 10; cnt++)
	{
		double cand = 1.0 / pow(2.0, cnt);
		bool isgcd = true;
		for(size_t i = 0; i < freqs.size() && isgcd; i++)
			for(size_t j = i + 1; j < freqs.size(); j++)
				isgcd = isgcd && isInteger(fabs(freqs[i] - freqs[j]) / cand);
		for(size_t i = 0; i < bws.size() && isgcd; i++)
			isgcd = isgcd && isInteger(bws[i] / cand);
		if(isgcd)
		{
			specres = cand;
			break;
		}
	}
	if(specres == 0.0)
	{
		cerr << "fxcorr-sim: cannot find a spectrum resolution (band frequencies "
		        "and bandwidths are not on a 1/2^n MHz grid down to 1/1024 MHz)" << endl;
		return false;
	}

	// coverage span: datasim getMaxChanFreq (band-0 bandwidth * band count,
	// i.e. each station's bands are contiguous), the maximum over stations
	double maxchanfreq = 0.0;
	double minstartfreq = freqs[0];
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		int nbands = config.getDNumRecordedBands(0, d);
		int fq0 = config.getDRecordedFreqIndex(0, d, 0);
		double span = config.getFreqTableBandwidth(fq0) * nbands;
		if(span > maxchanfreq)
			maxchanfreq = span;
	}
	for(size_t i = 0; i < freqs.size(); i++)
		if(freqs[i] < minstartfreq)
			minstartfreq = freqs[i];

	if(!isInteger(maxchanfreq / specres))
	{
		cerr << "fxcorr-sim: coverage span " << maxchanfreq
		     << " MHz is not an integer multiple of the spectrum resolution "
		     << specres << " MHz" << endl;
		return false;
	}
	grid->specresmhz = specres;
	grid->numsamps = (int)rint(maxchanfreq / specres);
	grid->minstartfreqmhz = minstartfreq;
	grid->stimeus = 1.0 / specres;
	grid->slicesperblock = (long long)rint(500000.0 / grid->stimeus);

	// every band must be a contiguous cut of the slice
	for(size_t i = 0; i < freqs.size(); i++)
	{
		double startidx = (freqs[i] - minstartfreq) / specres;
		double blksize = bws[i] / specres;
		if(!isInteger(startidx) || !isInteger(blksize) || blksize < 4.0 ||
		   (long long)rint(startidx + blksize) > (long long)grid->numsamps)
		{
			cerr << "fxcorr-sim: band at " << freqs[i] << " MHz (bw " << bws[i]
			     << " MHz) does not fit the spectrum grid" << endl;
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// generation

static bool writeMeta(const Grid &grid, long long totalslices, unsigned long seed,
                      const string &dir, const string &batchid, double startmjd,
                      const char *status)
{
	long long blockfloats = (long long)grid.numsamps * 2 * grid.slicesperblock;
	long long nblocks = (totalslices + grid.slicesperblock - 1) / grid.slicesperblock;
	ostringstream os;
	os << setprecision(17);   // start_mjd needs full f64 precision
	os << "{\n"
	   << "\t\"version\": 1,\n"
	   << "\t\"dtype\": \"float32\",\n"
	   << "\t\"spec_res_mhz\": " << grid.specresmhz << ",\n"
	   << "\t\"numsamps\": " << grid.numsamps << ",\n"
	   << "\t\"min_start_freq_mhz\": " << grid.minstartfreqmhz << ",\n"
	   << "\t\"block_bytes\": " << blockfloats * 4 << ",\n"
	   << "\t\"slices_per_block\": " << grid.slicesperblock << ",\n"
	   << "\t\"nblocks\": " << nblocks << ",\n"
	   << "\t\"seed\": " << seed << ",\n"
	   << "\t\"batch_id\": \"" << batchid << "\",\n"
	   << "\t\"start_mjd\": " << startmjd << ",\n"
	   << "\t\"status\": \"" << status << "\"\n"
	   << "}\n";
	string path = dir + "/meta.json";
	ofstream f(path.c_str());
	if(!f.is_open())
	{
		cerr << "fxcorr-sim: cannot write " << path << endl;
		return false;
	}
	f << os.str();
	f.close();
	return true;
}

static bool writeBlockFile(const string &path, const float *data, long long nfloats)
{
	string tmppath = path + ".tmp";
	FILE *f = fopen(tmppath.c_str(), "wb");
	if(!f)
	{
		cerr << "fxcorr-sim: cannot write " << tmppath << endl;
		return false;
	}
	size_t written = fwrite(data, sizeof(float), (size_t)nfloats, f);
	if(fclose(f) != 0 || written != (size_t)nfloats)
	{
		cerr << "fxcorr-sim: short write to " << tmppath << endl;
		remove(tmppath.c_str());
		return false;
	}
	if(rename(tmppath.c_str(), path.c_str()) != 0)
	{
		cerr << "fxcorr-sim: cannot rename " << tmppath << " to " << path << endl;
		remove(tmppath.c_str());
		return false;
	}
	return true;
}

bool generate(const Grid &grid, long long totalslices, unsigned long seed,
              const string &outdir, const string &batchid, double startmjd)
{
	string dir = outdir + "/common/" + batchid;
	string mkdircommand = "mkdir -p " + dir;
	if(system(mkdircommand.c_str()) != 0)
	{
		cerr << "fxcorr-sim: cannot create " << dir << endl;
		return false;
	}

	// running first: a station seeing this meta knows the batch is not ready
	if(!writeMeta(grid, totalslices, seed, dir, batchid, startmjd, "running"))
		return false;

	// deterministic per-run noise stream, reseeded once
	mt19937 engine(seed);
	normal_distribution<double> gauss(0.0, 1.0);

	long long blockfloats = (long long)grid.numsamps * 2 * grid.slicesperblock;
	long long nblocks = (totalslices + grid.slicesperblock - 1) / grid.slicesperblock;
	vector<float> block((size_t)blockfloats);

	for(long long n = 0; n < nblocks; n++)
	{
		long long slices = grid.slicesperblock;
		if(n == nblocks - 1)
			slices = totalslices - n * grid.slicesperblock;
		long long nfloats = slices * 2LL * grid.numsamps;

		// gencplx semantics: independent real/imag Gaussians, STDEV 1;
		// slice-major order, frequency points ascending within a slice
		for(long long i = 0; i < nfloats; i++)
			block[(size_t)i] = (float)gauss(engine);

		// block visibility: write to <name>.tmp, then rename
		char name[32];
		snprintf(name, sizeof(name), "data_%02lld.bin", n);
		if(!writeBlockFile(dir + "/" + name, &block[0], nfloats))
			return false;
	}

	if(!writeMeta(grid, totalslices, seed, dir, batchid, startmjd, "done"))
		return false;
	cerr << "wrote " << dir << ": " << nblocks << " block(s), "
	     << totalslices << " slices" << endl;
	return true;
}

// ---------------------------------------------------------------------------
// reader

bool Reader::extractJsonDouble(const string &json, const string &key, double *value)
{
	size_t pos = json.find("\"" + key + "\"");
	if(pos == string::npos)
		return false;
	pos = json.find(':', pos);
	if(pos == string::npos)
		return false;
	char *end;
	*value = strtod(json.c_str() + pos + 1, &end);
	return (end != json.c_str() + pos + 1);
}

bool Reader::extractJsonInt(const string &json, const string &key, int *value)
{
	double d;
	if(!extractJsonDouble(json, key, &d))
		return false;
	*value = (int)(d + 0.5);
	return true;
}

bool Reader::extractJsonString(const string &json, const string &key, string *value)
{
	size_t pos = json.find("\"" + key + "\"");
	if(pos == string::npos)
		return false;
	pos = json.find(':', pos);
	if(pos == string::npos)
		return false;
	size_t start = json.find('"', pos);
	size_t end = (start == string::npos) ? string::npos : json.find('"', start + 1);
	if(start == string::npos || end == string::npos)
		return false;
	*value = json.substr(start + 1, end - start - 1);
	return true;
}

Reader::Reader()
	: ok(false), dir(""), nblocks_(0), blockfloats_(0), numsamps_(0), seed_(0)
{
}

Reader::~Reader()
{
}

bool Reader::open(const string &commondir, const string &batchid,
                  const Grid &expected)
{
	string path = commondir + "/meta.json";
	string json = readAll(path);
	if(json.empty())
	{
		cerr << "fxcorr-sim: common signal not ready: " << path
		     << " missing or unreadable" << endl;
		return false;
	}
	int version = 0;
	string dtype, status, mbatch;
	double specres = 0.0, minfreq = 0.0, seedsigned = 0.0;
	int numsamps = 0, blockbytes = 0, slicesperblock = 0, nblocks = 0;
	if(!extractJsonInt(json, "version", &version) ||
	   !extractJsonString(json, "dtype", &dtype) ||
	   !extractJsonString(json, "status", &status) ||
	   !extractJsonString(json, "batch_id", &mbatch) ||
	   !extractJsonDouble(json, "spec_res_mhz", &specres) ||
	   !extractJsonDouble(json, "min_start_freq_mhz", &minfreq) ||
	   !extractJsonDouble(json, "seed", &seedsigned) ||
	   !extractJsonInt(json, "numsamps", &numsamps) ||
	   !extractJsonInt(json, "block_bytes", &blockbytes) ||
	   !extractJsonInt(json, "slices_per_block", &slicesperblock) ||
	   !extractJsonInt(json, "nblocks", &nblocks))
	{
		cerr << "fxcorr-sim: meta.json missing required fields" << endl;
		return false;
	}
	if(version != 1)
	{
		cerr << "fxcorr-sim: unsupported common signal version " << version << endl;
		return false;
	}
	if(dtype != "float32")
	{
		cerr << "fxcorr-sim: unsupported common signal dtype " << dtype << endl;
		return false;
	}
	if(status != "done")
	{
		cerr << "fxcorr-sim: common signal status is \"" << status
		     << "\", not done - run fxcorr-sim common first" << endl;
		return false;
	}
	if(mbatch != batchid)
	{
		cerr << "fxcorr-sim: common signal belongs to batch " << mbatch
		     << ", not " << batchid << endl;
		return false;
	}
	// the grid must agree with the whole-experiment layout this .input
	// derives; a stale common/ from a different config is rejected here
	if(specres != expected.specresmhz || numsamps != expected.numsamps ||
	   minfreq != expected.minstartfreqmhz)
	{
		cerr << "fxcorr-sim: common signal grid (" << specres << " MHz res, "
		     << numsamps << " points, " << minfreq << " MHz origin) does not "
		     "match the .input band layout" << endl;
		return false;
	}
	if((long long)blockbytes != (long long)numsamps * 2 * (long long)slicesperblock * 4)
	{
		cerr << "fxcorr-sim: meta.json block_bytes inconsistent" << endl;
		return false;
	}
	dir = commondir;
	nblocks_ = nblocks;
	blockfloats_ = (long long)numsamps * 2 * (long long)slicesperblock;
	numsamps_ = numsamps;
	seed_ = (unsigned long)seedsigned;
	ok = true;
	return true;
}

long long Reader::readBlock(long long n, float *buf)
{
	if(!ok || n < 0 || n >= nblocks_)
		return -1;
	char name[32];
	snprintf(name, sizeof(name), "data_%02lld.bin", n);
	string path = dir + "/" + name;
	FILE *f = fopen(path.c_str(), "rb");
	if(!f)
	{
		cerr << "fxcorr-sim: cannot open " << path << endl;
		return -1;
	}
	size_t want = (size_t)blockfloats_;
	if(n == nblocks_ - 1)
	{
		// last block is truncated to the batch length; discover its size
		fseek(f, 0, SEEK_END);
		long long bytes = ftell(f);
		fseek(f, 0, SEEK_SET);
		if(bytes <= 0 || bytes % (4 * (long long)numsamps_) != 0)
		{
			cerr << "fxcorr-sim: corrupt last block " << path << endl;
			fclose(f);
			return -1;
		}
		want = (size_t)(bytes / 4);
	}
	size_t got = fread(buf, sizeof(float), want, f);
	fclose(f);
	if(got != want)
	{
		cerr << "fxcorr-sim: short read from " << path << endl;
		return -1;
	}
	return (long long)got;
}

} // namespace CommonSignal
