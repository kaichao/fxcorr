#include "signalgen.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <random>

// f64 etc. come from architecture.h (GENERIC branch); model.h uses f64
// without including it itself, so include it first like fxcorr-f datareader.h
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/model.h>

using namespace std;

void SignalGen::init(const vector<double> &bandwidthshz,
                     const vector<double> &tonehz_,
                     const vector<vector<double> > &pcaltonehz_,
                     double noisesigma, unsigned long seed_, double toneamp_,
                     bool adaptive_)
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
	toneamp = toneamp_;
	adaptive = adaptive_;
	adaptsampcount = 0;
	adaptsquare = 0.0;
	adaptthresh = 1.0;   // datasim d_tmul initial value
	seed = seed_;
	seeded = false;

	model = 0;
	scan = 0;
	antennaindex = 0;
	srcindex = 0;
	batchstartsec = 0.0;
	framesec = 0.0;
	framecounter = 0;
	delay0 = 0.0;
	delayrate = 0.0;
}

void SignalGen::enableDelayInjection(Model *model_, int scan_, int antennaindex_,
                                     int srcindex_, double batchstartsec_,
                                     double framesec_, const vector<double> &bandrfhz)
{
	model = model_;
	scan = scan_;
	antennaindex = antennaindex_;
	srcindex = srcindex_;
	batchstartsec = batchstartsec_;
	framesec = framesec_;
	framecounter = 0;
	delay0 = 0.0;
	delayrate = 0.0;
	bandrfmhz.resize(bandrfhz.size());
	for(size_t k = 0; k < bandrfhz.size(); k++)
		bandrfmhz[k] = bandrfhz[k] / 1.0e6;
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
	// geometric delay for this frame (FXSIM_DELAY path): the .calc model
	// delay at the frame start, order 1, the same per-packet call datasim
	// makes (subband.cpp updatevalues).  The model is deterministic and
	// consumes no noise stream samples, so the byte-identical path without
	// injection is untouched.
	if(model)
	{
		double tframestart = batchstartsec + (double)framecounter * framesec;
		double coeffs[3];
		if(model->calculateDelayInterpolator(scan, tframestart, framesec, 1,
		                                     antennaindex, srcindex, 1, coeffs))
		{
			delay0 = coeffs[1];               // delay at the frame start (s)
			delayrate = coeffs[0] / framesec; // s/s, linear across the frame
		}
		framecounter++;
	}

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
			if(model && bandrfmhz[band] != 0.0)
			{
				// +2*pi*f_RF*tau(t): the delayed tone, datasim-compatible
				// fringe rotation sign (applyfringerotation); tau is linear
				// within the frame, f_RF in MHz * tau in s = whole turns
				double tsec = (double)i / ratemhz[band] * 1.0e-6;
				phase += 2.0 * M_PI * bandrfmhz[band] * (delay0 + delayrate * tsec);
			}
			v += toneamp * sin(phase);
		}
		if(sigma > 0.0)
			v += sigma * gauss(engine);
		for(size_t k = 0; k < pcalmhz[band].size(); k++)
		{
			double phase = 2.0 * M_PI * pcalmhz[band][k] * (double)i / ratemhz[band];
			// 0.7 like the main tone: a 0.1 amplitude never crosses the 2-bit
			// quantisation boundaries (|2v| < 0.5 -> always the same level),
			// so the injected pcal signal vanished entirely in the quantiser
			v += 0.7 * sin(phase);
		}

		int q;
		if(adaptive)
		{
			// adaptive threshold (datasim d_tmul semantics): rescale to the
			// fixed rint quantiser's dynamic range (rms -> 0.5) and keep the
			// four-level mapping.  datasim's own 2-level x sign mapping is
			// NOT copied: it quantises weak signals less efficiently than
			// the uniform rint mapping (measured 7.6 -> 3.6 in the F=1
			// comparison), and the point here is the threshold adaptation,
			// not the level layout.
			// Statistics accumulate after quantising, capped at BSMX = 1M
			// samples; the threshold updates once per frame like datasim's
			// packet-end update.
			if(adaptsampcount < 1000000)
			{
				adaptsampcount++;
				adaptsquare += v * v;
			}
			q = quantise2bit(v / adaptthresh * 0.5);
		}
		else
			q = quantise2bit(v);
		payload[g / 4] |= q << (2 * (g % 4));
	}
	if(adaptive && adaptsampcount > 0)
		adaptthresh = sqrt(adaptsquare / (double)adaptsampcount);
}

