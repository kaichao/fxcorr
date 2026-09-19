#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <fxcorrcommon/configuration.h>

#include "commonsignal.h"
#include "signalgen.h"
#include "vdifwriter.h"

using namespace std;

// VDIF epoch 32 = 2000-01-01, MJD 51544
#define VDIF_EPOCH_MJD 51544.0

// Batch context loaded from batches/<batch_id>.json (D9; pre-written by the
// orchestration scripts, same fields fxcorr-f reads)
struct BatchInfo
{
	string workdir;
	string batchid;
	double startmjd;
	int nsubints;
	string inputfile;
};

// Everything derived from .input for one station (frame structure, bands,
// frame-aligned batch start); shared by the new frequency-domain path and
// the legacy time-domain path, plus the legacy-only extras.
struct StationSetup
{
	string station;
	int dsindex;
	int nbands;
	vector<double> bandfreqmhz;     // band start frequencies, MHz (FREQ table)
	vector<double> bandbwmhz;       // band bandwidths, MHz
	long long ratehz;               // per-band sample rate, Hz (2x the bandwidth)
	long long nsampframe;           // real samples per band per frame
	int framespersecond;
	long long framens;              // frame duration in ns
	int bytesperbandframe;
	int subintns;
	long long nframestotal;         // frames to generate (batch rounded up)
	long long startsec;             // VDIF epoch seconds at the batch start
	long long framestart;           // in-second frame offset of the batch start
	// legacy-only
	vector<double> tonehz;          // per-band baseband tone, Hz (0 = none)
	vector<vector<double> > pcalhz;
	vector<double> tonerfhz;        // per-band tone RF frequency, Hz
	double batchstartscanrel;       // batch start in scan-relative seconds
	int srcindex;
};

static void usage()
{
	cerr << "usage:\n"
	     << "  fxcorr-sim common  <batch_id> [workdir]\n"
	     << "      generate the shared common signal only (common/<batch_id>/)\n"
	     << "  fxcorr-sim station <batch_id> <station> [workdir] [tone_mhz ...]\n"
	     << "      read the common signal, generate one station's VDIF;\n"
	     << "      one tone value applies to all bands, nbands values apply band\n"
	     << "      by band; any tone argument switches to the legacy time-domain\n"
	     << "      synthesis path (byte-comparison regression only)\n"
	     << "  fxcorr-sim         <batch_id> [workdir]\n"
	     << "      serial: common + every station in .input (single machine only)\n"
	     << "  env (new path): FXSIM_NOISE (default 0.02, 0 disables), FXSIM_SEED,\n"
	     << "      FXSIM_ADAPTIVE (1 = running-rms quantiser), FXSIM_SPECRES\n"
	     << "      (grid scaling factor, positive integer), FXSIM_LINE (spectral\n"
	     << "      line freq,amp,rms - freq in MHz, rms in grid points),\n"
	     << "      FXSIM_FLUX/FXSIM_SEFD (Jy; flux > 0 enables the datasim\n"
	     << "      scaling chain x sqrt(F) + sqrt(SEFD) noise / sqrt(F+SEFD),\n"
	     << "      replacing FXSIM_NOISE; SEFD is one value or a per-station\n"
	     << "      comma list, default 1000), FXSIM_DELAY (0 = disable the full\n"
	     << "      delay chain: model delay + fractional sample correction +\n"
	     << "      fringe rotation, on by default), FXSIM_GAPS (recording\n"
	     << "      interruptions, for the t25362 regression: comma-separated\n"
	     << "      <sec>:<frames>[:f[<fillframes>]] loses that many frame\n"
	     << "      numbers at that second into the batch; the optional 'f'\n"
	     << "      writes all-zero-header filler frames in their place instead\n"
	     << "      of leaving the file short, one per lost frame unless a count\n"
	     << "      follows it ('f300'); 'p'/'h' write placeholders made of\n"
	     << "      vdifio's FILL_PATTERN (0x11223344) instead, whole frame or\n"
	     << "      leading four bytes, the two forms vdifmux skips by different\n"
	     << "      amounts - a real interruption shows both forms\n"
	     << "      across datastreams, and t25362's have far more filler frames\n"
	     << "      than lost ones), FXSIM_STARTOFFSET (frames; the recorder\n"
		     << "      began that many frames after the batch start, so the file\n"
		     << "      begins late and its first frame carries the later\n"
		     << "      timestamp - t25362's BA starts 1269 frames in),\n"
		     << "      FXCORR_WORKDIR\n"
	     << "  env (legacy path): FXSIM_DELAY (1 = inject .calc geometric delay\n"
	     << "      into the tone phase), FXSIM_FLUX/FXSIM_SEFD (Jy; both set =\n"
	     << "      SNR scaling), plus the new-path variables above\n";
}

