#include "datareader.h"
#include "corrections.h"
#include "frametimeline.h"
#include "log.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT

using namespace std;

// Layer 1 of the rework (v4-plan.md phase C): the frame-number and slot-grid
// algorithms live in frametimeline.h as pure functions -- no datastream
// members, no I/O -- so they can be unit-tested on synthetic frame sequences
// (fxcorr/test/reader/test_timeline.cpp).  This file keeps the datastream
// state they are used from.
using namespace frametimeline;

DataReader::DataReader(Configuration *conf, int confindex, int ds, Model *mdl,
                       long long batchstartsec, int batchstartns) :
	config(conf), model(mdl), configindex(confindex), dsindex(ds),
	kind(KIND_VDIF),
	framebytes(0), payloadbytes(0), framespersecond(0), sendbytes(0),
	blockspersend(0), intclockseconds(0), nummuxthreads(1),
	bytespersamplenum(0), bytespersampledenom(0), sampletimens(0.0),
	blockbytes(0), bytesbetweenintegerns(0), nsinc(0), lastcount(0),
	numfiles(0), datafilenames(0), currentfile(-1), currentscanstartsec(0),
	currentscan(0), batchstartabsns(0), lastfileoffset(0), anchorbytes(0),
	muxer(0),
	readbuffer(0), readbuffersize(0),
	headerbytes(0), bytesperns(0.0),
	gapchecksubints(0), gapcheckframes(0), gapcheckgaps(0),
	gapcheckmissing(0), gapcheckfillers(0), gapchecklastfr(0),
	gapcheckcrossmin(0), gapcheckcrossmax(0), gapcheckcrosscount(0),
	gapcheckcrossprinted(0), gapchecklastoff(0), gapchecklastvalid(false),
	gapstretchseam(false),
	lastframesin(0), lastframens(0),
	lastgapframes(0), lastshiftdst(0),
	gapbuffer(0), inbuf(0), inbufsize(0), lastcontiguous(true)
{
	batchstartabsns = batchstartsec*1000000000LL + (long long)batchstartns;

	numfiles = config->getDNumFiles(configindex, dsindex);
	if(numfiles < 1)
	{
		cerr << "DataReader: no data files for datastream " << dsindex << endl;
		exit(EXIT_FAILURE);
	}
	datafilenames = config->getDDataFileNames(configindex, dsindex);

	// format dispatch (algo-plan.md P10); K5VSSP/K5VSSP32 have no working
	// path upstream (mark5access K5 is "Not Yet Implemented", genMk5FormatName
	// has no K5 branch), so they stay rejected
	Configuration::dataformat format = config->getDataFormat(configindex, dsindex);
	nummuxthreads = config->getDNumMuxThreads(configindex, dsindex);
	switch(format)
	{
	case Configuration::VDIF:
	case Configuration::VDIFL:
		kind = (nummuxthreads > 1) ? KIND_MUXEDVDIF : KIND_VDIF;
		break;
	case Configuration::INTERLACEDVDIF:
		kind = KIND_MUXEDVDIF;
		break;
	case Configuration::MARK5B:
		kind = KIND_MARK5B;
		break;
	case Configuration::MKIV:
	case Configuration::VLBA:
	case Configuration::VLBN:
	case Configuration::KVN5B:
	case Configuration::CODIF:
		kind = KIND_MK5STREAM;
		break;
	case Configuration::LBASTD:
	case Configuration::LBAVSOP:
	case Configuration::LBA8BIT:
	case Configuration::LBA16BIT:
		kind = KIND_LBA;
		break;
	default:
		cerr << "DataReader: unsupported data format " << format << " (K5VSSP/K5VSSP32 are not supported upstream either)" << endl;
		exit(EXIT_FAILURE);
	}

	// common parameters
	sendbytes = config->getDataBytes(configindex, dsindex);
	blockspersend = config->getBlocksPerSend(configindex);

	// internal clock offset in whole seconds (datastream.cpp:158)
	intclockseconds = (long long)floor(config->getDClockCoeff(0, dsindex, 0)/1000000.0 + 0.5);

	// per-sample timing and byte-grid parameters (datastream.cpp:750-761):
	// bufferindex arithmetic and the P11 delay realignment work on these
	bytespersamplenum = config->getDBytesPerSampleNum(configindex, dsindex);
	bytespersampledenom = config->getDBytesPerSampleDenom(configindex, dsindex);
	sampletimens = 500.0/config->getDRecordedBandwidth(configindex, dsindex, 0);
	if(config->getDSampling(configindex, dsindex) == Configuration::COMPLEX)
		sampletimens *= 2;
	int fftchannels = config->getFNumChannels(config->getDRecordedFreqIndex(configindex, dsindex, 0))*2;
	if(config->getDSampling(configindex, dsindex) == Configuration::COMPLEX)
		fftchannels = config->getFNumChannels(config->getDRecordedFreqIndex(configindex, dsindex, 0));
	blockbytes = (fftchannels*bytespersamplenum)/bytespersampledenom;
	bytesperns = (double)bytespersamplenum/((double)bytespersampledenom*sampletimens);
	bytesbetweenintegerns = 0;
	double nsaccumulate = 0.0;
	do {
		nsaccumulate += (double)bytespersampledenom*sampletimens;
		bytesbetweenintegerns += bytespersamplenum;
	} while (!(fabs(nsaccumulate - int(nsaccumulate+0.5)) < 50*Mode::TINY));
	// segment span used by upstream as the early-bail bound (datastream.cpp:763)
	long long readbytes = (long long)config->getMaxDataBytes(dsindex)
	                    * config->getDDataBufferFactor()/config->getDNumDataSegments();
	nsinc = (long long)(sampletimens*(double)readbytes*(double)bytespersampledenom/bytespersamplenum + 0.5);

	if(kind == KIND_VDIF)
	{
		// fixed frame parameters (vdiffile.cpp:397-401)
		payloadbytes = config->getFramePayloadBytes(configindex, dsindex);
		framespersecond = config->getFramesPerSecond(configindex, dsindex);	// nthreads==1 here
		framebytes = config->getFrameBytes(configindex, dsindex);

		// The file does not have to begin on the batch's first second: the
		// frame counters of the recording systems are not in phase, so a file
		// can start partway into a second (t25362's BA starts 1269 frames in,
		// while S6 starts on the boundary).  Every later read position is
		// computed relative to the file's own start, so that distance has to
		// be absorbed here -- the same thing the other kinds do with the first
		// frame's timestamp (vdiffile.cpp:429-444; upstream learns it from
		// vdifmux's output, which is why it never had to care).
		{
			ifstream hf(datafilenames[0].c_str(), ios::in | ios::binary);
			unsigned char h[8];
			if(!hf.is_open() || !hf.read(reinterpret_cast<char *>(h), 8))
			{
				cerr << "DataReader: cannot read the first frame of " << datafilenames[0] << endl;
				exit(EXIT_FAILURE);
			}
			u32 w0 = (u32)h[0] | ((u32)h[1] << 8) | ((u32)h[2] << 16) | ((u32)h[3] << 24);
			u32 w1 = (u32)h[4] | ((u32)h[5] << 8) | ((u32)h[6] << 16) | ((u32)h[7] << 24);
			long long sec = (long long)(w0 & 0x3FFFFFFF);	// word 0: seconds
			long long fr = (long long)(w1 & 0xFFFFFF);	// word 1: frame number
			// VDIF carries no day, so the second is taken as a time of day;
			// batchstartabsns is in the same (StartSeconds-based) frame
			long long filefirstabsns = (long long)(sec % 86400)*1000000000LL
				+ (long long)((double)fr*1.0e9/framespersecond + 0.5);
			long long delta = batchstartabsns - filefirstabsns;
			// Normalise across midnight; a file may also start *after* the
			// batch's first second (t25362's BA starts 79.3 ms late), in which
			// case the batch head simply has no data: the anchor is negative
			// and those subints fail to seek and come back invalid.
			if(delta > 43200LL*1000000000LL)
				delta -= 86400LL*1000000000LL;
			else if(delta < -43200LL*1000000000LL)
				delta += 86400LL*1000000000LL;
			// floor(), not a plain cast: for a negative delta (the file starts
			// after the batch does) truncation rounds *up*, which puts the
			// whole anchor one frame off -- one frame at 16 kfps is 62.5 us,
			// and 62.5 us is exactly 100 ns short of an integer number of
			// 5 MHz pcal cycles, which flips every tone of odd order (the
			// "alternate tones inverted" signature seen on t25362's BA).
			anchorbytes = (long long)floor((double)delta*framespersecond/1.0e9 + 0.5)*(long long)framebytes;
		}
	}
	else if(kind == KIND_MUXEDVDIF)
	{
		// muxed frame parameters (vdiffile.cpp:396-401): the "frame" here is
		// one frame group of all threads
		payloadbytes = config->getMultiplexedFramePayloadBytes(configindex, dsindex);
		framespersecond = config->getFramesPerSecond(configindex, dsindex)/nummuxthreads;
		framebytes = config->getMultiplexedFrameBytes(configindex, dsindex);

		// corner-turner, same construction as mk5.cpp:124-128.  Two byte
		// coordinate systems: input stream is single-thread frames
		// (iframebytes), muxed output is frame groups (framebytes =
		// (iframebytes-32)*nthreads+32).  One demux segment holds exactly one
		// subint: rframes = outframes*nthreads input frames where outframes =
		// sendbytes/framebytes output frames.
		inputframebytes = config->getFrameBytes(configindex, dsindex);
		int outframes = sendbytes/framebytes;
		muxer = new VDIFMuxer(config, dsindex, dsindex+1, nummuxthreads, inputframebytes,
			outframes*nummuxthreads, (int)framespersecond,
			config->getDNumBits(configindex, dsindex),
			config->getDMuxThreadMap(configindex, dsindex));
	}
	else if(kind == KIND_MARK5B)
	{
		payloadbytes = config->getFramePayloadBytes(configindex, dsindex);
		framespersecond = config->getFramesPerSecond(configindex, dsindex);
		framebytes = config->getFrameBytes(configindex, dsindex);

		// first frame of the file, like Mark5BDataStream::initialiseFile
		// (mark5bfile.cpp:313-327); anchorbytes absorbs the distance to the
		// batch start (the file may start earlier, data-spec 5.2)
		struct mark5b_file_summary fileSummary;
		if(summarizemark5bfile(&fileSummary, datafilenames[0].c_str()) < 0)
		{
			cerr << "DataReader: " << datafilenames[0] << " does not look like valid Mark5B data" << endl;
			exit(EXIT_FAILURE);
		}
		mark5bfilesummaryfixmjd(&fileSummary, config->getStartMJD());
		long long filefirstabsns = (long long)(fileSummary.startDay - config->getStartMJD())*86400000000000LL
			+ (long long)fileSummary.startSecond*1000000000LL
			+ (long long)((double)fileSummary.startFrame*1.0e9/fileSummary.framesPerSecond + 0.5);
		if(batchstartabsns < filefirstabsns)
		{
			cerr << "DataReader: file " << datafilenames[0] << " starts after the batch start" << endl;
			exit(EXIT_FAILURE);
		}
		// floor(), not a plain cast: rounding must stay "nearest" if the
		// non-negative guarantee from the check above is ever relaxed (see the
		// VDIF branch, where a negative distance is legitimate and truncation
		// would round the anchor up by a whole frame)
		anchorbytes = fileSummary.firstFrameOffset
			+ (long long)floor((double)(batchstartabsns - filefirstabsns)*framespersecond/1.0e9 + 0.5)*framebytes;

		// fix buffer: one subint plus two frames of margin for interlopers
		readbuffersize = sendbytes + 2*framebytes;
		readbuffer = new unsigned char[readbuffersize];
		resetmark5bfixstatistics(&m5bstats);
	}
	else if(kind == KIND_MK5STREAM)
	{
		payloadbytes = config->getFramePayloadBytes(configindex, dsindex);
		framespersecond = config->getFramesPerSecond(configindex, dsindex);
		framebytes = config->getFrameBytes(configindex, dsindex);

		// mark5access generic stream, like Mk5DataStream::initialiseFile
		// (mk5.cpp:315-390): open with the derived format name, read the
		// first frame's time, then close -- we only need the anchor
		char formatname[64];
		int fanout = config->genMk5FormatName(format, config->getDNumRecordedBands(configindex, dsindex),
			config->getDRecordedBandwidth(configindex, dsindex, 0), config->getDNumBits(configindex, dsindex),
			config->getDSampling(configindex, dsindex), framebytes,
			config->getDDecimationFactor(configindex, dsindex), config->getDAlignmentSeconds(configindex, dsindex),
			nummuxthreads, formatname);
		if(fanout < 0)
		{
			cerr << "DataReader: impossible fanout for format " << format << endl;
			exit(EXIT_FAILURE);
		}
		struct mark5_stream *ms = new_mark5_stream(
			new_mark5_stream_file(datafilenames[0].c_str(), 0),
			new_mark5_format_generic_from_string(formatname));
		if(ms == 0)
		{
			cerr << "DataReader: could not open " << datafilenames[0] << " as " << formatname << endl;
			exit(EXIT_FAILURE);
		}
		mark5_stream_fix_mjd(ms, config->getStartMJD());
		int mjd = 0, sec = 0;
		double ns = 0.0;
		mark5_stream_get_frame_time(ms, &mjd, &sec, &ns);
		long long filefirstabsns = (long long)(mjd - config->getStartMJD())*86400000000000LL
			+ (long long)sec*1000000000LL + (long long)(ns + 0.5);
		anchorbytes = ms->frameoffset;
		delete_mark5_stream(ms);
		if(batchstartabsns < filefirstabsns)
		{
			cerr << "DataReader: file " << datafilenames[0] << " starts after the batch start" << endl;
			exit(EXIT_FAILURE);
		}
		anchorbytes += (long long)floor((double)(batchstartabsns - filefirstabsns)*framespersecond/1.0e9 + 0.5)*framebytes;
	}
	else	// KIND_LBA
	{
		// ASCII header + raw payload, upstream base DataStream::initialiseFile
		// (datastream.cpp:1825-1875): old style is a 15-char "YYYYMMDD:HHMMSS"
		// first line, new style has a "TIME ..." keyword line; the payload
		// starts after 16 bytes (old) or a fixed 4096-byte header (new)
		ifstream headerfile(datafilenames[0].c_str(), ios::in | ios::binary);
		if(!headerfile.is_open())
		{
			cerr << "DataReader: cannot open " << datafilenames[0] << endl;
			exit(EXIT_FAILURE);
		}
		string inputline;
		getline(headerfile, inputline);
		if(headerfile.fail())	// problems caused by "peeking" for EOF (datastream.cpp:1832-1837)
		{
			headerfile.clear();
			getline(headerfile, inputline);
		}
		int headerbytesacc = 0;
		if(inputline.length() != 15 || inputline.c_str()[8] != ':')
		{
			// new style header: find the TIME keyword line
			headerbytesacc = inputline.length() + 1;
			while(inputline.substr(0,4) != "TIME")
			{
				getline(headerfile, inputline);
				headerbytesacc += inputline.length() + 1;
				if(headerbytesacc > 4096)
				{
					cerr << "DataReader: no TIME line in LBA header of " << datafilenames[0] << endl;
					exit(EXIT_FAILURE);
				}
			}
			inputline = inputline.substr(5,15);
			headerbytes = 4096;
		}
		else
		{
			headerbytes = 16;	// 15-char time line + newline
		}
		int year = atoi(inputline.substr(0,4).c_str());
		int month = atoi(inputline.substr(4,2).c_str());
		int day = atoi(inputline.substr(6,2).c_str());
		int hour = atoi(inputline.substr(9,2).c_str());
		int minute = atoi(inputline.substr(11,2).c_str());
		int second = atoi(inputline.substr(13,2).c_str());
		headerfile.close();
		int filestartday = 0, filestartseconds = 0;
		config->getMJD(filestartday, filestartseconds, year, month, day, hour, minute, second);
		long long filefirstabsns = (long long)(filestartday - config->getStartMJD())*86400000000000LL
			+ (long long)filestartseconds*1000000000LL;
		if(batchstartabsns < filefirstabsns)
		{
			cerr << "DataReader: file " << datafilenames[0] << " starts after the batch start" << endl;
			exit(EXIT_FAILURE);
		}

		// no framing: time<->byte mapping is the pure payload rate (upstream
		// base DataStream::calculateControlParams); bytesperns is the common
		// per-sample value set above
		anchorbytes = (long long)floor((double)(batchstartabsns - filefirstabsns)*bytesperns + 0.5);
	}

	// C2: the corrections ledger places a gap on the time axis from the file
	// offset it was found at, which needs the file's own frame grid -- known
	// only now that the format branches above have run (corrections.h)
	ledger.setOrigin(anchorbytes, framebytes);
}

