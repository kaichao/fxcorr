#ifndef FXSIM_SIGNALGEN_H
#define FXSIM_SIGNALGEN_H

#include <random>
#include <vector>

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
	void init(const std::vector<double> &bandwidthshz,
	          const std::vector<double> &tonehz,
	          const std::vector<std::vector<double> > &pcaltonehz,
	          double noisesigma, unsigned long seed);

	// Fill one frame payload: payloadbytes = bytes per band per frame * nbands,
	// bands interleaved sample-wise within each byte (mark5access
	// vdif_decode_2channel_2bit layout generalised to nbands).
	void fillFramePayload(unsigned char *payload, int payloadbytes, int nbands);

private:
	std::vector<double> ratemhz;
	std::vector<double> tonemhz;
	std::vector<std::vector<double> > pcalmhz;
	double sigma;
	unsigned long seed;
	bool seeded;
	std::mt19937 engine;
	std::normal_distribution<double> gauss;
};

#endif
