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
//   DFT(/N) -> DC zero -> fractional sample correction (freq domain,
//   datasim applyfracsamperrcorrection) -> IDFT -> fringe rotation (time
//   domain at the band start frequency, datasim applyfringerotation) ->
//   DFT(/N) -> Hermitian extension to 2N -> 2N IDFT -> real part
// With delay injection disabled (enableDelayInjection not called, or a
// delay-free model) the two correction steps multiply by e^{j0} exactly and
// the frame window shift is zero, so the chain is byte-identical to the P0
// identity round-trip (same layout as datasim; the extra float round-off is
// shared by every station and does not affect cross-station bit identity).
//
// Scaling: the common signal has frequency-domain rms 1 per component.  A
// station slice is cut, then either (P0, flux <= 0) noise of sigma added
// and scaled by 0.5 / (sqrt(1+sigma^2) * sqrt(blksize)), or (P2, flux > 0)
// the datasim fabricatedata chain: x sqrt(F), + sqrt(SEFD) station noise,
// / sqrt(F+SEFD), so the slice rms returns to 1 and the gain becomes
// 0.5 / sqrt(blksize) - the two paths agree at flux=1, sefd=0, sigma=0.
// The final real frame samples then have rms 0.5, the healthy operating
// point of the fixed rint quantiser (the frame chain gain is sqrt(blksize),
// independent of vpsamps: block IDFT x sqrt(blksize), frame DFT /N, 2N
// IDFT x sqrt(2N), Hermitian half-occupancy /sqrt(2)).
class FreqStationGen
{
public:
	// bandfreqmhz / bandbwmhz: per band start frequency / bandwidth in MHz
	// grid: common signal grid (origin + specres)
	// noisesigma: station noise sigma (FXSIM_NOISE), frequency-domain,
	//   0 disables it (P0 path, used when flux <= 0)
	// flux / sefd: FXSIM_FLUX / FXSIM_SEFD (Jy, datasim fabricatedata
	//   semantics).  flux > 0 enables the datasim scaling chain
	//   (x sqrt(F) -> + sqrt(SEFD) noise -> / sqrt(F+SEFD)), which replaces
	//   the noisesigma path; flux <= 0 keeps the P0 noisesigma behaviour.
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
	          int vpsamps_, bool adaptive, double flux = 0.0, double sefd = 0.0);

	// Geometric delay injection (P2, datasim updatevalues + processdata
	// semantics): every frame the .im model delay is evaluated at the frame
	// start (order 1, the same call datasim makes per packet); the fractional
	// sample error accumulates the departure from the linear prediction and a
	// whole complex sample is shifted when it crosses half a sample time.
	// The frame chain then applies the fractional-sample correction in the
	// frequency domain (datasim applyfracsamperrcorrection), the fringe
	// rotation in the time domain at the band start frequency (datasim
	// applyfringerotation, fraction_of semantics) and takes the frame window
	// from the baseband stream at the accumulated whole-sample shift.  Not
	// called = the delay-free identity chain (P0 behaviour), byte-identical.
	//
	// model/scan/antennaindex/srcindex: the .im model context, same as
	//   SignalGen::enableDelayInjection (antennaindex = .input model file
	//   index, srcindex = phase/pointing centre choice)
	// batchstartsec: batch start in scan-relative seconds (the frame the
	//   delay model expects, same as fxcorr-f datareader.cpp)
	// framesec: VDIF frame duration in seconds
	void enableDelayInjection(Model *model, int scan, int antennaindex,
	                          int srcindex, double batchstartsec,
	                          double framesec);

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
		double flux;                     // FXSIM_FLUX (> 0 = datasim chain)
		double sefd;                     // FXSIM_SEFD (Jy)
		double startfreqmhz;             // band start frequency (fringe rotation)
		fftwf_plan planidft;             // blksize, backward, unnormalised
		fftwf_plan planfwd;              // vpsamps, forward (1/N applied by hand)
		fftwf_plan planbwd;              // vpsamps, backward, unnormalised
		fftwf_plan planbwd2n;            // 2*vpsamps, backward, unnormalised
		fftwf_complex *temp;             // blksize slice workspace
		fftwf_complex *baseband;         // rolling buffer: tail + block length
		fftwf_complex *procbuf;          // vpsamps frame workspace
		fftwf_complex *buffreqtemp;      // 2*vpsamps Hermitian spectrum
		fftwf_complex *realc;            // 2*vpsamps 2N IDFT output
		std::mt19937 engine;
		std::normal_distribution<double> gauss;
		long long sampcount;             // adaptive quantiser statistics
		double square;
		double thresh;
		// delay injection state (datasim updatevalues; all zero when idle)
		double fracerr;                  // fractional sample error (s)
		long long shift;                 // accumulated whole-sample shift
		double delay0;                   // model delay at the frame start (s)
		double delayrate;                // per-second delay change (s/s)
		double prevdelay;                // previous frame's delay0
		double prevrate;                 // previous frame's change over framesec
	};

	std::vector<Band> bands;
	int numsamps;                       // frequency points per common slice
	long long slicesperblock;
	long long framesinblock;
	long long framecounter;             // frame index within the current block
	long long frameglobal;              // frame index since the batch start
	long long blockstartglobal;         // block start in complex samples
	int vpsamps;
	double noisesigma;
	double sampletime;                  // complex sample time (framesec/vpsamps)
	bool adaptive;
	bool ready;
	// delay model context (all zero / null when disabled)
	Model *model;
	int scan;
	int antennaindex;
	int srcindex;
	double batchstartsec;
	double framesec;
};

#endif