DataReader::~DataReader()
{
	if(input.is_open())
		input.close();
	if(muxer)
		delete muxer;
	if(readbuffer)
		delete [] readbuffer;
	if(gapbuffer)
		delete [] gapbuffer;
	if(inbuf)
		delete [] inbuf;

	// P12 step 1: report the frame-number continuity statistics (observation
	// only; see checkFrameContinuity).  One line per run, and the only signal
	// that a datastream had gaps or filler at all, so it sits at info while
	// the per-event lines behind it sit at verbose.
	if(gapchecksubints > 0 && fxLogLevel() >= FXLOG_INFO)
	{
		cerr << "GAPCHECK summary: buffers " << gapchecksubints
		     << " frames " << gapcheckframes
		     << " discontinuities " << gapcheckgaps
		     << " missing frames " << gapcheckmissing
		     << " filler frames " << gapcheckfillers
		     << " boundaries " << gapcheckcrosscount;
		if(gapcheckcrosscount > 0)
			cerr << " step range " << gapcheckcrossmin << ".." << gapcheckcrossmax;
		cerr << endl;
	}
}

// P12 step 1: frame-number continuity check.
//
// The VDIF header carries a frame number in word 1 ([23:0]; word layout per
// vdifio's vdif_header, not the vlbi.org spec).  Within one file the frame
// number advances by one per frame, so any other step means the file is
// missing frames there -- the byte stream is then shorter than the time axis
// and every later subint is read from the wrong position (the locate()
// arithmetic assumes a strictly linear time<->byte mapping).
//
// Filler frames are the other way round and are skipped here: they occupy
// bytes in the file but no slot on the time axis, so they do not break the
// frame-number chain, and what they say about the read position has the
// opposite sign to a gap.
//
// P12 step 2a turns both observations into corrections -- the gaps and filler
// the ledger keeps (corrections.h, C2), applied in readSubint -- and step 2b
// (shiftFrameGaps)
// rebuilds this buffer's frame grid.  A file with neither follows exactly the
// old code path.
int DataReader::checkFrameContinuity(const u8 *src, int srcbytes, long long readoffset,
                                     u8 *dst, int slots)
{
	if(kind != KIND_VDIF || framebytes < 8 || framespersecond < 1)
		return slots;

	// nframes is how much of the file this scan looks at; slots is how many
	// time slots the subint has.  They differ once the read had to bridge a
	// filler run: the bytes past the point where the slots fill up belong to
	// the next subint and must not be counted here (B2, reader-model.md 4.8).
	int nframes = srcbytes/framebytes;
	gapchecksubints++;
	// Both gap lists describe the CURRENT buffer only and are rebuilt from
	// scratch on every scan: the holes are refilled by this subint's own read,
	// and a stale entry would invalidate blocks of every later subint.
	gapinvalid.clear();
	if(nframes < 1)
		return 0;

	// The frame counter restarts once per second, so all steps below are
	// taken modulo frames-per-second.
	long long fps = (long long)framespersecond;
	long long firstany = -1, lastany = -1;
	long long missing = 0;

	// Gaps are looked for strictly *inside* this buffer.  A step between the
	// end of one buffer and the start of the next is not a gap: the read
	// position advances by the subint length while the frame counter advances
	// by whole frames, so the two drift by a fraction of a frame per subint
	// and the boundary step comes out as overlap or a small skip of its own
	// accord (synthetic test data with no gaps at all shows +-4 frame steps
	// there).  Only an interior discontinuity moves data inside the buffer.
	long long chainfr = -1;		// frame number the chain continues from
	long long fillernew = 0;
	bool reorder = false;

	// A buffer whose filler made the ledger's tally grow also moved the next
	// read position forward by that growth, so the next buffer starts past a
	// stretch that was never scanned.  Those frames still have to be counted
	// (nothing else will ever look at them) but they cannot be assumed to be
	// filler -- the stretch is "the filler this buffer found" plus ordinary
	// data, and counting the data as filler overshoots the correction, which
	// moves the following read even further and runs away.  So read the
	// stretch and count what is really there.
	//
	// readSubint runs this scan *before* it reads (scanStretch), because
	// a gap sitting in the stretch shortens the very position the read is about
	// to take; by the time control reaches here the stretch is empty and this
	// is a no-op, left in place so a read arriving by another path is covered.
	{
		long long gapnew = 0;
		fillernew += scanStretch(readoffset, &gapnew);
		missing += gapnew;	// this buffer's own tally, for the READPOS line
		if(gapstretchseam)
		{
			// the stretch ended on a data frame, so the chain continues into
			// this buffer: the seam between the two is where the rest of ds_2's
			// gaps sat, and neither side used to look at it.  Only a stretch
			// scan may seed it -- the previous buffer overlaps this one by the
			// guard margin, and comparing across that overlap reads as a gap of
			// its own length (B2).
			chainfr = gapchecklastfr;
			gapstretchseam = false;
		}
	}

	// The walk itself is layer 1's (frametimeline.h).  Data frames carry the
	// numbers and the chain runs over those; filler has none and comes back as
	// a list of positions, because what it says about the read position has the
	// opposite sign to a gap.
	FrameRun run = walkFrameChain(src, nframes, framebytes, readoffset, fps, chainfr);
	firstany = run.first;
	lastany = run.last;
	gapcheckframes += run.frames;
	reorder = !run.fillers.empty() || !run.gaps.empty();

	// Filler and gaps go to the ledger in scan order (C2), because how much
	// filler lies ahead of a gap is a question about their positions in the
	// file and only a walk that keeps the two in order can answer it.
	//
	// Filler: bytes in the file, no slot on the time axis.  It neither breaks
	// the frame-number chain (the real frames either side of a run are
	// consecutive apart from the frames actually lost) nor counts as missing,
	// but everything after it sits further along the file than the time axis
	// says -- that is the ledger's filler tally, applied in readSubint with the
	// opposite sign to a real gap.  Counted once per filler frame, by file
	// position (the corrected read position stays monotonic), because
	// consecutive subint reads overlap by their guard margin.
	//
	// A gap, in turn, is identified by where it sits in the file -- the offset
	// of the frame right after it -- for the same reason: successive subints
	// keep seeing the same gap while the corrected read position catches up
	// with it, and the frame number wraps once a second.
	size_t fi = 0, gi = 0;
	while(fi < run.fillers.size() || gi < run.gaps.size())
	{
		bool nextfiller = (fi < run.fillers.size() &&
		                   (gi >= run.gaps.size() || run.fillers[fi] < run.gaps[gi].index));
		if(nextfiller)
		{
			long long fpos = readoffset + (long long)run.fillers[fi]*framebytes;
			if(ledger.noteFiller(fpos))
				fillernew++;	// frames in the guard overlap were counted before
			fi++;
			continue;
		}

		const FrameGap &g = run.gaps[gi];
		gi++;
		// everything counted ahead of this gap in the file: what earlier scans
		// found plus what this buffer has walked past so far
		long long fillerbefore = ledger.fillerFrames();
		if(ledger.noteGap(g.offset, g.missing, fillerbefore))
		{
			missing += g.missing;
			gapcheckgaps++;
			FXLOG(FXLOG_VERBOSE) << "GAPCHECK buffer " << gapchecksubints << " frame " << g.index
			     << ": frameno " << g.after << " -> " << g.frame
			     << " (step " << g.step << ", missing " << g.missing << ")" << endl;
		}
	}

	// P12 step 2a: account for the newly found missing frames and filler
	// bytes.  A gap makes the file shorter than the time axis (later reads are
	// too far along, so subtract); filler makes it longer (later reads are too
	// early, so add).  Both are whole frames, so frame alignment is preserved.
	// The gaps themselves went to the ledger as they were found; how much of
	// each one applies to a given subint is decided by its place in time, at
	// read time (ledger.gapShift).
	if(missing > 0)
		gapcheckmissing += missing;
	if(fillernew > 0)
		gapcheckfillers += fillernew;	// the tally itself is the ledger's

	// watermark for the next buffer: the furthest whole frame this scan
	// covered, so the guard overlap between consecutive reads is not counted
	// twice and the distance to the next read position is a whole number of
	// frames
	ledger.advanceFillerWatermark(readoffset + (long long)(srcbytes/framebytes)*framebytes);

	// A buffer can need rebuilding even with unbroken frame numbers: when a
	// gap covers the subint's own start, the read position lands on the first
	// frame after the gap, so the whole buffer sits that much further into the
	// subint and the slots ahead of it are the hole.  Frame numbers say by how
	// much -- the buffer's first frame is that far from the start's own
	// in-second number (lastframens, from locate), and shiftFrameGaps places
	// it there.  Without this the buffer would be handed over starting at slot
	// 0 and the hole would be integrated as data.
	if(!reorder && !ledger.empty() && firstany >= 0 &&
	   (int)((firstany - (long long)lastframens + fps) % fps) > 0)
		reorder = true;

	// A frame the recorder marked invalid needs the pass too, even though its
	// number is where it should be: the slot it keeps has to be reported so the
	// valid flags can clear it (reader-model.md 4.12).  Without an invalid frame
	// in the buffer the condition above is untouched, so the no-interruption
	// path stays byte for byte what it was.
	if(!reorder && !run.invalids.empty())
		reorder = true;

	// P12 step 2b: put the frames where the time axis says they belong.  When
	// there is neither a gap nor filler nothing moves and the source is the
	// destination buffer -- which is what keeps the no-interruption path byte
	// for byte what it was before the window became slot-based (B2).
	int placed;
	if(reorder)
		placed = shiftFrameGaps(src, nframes, dst, slots);
	else
	{
		lastshiftdst = 0;
		placed = (nframes < slots) ? nframes : slots;
	}

	// Boundary to the next buffer.  The step across buffers is not a gap by
	// itself -- one subint spans 81.92 frames here, so consecutive buffers
	// overlap or skip by a frame depending on the frame/sample alignment --
	// so only the range of observed steps is recorded for later analysis.
	if(gapchecklastvalid)
	{
		long long step = firstany - gapchecklastfr;
		if(gapcheckcrosscount == 0 || step < gapcheckcrossmin)
			gapcheckcrossmin = step;
		if(gapcheckcrosscount == 0 || step > gapcheckcrossmax)
			gapcheckcrossmax = step;
		gapcheckcrosscount++;
		// one subint spans 81.92 frames and each read carries a frame-aligned
		// guard overlap, so a healthy boundary steps by -1..1; anything
		// further off means the read position itself moved, which is worth
		// seeing explicitly at verbose -- capped, since a datastream whose
		// position jumps at every boundary would otherwise flood the log
		if((step < -3 || step > 3) && gapcheckcrossprinted < 40)
		{
			gapcheckcrossprinted++;
			long long offdelta = lastfileoffset - gapchecklastoff;
			FXLOG(FXLOG_VERBOSE) << "GAPCHECK boundary " << gapcheckcrosscount << ": prev end frameno "
			     << gapchecklastfr << " -> first " << firstany << " (step " << step
			     << "; read offsets " << gapchecklastoff << " -> " << lastfileoffset
			     << ", advance " << offdelta << " bytes = "
			     << (framebytes ? offdelta/framebytes : 0) << " frames)" << endl;
		}
	}
	// A buffer made entirely of filler has no data frame to report, but it must
	// not clear the chain: the next buffer's skipped-stretch scan starts from
	// this number, and the gap that sits right after a long filler run has no
	// predecessor to be compared against otherwise (t25362 ds_2: two gaps of 28
	// and 22 frames, sitting after runs of 229 and 180 filler frames that each
	// span whole buffers).  Keeping the last real frame number also makes the
	// boundary step report the whole run, which is the honest reading.
	if(lastany >= 0)
		gapchecklastfr = lastany;
	gapchecklastoff = lastfileoffset;
	gapchecklastvalid = true;

	// P12 diagnosis (t25362, 2026-09-18): one line per subint carrying the
	// corrected read position and the frame numbers actually found there.  Two
	// datastreams of the same recording interruption describe the same hole on
	// the time axis -- one as plain missing frames, the other as filler frames
	// (ds_2 here) -- so if the corrections are equivalent they must land on the
	// same byte for the same time slot.  Diffing this line between such a pair
	// is what tells a position error from a data error.
	//
	// Verbose level: 2200 lines per datastream per batch.
	// gapshift here is what this subint's read position was actually shortened
	// by (the part of the gaps lying before its start), not the running total
	// of everything noticed so far -- the two differ exactly at the subint a
	// gap straddles, which is what this line exists to show.
	//
	// CONTRACT (reader-model.md 6.6): the field names, order and units of this
	// line and of the GAPCHECK family are frozen -- check_reader.py, the gaps/
	// scripts and run_t25362.sh parse them, and the t25362 baseline compares
	// them field by field.  New fields go at the END of the line (the parsers
	// take them as trailing optional groups).
	FXLOG(FXLOG_VERBOSE) << "READPOS subint " << gapchecksubints
	     << ": readoff " << readoffset
	     << " firstfno " << firstany << " lastfno " << lastany
	     << " nframes " << nframes << " slots " << slots
	     << " missing " << missing << " filler " << fillernew
	     << " gapshift " << lastgapframes*(long long)framebytes
	     << " fillershift " << ledger.fillerFrames()*(long long)framebytes
	     << " gapframes " << lastgapframes << " dst " << lastshiftdst
	     << " framens " << lastframens
	     << " uncorr " << lastuncorrected << " passes " << lastsettlepasses
	     << endl;

	// slots placed in dst (holes included): the caller turns this into the
	// buffer's data length, since with a slot grid a hole is a slot the block
	// validators need to see rather than a byte count that never arrived
	return placed;
}

