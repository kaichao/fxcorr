#ifndef FXSIM_SIGNALGEN_H
#define FXSIM_SIGNALGEN_H

#include <fftw3.h>
#include <random>
#include <vector>

#include "commonsignal.h"

class Model;

// Signal synthesis and 2-bit quantisation for simulated VDIF data.
//
// One independent real-valued sample stream per band.  A sample at band-local
// index i (0-based, counting from the batch start) is
//
//   x[i] = A_tone * sin(2*pi*f_tone*i/rate)        if a tone is configured
//        + noise_sigma * N(0,1)                    if noise_sigma > 0
//        + sum_k A_pcal * sin(2*pi*f_pcal_k*i/rate)  per injected pcal tone
//
// 2-bit quantisation matches gen_test_vdif.py: q = rint(v*2) + 2 clamped to
// 0..3, samples packed low bits first (mark5access lut2bit bit order).  The
// tone phase is computed as 2*pi*tone_mhz*i/rate_mhz with the same left-to-
// right float evaluation order as the Python reference, so a noise-free run
// with identical parameters reproduces it byte for byte.
class SignalGen
{
public:
	// bandwidthshz:  per-band sample rate in Hz (all bands must share one rate)
	// tonehz:        per-band baseband tone frequency in Hz, 0 = no tone
	// pcaltonehz:    per-band baseband pcal tone frequencies in Hz
	// noisesigma:    0 disables Gaussian noise
	// toneamp:       tone amplitude, 0.7 by default (SNR scaling overrides it)
	// adaptive:      datasim quantise semantics (running-rms threshold, two
	//                amplitude levels x sign); false = the historical fixed
	//                rint mapping the byte comparison relies on
	void init(const std::vector<double> &bandwidthshz,
	          const std::vector<double> &tonehz,
	          const std::vector<std::vector<double> > &pcaltonehz,
	          double noisesigma, unsigned long seed, double toneamp = 0.7,
	          bool adaptive = false);

	// Geometric delay injection (FXSIM_DELAY): the tone phase gains
	// +2*pi*f_RF*tau(t), where f_RF is the tone's RF frequency (bandrfhz)
	// and tau comes from the .calc delay model, evaluated once per VDIF
	// frame with order 1 (the same call datasim makes per packet,
	// subband.cpp updatevalues).  All bands of one datastream share the
	// model delay; only f_RF differs per band.  Not called = the historical
	// byte-compatible path, unchanged.
	//
	// batchstartsec: batch start in scan-relative seconds (the frame the
	//                delay model expects, same as fxcorr-f datareader.cpp)
	// framesec:      VDIF frame duration in seconds
	// bandrfhz:      per-band tone RF frequency in Hz (0 disables the
	//                injection for that band)
	void enableDelayInjection(Model *model, int scan, int antennaindex, int srcindex,
	                          double batchstartsec, double framesec,
	                          const std::vector<double> &bandrfhz);

	// Fill one frame payload: payloadbytes = bytes per band per frame * nbands,
	// bands interleaved sample-wise within each byte (mark5access
	// vdif_decode_2channel_2bit layout generalised to nbands).
	void fillFramePayload(unsigned char *payload, int payloadbytes, int nbands);

private:
	std::vector<double> ratemhz;
	std::vector<double> tonemhz;
	std::vector<std::vector<double> > pcalmhz;
	double sigma;
	double toneamp;
	bool adaptive;
	unsigned long seed;
	bool seeded;
	size_t adaptsampcount;   // adaptive quantiser statistics (datasim d_sampcount)
	double adaptsquare;      // accumulated sample squares (datasim d_square)
	double adaptthresh;      // running rms threshold (datasim d_tmul)
	std::mt19937 engine;
	std::normal_distribution<double> gauss;

	// delay injection state (all zero / null when disabled)
	Model *model;
	int scan;
	int antennaindex;
	int srcindex;
	double batchstartsec;
	double framesec;
	long long framecounter;
	std::vector<double> bandrfmhz;   // MHz, matches the tonemhz units
	double delay0;                   // model delay at the current frame start (s)
	double delayrate;                // per-second delay change across the frame (s/s)
};