// batch.json field extraction, shared with CommonSignal::Reader
static bool loadBatchInfo(const string &workdir, const string &batchid, BatchInfo *bi)
{
	bi->workdir = workdir;
	bi->batchid = batchid;
	string path = workdir + "/batches/" + batchid + ".json";
	ifstream jf(path.c_str());
	if(!jf.is_open())
	{
		cerr << "fxcorr-sim: cannot open " << path << endl;
		return false;
	}
	stringstream jss;
	jss << jf.rdbuf();
	string json = jss.str();
	jf.close();
	if(!CommonSignal::Reader::extractJsonDouble(json, "start_mjd", &bi->startmjd) ||
	   !CommonSignal::Reader::extractJsonInt(json, "n_subints", &bi->nsubints) ||
	   !CommonSignal::Reader::extractJsonString(json, "config_file", &bi->inputfile))
	{
		cerr << "fxcorr-sim: batch.json missing required fields" << endl;
		return false;
	}
	return true;
}

// VDIF frame structure from .input (not from the band bandwidth: a 4 MHz
// band is recorded at 8 Ms/s); same derivation as fxcorr-f datareader.cpp
static bool deriveFrame(Configuration &config, int dsindex, int *bytesperbandframe,
                        int *framespersecond, long long *ratehz, long long *nsampframe,
                        long long *framens)
{
	*bytesperbandframe = config.getFramePayloadBytes(0, dsindex);
	*framespersecond = config.getFramesPerSecond(0, dsindex);
	if(*bytesperbandframe <= 0 || *framespersecond <= 0)
	{
		cerr << "fxcorr-sim: cannot derive frame structure from .input" << endl;
		return false;
	}
	*nsampframe = *bytesperbandframe * 4;
	*ratehz = *nsampframe * (long long)*framespersecond;
	*framens = 1000000000LL / (long long)*framespersecond;
	if(*framens <= 0 || *framens * *framespersecond != 1000000000LL)
	{
		cerr << "fxcorr-sim: frame duration is not an integer number of ns at "
		     << *ratehz << " Hz" << endl;
		return false;
	}
	return true;
}