// P12 step 2b: rebuild the subint's frame grid inside the buffer.
//
// mark5_unpack_with_offset walks the buffer by sample offset and therefore
// assumes position i is the i-th frame of the subint, but the file hands over
// frames in file order, which is not time order:
//
//   * a gap leaves the frames behind it sitting too early -- they have to move
//     up by the number of frames missing there, and the vacated slots (whose
//     data lives in the file but belongs to another subint's time) are
//     recorded as invalid;
//   * filler frames have no time slot at all -- they are dropped and the
//     frames behind them move up to close the space.
//
// Both are just "walk the source frames and place each where its frame number
// says", so the two cases are handled by one pass: filler is skipped and a
// jump in the frame number advances the destination by the jump instead of by
// one.  Slots past the last placed frame are invalid too -- their data is not
// in this buffer (it belongs to a later subint, which reads it from the file
// itself).  Without gaps or filler the destination always runs one ahead of
// the source, so the buffer comes back unchanged.
int DataReader::shiftFrameGaps(const u8 *src, int srcframes, u8 *dstbuf, int slots,
                               int *usedframesp, bool dryrun)
{
	long long fps = (long long)framespersecond;
	u8 *dst = NULL;

	if(!dryrun)
	{
		if(!gapbuffer)
			gapbuffer = new u8[sendbytes];
		memset(gapbuffer, 0, (size_t)sendbytes);

		gapinvalid.clear();
		dst = gapbuffer;
	}

	// Layer 1 walks the frames (frametimeline.h placeFrames: filler dropped,
	// each frame placed where its number says, unclaimed slots reported as
	// holes); what is left here is the datastream side of it -- the scratch
	// buffer, the diagnostics, and the hole list fillValidFlags reads.
	SlotPlacement p = placeFrames(src, srcframes, framebytes, fps, lastframens, dst, slots);

	if(!dryrun)
	{
		lastshiftdst = p.firstslot;
		// holes (slots nobody claimed) and invalid-marked slots (frames the
		// recorder said not to use) both have to be cleared from the valid
		// flags -- reader-model.md 4.12
		gapinvalid = p.invalidRanges();

		memcpy(dstbuf, gapbuffer, (size_t)slots*framebytes);

		// Where the holes landed, as post-shift frame ranges: the counterpart
		// of READPOS's dst for this subint's valid flags.  One line per rebuilt
		// buffer, so t25362's filler datastream (1162 frames) prints a few
		// hundred at verbose -- that is the level for exactly this kind of
		// question.  Frozen format: E3 of check_reader.py reads these ranges
		// (reader-model.md 6.6).
		FXLOG(FXLOG_VERBOSE) << "GAPCHECK holes buf " << gapchecksubints << ":";
		for(size_t g=0;g<gapinvalid.size();g++)
			FXLOG(FXLOG_VERBOSE) << " [" << gapinvalid[g].first << "," << gapinvalid[g].second << ")";
		FXLOG(FXLOG_VERBOSE) << endl;
	}

	if(usedframesp)
		*usedframesp = p.usedframes;
	return p.placed;
}