// Frequency-domain station synthesis (new path, P0).  Reads the common
// signal slice stream, cuts this station's band, adds station noise,
// normalises, applies the Ormsby band-edge filter, inverse-DFTs to a
// complex baseband stream, and per frame converts complex baseband to
// real 2x-oversampled samples for 2-bit quantisation and packing.
//
// The frame correction chain mirrors datasim subband.cpp processdata:
//   DFT(/N) -> DC zero -> [P2: fractional sample correction] -> IDFT ->
//   [P2: fringe rotation] -> DFT(/N) -> Hermitian extension to 2N ->
//   2N IDFT -> real part
// With no delay injected the two marked steps are identities and the
// intermediate IDFT+DFT round-trip is kept so the P2 insertion points stay
// in place (same layout as datasim; the extra float round-off is shared by
// every station and does not affect cross-station bit identity).
//
// Scaling: the common signal has frequency-domain rms 1 per component.  A
// station slice is cut, noise of sigma added, then scaled by
// 0.5 / (sqrt(1+sigma^2) * sqrt(blksize)) before the block IDFT so that
// the final real frame samples have rms 0.5, the healthy operating point
// of the fixed rint quantiser (the frame chain gain is sqrt(blksize),
// independent of vpsamps: block IDFT x sqrt(blksize), frame DFT /N, 2N
// IDFT x sqrt(2N), Hermitian half-occupancy /sqrt(2)).
class FreqStationGen
{
public:
	// bandfreqmhz / bandbwmhz: per band start frequency / bandwidth in MHz
	// grid: common signal grid (origin + specres)
	// noisesigma: station noise sigma (FXSIM_NOISE), frequency-domain,
	//   0 disables it
	// seed / station: station noise seeds derive from (seed, station, band)
	//   so the common stream and the per-station streams never share an RNG
	// vpsamps: complex baseband samples per band per frame
	//   (= bytes per band per frame * 2, the 2x oversampling of 2-bit real)
	// adaptive: datasim d_tmul-style running-rms quantiser threshold
	FreqStationGen();
	~FreqStationGen();

	bool init(const std::vector<double> &bandfreqmhz,
	          const std::vector<double> &bandbwmhz,
	          const CommonSignal::Grid &grid,
	          double noisesigma, unsigned long seed, const std::string &station,
	          int vpsamps_, bool adaptive);

	// process one common-signal block; nfloats is what
	// CommonSignal::Reader::readBlock returned (the last block is shorter).
	// After this, fillFramePayload() may be called framesPerBlock() times.
	bool processBlock(const float *blockdata, long long nfloats);

	int framesPerBlock() const { return framesinblock; }

	// fill one frame payload, bands interleaved sample-wise within each byte
	// (same layout as SignalGen::fillFramePayload); payload must be zeroed
	void fillFramePayload(unsigned char *payload, int payloadbytes, int nbands);

private:
	struct Band
	{
		long long startidx;              // slice index of the band start
		int blksize;                     // frequency points per slice
		double scale;                    // frequency-domain gain, see above
		fftwf_plan planidft;             // blksize, backward, unnormalised
		fftwf_plan planfwd;              // vpsamps, forward (1/N applied by hand)
		fftwf_plan planbwd;              // vpsamps, backward, unnormalised
		fftwf_plan planbwd2n;            // 2*vpsamps, backward, unnormalised
		fftwf_complex *temp;             // blksize slice workspace
		fftwf_complex *baseband;         // blksize * slicesperblock
		fftwf_complex *procbuf;          // vpsamps frame workspace
		fftwf_complex *buffreqtemp;      // 2*vpsamps Hermitian spectrum
		fftwf_complex *realc;            // 2*vpsamps 2N IDFT output
		std::mt19937 engine;
		std::normal_distribution<double> gauss;
		long long sampcount;             // adaptive quantiser statistics
		double square;
		double thresh;
	};

	std::vector<Band> bands;
	int numsamps;                       // frequency points per common slice
	long long framesinblock;
	long long framecounter;             // frame index within the current block
	int vpsamps;
	double noisesigma;
	bool adaptive;
	bool ready;
};

#endif
