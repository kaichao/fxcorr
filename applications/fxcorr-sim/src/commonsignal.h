#ifndef FXSIM_COMMONSIGNAL_H
#define FXSIM_COMMONSIGNAL_H

#include <cstdio>
#include <random>
#include <string>
#include <vector>

class Configuration;

// Common frequency-domain signal: grid derivation and slice-stream generation.
//
// The signal is the cross-station coherence source: every station of a batch
// runs the same PRNG stream (same seed, same order) and cuts its own band out
// of it, which is what makes the stations mutually coherent.  Nothing about it
// is stored any more (V6 S2.5, plan E) - see SliceStream.  The one parameter
// that has to be shared is the seed, and it travels in batches/<id>.json; the
// grid and the batch extent are pure functions of .input, recomputed on the
// spot by whoever needs them.
//
// Signal semantics (datasim gencplx port): a stream of complex spectra, one
// slice of numsamps points every stime = 1/specres us, each point an
// independent real/imag Gaussian pair with STDEV 1.  The slice covers the
// whole-experiment band span [minstartfreq, minstartfreq + numsamps*specres)
// so every station band is a contiguous cut of it.
namespace CommonSignal
{

// Grid derived from the whole-experiment band layout (ported from datasim
// util.cpp getSpecRes / getMaxChanFreq / getMinStartFreq, with two fixes:
// the GCD candidate loop resets its accumulator per candidate and accepts
// down to 1/2^10, both per fxcorr-sim-arch.md).
struct Grid
{
	double specresmhz;        // spectrum resolution in MHz (0.5 down to 1/2^10)
	int numsamps;             // frequency points per slice
	double minstartfreqmhz;   // lowest band frequency in MHz (grid origin)
	double stimeus;           // slice duration in us (= 1/specresmhz)
	long long slicesperblock; // slices per 0.5 s block (500000 / stimeus)
};

// Spectral line parameters (FXSIM_LINE, datasim --specline / util.cpp
// gengaussianfilter semantics): a Gaussian line at freqmhz (absolute MHz)
// with amplitude sqrt(amp) and rms in grid points, multiplied onto every
// slice of the common signal.  freqmhz <= 0 disables the line.
struct LineSpec
{
	double freqmhz;
	double amp;
	double rms;
	LineSpec() : freqmhz(0.0), amp(0.0), rms(0.0) {}
};

// Derive the grid from config; returns false (with a message on stderr) if
// the band layout is not representable on a power-of-two grid.  specresfac
// (FXSIM_SPECRES, datasim --specres semantics: the GCD grid divided by the
// scaling factor, datasim.cpp "specRes /= setupinfo.specres") must be a
// positive integer; every consistency check runs on the scaled grid.
bool deriveGrid(Configuration &config, Grid *grid, int specresfac = 1);

// Node-local streaming generator (V6 S2.5, plan E): the same slice stream
// generate() would have written, produced block by block without touching the
// filesystem.  The PRNG is reseeded the same way and every block goes through
// the same slice-filling routine, so a station fed by this stream sees
// bit-identical data - which is what the S2.5 acceptance test checks (the raw
// output must match the file-fed path byte for byte).
//
// Why not just generate the band a station needs: the stream is a sequential
// PRNG (slice-major, continuous across slices), so reaching slice s point k
// means computing everything before it.  Generating the whole span and keeping
// only one band's cut is the point of this class.
class SliceStream
{
public:
	SliceStream();

	// Same arguments as generate() minus the paths: a batch is described by
	// (grid, totalslices, seed, line) alone.  A line outside the grid span is
	// rejected the same way generate() rejects it.
	bool init(const Grid &grid, long long totalslices, unsigned long seed,
	          const LineSpec &line = LineSpec());

	long long nblocks() const { return nblocks_; }
	long long blockfloats() const { return blockfloats_; }
	int numsamps() const { return grid_.numsamps; }
	unsigned long seed() const { return seed_; }

	// Fill block n (0-based) into buf, which must hold blockfloats() floats
	// (the last block may be shorter); returns the number of floats produced,
	// or -1 if n is out of range.  Blocks are requested in order - the PRNG
	// stream is continuous across them, exactly as in the file path.
	long long fillBlock(long long n, float *buf);

private:
	Grid grid_;
	unsigned long seed_;
	long long totalslices_;
	long long nblocks_;
	long long blockfloats_;
	std::mt19937 engine_;
	std::normal_distribution<double> gauss_;
	std::vector<float> linefilter_;
};

// Small JSON text helpers.  After plan E the only JSON left to parse is
// batches/<id>.json (main.cpp's loadBatchInfo); they stay in this header
// because this is where the parsing lived when a common/ signal had to be
// read back.
bool jsonDouble(const std::string &json, const std::string &key, double *value);
bool jsonInt(const std::string &json, const std::string &key, int *value);
bool jsonInt(const std::string &json, const std::string &key, long long *value);
bool jsonString(const std::string &json, const std::string &key, std::string *value);

} // namespace CommonSignal

#endif