// Count the filler frames in a file range the read position skipped over, and
// walk the frame-number chain across it.
//
// Called with the stretch between where the last scan stopped and where the
// current buffer starts, which is non-empty only after a filler run moved the
// read position forward.  Those frames have to be counted -- the scan will
// never see them again -- but reading the stretch is the only honest way:
// assuming it is all filler overshoots by the data frames that sit after the
// run, and an overshoot moves the next read position further still, so the
// error grows on every run.
//
// Counting filler is not enough (t25362 ds_2, 2026-09-18): this stretch can
// also hold REAL gaps, and because the main loop only looks for gaps strictly
// inside a buffer, a gap that sits in a skipped stretch was never counted at
// all -- ds_2 lost 68 of its 70 missing frames that way, leaving every later
// read 70 frames too far along the file (4.375 ms of wrong data, silently
// marked valid).  The gaps cluster at filler-run boundaries, which is exactly
// what a skipped stretch is made of.  So the chain is walked here too, with
// the same de-duplication key (the file offset of the frame after the gap) so
// a gap seen by both this and the main loop is still counted once.
//
// chainfr is the last data frame number seen before this stretch (-1 if none),
// so a gap straddling the boundary into the stretch is caught as well.
// *gapsp receives the missing frames found here; the caller adds them to its
// own tally so they reach the ledger with the same treatment.  *lastfrp
// gets the last data frame number in the stretch so the caller can continue
// the chain into its own buffer -- a gap lying exactly on that seam is
// otherwise checked by neither side (t25362 ds_2 lost 50 more frames there).
// Scan the file stretch between the last scan's watermark and `upto`, counting
// its filler and its gaps into the ledger -- both through countFillerRange,
// which walks the frame-number chain across it and skips what another scan
// already counted.
//
// A read position is built from the corrections found so far, so a stretch that
// has never been scanned is ground the position knows nothing about -- and it
// is exactly where the gaps a filler run skipped over sit, and a gap found
// there shortens the very position the read is about to use.  readSubint
// therefore calls this *before* reading and recomputes the position until it
// settles; checkFrameContinuity calls it again (as a no-op) so a read that
// arrived by another path is covered too.
//
// Returns the filler frames found; *gapsp receives the missing ones, which the
// caller adds to its own per-buffer tally (the gaps themselves are already in
// the ledger, and the lifetime counters are kept here).
long long DataReader::scanStretch(long long upto, long long *gapsp)
{
	long long start = 0;
	long long missing = 0;
	long long counted = 0;
	// the chain continues from the last data frame number this reader saw; a
	// gap straddling the seam into the stretch is caught here as well
	long long pfr = gapchecklastvalid ? gapchecklastfr : -1;

	*gapsp = 0;

	// Nothing new to look at.  The watermark is where the last scan stopped, so
	// this is the ordinary case for every read but the one that follows a filler
	// run.
	if(!ledger.fillerWatermarkValid() || upto <= ledger.fillerWatermark())
	{
		gapstretchseam = false;
		return 0;
	}
	start = ledger.fillerWatermark();

	long long nframes = (upto - start)/(long long)framebytes;
	if(framebytes >= 8 && nframes >= 1)
	{
		const long long nbuf = 512;
		u8 *buf = new u8[(size_t)(nbuf*(long long)framebytes)];
		long long fps = (long long)framespersecond;
		long long pos = 0;
		while(pos < nframes)
		{
			long long want = (nframes - pos > nbuf) ? nbuf : (nframes - pos);
			input.clear();
			input.seekg(start + pos*(long long)framebytes, ios::beg);
			if(!input.good())
				break;
			input.read(reinterpret_cast<char *>(buf), (streamsize)(want*(long long)framebytes));
			int gotframes = (int)(input.gcount()/(streamsize)framebytes);
			if(gotframes <= 0)
				break;

			// layer 1 walks this block's chain; the block is one stretch of the
			// file, so its frames' offsets start here
			FrameRun run = walkFrameChain(buf, gotframes, framebytes,
			                              start + pos*(long long)framebytes, fps, pfr);
			// filler and gaps in scan order, as in checkFrameContinuity: the
			// ledger has to know how much filler lies ahead of a gap when it
			// records it
			size_t fi = 0, gi = 0;
			while(fi < run.fillers.size() || gi < run.gaps.size())
			{
				bool nextfiller = (fi < run.fillers.size() &&
				                   (gi >= run.gaps.size() || run.fillers[fi] < run.gaps[gi].index));
				if(nextfiller)
				{
					long long fpos = start + (pos + (long long)run.fillers[fi])*(long long)framebytes;
					if(ledger.noteFiller(fpos))
						counted++;
					fi++;
					continue;
				}

				const FrameGap &g = run.gaps[gi];
				gi++;
				long long fillerbefore = ledger.fillerFrames();	// everything ahead of it
				if(ledger.noteGap(g.offset, g.missing, fillerbefore))
				{
					missing += g.missing;
					gapcheckgaps++;
					FXLOG(FXLOG_VERBOSE) << "GAPCHECK skipped " << gapchecksubints
					     << ": frameno " << g.after << " -> " << g.frame
					     << " (step " << g.step << ", missing " << g.missing << ")" << endl;
				}
			}
			// a block made of filler reports no frame number: the chain then
			// continues from wherever it stood
			if(run.last >= 0)
				pfr = run.last;
			pos += gotframes;
		}
		delete [] buf;
	}

	ledger.advanceFillerWatermark(upto);
	// the chain runs into whatever is read from `upto` when it is intact at the
	// end of the stretch -- a stretch that never held a data frame leaves
	// nothing for the buffer's own scan to compare against
	gapstretchseam = (pfr >= 0);
	if(pfr >= 0)
		gapchecklastfr = pfr;

	*gapsp = missing;
	return counted;
}