// All per-station checks (sampling mode, band count, 2-bit, frame structure,
// subint/frame-aligned batch start) plus the legacy tone/pcal/delay extras.
// Shared by both synthesis paths so the two produce identical VDIF framing.
static bool setupStation(Configuration &config, Model *model, const BatchInfo &bi,
                         const string &station, bool legacy,
                         const vector<double> &tonemhzarg, StationSetup *st)
{
	st->station = station;
	st->dsindex = -1;
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		if(config.getDStationName(0, d) == station)
		{
			st->dsindex = d;
			break;
		}
	}
	if(st->dsindex < 0)
	{
		cerr << "fxcorr-sim: station " << station << " not found in .input" << endl;
		return false;
	}

	// V1: real (baseband) sampling and one shared sample rate across bands
	if(config.getDSampling(0, st->dsindex) == Configuration::COMPLEX)
	{
		cerr << "fxcorr-sim: V1 supports real (baseband) sampling only" << endl;
		return false;
	}
	// band count, not distinct-frequency count: dual-pol setups record two
	// bands (R/L) at the same frequency, which numrecordedfreqs would count once
	st->nbands = config.getDNumRecordedBands(0, st->dsindex);
	if(st->nbands < 1)
	{
		cerr << "fxcorr-sim: station " << station << " has no recorded bands" << endl;
		return false;
	}
	for(int b = 0; b < st->nbands; b++)
	{
		int fq = config.getDRecordedFreqIndex(0, st->dsindex, b);
		// the FREQ table stores MHz (BW (MHZ) parsed with atof)
		st->bandfreqmhz.push_back(config.getFreqTableFreq(fq));
		st->bandbwmhz.push_back(config.getFreqTableBandwidth(fq));
	}

	// V1: 2-bit samples (4 samples per byte, per band); the .input field
	// counts all bands, so num/denom = nbands/4
	if(config.getDBytesPerSampleNum(0, st->dsindex) * 4 !=
	   config.getDBytesPerSampleDenom(0, st->dsindex) * st->nbands)
	{
		cerr << "fxcorr-sim: V1 supports 2-bit samples only" << endl;
		return false;
	}

	if(!deriveFrame(config, st->dsindex, &st->bytesperbandframe,
	                &st->framespersecond, &st->ratehz, &st->nsampframe, &st->framens))
		return false;
	// getFramePayloadBytes is the whole-frame payload across all bands; the
	// fields above are per-band quantities (single-band setups hide this)
	st->bytesperbandframe /= st->nbands;
	st->nsampframe /= st->nbands;
	st->ratehz /= st->nbands;

	// batch start must lie on a subint boundary (data-spec section 12);
	// tolerance absorbs the f64 representation error of start_mjd (~1 us)
	st->subintns = config.getSubintNS(0);
	long long durationns = (long long)bi.nsubints * (long long)st->subintns;
	{
		double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds() / 86400.0;
		long long batchstartns = (long long)floor((bi.startmjd - jobstart) * 86400.0 * 1.0e9 + 0.5);
		if(batchstartns % st->subintns > 1000 && (st->subintns - batchstartns % st->subintns) > 1000)
		{
			cerr << "fxcorr-sim: batch start " << bi.startmjd << " is not on a subint boundary ("
			     << batchstartns % st->subintns << " ns into a " << st->subintns << " ns subint)" << endl;
			return false;
		}
	}

	// VDIF frame numbers wrap at each whole second (0..fps-1); the batch may
	// start at any frame boundary within a second.  The tolerance absorbs the
	// f64 representation error of start_mjd (~100s of ns at these epochs).
	long long startns = (long long)((bi.startmjd - VDIF_EPOCH_MJD) * 86400.0 * 1.0e9 + 0.5);
	st->startsec = startns / 1000000000LL;
	long long secns = startns - st->startsec * 1000000000LL;
	{
		// a start within 1 us of a whole second snaps to the whole second,
		// keeping frame numbering from 0 there (the historical behaviour the
		// byte-comparison tests rely on); anything else must be on a frame
		// boundary
		if(secns < 1000)
			secns = 0;
		else if(1000000000LL - secns < 1000)
		{
			secns = 0;
			st->startsec += 1;
		}
		long long rem = secns % st->framens;
		if(rem > 1000 && st->framens - rem > 1000)
		{
			cerr << "fxcorr-sim: batch start is not on a frame boundary" << endl;
			return false;
		}
		secns -= rem;                      // snap to the frame boundary
	}
	st->framestart = secns / st->framens;
	// The batch duration need not be an integer number of frames: the file
	// runs to the next frame boundary and fxcorr-f reads only the batch span.
	st->nframestotal = (durationns + st->framens - 1) / st->framens;

	// delay model context (new path P2 and the legacy FXSIM_DELAY path):
	// the phase-centre choice and the batch start in scan-relative seconds
	// (the frame the delay model expects, same as fxcorr-f datareader.cpp)
	st->batchstartscanrel = 0.0;
	st->srcindex = 0;
	if(model->getNumPhaseCentres(0) == 1 && !model->isPointingCentreCorrelated(0))
		st->srcindex = 1;
	{
		long long scanstartsec = (long long)model->getScanStartSec(0, config.getStartMJD(), config.getStartSeconds());
		double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds() / 86400.0;
		long long batchstartns = (long long)floor((bi.startmjd - jobstart) * 86400.0 * 1.0e9 + 0.5);
		st->batchstartscanrel = (double)(batchstartns - scanstartsec * 1000000000LL) / 1.0e9;
	}

	// pcal tones from the .input PHASE CAL config, converted to baseband
	// frequencies (the same grid configuration.cpp counts when it reads
	// .input); shared by both paths - the legacy time-domain injection and
	// the new frequency-domain injection in FreqStationGen::fillFramePayload
	st->pcalhz.assign(st->nbands, vector<double>());
	if(config.getDPhaseCalIntervalHz(0, st->dsindex) > 0)
	{
		for(int b = 0; b < st->nbands; b++)
		{
			int fq = config.getDRecordedFreqIndex(0, st->dsindex, b);
			double bandedge = config.getFreqTableFreq(fq) * 1.0e6;   // MHz -> Hz
			bool lsb = config.getFreqTableLowerSideband(fq);
			int nt = config.getDRecordedFreqNumPCalTones(0, st->dsindex, b);
			for(int k = 0; k < nt; k++)
			{
				double rf = config.getDRecordedFreqPCalToneFreqHz(0, st->dsindex, b, k);
				double base = lsb ? (bandedge - rf) : (rf - bandedge);
				if(base >= 0.0 && base < st->bandbwmhz[b] * 1.0e6)
					st->pcalhz[b].push_back(base);
			}
		}
	}

	if(!legacy)
		return true;

	// --- legacy extras: tone and the FXSIM_DELAY model context ---

	// tones: 0 values = no tone, 1 value = all bands, nbands values = per band
	st->tonehz.assign(st->nbands, 0.0);
	if(tonemhzarg.size() == 1)
	{
		for(int b = 0; b < st->nbands; b++)
			st->tonehz[b] = tonemhzarg[0] * 1.0e6;
	}
	else if(tonemhzarg.size() == (size_t)st->nbands)
	{
		for(int b = 0; b < st->nbands; b++)
			st->tonehz[b] = tonemhzarg[b] * 1.0e6;
	}
	else if(tonemhzarg.size() > 0)
	{
		cerr << "fxcorr-sim: tone count must be 0, 1 or the band count (" << st->nbands
		     << "), got " << tonemhzarg.size() << endl;
		return false;
	}

	// FXSIM_DELAY (legacy): per-band tone RF frequency (band edge +/- the
	// baseband tone offset, sideband-corrected) for the tone-phase delay
	// injection; the shared delay model context is set above
	st->tonerfhz.assign(st->nbands, 0.0);
	if(getenv("FXSIM_DELAY") && strcmp(getenv("FXSIM_DELAY"), "1") == 0)
	{
		for(int b = 0; b < st->nbands; b++)
		{
			int fq = config.getDRecordedFreqIndex(0, st->dsindex, b);
			double bandedge = config.getFreqTableFreq(fq) * 1.0e6;   // Hz
			st->tonerfhz[b] = config.getFreqTableLowerSideband(fq) ?
			              bandedge - st->tonehz[b] : bandedge + st->tonehz[b];
		}
	}
	return true;
}