// ---------------------------------------------------------------------------
// FreqStationGen: frequency-domain station synthesis (new path)

// deterministic seed mixer for the per-station noise streams: FNV-1a over
// the station name, mixed with the common seed and the band index so that
// (a) different stations never share a noise stream and (b) the noise does
// not depend on unrelated details of the batch
static unsigned long stationNoiseSeed(unsigned long seed, const string &station, int band)
{
	uint64_t h = 1469598103934665603ULL;   // FNV-1a 64-bit basis
	for(size_t i = 0; i < station.size(); i++)
	{
		h ^= (unsigned char)station[i];
		h *= 1099511628211ULL;
	}
	h ^= (uint64_t)seed;
	h ^= 0x9E3779B97F4A7C15ULL * (uint64_t)(band + 1);
	return (unsigned long)(h & 0xFFFFFFFFUL);
}

// rolling baseband tail in complex samples: the delay absorption shifts the
// frame window by up to a few samples per block (see class comment); the
// tail keeps the previous block's ending reachable.  4096 complex samples =
// 1 ms at 4 M complex/s, far above realistic delay rates (~us/s).
static const long long BASEBAND_TAIL = 4096;

FreqStationGen::FreqStationGen()
	: numsamps(0), slicesperblock(0), framesinblock(0), framecounter(0),
	  frameglobal(0), blockstartglobal(0), vpsamps(0), noisesigma(0.0),
	  sampletime(0.0), adaptive(false), ready(false), model(0), scan(0),
	  antennaindex(0), srcindex(0), batchstartsec(0.0), framesec(0.0)
{
}

FreqStationGen::~FreqStationGen()
{
	for(size_t b = 0; b < bands.size(); b++)
	{
		if(bands[b].planidft)
			fftwf_destroy_plan(bands[b].planidft);
		if(bands[b].planfwd)
			fftwf_destroy_plan(bands[b].planfwd);
		if(bands[b].planbwd)
			fftwf_destroy_plan(bands[b].planbwd);
		if(bands[b].planbwd2n)
			fftwf_destroy_plan(bands[b].planbwd2n);
		delete [] bands[b].temp;
		delete [] bands[b].baseband;
		delete [] bands[b].procbuf;
		delete [] bands[b].buffreqtemp;
		delete [] bands[b].realc;
	}
}

// Geometric delay injection (P2, datasim updatevalues semantics, difxio
// seconds): evaluate the .im model at each frame start, accumulate the
// fractional sample error against the linear prediction, shift a whole
// complex sample when it crosses half a sample time.
void FreqStationGen::enableDelayInjection(Model *model_, int scan_,
                                          int antennaindex_, int srcindex_,
                                          double batchstartsec_,
                                          double framesec_)
{
	model = model_;
	scan = scan_;
	antennaindex = antennaindex_;
	srcindex = srcindex_;
	batchstartsec = batchstartsec_;
	framesec = framesec_;
	// complex sample time (datasim d_sampletime = 1/bandwidth); framesec/vpsamps
	// gives the same value from the frame structure
	if(vpsamps > 0 && framesec > 0.0)
		sampletime = framesec / (double)vpsamps;
}