void DataReader::openFile(int fileindex)
{
	if(currentfile == fileindex && input.is_open())
		return;

	input.close();
	input.clear();
	input.open(datafilenames[fileindex].c_str(), ios::in | ios::binary);
	if(!input.is_open())
	{
		cerr << "DataReader: cannot open " << datafilenames[fileindex] << endl;
		exit(EXIT_FAILURE);
	}
	currentfile = fileindex;
	// V1: the file holds one scan starting at the scan start time
	currentscan = fileindex;
	currentscanstartsec = (long long)model->getScanStartSec(currentscan, config->getStartMJD(), config->getStartSeconds());
}

bool DataReader::locate(int scan, int offsetsec, int offsetns, int *sec, int *ns, long long *fileoffset)
{
	// srcindex: phase centre vs pointing centre (datastream.cpp:375-377)
	int srcindex = 0;
	if(model->getNumPhaseCentres(scan) == 1 && !model->isPointingCentreCorrelated(scan))
		srcindex = 1;

	// geometric delay at the subint start (datastream.cpp:381-383)
	f64 delayus1 = 0.0;
	bool foundok = model->calculateDelayInterpolator(scan, (double)offsetsec + ((double)offsetns)/1.0e9, 0.0, 0,
		config->getDModelFileIndex(configindex, dsindex), srcindex, 0, &delayus1);
	delayus1 -= (double)intclockseconds*1000000.0;
	long long firstoffsetns = ((long long)offsetns) - (long long)(delayus1*1000.0);

	// geometric delay at the subint end (datastream.cpp:384-388), needed by
	// the realignment compensation below (P11)
	int fftchannels = config->getFNumChannels(config->getDRecordedFreqIndex(configindex, dsindex, 0))*2;
	if(config->getDSampling(configindex, dsindex) == Configuration::COMPLEX)
		fftchannels = config->getFNumChannels(config->getDRecordedFreqIndex(configindex, dsindex, 0));
	long long dataspanns = (long long)((double)blockspersend*(double)fftchannels*sampletimens + 0.5);
	f64 delayus2 = 0.0;
	foundok = foundok && model->calculateDelayInterpolator(scan, (double)offsetsec + ((double)offsetns + dataspanns)/1.0e9, 0.0, 0,
		config->getDModelFileIndex(configindex, dsindex), srcindex, 0, &delayus2);
	delayus2 -= (double)intclockseconds*1000000.0;
	if(!foundok)
		return false;

	// delay-corrected start relative to the scan start, in ns
	long long relstartns = (long long)offsetsec*1000000000LL + firstoffsetns;

	// byte offsets are relative to the batch start, not the scan start: the
	// file holds this batch's data (data-spec 5.2 file-per-batch layout).
	// batchstartabsns is in day-seconds (config->getStartSeconds() frame); the
	// scan-relative frame here is offset by experseconds + scanstartsec.
	long long batchrelscan = batchstartabsns
	                       - (config->getStartSeconds() + currentscanstartsec)*1000000000LL;
	long long batchrel = relstartns - batchrelscan;

	// P11 realignment (datastream.cpp:516-583): the delay-corrected start may
	// lie before the data origin (segment 0 = batch start under the
	// file-per-batch layout; the segment grid degenerates to this single
	// boundary because the reader has no gap detection, so every segment is
	// always "full").  Upstream skips whole FFT blocks in that case and
	// compensates for the delay change across the skipped blocks.
	lastcount = 0;
	// whole subint discarded if the corrected start is more than one segment
	// before the data origin (datastream.cpp:463-470)
	if(batchrel < -nsinc)
		return false;
	// corrected position in bytes, assuming no framing overhead (datastream.cpp:516)
	long long bufbytes = ((long long)((double)batchrel/sampletimens + 0.5)*bytespersamplenum)/bytespersampledenom;
	// align to the nearest previous 16 bit boundary (datastream.cpp:518-519)
	if(bufbytes % 2 != 0)
		bufbytes--;
	// skip whole FFT blocks until inside the data (datastream.cpp:538-548)
	int count = 0;
	while(bufbytes < 0 && count < blockspersend)
	{
		bufbytes += blockbytes;
		count++;
	}
	if(bufbytes < 0)
		return false;
	// account for the delay change over the skipped blocks (datastream.cpp:550-568).
	// Upstream quirk kept for identical behaviour: tosubtract is applied only
	// when it would push the position back before the data origin (then one
	// more block is skipped and the compensation recomputed); otherwise it is
	// NOT subtracted.
	if(count > 0)
	{
		int tosubtract = (int)(count*1000.0*(delayus2 - delayus1)/(sampletimens*blockspersend) + 0.5)*bytespersamplenum/bytespersampledenom;
		if(bufbytes - tosubtract < 0)
		{
			count++;
			bufbytes += blockbytes;
			if(count == blockspersend)
				return false;
			tosubtract = (int)(count*1000.0*(delayus2 - delayus1)/(sampletimens*blockspersend) + 0.5)*bytespersamplenum/bytespersampledenom;
			bufbytes -= tosubtract;
		}
		// re-align to the nearest previous 16 bit boundary (datastream.cpp:567)
		bufbytes -= bufbytes % 2;
	}
	lastcount = count;
	// back off to a byte boundary where the ns is an integer value
	// (datastream.cpp:570-573)
	bufbytes -= bufbytes % bytesbetweenintegerns;

	if(kind == KIND_LBA)
	{
		// no framing: raw payload byte rate (upstream base DataStream)
		*fileoffset = anchorbytes + bufbytes;

		// data block start time: no frame rounding for LBA
		double abstime = (double)batchrelscan/1000000000.0 + (double)(*fileoffset - anchorbytes)/bytesperns/1.0e9;
		long long abssec = (long long)floor(abstime);
		*sec = (int)abssec;
		*ns = (int)((abstime - (double)abssec)*1.0e9 + 0.5);
		return true;
	}

	// frame-aligned start (vdiffile.cpp:429-444; V1 frame granularity is 1):
	// framesin from the payload-equivalent byte offset (upstream vlbaoffset)
	long long framesin = bufbytes/payloadbytes;
	// the frame index this subint maps onto: the read position is corrected by
	// the gaps that lie before it on the time axis (ledger.gapShift)
	lastframesin = framesin;

	if(kind == KIND_MUXEDVDIF)
	{
		// file offsets live in the input (interlaced) stream: one output
		// frame group occupies nthreads input frames
		*fileoffset = anchorbytes + framesin*nummuxthreads*(long long)inputframebytes;
	}
	else
	{
		*fileoffset = anchorbytes + framesin*(long long)framebytes;
	}

	// data block start time in the same frame of reference as offsetsec/offsetns
	// (seconds relative to the scan start, what DataStream's controlbuffer
	// carries): batch start relative to the scan start + in-batch frame offset
	double abstime = (double)batchrelscan/1000000000.0 + (double)framesin/framespersecond;
	long long abssec = (long long)floor(abstime);
	*sec = (int)abssec;
	*ns = (int)((abstime - (double)abssec)*1.0e9 + 0.5);
	// the in-second frame number this block starts on: a buffer whose first
	// frame carries a later number than this has started past a gap, and the
	// frames between belong to the hole (shiftFrameGaps)
	lastframens = (int)((abstime - (double)abssec)*framespersecond + 0.5);

	return true;
}