// ---- common subcommand: derive the grid and write the shared signal ----

// FXSIM_SPECRES / FXSIM_LINE parsing, shared by the common and station
// entries so both derive the same grid (a station's expected grid must
// match the one the common signal was generated with)
static int parseSpecres()
{
	int specres = 1;
	if(const char *sre = getenv("FXSIM_SPECRES"))
	{
		char *end;
		long v = strtol(sre, &end, 10);
		if(end == sre || *end != '\0' || v < 1)
		{
			cerr << "fxcorr-sim: FXSIM_SPECRES must be a positive integer" << endl;
			return -1;
		}
		specres = (int)v;
	}
	return specres;
}

static int parseLine(CommonSignal::LineSpec *line)
{
	if(const char *le = getenv("FXSIM_LINE"))
	{
		if(sscanf(le, "%lf,%lf,%lf", &line->freqmhz, &line->amp, &line->rms) != 3)
		{
			cerr << "fxcorr-sim: FXSIM_LINE must be freq,amp,rms (MHz)" << endl;
			return -1;
		}
	}
	return 0;
}

// FXSIM_GAPS: recording interruptions, for the t25362 regression (data-spec
// 5.2).  Comma-separated "<sec>:<frames>[:f[<fillframes>]]" where sec is
// seconds into the batch and frames is how many frame numbers are lost there;
// a trailing "f" makes the recorder write all-zero-header filler frames in
// their place instead of leaving the file short, and an optional count after
// it sets how many (default: one per lost frame).  A real observation carries
// BOTH forms at the same interruption -- one datastream filled, the rest
// simply short -- so covering fxcorr-f's gap and filler corrections means
// running a station twice with the two forms.
//   FXSIM_GAPS="2.5:10:f,4.1:63:f"       two interruptions, filler form
//   FXSIM_GAPS="2.5:10,4.1:63"           same two, plain missing-frame form
//   FXSIM_GAPS="2.5:10:f300,4.1:63:f300" same gaps, 300 filler frames each
//
// The last form is t25362's own shape and the reason the count is independent
// of `frames`: its interruptions wrote far more filler than the frame numbers
// they cost (81..508 filler frames against 10..63 lost), which is what makes
// the filler correction large enough for the read position to misplace.
static int applyGaps(VDIFWriter &writer)
{
	const char *ge = getenv("FXSIM_GAPS");
	if(!ge || !*ge)
		return 0;
	string spec(ge);
	size_t pos = 0;
	while(pos <= spec.size())
	{
		size_t comma = spec.find(',', pos);
		string item = spec.substr(pos, comma == string::npos ? string::npos : comma - pos);
		pos = (comma == string::npos) ? spec.size() + 1 : comma + 1;
		if(item.empty())
			continue;
		size_t c1 = item.find(':');
		size_t c2 = (c1 == string::npos) ? string::npos : item.find(':', c1 + 1);
		if(c1 == string::npos)
		{
			cerr << "fxcorr-sim: FXSIM_GAPS item '" << item
			     << "' must be <sec>:<frames>[:f]" << endl;
			return -1;
		}
		double atsec = atof(item.substr(0, c1).c_str());
		string framestr = item.substr(c1 + 1, (c2 == string::npos) ? string::npos : c2 - c1 - 1);
		long long missing = atoll(framestr.c_str());
		long long fillerframes = 0;
		int fillform = VDIFWriter::FILL_ZERO;
		if(c2 != string::npos)
		{
			string form = item.substr(c2 + 1);
			if(form.empty())
			{
				cerr << "fxcorr-sim: FXSIM_GAPS item '" << item << "' has an empty form" << endl;
				return -1;
			}
			// f = all-zero placeholder (t25362's form), p = whole-frame
			// FILL_PATTERN, h = only its leading four bytes.  The two pattern
			// forms exist because vdifmux tests those two places separately
			// and skips different amounts for them (vdifmux.c:598/606) --
			// fxcorr/test/gaps/run_pattern.sh drives both.
			char fc = form[0];
			if(fc == 'F') fc = 'f';
			else if(fc == 'P') fc = 'p';
			else if(fc == 'H') fc = 'h';
			if(fc == 'f')
				fillform = VDIFWriter::FILL_ZERO;
			else if(fc == 'p')
				fillform = VDIFWriter::FILL_PATTERN;
			else if(fc == 'h')
				fillform = VDIFWriter::FILL_PATTERN_HEAD;
			else
			{
				cerr << "fxcorr-sim: FXSIM_GAPS item '" << item << "' has unknown form '"
				     << form << "' (defined: 'f' all-zero filler, 'p' whole-frame "
				     << "FILL_PATTERN, 'h' leading-four-bytes pattern, each optionally "
				     << "followed by a frame count)" << endl;
				return -1;
			}
			// bare letter: one filler frame per lost frame (the original form);
			// "<letter>N": N filler frames regardless of how many were lost
			fillerframes = (form.size() > 1) ? atoll(form.c_str() + 1) : missing;
			if(fillerframes < 0)
			{
				cerr << "fxcorr-sim: FXSIM_GAPS item '" << item
				     << "' has a negative filler count" << endl;
				return -1;
			}
		}
		if(atsec < 0.0 || missing <= 0)
		{
			cerr << "fxcorr-sim: FXSIM_GAPS item '" << item
			     << "' needs seconds >= 0 and frames > 0" << endl;
			return -1;
		}
		writer.addGap(atsec, missing, fillerframes, fillform);
	}
	return 0;
}