bool FreqStationGen::init(const vector<double> &bandfreqmhz,
                          const vector<double> &bandbwmhz,
                          const CommonSignal::Grid &grid,
                          double noisesigma_, unsigned long seed,
                          const string &station, int vpsamps_, bool adaptive_,
                          double flux, double sefd)
{
	ready = false;
	noisesigma = noisesigma_;
	adaptive = adaptive_;
	vpsamps = vpsamps_;
	if(flux < 0.0 || sefd < 0.0)
	{
		cerr << "fxcorr-sim: FXSIM_FLUX/FXSIM_SEFD must be non-negative" << endl;
		return false;
	}
	if(bandfreqmhz.empty() || bandfreqmhz.size() != bandbwmhz.size())
	{
		cerr << "fxcorr-sim: station has no recorded bands" << endl;
		return false;
	}
	if(vpsamps <= 0 || grid.slicesperblock <= 0)
	{
		cerr << "fxcorr-sim: invalid frame/block structure" << endl;
		return false;
	}
	numsamps = grid.numsamps;
	slicesperblock = grid.slicesperblock;

	bands.resize(bandfreqmhz.size());
	for(size_t b = 0; b < bandfreqmhz.size(); b++)
	{
		Band &bd = bands[b];
		double startidx = (bandfreqmhz[b] - grid.minstartfreqmhz) / grid.specresmhz;
		double blksize = bandbwmhz[b] / grid.specresmhz;
		// deriveGrid already validated this layout experiment-wide; the
		// station-side check guards against a stale grid passed by hand
		if(fabs(startidx - rint(startidx)) > 1.0e-9 ||
		   fabs(blksize - rint(blksize)) > 1.0e-9 || blksize < 4.0 ||
		   (long long)rint(startidx + blksize) > (long long)grid.numsamps)
		{
			cerr << "fxcorr-sim: band " << bandfreqmhz[b] << " MHz (bw "
			     << bandbwmhz[b] << " MHz) does not fit the common signal grid" << endl;
			return false;
		}
		bd.startidx = (long long)rint(startidx);
		bd.blksize = (int)rint(blksize);
		bd.flux = flux;
		bd.sefd = sefd;
		bd.startfreqmhz = bandfreqmhz[b];
		// a frame must be a whole number of slices so frame/block and
		// frame/slice boundaries coincide
		if(vpsamps % bd.blksize != 0)
		{
			cerr << "fxcorr-sim: frame (" << vpsamps
			     << " complex samples) is not a whole number of slices (" << bd.blksize
			     << ")" << endl;
			return false;
		}
		// frequency-domain gain: the real frame samples must come out with
		// rms 0.5 for the fixed rint quantiser (see class comment for the
		// gain budget).  P0 (flux <= 0): sigma enters as the noise power
		// added to the rms-1 common signal.  P2 (flux > 0): the datasim
		// chain normalises by sqrt(F+SEFD) itself, so the gain drops that
		// factor (the two paths agree at flux=1, sefd=0, sigma=0).
		if(flux > 0.0)
			bd.scale = 0.5 / (sqrt(flux + sefd) * sqrt((double)bd.blksize));
		else
			bd.scale = 0.5 / (sqrt(1.0 + noisesigma * noisesigma) * sqrt((double)bd.blksize));

		bd.temp = new fftwf_complex[bd.blksize];
		bd.baseband = new fftwf_complex[(size_t)((long long)bd.blksize * grid.slicesperblock + BASEBAND_TAIL)];
		bd.procbuf = new fftwf_complex[vpsamps];
		bd.buffreqtemp = new fftwf_complex[2 * vpsamps];
		bd.realc = new fftwf_complex[2 * vpsamps];
		bd.planidft = fftwf_plan_dft_1d(bd.blksize, bd.temp, bd.temp,
		                                FFTW_BACKWARD, FFTW_ESTIMATE);
		// both vpsamps plans are in-place over procbuf; the forward transform
		// is unnormalised here, 1/N is applied by hand (datasim's
		// IPP_FFT_DIV_INV_BY_N semantics mapped to fftw)
		bd.planfwd = fftwf_plan_dft_1d(vpsamps, bd.procbuf, bd.procbuf,
		                               FFTW_FORWARD, FFTW_ESTIMATE);
		bd.planbwd = fftwf_plan_dft_1d(vpsamps, bd.procbuf, bd.procbuf,
		                               FFTW_BACKWARD, FFTW_ESTIMATE);
		bd.planbwd2n = fftwf_plan_dft_1d(2 * vpsamps, bd.buffreqtemp, bd.realc,
		                                 FFTW_BACKWARD, FFTW_ESTIMATE);
		if(!bd.planidft || !bd.planfwd || !bd.planbwd || !bd.planbwd2n)
		{
			cerr << "fxcorr-sim: cannot create fftw plans" << endl;
			return false;
		}
		bd.engine.seed((unsigned)stationNoiseSeed(seed, station, (int)b));
		bd.sampcount = 0;
		bd.square = 0.0;
		bd.thresh = 1.0;   // datasim d_tmul initial value
		// delay injection state, all zero until enableDelayInjection() runs
		bd.fracerr = 0.0;
		bd.shift = 0;
		bd.delay0 = 0.0;
		bd.delayrate = 0.0;
		bd.prevdelay = 0.0;
		bd.prevrate = 0.0;
	}
	framesinblock = 0;
	framecounter = 0;
	frameglobal = 0;
	blockstartglobal = 0;
	ready = true;
	return true;
}