// The file offset this subint is read from: locate()'s linear map, corrected by
// the filler and the gaps counted so far, settled against the stretches in
// between, and clamped at the file start.
//
// A read position is built from the corrections counted so far, but the stretch
// a filler run made the reader skip has not been scanned yet -- and the gaps
// sitting in it shorten this very position.  Taking the read anyway is what
// marked good data invalid: the position came out that many frames too far
// along, the buffer's first frame no longer sat at the subint's start, and
// shiftFrameGaps marked the slots ahead of it -- the good frames it had just
// read -- as holes (t25362 ds_2: 63..73 frames per subint for twelve subints
// after its long runs, and whole subints where the offset wrapped past the
// buffer; fxcorr/test/gaps/run_filler.sh reproduces both).  So scan first,
// recompute, and repeat until the position stops moving: a gap pulls it back
// before the stretch's end, filler pushes it further into a region that scan
// has already counted.
//
// `uncorrected` is locate()'s raw value and `locatecount` its delay-realignment
// skip, both taken before this call and reapplied on every attempt -- a read
// whose own scan finds filler or a gap calls this again (readSubint), and the
// clamp below must not accumulate its block skip twice.  Returns false when the
// subint lies entirely before the file's first frame.
long long DataReader::correctedPosition(long long uncorrected)
{
	long long pos = uncorrected + ledger.fillerFrames()*(long long)framebytes;
	lastgapframes = 0;
	if(!ledger.empty())
	{
		// gapShift moves the query point out of any hole it fell inside, so
		// the read lands on the first frame this subint's time range actually
		// has; the move itself is a shift of the position, not of the count.
		long long t = lastframesin;
		lastgapframes = ledger.gapShift(t);
		pos += (t - lastframesin)*(long long)framebytes;
		pos -= lastgapframes*(long long)framebytes;
	}
	return pos;
}