// FXSIM_STARTOFFSET: the recorder began this many frames after the batch start,
// so the file begins late -- the third form real recordings show, alongside a
// short file (frame numbers missing) and a long one (filler frames).  t25362's
// BA starts 79.3 ms / 1269 frames into its batch, and its effect is the A class
// in reader-model.md 4.1: every time the reader maps time onto bytes it lands
// that far off, and the arithmetic that turns the first frame's timestamp into
// anchorbytes is where a whole-frame error (A2: truncation instead of floor)
// flips the odd pcal tones.  Until now those three defects were only ever
// verified on the real observation, so nothing caught a regression (v4-plan.md
// A2).  Unset = the file starts exactly at the batch start, as before.
static int applyFrameOffset(VDIFWriter &writer)
{
	const char *oe = getenv("FXSIM_STARTOFFSET");
	if(!oe || !*oe)
		return 0;
	long long frames = atoll(oe);
	if(frames < 0)
	{
		cerr << "fxcorr-sim: FXSIM_STARTOFFSET must be >= 0 (got " << frames << ")" << endl;
		return -1;
	}
	writer.setFileStartOffset(frames);
	return 0;
}

static int doCommon(Configuration &config, const BatchInfo &bi)
{
	int specres = parseSpecres();
	if(specres < 0)
		return EXIT_FAILURE;
	CommonSignal::LineSpec line;
	if(parseLine(&line) != 0)
		return EXIT_FAILURE;

	CommonSignal::Grid grid;
	if(!CommonSignal::deriveGrid(config, &grid, specres))
		return EXIT_FAILURE;

	// total slice count: the batch duration rounded up to a frame boundary,
	// the maximum over stations (the common signal covers the whole batch);
	// each frame must be a whole number of slices
	long long totalslices = 0;
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		int bpf, fps;
		long long ratehz, nsampframe, framens;
		if(!deriveFrame(config, d, &bpf, &fps, &ratehz, &nsampframe, &framens))
			return EXIT_FAILURE;
		double slicesperframe = (double)framens / (grid.stimeus * 1000.0);
		if(fabs(slicesperframe - rint(slicesperframe)) > 1.0e-9)
		{
			cerr << "fxcorr-sim: frame duration " << framens
			     << " ns is not a whole number of " << grid.stimeus
			     << " us slices" << endl;
			return EXIT_FAILURE;
		}
		long long durationns = (long long)bi.nsubints * (long long)config.getSubintNS(0);
		long long nframes = (durationns + framens - 1) / framens;
		long long slices = nframes * (long long)rint(slicesperframe);
		if(slices > totalslices)
			totalslices = slices;
	}
	if(totalslices <= 0)
	{
		cerr << "fxcorr-sim: empty batch" << endl;
		return EXIT_FAILURE;
	}

	unsigned long seed = 20260912UL;
	if(const char *sd = getenv("FXSIM_SEED"))
		seed = strtoul(sd, 0, 10);

	if(!CommonSignal::generate(grid, totalslices, seed, bi.workdir, bi.batchid, bi.startmjd,
	                            line))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