bool FreqStationGen::processBlock(const float *blockdata, long long nfloats)
{
	if(!ready)
		return false;
	if(nfloats <= 0 || nfloats % (2LL * numsamps) != 0)
	{
		cerr << "fxcorr-sim: corrupt common signal block (length " << nfloats << ")" << endl;
		return false;
	}
	long long nslices = nfloats / (2LL * numsamps);

	// rolling buffer: keep the previous block's tail reachable for the delay
	// absorption window; the first block's tail is zero (frames never reach
	// back before the batch start with positive delays)
	for(size_t b = 0; b < bands.size(); b++)
	{
		Band &bd = bands[b];
		if(blockstartglobal == 0)
			memset(bd.baseband, 0, (size_t)BASEBAND_TAIL * sizeof(fftwf_complex));
		else
			memmove(bd.baseband,
			        bd.baseband + (long long)bd.blksize * slicesperblock,
			        (size_t)BASEBAND_TAIL * sizeof(fftwf_complex));
	}

	// per slice: cut this station's band, add station noise, scale, Ormsby,
	// inverse DFT to the complex baseband stream.  flux > 0 runs the datasim
	// fabricatedata chain (x sqrt(F), + sqrt(SEFD) gencplx noise, then the
	// / sqrt(F+SEFD) normalisation folded into scale); flux <= 0 keeps the
	// P0 noisesigma path.
	for(long long s = 0; s < nslices; s++)
	{
		const float *src = blockdata + s * 2LL * numsamps;
		for(size_t b = 0; b < bands.size(); b++)
		{
			Band &bd = bands[b];
			const float *seg = src + 2 * bd.startidx;
			if(bd.flux > 0.0)
			{
				double sqf = sqrt(bd.flux);
				double sqn = sqrt(bd.sefd);
				for(int k = 0; k < bd.blksize; k++)
				{
					bd.temp[k][0] = (float)(seg[2 * k] * sqf);
					bd.temp[k][1] = (float)(seg[2 * k + 1] * sqf);
					if(bd.sefd > 0.0)
					{
						bd.temp[k][0] += (float)(sqn * bd.gauss(bd.engine));
						bd.temp[k][1] += (float)(sqn * bd.gauss(bd.engine));
					}
					bd.temp[k][0] *= (float)bd.scale;
					bd.temp[k][1] *= (float)bd.scale;
				}
			}
			else
			{
				for(int k = 0; k < bd.blksize; k++)
				{
					bd.temp[k][0] = seg[2 * k];
					bd.temp[k][1] = seg[2 * k + 1];
					if(noisesigma > 0.0)
					{
						bd.temp[k][0] += (float)(noisesigma * bd.gauss(bd.engine));
						bd.temp[k][1] += (float)(noisesigma * bd.gauss(bd.engine));
					}
					bd.temp[k][0] *= (float)bd.scale;
					bd.temp[k][1] *= (float)bd.scale;
				}
			}
			// Ormsby band edges 0, 1/2, 4/5, 1, ..., 1, 4/5, 1/2 (datasim
			// applyfilter: only the five edge points differ from 1)
			bd.temp[0][0] = 0.0f;
			bd.temp[0][1] = 0.0f;
			bd.temp[1][0] *= 0.5f;
			bd.temp[1][1] *= 0.5f;
			bd.temp[2][0] *= 0.8f;
			bd.temp[2][1] *= 0.8f;
			bd.temp[bd.blksize - 1][0] *= 0.5f;
			bd.temp[bd.blksize - 1][1] *= 0.5f;
			bd.temp[bd.blksize - 2][0] *= 0.8f;
			bd.temp[bd.blksize - 2][1] *= 0.8f;

			// unnormalised inverse DFT (datasim ippsDFTInv semantics)
			fftwf_execute(bd.planidft);
			memcpy(bd.baseband + BASEBAND_TAIL + s * bd.blksize, bd.temp,
			       (size_t)bd.blksize * sizeof(fftwf_complex));
		}
	}
	if(nslices * (long long)bands[0].blksize % vpsamps != 0)
	{
		cerr << "fxcorr-sim: block (" << nslices << " slices) is not a whole "
		        "number of frames" << endl;
		return false;
	}
	framesinblock = nslices * bands[0].blksize / vpsamps;
	framecounter = 0;
	// blockstartglobal stays at this block's start while its frames are
	// filled; it advances when the block's last frame is done (see
	// fillFramePayload)
	return true;
}