bool DataReader::settleReadPosition(long long uncorrected, int locatecount, long long *fileoffset)
{
	*fileoffset = uncorrected;
	lastcount = locatecount;
	lastgapframes = 0;
	lastuncorrected = uncorrected;
	lastsettlepasses = 0;

	// Two things happen here and nowhere else: the position is computed from the
	// corrections (correctedPosition, pure arithmetic on the ledger) and the
	// stretch it implies is read and counted (scanStretch, the one piece of I/O
	// in it).  The loop is between the two -- a scan that finds filler or a gap
	// has moved the very position it was scanning towards, so the position is
	// computed again and looked at again until it stops moving.
	for(int pass=0; ; pass++)
	{
		*fileoffset = correctedPosition(uncorrected);
		long long gapnew = 0;
		long long fillernew = 0;
		if(*fileoffset > 0)
			fillernew = scanStretch(*fileoffset, &gapnew);
		if(fillernew == 0 && gapnew == 0)
			break;
		// the lifetime counters are kept by checkFrameContinuity for what it
		// scans itself; what the scan here picks up would otherwise never reach
		// them (this read never gets there)
		gapcheckmissing += gapnew;
		gapcheckfillers += fillernew;
		lastsettlepasses = pass + 1;
		if(pass >= 63)
		{
			// Each pass scans the stretch the previous one's filler uncovered,
			// so the position walks a filler run in steps of that stretch's
			// length and the total work is the run's own length, not its
			// square -- t25362's longest run (508 frames) settles in a dozen.
			// The cap is here for a file with no data frame for megabytes.
			cerr << "DataReader: skipped-stretch scan did not settle after 64 passes "
			     << "(offset " << *fileoffset << ", frame " << lastframesin << ")" << endl;
			break;
		}
	}

	if(*fileoffset >= 0)
		return true;

	// The subint starts before the file's first frame.  That does NOT mean the
	// subint is empty: a file may begin mid-second (anchorbytes holds the
	// distance), so this subint's head has no data while its tail does.  Read
	// from the file start and let fillValidFlags clear the blocks the missing
	// head covers -- the same treatment locate() gives a delay-realignment skip,
	// and where upstream's segoffbytes lands.
	//
	// Dropping the whole subint here instead threw the tail away too: on
	// t25362's first integration that cost 25% of the weight (fxcorr 0.9000 vs
	// mpifxcorr 0.9039 and rising with the file's start offset), and on a
	// synthetic file starting 80 ms into the batch it halved it (0.5 vs 0.912).
	// Only a subint lying entirely before the file start is dead.
	long long skippedframes = (-*fileoffset + (long long)framebytes - 1)/(long long)framebytes;
	long long skippedblocks = (skippedframes*(long long)payloadbytes + (long long)blockbytes - 1)/(long long)blockbytes;
	if(skippedblocks >= (long long)blockspersend)
		return false;

	lastcount += (int)skippedblocks;
	*fileoffset = 0;
	return true;
}