// ---- station subcommand, new path: read the common signal, synth + pack ----

static int doStationNew(Configuration &config, Model *model, const BatchInfo &bi,
                        const StationSetup &st)
{
	// full delay chain (P2): enabled by default (datasim semantics, the
	// complete V_i = S(t - tau) e^{j phi} model); FXSIM_DELAY=0 restores the
	// delay-free identity chain
	bool dodgen = true;
	if(getenv("FXSIM_DELAY") && strcmp(getenv("FXSIM_DELAY"), "0") == 0)
		dodgen = false;
	double noisesigma = 0.02;
	bool noiseexplicit = false;
	if(const char *ns = getenv("FXSIM_NOISE"))
	{
		noisesigma = atof(ns);
		noiseexplicit = true;
	}

	// FXSIM_FLUX / FXSIM_SEFD (P2, datasim fabricatedata chain): flux > 0
	// enables the chain and replaces the FXSIM_NOISE path.  SEFD takes one
	// value for all stations or a comma-separated list indexed by .input
	// datastream order (datasim -s), defaulting to 1000 (datasim's default)
	// when flux is set without it.
	double flux = 0.0;
	if(const char *fe = getenv("FXSIM_FLUX"))
	{
		flux = atof(fe);
		if(flux < 0.0)
		{
			cerr << "fxcorr-sim: FXSIM_FLUX must be non-negative" << endl;
			return EXIT_FAILURE;
		}
	}
	double sefd = 0.0;
	if(const char *se = getenv("FXSIM_SEFD"))
	{
		if(strchr(se, ',') != 0)
		{
			// per-station list, indexed by datastream order
			vector<double> sefds;
			stringstream ss(se);
			string tok;
			while(getline(ss, tok, ','))
				sefds.push_back(atof(tok.c_str()));
			if((int)sefds.size() <= st.dsindex)
			{
				cerr << "fxcorr-sim: FXSIM_SEFD list has " << sefds.size()
				     << " entries, station " << st.station << " is datastream "
				     << st.dsindex << endl;
				return EXIT_FAILURE;
			}
			sefd = sefds[st.dsindex];
		}
		else
			sefd = atof(se);
		if(sefd < 0.0)
		{
			cerr << "fxcorr-sim: FXSIM_SEFD must be non-negative" << endl;
			return EXIT_FAILURE;
		}
	}
	else if(flux > 0.0)
		sefd = 1000.0;
	if(flux > 0.0 && noiseexplicit)
	{
		cerr << "fxcorr-sim: FXSIM_NOISE is ignored when FXSIM_FLUX enables the "
		        "datasim scaling chain (use FXSIM_SEFD for the station noise)" << endl;
		return EXIT_FAILURE;
	}
	unsigned long seed = 20260912UL;
	if(const char *sd = getenv("FXSIM_SEED"))
		seed = strtoul(sd, 0, 10);
	bool adaptive = false;
	if(const char *adenv = getenv("FXSIM_ADAPTIVE"))
		adaptive = (strcmp(adenv, "1") == 0);
	// FXSIM_PCAL: datasim -p comb interval in MHz (tones at k*interval MHz,
	// 1/500 amplitude, frame-edge taper); the .input PHASE CAL grid tones are
	// always injected on the new path (st.pcalhz, 0.7 amplitude)
	double pcalcomb = 0.0;
	if(const char *penv = getenv("FXSIM_PCAL"))
	{
		pcalcomb = atof(penv);
		if(!(pcalcomb > 0.0))
		{
			cerr << "fxcorr-sim: FXSIM_PCAL must be a positive interval in MHz" << endl;
			return EXIT_FAILURE;
		}
	}

	CommonSignal::Grid grid;
	int specres = parseSpecres();
	if(specres < 0)
		return EXIT_FAILURE;
	if(!CommonSignal::deriveGrid(config, &grid, specres))
		return EXIT_FAILURE;

	// the common signal must be complete before a station starts
	CommonSignal::Reader reader;
	if(!reader.open(bi.workdir + "/common/" + bi.batchid, bi.batchid, grid))
		return EXIT_FAILURE;

	// vpsamps: complex baseband samples per band per frame (= payload bytes
	// per band * 2, datasim's 4-bit-complex counting of 2-bit real samples)
	int vpsamps = st.bytesperbandframe * 2;
	FreqStationGen gen;
	if(!gen.init(st.bandfreqmhz, st.bandbwmhz, grid, noisesigma, seed,
	             st.station, vpsamps, adaptive, flux, sefd, st.pcalhz,
	             pcalcomb, st.ratehz))
		return EXIT_FAILURE;
	if(dodgen)
		gen.enableDelayInjection(model, 0, config.getDModelFileIndex(0, st.dsindex),
		                         st.srcindex, st.batchstartscanrel,
		                         (double)st.framens / 1.0e9);

	// output raw/<station>/<station>_<batch_id>.vdif (data-spec 5.2)
	string outdir = bi.workdir + "/raw/" + st.station;
	string outpath = outdir + "/" + st.station + "_" + bi.batchid + ".vdif";
	string mkdircommand = "mkdir -p " + outdir;
	if(system(mkdircommand.c_str()) != 0)
	{
		cerr << "fxcorr-sim: cannot create " << outdir << endl;
		return EXIT_FAILURE;
	}

	VDIFWriter writer(outpath, st.startsec, st.framestart, st.ratehz, st.nbands,
	                  st.bytesperbandframe);
	if(!writer.isOpen())
		return EXIT_FAILURE;
	if(applyGaps(writer) != 0)
		return EXIT_FAILURE;
	if(applyFrameOffset(writer) != 0)
		return EXIT_FAILURE;

	// block buffer sized for one full common-signal block; the reader
	// delivers the last block truncated to the batch length
	vector<float> blockbuf((size_t)reader.blockfloats());
	int payloadbytes = st.bytesperbandframe * st.nbands;
	unsigned char *payload = new unsigned char[payloadbytes];
	long long frameswritten = 0;
	for(long long blk = 0; blk < reader.nblocks(); blk++)
	{
		long long nfloats = reader.readBlock(blk, &blockbuf[0]);
		if(nfloats < 0)
		{
			delete [] payload;
			return EXIT_FAILURE;
		}
		if(!gen.processBlock(&blockbuf[0], nfloats))
		{
			delete [] payload;
			return EXIT_FAILURE;
		}
		for(int f = 0; f < gen.framesPerBlock(); f++)
		{
			memset(payload, 0, (size_t)payloadbytes);
			gen.fillFramePayload(payload, payloadbytes, st.nbands);
			if(!writer.writeFrame(payload, payloadbytes))
			{
				cerr << "fxcorr-sim: write failed at frame " << frameswritten << endl;
				delete [] payload;
				return EXIT_FAILURE;
			}
			frameswritten++;
		}
	}
	delete [] payload;
	if(frameswritten != st.nframestotal)
	{
		cerr << "fxcorr-sim: common signal covers " << frameswritten
		     << " frames, expected " << st.nframestotal << endl;
		return EXIT_FAILURE;
	}

	cerr << "wrote " << outpath << ": " << st.nframestotal << " frames, "
	     << st.nframestotal * (32 + payloadbytes) << " bytes" << endl;
	return EXIT_SUCCESS;
}