void FreqStationGen::fillFramePayload(unsigned char *payload, int payloadbytes, int nbands)
{
	if(nbands != (int)bands.size() || framecounter >= framesinblock)
	{
		cerr << "fxcorr-sim: frame payload mismatch (bands/frame counter)" << endl;
		return;
	}
	// bytes per band per frame = vpsamps/2 (2-bit real, 4 samples/byte)
	if(payloadbytes != vpsamps * nbands / 2)
	{
		cerr << "fxcorr-sim: frame payload size mismatch" << endl;
		return;
	}
	double invN = 1.0 / (double)vpsamps;

	for(int b = 0; b < nbands; b++)
	{
		Band &bd = bands[b];
		// frame chain mirroring datasim subband.cpp processdata:
		//   DFT(/N) -> DC zero -> fracsample correction (freq domain) ->
		//   IDFT -> fringe rotation (time domain) -> DFT(/N) ->
		//   Hermitian 2N -> 2N IDFT
		//
		// updatevalues first (datasim): evaluate the delay model at the frame
		// start, accumulate the fractional sample error against the linear
		// prediction and shift a whole complex sample past half a sample time
		if(model)
		{
			double tframe = batchstartsec + (double)frameglobal * framesec +
			                (double)bd.shift * sampletime;
			double coeffs[3];
			if(!model->calculateDelayInterpolator(scan, tframe, framesec, 1,
			                                      antennaindex, srcindex, 1, coeffs))
			{
				// keep the previous frame's values, linearly extrapolated; a
				// whole frame of zeroed payload would corrupt the output
				cerr << "fxcorr-sim: delay model evaluation failed at frame "
				     << frameglobal << ", using previous delay" << endl;
				coeffs[0] = bd.prevrate;
				coeffs[1] = bd.prevdelay + bd.prevrate;
			}
			bd.fracerr += coeffs[1] - (bd.prevdelay + bd.prevrate);
			if(bd.fracerr > 0.5 * sampletime)
			{
				bd.shift++;
				bd.fracerr -= sampletime;
			}
			if(bd.fracerr < -0.5 * sampletime)
			{
				bd.shift--;
				bd.fracerr += sampletime;
			}
			bd.prevdelay = coeffs[1];           // delay at the frame start (s)
			bd.prevrate = coeffs[0];            // change across the frame (s)
			bd.delay0 = coeffs[1];
			bd.delayrate = coeffs[0] / framesec;
		}

		// frame window at the accumulated whole-sample shift inside the
		// rolling baseband buffer (tail keeps the previous block reachable);
		// clamped at the buffer edges (a delay absorption window wider than
		// the tail only arises after hours of unrealistically fast delay)
		long long fstart = frameglobal * (long long)vpsamps + bd.shift -
		                   blockstartglobal + BASEBAND_TAIL;
		long long baselen = (long long)bd.blksize * slicesperblock + BASEBAND_TAIL;
		if(fstart < 0)
			fstart = 0;
		if(fstart + vpsamps > baselen)
			fstart = baselen - vpsamps;
		memcpy(bd.procbuf, bd.baseband + fstart,
		       (size_t)vpsamps * sizeof(fftwf_complex));
		fftwf_execute(bd.planfwd);
		for(int k = 0; k < vpsamps; k++)
		{
			bd.procbuf[k][0] *= (float)invN;
			bd.procbuf[k][1] *= (float)invN;
		}
		bd.procbuf[0][0] = 0.0f;   // DC zero (datasim: manual, asserted later)
		bd.procbuf[0][1] = 0.0f;
		// fractional sample correction (datasim applyfracsamperrcorrection):
		// e^{j*2*pi*bandwidth*idx/vpsamps*fracerr}, bandwidth = complex rate
		if(model)
		{
			double arg = 2.0 * M_PI / framesec;   // 2*pi*bandwidth/vpsamps
			for(int k = 0; k < vpsamps; k++)
			{
				double ph = arg * (double)k * bd.fracerr;
				float c = (float)cos(ph), s = (float)sin(ph);
				float re = bd.procbuf[k][0], im = bd.procbuf[k][1];
				bd.procbuf[k][0] = re * c - im * s;
				bd.procbuf[k][1] = re * s + im * c;
			}
		}
		fftwf_execute(bd.planbwd);
		// fringe rotation (datasim applyfringerotation): the carrier phase at
		// the band start frequency, fractional part to the nearest integer
		// (datasim fraction_of = val - rint(val - 0.5)); phase in (-pi, pi]
		if(model)
		{
			double freqhz = bd.startfreqmhz * 1.0e6;
			for(int k = 0; k < vpsamps; k++)
			{
				double t = bd.delay0 + bd.delayrate * (double)k / (double)vpsamps;
				double ft = freqhz * t;
				double ph = 2.0 * M_PI * (ft - rint(ft - 0.5));
				float c = (float)cos(ph), s = (float)sin(ph);
				float re = bd.procbuf[k][0], im = bd.procbuf[k][1];
				bd.procbuf[k][0] = re * c - im * s;
				bd.procbuf[k][1] = re * s + im * c;
			}
		}
		fftwf_execute(bd.planfwd);
		for(int k = 0; k < vpsamps; k++)
		{
			bd.procbuf[k][0] *= (float)invN;
			bd.procbuf[k][1] *= (float)invN;
		}
		// Hermitian extension; DC and Nyquist zeroed (datasim
		// fillBuffreqtemp, minus its off-by-one write past Y[2N])
		for(int k = 0; k < vpsamps; k++)
		{
			bd.buffreqtemp[k][0] = bd.procbuf[k][0];
			bd.buffreqtemp[k][1] = bd.procbuf[k][1];
		}
		for(int k = 1; k < vpsamps; k++)
		{
			bd.buffreqtemp[2 * vpsamps - k][0] = bd.procbuf[k][0];
			bd.buffreqtemp[2 * vpsamps - k][1] = -bd.procbuf[k][1];
		}
		bd.buffreqtemp[0][0] = bd.buffreqtemp[0][1] = 0.0f;
		bd.buffreqtemp[vpsamps][0] = bd.buffreqtemp[vpsamps][1] = 0.0f;
		fftwf_execute(bd.planbwd2n);

		// the imaginary part of the 2N IDFT output is ~0 (the Hermitian
		// symmetry of a real signal); take the real part, quantise and pack
		// sample-wise interleaved across bands (same layout as
		// SignalGen::fillFramePayload)
		for(int m = 0; m < 2 * vpsamps; m++)
		{
			double v = bd.realc[m][0];
			int q;
			if(adaptive)
			{
				if(bd.sampcount < 1000000)
				{
					bd.sampcount++;
					bd.square += v * v;
				}
				q = quantise2bit(v / bd.thresh * 0.5);
			}
			else
				q = quantise2bit(v);
			int g = m * nbands + b;
			payload[g / 4] |= q << (2 * (g % 4));
		}
		if(adaptive && bd.sampcount > 0)
			bd.thresh = sqrt(bd.square / (double)bd.sampcount);
	}
	framecounter++;
	frameglobal++;
	if(framecounter >= framesinblock)
		blockstartglobal += framesinblock * (long long)vpsamps;
}
