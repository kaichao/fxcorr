#include "signalgen.h"

#include <cmath>
#include <iostream>
#include <random>

using namespace std;

void SignalGen::init(const vector<double> &bandwidthshz,
                     const vector<double> &tonehz_,
                     const vector<vector<double> > &pcaltonehz_,
                     double noisesigma, unsigned long seed_)
{
	// internal storage in MHz so the sample loop evaluates
	// 2*pi*tone_mhz*i/rate_mhz exactly as gen_test_vdif.py does
	for(size_t k = 0; k < bandwidthshz.size(); k++)
		ratemhz.push_back(bandwidthshz[k] / 1.0e6);
	for(size_t k = 0; k < tonehz_.size(); k++)
		tonemhz.push_back(tonehz_[k] / 1.0e6);
	pcalmhz.resize(pcaltonehz_.size());
	for(size_t k = 0; k < pcaltonehz_.size(); k++)
		for(size_t j = 0; j < pcaltonehz_[k].size(); j++)
			pcalmhz[k].push_back(pcaltonehz_[k][j] / 1.0e6);
	sigma = noisesigma;
	seed = seed_;
	seeded = false;
}

// One band's 2-bit sample, low-bits-first packed into the current byte.
// q = rint(v*2)+2 clamped to 0..3; rint is IEEE round-half-even, the same
// rounding as Python's round() used by gen_test_vdif.py.
static inline int quantise2bit(double v)
{
	int q = (int)rint(v * 2.0) + 2;
	if(q < 0)
		q = 0;
	if(q > 3)
		q = 3;
	return q;
}

void SignalGen::fillFramePayload(unsigned char *payload, int payloadbytes, int nbands)
{
	// deterministic per-run noise stream: reseeded once on first use
	if(!seeded)
	{
		engine.seed(seed);
		seeded = true;
	}

	int nsamp = payloadbytes * 4;
	for(int g = 0; g < nsamp; g++)
	{
		int band = g % nbands;
		// band-local sample index; counts from the batch start
		long long i = g / nbands;

		double v = 0.0;
		if(tonemhz[band] != 0.0)
		{
			// same evaluation order as gen_test_vdif.py:
			// 2.0 * pi * tone_mhz * i / rate_mhz
			double phase = 2.0 * M_PI * tonemhz[band] * (double)i / ratemhz[band];
			v += 0.7 * sin(phase);
		}
		if(sigma > 0.0)
			v += sigma * gauss(engine);
		for(size_t k = 0; k < pcalmhz[band].size(); k++)
		{
			double phase = 2.0 * M_PI * pcalmhz[band][k] * (double)i / ratemhz[band];
			v += 0.1 * sin(phase);
		}

		int q = quantise2bit(v);
		payload[g / 4] |= q << (2 * (g % 4));
	}
}