// ---- station subcommand, legacy path: the pre-architecture time-domain
// synthesis, byte-compatible with the historical output ----

static int doStationLegacy(Configuration &config, Model *model, const BatchInfo &bi,
                           const StationSetup &st)
{
	double noisesigma = 0.02;
	if(const char *ns = getenv("FXSIM_NOISE"))
		noisesigma = atof(ns);
	unsigned long seed = 20260912UL;
	if(const char *sd = getenv("FXSIM_SEED"))
		seed = strtoul(sd, 0, 10);
	// geometric delay injection into the tone phase (algo-plan A3/A4):
	// off by default so the byte-compatible comparison path is unchanged
	bool dodgen = false;
	if(const char *dgenenv = getenv("FXSIM_DELAY"))
		dodgen = (strcmp(dgenenv, "1") == 0);

	// SNR scaling (algo-plan A2/A11, datasim fabricatedata semantics): the
	// tone carries the source flux (amplitude sqrt(2*F)) and the noise
	// carries the station SEFD (sigma sqrt(SEFD)); the sum is normalised so
	// the output rms matches the historical fixed-gain quantiser range
	// (0.5).  The station SNR is then F/SEFD.  Both variables must be set to
	// enable this path; without them the tone amplitude stays 0.7 and the
	// noise sigma comes from FXSIM_NOISE, unchanged.
	double toneamp = 0.7;
	if(const char *fenv = getenv("FXSIM_FLUX"))
	{
		if(const char *senv = getenv("FXSIM_SEFD"))
		{
			double flux = atof(fenv);
			double sefd = atof(senv);
			if(flux > 0.0 && sefd > 0.0)
			{
				toneamp = 0.5 * sqrt(2.0 * flux / (flux + sefd));
				noisesigma = 0.5 * sqrt(sefd / (flux + sefd));
			}
		}
	}
	// adaptive quantisation (algo-plan A8, datasim quantize semantics):
	// off by default so the fixed rint mapping the byte comparison relies
	// on is unchanged
	bool adaptive = false;
	if(const char *adenv = getenv("FXSIM_ADAPTIVE"))
		adaptive = (strcmp(adenv, "1") == 0);

	string outdir = bi.workdir + "/raw/" + st.station;
	string outpath = outdir + "/" + st.station + "_" + bi.batchid + ".vdif";
	string mkdircommand = "mkdir -p " + outdir;
	if(system(mkdircommand.c_str()) != 0)
	{
		cerr << "fxcorr-sim: cannot create " << outdir << endl;
		return EXIT_FAILURE;
	}

	SignalGen gen;
	vector<double> ratehzvec(st.nbands, (double)st.ratehz);
	gen.init(ratehzvec, st.tonehz, st.pcalhz, noisesigma, seed, toneamp, adaptive);
	if(dodgen)
		gen.enableDelayInjection(model, 0, config.getDModelFileIndex(0, st.dsindex), st.srcindex,
		                         st.batchstartscanrel, (double)st.framens / 1.0e9, st.tonerfhz);
	VDIFWriter writer(outpath, st.startsec, st.framestart, st.ratehz, st.nbands,
	                  st.bytesperbandframe);
	if(!writer.isOpen())
		return EXIT_FAILURE;
	if(applyGaps(writer) != 0)
		return EXIT_FAILURE;
	if(applyFrameOffset(writer) != 0)
		return EXIT_FAILURE;

	int payloadbytes = st.bytesperbandframe * st.nbands;
	unsigned char *payload = new unsigned char[payloadbytes];
	for(long long n = 0; n < st.nframestotal; n++)
	{
		memset(payload, 0, (size_t)payloadbytes);
		gen.fillFramePayload(payload, payloadbytes, st.nbands);
		if(!writer.writeFrame(payload, payloadbytes))
		{
			cerr << "fxcorr-sim: write failed at frame " << n << endl;
			delete [] payload;
			return EXIT_FAILURE;
		}
	}
	delete [] payload;

	cerr << "wrote " << outpath << ": " << st.nframestotal << " frames, "
	     << st.nframestotal * (32 + payloadbytes) << " bytes" << endl;
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		usage();
		return EXIT_FAILURE;
	}
	string cmd = argv[1];

	// workdir: argument > FXCORR_WORKDIR > "."
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;

	string batchid, station;
	vector<double> tonemhzarg;

	if(cmd == "common" || cmd == "station")
	{
		if(argc < (cmd == "common" ? 3 : 4))
		{
			usage();
			return EXIT_FAILURE;
		}
		batchid = argv[2];
		if(cmd == "station")
		{
			station = argv[3];
			if(argc > 4)
				workdir = argv[4];   // argument takes precedence over the environment
			for(int a = 5; a < argc; a++)
				tonemhzarg.push_back(atof(argv[a]));
		}
		else
		{
			if(argc > 3)
				workdir = argv[3];
		}
	}
	else
	{
		// default serial mode: batch_id [workdir]
		batchid = argv[1];
		if(argc > 2)
			workdir = argv[2];
		if(argc > 3)
		{
			cerr << "fxcorr-sim: the 4-argument form (batch station workdir "
			        "tone...) is obsolete; use\n"
			        "  fxcorr-sim station <batch_id> <station> [workdir] [tone_mhz ...]" << endl;
			return EXIT_FAILURE;
		}
	}
	bool legacy = !tonemhzarg.empty();

	BatchInfo bi;
	if(!loadBatchInfo(workdir, batchid, &bi))
		return EXIT_FAILURE;

	// parse .input (non-MPI constructor), same as fxcorr-f
	Configuration config((workdir + "/" + bi.inputfile).c_str(), 0);
	if(config.getNumDataStreams() == 0)
	{
		cerr << "fxcorr-sim: failed to parse " << bi.inputfile << endl;
		return EXIT_FAILURE;
	}
	Model *model = config.getModel();

	// V1 restriction: single-scan experiments only
	if(model->getNumScans() != 1)
	{
		cerr << "fxcorr-sim: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)" << endl;
		return EXIT_FAILURE;
	}

	if(cmd == "common")
		return doCommon(config, bi);

	if(cmd == "station")
	{
		StationSetup st;
		if(!setupStation(config, model, bi, station, legacy, tonemhzarg, &st))
			return EXIT_FAILURE;
		if(legacy)
			return doStationLegacy(config, model, bi, st);
		return doStationNew(config, model, bi, st);
	}

	// default: serial common, then one station task per datastream
	if(doCommon(config, bi) != EXIT_SUCCESS)
		return EXIT_FAILURE;
	for(int d = 0; d < config.getNumDataStreams(); d++)
	{
		StationSetup st;
		if(!setupStation(config, model, bi, config.getDStationName(0, d), false,
		                 tonemhzarg, &st))
			return EXIT_FAILURE;
		if(doStationNew(config, model, bi, st) != EXIT_SUCCESS)
			return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
