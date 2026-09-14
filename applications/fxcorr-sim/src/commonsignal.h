#ifndef FXSIM_COMMONSIGNAL_H
#define FXSIM_COMMONSIGNAL_H

#include <cstdio>
#include <random>
#include <string>
#include <vector>

class Configuration;

// Common frequency-domain signal (D15, fxcorr/data-spec.md 5.8): grid
// derivation, generation and file I/O.  The common signal is the cross-station
// coherence source: one authoritative copy per batch on shared storage,
// generated once by "fxcorr-sim common" and read (never written) by each
// "fxcorr-sim station" task.
//
// Signal semantics (datasim gencplx port): a stream of complex spectra, one
// slice of numsamps points every stime = 1/specres us, each point an
// independent real/imag Gaussian pair with STDEV 1.  The slice covers the
// whole-experiment band span [minstartfreq, minstartfreq + numsamps*specres)
// so every station band is a contiguous cut of it; a station's cut is
// bit-identical for every station reading it.
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

// Generate the common signal for one batch:
//   outdir/common/<batchid>/meta.json  +  data_XX.bin (one file per 0.5 s
//   block, XX zero-padded).  Block files are written to <name>.tmp and
//   renamed; meta.json starts with status "running" and flips to "done"
//   after the last block, which is what makes the batch visible to stations.
//   Deterministic in (seed, totalslices, grid, line) only - no station input.
//   With a line configured the Gaussian filter is applied per slice right
//   after gencplx (datasim.cpp generation loop); a line frequency outside
//   the grid span is rejected.
bool generate(const Grid &grid, long long totalslices, unsigned long seed,
              const std::string &outdir, const std::string &batchid,
              double startmjd, const LineSpec &line = LineSpec());

// Station-side reader.  open() parses meta.json and rejects it unless the
// batch id matches and status is "done"; the grid fields must agree with the
// expected grid (a stale common/ from a different .input must not be read).
class Reader
{
public:
	Reader();
	~Reader();

	bool open(const std::string &commondir, const std::string &batchid,
	          const Grid &expected);
	bool isOpen() const { return ok; }

	// read block n (0-based) into buf; returns the number of floats actually
	// read (the last block may be shorter), -1 on error
	long long readBlock(long long n, float *buf);

	long long nblocks() const { return nblocks_; }
	long long blockfloats() const { return blockfloats_; }
	int numsamps() const { return numsamps_; }
	unsigned long seed() const { return seed_; }

	// static helpers shared with main.cpp's batch.json parsing
	static bool extractJsonDouble(const std::string &json, const std::string &key, double *value);
	static bool extractJsonInt(const std::string &json, const std::string &key, int *value);
	static bool extractJsonString(const std::string &json, const std::string &key, std::string *value);

private:
	bool ok;
	std::string dir;
	long long nblocks_;
	long long blockfloats_;
	int numsamps_;
	unsigned long seed_;
};

} // namespace CommonSignal

#endif