int DataReader::readSubint(int scan, int offsetsec, int offsetns, u8 *buffer, int bufsize, int *sec, int *ns)
{
	if(bufsize < sendbytes)
	{
		cerr << "DataReader: buffer too small (" << bufsize << " < " << sendbytes << ")" << endl;
		exit(EXIT_FAILURE);
	}

	// V1: one file per scan, files in scan order
	if(scan < 0 || scan >= numfiles)
	{
		*sec = Mode::INVALID_SUBINT;
		*ns = 0;
		return 0;
	}
	openFile(scan);

	// Every subint's read starts from a clean stream state.  readWindow's
	// doubling loop walks to the end of the file when the slots cannot be
	// filled before it, which sets eofbit and failbit; openFile only clears
	// them when it actually opens a file, so within one file the next subint
	// would inherit them -- seekg leaves the failbit set, its good() test then
	// fails and readWindow returns -1, silently dropping that whole subint
	// (M2 of fxcorr/test/gaps/run_mixed.sh: a filler run longer than the
	// subint's own frame count leaves less than a subint of data after it, so
	// filling the slots means reading to the end of the file).  For a read
	// that stopped inside the file this is a no-op.
	input.clear();

	long long fileoffset = 0;
	if(!locate(scan, offsetsec, offsetns, sec, ns, &fileoffset))
	{
		*sec = Mode::INVALID_SUBINT;
		*ns = 0;
		return 0;
	}

	// P12 step 2a: locate() maps time onto file bytes linearly, which only
	// holds while the file's frame sequence is one frame per time slot.  Both
	// kinds of damage seen so far shift the map, in opposite directions:
	//
	//   * a frame missing from the file leaves it shorter than the time axis,
	//     so a later subint's position points too far along -- subtract;
	//   * a filler frame has bytes but no time slot, leaving the file longer
	//     than the time axis, so a later position points too early -- add.
	//
	// Both shifts are whole frames, so frame alignment is preserved.
	//
	// The gap correction is computed for *this* subint's place on the time
	// axis: a gap that this subint's data starts after shortens the read, one
	// that still lies ahead of it does not (ledger.gapShift).  Everything the scan
	// has noticed so far would be too much for the subint the gap straddles.
	//
	// A negative result is NOT clamped: it means the subint asks for data
	// before the file's first frame (the file can start after the batch does
	// -- see anchorbytes), and letting the seek fail makes the subint come
	// back invalid, which is the honest answer.
	if(!settleReadPosition(fileoffset, lastcount, &fileoffset))
	{
		*sec = Mode::INVALID_SUBINT;
		*ns = 0;
		return 0;
	}
	lastfileoffset = fileoffset;

	if(kind == KIND_MUXEDVDIF)
	{
		// interlaced frames into the demux buffer, then corner-turn
		// (upstream datastream.cpp:810-843 readonedemux + diskToMemory
		// multiplex).  Per-subint self-contained: counters reset and the
		// reference frame re-read from this subint's first frame, so each
		// subint can be seeked independently.
		input.seekg(fileoffset, ios::beg);
		if(!input.good())
		{
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		// counters first: getCurrentDemuxBuffer() and deinterlace() both use
		// slot (counter%DEMUX_BUFFER_FACTOR), so a fresh subint must reset
		// before either touches the demux buffer
		muxer->resetcounters();
		u8 *demuxbuf = muxer->getCurrentDemuxBuffer();	// slot 0
		input.read(reinterpret_cast<char *>(demuxbuf), muxer->getSegmentBytes());
		int got = input.gcount();
		if(got <= 0)
		{
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		if(!muxer->initialise())
		{
			cerr << "DataReader: VDIF muxer initialise failed" << endl;
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		muxer->incrementReadCounter();
		if(!muxer->deinterlace(got))
		{
			cerr << "DataReader: VDIF deinterlace failed" << endl;
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		int muxbytes = muxer->multiplex(buffer);
		return muxbytes;
	}

	if(kind == KIND_MARK5B)
	{
		// sequential read + fix, like Mark5BDataStream::dataRead
		// (mark5bfile.cpp:412-533): mark5bfix syncs to the frame pattern,
		// drops interlopers and fills gaps; startOutputFrameNumber = -1
		// lets it re-sync on every call
		input.seekg(fileoffset, ios::beg);
		if(!input.good())
		{
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		input.read(reinterpret_cast<char *>(readbuffer), readbuffersize);
		int got = input.gcount();
		if(got <= 0)
		{
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		resetmark5bfixstatistics(&m5bstats);
		int fixReturn = mark5bfix(buffer, sendbytes, readbuffer, got, (int)framespersecond, -1, &m5bstats);
		if(fixReturn < 0)
		{
			cerr << "DataReader: mark5bfix returned " << fixReturn << " (srcsize=" << got
			     << ", destsize=" << sendbytes << ", framespersecond=" << framespersecond << ")" << endl;
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		return m5bstats.destUsed;
	}

	// KIND_VDIF, KIND_MK5STREAM and KIND_LBA all read raw bytes in sequence
	// (upstream: VDIFDataStream/mark5access-seeked Mk5DataStream/base DataStream);
	// LBA additionally skips the ASCII header in front of the payload.  The
	// window itself is readWindow's job (C3) -- what is left here is the order
	// of the steps: where to read, with which corrections, and what to hand back.
	const int headskip = (kind == KIND_LBA) ? headerbytes : 0;
	int bytes = readWindow(fileoffset, headskip, buffer, &lastcontiguous);
	if(bytes < 0)
	{
		// past end of file: nothing left for this subint
		*sec = Mode::INVALID_SUBINT;
		*ns = 0;
		return 0;
	}
	return bytes;
}

// Reads one subint's window from the file and rebuilds its frame grid (C3: the
// read half of readSubint, which used to run inline between the other kinds'
// branches).
//
// B2 (reader-model.md 4.8): the window is filled by TIME SLOTS, not by input
// bytes.  The first read goes straight into the output buffer -- the
// no-interruption case, byte for byte what it always was -- and is usually all
// there is to it.  A filler run inside it eats slots without supplying frames,
// though, and the frames making up the difference sit further along the file;
// upstream's vdifmux walks over the run for exactly that reason.  Only when the
// first stretch comes up short does the read move to inbuf and grow.
//
// Returns the bytes to hand over (for VDIF that is the slot grid, holes
// included -- not the byte count that came back from the file, since that is
// what the block validators have to see), or -1 when the position is past the
// end of the file.  *contiguousp is set to whether the window was a run of file
// bytes at all: once it bridged an interruption the buffer is a frame grid on
// the time axis (filler dropped, gap slots empty), which a byte-wise consumer
// cannot walk (P6's switched power checks this).
int DataReader::readWindow(long long fileoffset, int headskip, u8 *buffer, bool *contiguousp)
{
	const int slots = (framebytes > 0) ? sendbytes/framebytes : 0;
	const u8 *src = buffer;
	long long span;			// file bytes this read walks over

	input.seekg(fileoffset + headskip, ios::beg);
	if(!input.good())
		return -1;

	input.read(reinterpret_cast<char *>(buffer), sendbytes);
	int scanbytes = input.gcount();
	span = scanbytes;

	if(kind == KIND_VDIF && slots > 0 && scanbytes == sendbytes)
	{
		int used = 0;
		if(shiftFrameGaps(buffer, scanbytes/framebytes, NULL, slots, &used, true) < slots)
		{
			// Doubling finds the length in a few passes without having to know
			// the run's size beforehand.  The bound keeps a file that is filler
			// to the end from growing the buffer without end; past it the read
			// stays whatever the last attempt held, which is the pre-B2
			// behaviour.
			const int maxread = sendbytes*16;
			int want = sendbytes*2;
			while(want > 0)
			{
				if(want > inbufsize)
				{
					delete [] inbuf;
					inbufsize = want;
					inbuf = new u8[inbufsize];
				}
				input.clear();
				input.seekg(fileoffset + headskip, ios::beg);
				input.read(reinterpret_cast<char *>(inbuf), want);
				int have = input.gcount();
				int used2 = 0;
				int filled = shiftFrameGaps(inbuf, have/framebytes, NULL, slots, &used2, true);
				// done when the slots fill up, the file ends, or the bound is
				// reached
				if(filled >= slots || have < want || want >= maxread)
				{
					src = inbuf;
					// only as far as the slots needed: bytes past that point
					// belong to the next subint and must not enter this one's
					// gap/filler books, which decide the next read position
					// (corrections.h)
					scanbytes = (int)((long long)used2*(long long)framebytes);
					span = have;
					break;
				}
				want = (want > maxread/2) ? maxread : want*2;
			}
		}
	}

	// lastfileoffset (the corrected position actually read from) is what maps
	// a frame index onto the file, so that is what identifies a gap
	int placed = checkFrameContinuity(src, scanbytes, lastfileoffset, buffer, slots);	// P12: gaps and their fix

	// No retry here (tried 2026-09-19, measured wrong): a buffer whose own scan
	// found filler is NOT read from a stale position.  The filler sits after
	// the subint's data starts, so it does not enter this subint's correction at
	// all -- the filler tally the position is built from is the filler *before*
	// this subint, and re-reading with the new total in it moved
	// the position forward by the run's leading edge, pushing the subint's own
	// first frames out of the read (run_filler.sh: subint 2 went 36 -> 122
	// invalid blocks, all of them data).  shiftFrameGaps already treats the
	// trailing filler correctly: it drops it, shifts the data up and marks the
	// slots left at the end -- which are exactly the ones the interruption
	// emptied.
	if(kind == KIND_VDIF)
	{
		*contiguousp = (span == (long long)placed*(long long)framebytes);
		return placed*framebytes;
	}
	*contiguousp = true;
	return scanbytes;
}

void DataReader::fillValidFlags(s32 *flags, int validbytes) const
{
	// one bit per FFT block; a block is valid if its bytes are within the
	// data actually read (datastream.cpp:568-613 slow path).  Blocks skipped
	// by the delay realignment (locate's count) are forced invalid.  The
	// cross-segment clause (:604-605) degenerates here: a full read window
	// over a continuous file is exactly upstream's "full segment + contiguous
	// next segment" case, and the file tail is the "segment not full" case.
	memset(flags, 0, sizeof(s32)*((blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT));
	for(int i=lastcount;i<blockspersend;i++)
	{
		if((long long)(i-lastcount)*blockbytes < validbytes)
			flags[i/FLAGS_PER_INT] |= 1<<(i%FLAGS_PER_INT);
	}

	// P12 step 2b: the frames a gap swallowed carry no data, so the blocks
	// covering the holes left by shiftFrameGaps are cleared.  Block i is
	// unpacked at payload byte (i-lastcount)*blockbytes of the buffer
	// (mode.cpp:677-679 turns the block index into a sample offset, and the
	// sample the buffer starts at carries the delay-realignment skip), while
	// frame f contributes its payload at payload byte f*payloadbytes -- the
	// two are in the same coordinate system, so the blocks to clear are the
	// ones overlapping the hole.
	if(blockbytes > 0 && payloadbytes > 0)
	{
		for(size_t g=0; g<gapinvalid.size(); g++)
		{
			long long f0 = gapinvalid[g].first, f1 = gapinvalid[g].second;
			long long b0 = (f0*(long long)payloadbytes)/blockbytes + lastcount;
			long long b1 = (f1*(long long)payloadbytes + blockbytes - 1)/blockbytes + lastcount;
			for(long long b=b0; b<b1 && b<blockspersend; b++)
				flags[b/FLAGS_PER_INT] &= ~(1<<(b%FLAGS_PER_INT));
		}
	}
}
