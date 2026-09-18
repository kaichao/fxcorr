#include "datareader.h"
#include "log.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT

using namespace std;

namespace {

// Frame number of frame i of a raw VDIF buffer: word 1 of the VDIF header,
// low 24 bits (word layout per vdifio's vdif_header, not the vlbi.org spec).
inline long long vdifFrameNumber(const u8 *buffer, int i, int framebytes)
{
	const u8 *p = buffer + (long long)i*framebytes;
	u32 w1 = (u32)p[4] | ((u32)p[5] << 8) | ((u32)p[6] << 16) | ((u32)p[7] << 24);
	return (long long)(w1 & 0xFFFFFF);
}

// Is frame i a filler -- a frame the recorder wrote where a recording
// interruption left it without data?  Two shapes occur: the VDIF invalid bit
// (word 0 bit 31) set, or a completely zero header (which is what t25362's BA
// thread 2 uses -- seconds 0, frame number 0, frame length 0, so the frame
// also claims a length of 0 bytes).  Such a frame occupies its bytes in the
// file but no slot on the time axis: after a run of them the frame number
// picks up from the last real frame plus the frames actually lost, not plus
// the filler count.  Counting them as missing frames instead (which the
// frame-number step alone cannot distinguish) inflates the correction by
// roughly a whole second's worth of frames per run boundary.
inline bool vdifIsFiller(const u8 *buffer, int i, int framebytes)
{
	const u8 *p = buffer + (long long)i*framebytes;
	u32 w0 = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
	if(w0 & 0x80000000u)
		return true;	// invalid bit set
	u32 w1 = (u32)p[4] | ((u32)p[5] << 8) | ((u32)p[6] << 16) | ((u32)p[7] << 24);
	return (w0 & 0x3FFFFFFFu) == 0 && (w1 & 0xFFFFFFu) == 0;
}

}

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
	gapcountedthrough(0), gapcountedvalid(false),
	fillershiftbytes(0), fillercountedthrough(0), fillercountedvalid(false),
	lastframesin(0), lastframens(0),
	lastgapframes(0), lastshiftdst(0),
	gapbuffer(0)
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
// P12 step 2a turns both observations into corrections -- the gaps recorded in
// gapspan (read back through gapshiftAt) and fillershiftbytes, applied in
// readSubint -- and step 2b (shiftFrameGaps)
// rebuilds this buffer's frame grid.  A file with neither follows exactly the
// old code path.
void DataReader::checkFrameContinuity(u8 *buffer, int bytes, long long readoffset)
{
	if(kind != KIND_VDIF || framebytes < 8 || framespersecond < 1)
		return;

	int nframes = bytes/framebytes;
	gapchecksubints++;
	// Both gap lists describe the CURRENT buffer only and are rebuilt from
	// scratch on every scan: the holes are refilled by this subint's own read,
	// and a stale entry would invalidate blocks of every later subint.
	gapinvalid.clear();
	if(nframes < 1)
		return;

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
	long long pfr = -1;
	long long fillernew = 0;
	bool reorder = false;

	// A buffer whose filler made fillershiftbytes grow also moved the next
	// read position forward by that growth, so the next buffer starts past a
	// stretch that was never scanned.  Those frames still have to be counted
	// (nothing else will ever look at them) but they cannot be assumed to be
	// filler -- the stretch is "the filler this buffer found" plus ordinary
	// data, and counting the data as filler overshoots the correction, which
	// moves the following read even further and runs away.  So read the
	// stretch and count what is really there.  It is bounded by the filler run
	// that caused it (hundreds of frames in t25362, i.e. a few hundred kB),
	// and happens only when a run is found.
	if(fillercountedvalid && readoffset > fillercountedthrough)
	{
		long long gapnew = 0;
		long long chainlast = -1;
		fillernew += countFillerRange(fillercountedthrough, readoffset,
		                              gapchecklastvalid ? gapchecklastfr : -1, &gapnew, &chainlast);
		missing += gapnew;	// reaches gapspan with the main loop's tally
		if(chainlast >= 0)
		{
			pfr = chainlast;	// continue the chain into this buffer: the seam
			                    // between the stretch and the buffer is where the
			                    // rest of ds_2's gaps sat, and neither side used
			                    // to look at it
			gapchecklastfr = chainlast;	// and carry the chain forward, so the
			                            // next stretch starts where this one
			                            // stopped.  Without it every later stretch
			                            // re-compares against the same stale tail
			                            // number and one gap is counted again and
			                            // again (t25362 ds_2: 143 real missing
			                            // frames inflated to 252).
		}
	}

	for(int i=0;i<nframes;i++)
	{
		if(vdifIsFiller(buffer, i, framebytes))
		{
			// Filler: bytes in the file, no slot on the time axis.  It
			// neither breaks the frame-number chain (the real frames either
			// side of a run are consecutive apart from the frames actually
			// lost) nor counts as missing, but everything after it sits
			// further along the file than the time axis says -- that is
			// fillershiftbytes, applied in readSubint with the opposite sign
			// to a real gap.  Counted once per filler frame, by file position
			// (the corrected read position stays monotonic), because
			// consecutive subint reads overlap by their guard margin.
			long long fpos = readoffset + (long long)i*framebytes;
			if(!fillercountedvalid || fpos >= fillercountedthrough)
				fillernew++;	// frames in the guard overlap were counted before
			reorder = true;
			continue;	// not part of the frame-number chain
		}
		long long fr = vdifFrameNumber(buffer, i, framebytes);
		gapcheckframes++;
		if(firstany < 0)
			firstany = fr;
		lastany = fr;

		if(pfr >= 0)
		{
			long long step = (fr - pfr + fps) % fps;
			if(step != 1)
			{
				long long m = (step == 0) ? 0 : step - 1;
				if(m > 0)
				{
					reorder = true;
					// the read-position correction counts every missing frame
					// exactly once.  Successive subints keep seeing the same
					// gap while the corrected read position catches up with
					// it, and the frame number wraps once a second, so a gap
					// is identified by where it sits in the file: the offset
					// of the frame right after it.  readoffset is the
					// corrected position actually read from, which stays
					// monotonic despite the corrections.
					long long gapend = readoffset + (long long)i*framebytes;
					if(!gapcountedvalid || gapend > gapcountedthrough)
					{
						missing += m;
						gapcountedthrough = gapend;
						gapcountedvalid = true;
						gapcheckgaps++;
						gapspan.push_back(make_pair(gapend, m));
						FXLOG(FXLOG_VERBOSE) << "GAPCHECK buffer " << gapchecksubints << " frame " << i
						     << ": frameno " << pfr << " -> " << fr
						     << " (step " << step << ", missing " << m << ")" << endl;
					}
				}
			}
		}
		pfr = fr;
	}

	// P12 step 2a: account for the newly found missing frames and filler
	// bytes.  A gap makes the file shorter than the time axis (later reads are
	// too far along, so subtract); filler makes it longer (later reads are too
	// early, so add).  Both are whole frames, so frame alignment is preserved.
	// The gaps themselves went into gapspan as they were found; how much of
	// each one applies to a given subint is decided by gapshiftAt at read time.
	if(missing > 0)
		gapcheckmissing += missing;
	if(fillernew > 0)
	{
		gapcheckfillers += fillernew;
		fillershiftbytes += fillernew*(long long)framebytes;
	}

	// watermark for the next buffer: the furthest whole frame this scan
	// covered, so the guard overlap between consecutive reads is not counted
	// twice and the distance to the next read position is a whole number of
	// frames
	{
		long long end = readoffset + (long long)(bytes/framebytes)*framebytes;
		if(!fillercountedvalid || end > fillercountedthrough)
		{
			fillercountedthrough = end;
			fillercountedvalid = true;
		}
	}

	// A buffer can need rebuilding even with unbroken frame numbers: when a
	// gap covers the subint's own start, the read position lands on the first
	// frame after the gap, so the whole buffer sits that much further into the
	// subint and the slots ahead of it are the hole.  Frame numbers say by how
	// much -- the buffer's first frame is that far from the start's own
	// in-second number (lastframens, from locate), and shiftFrameGaps places
	// it there.  Without this the buffer would be handed over starting at slot
	// 0 and the hole would be integrated as data.
	if(!reorder && !gapspan.empty() && firstany >= 0 &&
	   (int)((firstany - (long long)lastframens + fps) % fps) > 0)
		reorder = true;

	// P12 step 2b: put the frames where the time axis says they belong
	if(reorder)
		shiftFrameGaps(buffer, nframes);
	else
		lastshiftdst = 0;

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
	FXLOG(FXLOG_VERBOSE) << "READPOS subint " << gapchecksubints
	     << ": readoff " << readoffset
	     << " firstfno " << firstany << " lastfno " << lastany
	     << " nframes " << nframes << " missing " << missing << " filler " << fillernew
	     << " gapshift " << lastgapframes*(long long)framebytes
	     << " fillershift " << fillershiftbytes
	     << " gapframes " << lastgapframes << " dst " << lastshiftdst
	     << " framens " << lastframens
	     << endl;
}

// Frames of the file missing *before* frame index t on the time axis.
//
// A gap is recorded by the file offset of the frame that follows it, so the
// slots it leaves empty sit at [start, end) with start = that offset (in
// frames) plus everything lost ahead of it.  Only the part lying before the
// query point counts: a gap whose frames are still ahead of this subint must
// not shorten its read, or the subint is read from an earlier time than its
// own and that data is integrated as if it belonged there.  t25362's boundary
// subint came out 51 frames early exactly that way (its gap was noticed inside
// the previous subint's buffer but sits after that subint's end, at the start
// of this one's); fxcorr/test/gaps/run_boundary.sh reproduces it on synthetic
// data where the expected values are known.
long long DataReader::gapshiftAt(long long &t) const
{
	long long lost = 0;	// frames lost ahead of the gap being measured
	long long total = 0;
	for(size_t i=0;i<gapspan.size();i++)
	{
		// gapspan holds absolute file offsets while t is a frame index counted
		// from the batch start; anchorbytes is the distance between the two
		// origins (negative when the file starts after the batch does, as
		// t25362's BA does).  Comparing them without it gets every gap wrong,
		// and on a file with no start offset the two coincide -- which is why
		// the synthetic tests cannot see it.
		long long start = (gapspan[i].first - anchorbytes)/(long long)framebytes + lost;
		long long end = start + gapspan[i].second;
		if(t < start)
			break;		// this gap and every later one start at or after t
		// A gap the query point falls inside swallows the whole of itself: the
		// frames before the query sit in it too, and the read has to land
		// after the hole rather than inside data belonging to a later slot.
		// Abutting gaps are handled by the next iterations of this loop.
		if(t < end)
			t = end;
		total += gapspan[i].second;
		lost += gapspan[i].second;
	}
	return total;
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
void DataReader::shiftFrameGaps(u8 *buffer, int nframes)
{
	if(!gapbuffer)
		gapbuffer = new u8[sendbytes];
	memset(gapbuffer, 0, (size_t)sendbytes);

	gapinvalid.clear();
	long long fps = (long long)framespersecond;
	int dst = 0;
	long long prevfr = -1;

	// The buffer's first data frame belongs where its own frame number says,
	// which is not the subint's start when a gap covers that start: the read
	// position then lands on the first frame after the gap, and the slots the
	// gap ate belong before it.  Placing that frame at 0 instead slides the
	// whole buffer back and marks the wrong slots invalid, so the hole is
	// integrated as if it were data.  The frame number carries the distance
	// needed -- lastframens (locate) is the start's own in-second number.
	for(int src=0; src<nframes; src++)
	{
		if(vdifIsFiller(buffer, src, framebytes))
			continue;
		dst = (int)((vdifFrameNumber(buffer, src, framebytes) - (long long)lastframens
		             + fps) % fps);
		break;
	}
	lastshiftdst = dst;
	if(dst >= nframes)
	{
		// the gap reaches past this whole buffer: none of it is this subint's data
		gapinvalid.push_back(make_pair(0, nframes));
		memset(buffer, 0, (size_t)nframes*framebytes);
		return;
	}
	if(dst > 0)
		gapinvalid.push_back(make_pair(0, dst));

	for(int src=0; src<nframes && dst<nframes; src++)
	{
		if(vdifIsFiller(buffer, src, framebytes))
			continue;	// no time slot: drop it, later frames move up

		long long fr = vdifFrameNumber(buffer, src, framebytes);
		if(prevfr >= 0)
		{
			long long miss = (fr - prevfr - 1 + fps) % fps;
			if(miss > 0)
			{
				long long hole = miss;
				if(hole > nframes - dst)
					hole = nframes - dst;
				if(hole > 0)
					gapinvalid.push_back(make_pair(dst, dst + (int)hole));
				dst += (int)miss;
				if(dst >= nframes)
					break;
			}
		}
		memcpy(gapbuffer + (long long)dst*framebytes,
		       buffer + (long long)src*framebytes, (size_t)framebytes);
		dst++;
		prevfr = fr;
	}

	// everything the source did not fill (a dropped filler run at the end, or
	// frames pushed past the buffer) holds no data
	if(dst < nframes)
		gapinvalid.push_back(make_pair(dst, nframes));

	memcpy(buffer, gapbuffer, (size_t)nframes*framebytes);

	// Where the holes landed, as post-shift frame ranges: the counterpart of
	// READPOS's dst for this subint's valid flags.  One line per rebuilt buffer,
	// so t25362's filler datastream (1162 frames) prints a few hundred at
	// verbose -- that is the level for exactly this kind of question.
	FXLOG(FXLOG_VERBOSE) << "GAPCHECK holes buf " << gapchecksubints << ":";
	for(size_t g=0;g<gapinvalid.size();g++)
		FXLOG(FXLOG_VERBOSE) << " [" << gapinvalid[g].first << "," << gapinvalid[g].second << ")";
	FXLOG(FXLOG_VERBOSE) << endl;
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
// own tally so they reach gapspan with the same treatment.  *lastfrp
// gets the last data frame number in the stretch so the caller can continue
// the chain into its own buffer -- a gap lying exactly on that seam is
// otherwise checked by neither side (t25362 ds_2 lost 50 more frames there).
long long DataReader::countFillerRange(long long start, long long end, long long chainfr, long long *gapsp, long long *lastfrp)
{
	if(end <= start || framebytes < 8)
		return 0;
	long long nframes = (end - start)/(long long)framebytes;
	if(nframes < 1)
		return 0;

	const long long nbuf = 512;
	u8 *buf = new u8[(size_t)(nbuf*(long long)framebytes)];
	long long fps = (long long)framespersecond;
	long long counted = 0;
	long long missing = 0;
	long long pfr = chainfr;
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
		for(int i=0;i<gotframes;i++)
		{
			if(vdifIsFiller(buf, i, framebytes))
			{
				counted++;
				continue;	// not part of the frame-number chain
			}
			long long fr = vdifFrameNumber(buf, i, framebytes);
			if(pfr >= 0)
			{
				long long step = (fr - pfr + fps) % fps;
				if(step != 1)
				{
					long long m = (step == 0) ? 0 : step - 1;
					if(m > 0)
					{
						long long gapend = start + (pos + i)*(long long)framebytes;
						if(!gapcountedvalid || gapend > gapcountedthrough)
						{
							missing += m;
							gapcountedthrough = gapend;
							gapcountedvalid = true;
							gapcheckgaps++;
							gapspan.push_back(make_pair(gapend, m));
							FXLOG(FXLOG_VERBOSE) << "GAPCHECK skipped " << gapchecksubints
							     << ": frameno " << pfr << " -> " << fr
							     << " (step " << step << ", missing " << m << ")" << endl;
						}
					}
				}
			}
			pfr = fr;
		}
		pos += gotframes;
	}
	delete [] buf;
	*gapsp = missing;
	*lastfrp = pfr;
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
	// the gaps that lie before it on the time axis (see gapshiftAt)
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
	// that still lies ahead of it does not (gapshiftAt).  Everything the scan
	// has noticed so far would be too much for the subint the gap straddles.
	//
	// A negative result is NOT clamped: it means the subint asks for data
	// before the file's first frame (the file can start after the batch does
	// -- see anchorbytes), and letting the seek fail makes the subint come
	// back invalid, which is the honest answer.
	fileoffset += fillershiftbytes;
	lastgapframes = 0;
	if(!gapspan.empty())
	{
		// gapshiftAt moves the query point out of any hole it fell inside, so
		// the read lands on the first frame this subint's time range actually
		// has; the move itself is a shift of the position, not of the count.
		long long t = lastframesin;
		lastgapframes = gapshiftAt(t);
		fileoffset += (t - lastframesin)*(long long)framebytes;
		fileoffset -= lastgapframes*(long long)framebytes;
	}
	if(fileoffset < 0)
	{
		// The subint starts before the file's first frame.  That does NOT mean
		// the subint is empty: a file may begin mid-second (anchorbytes holds
		// the distance), so this subint's head has no data while its tail
		// does.  Read from the file start and let fillValidFlags clear the
		// blocks the missing head covers -- the same treatment locate() gives
		// a delay-realignment skip, and where upstream's segoffbytes lands.
		//
		// Dropping the whole subint here instead threw the tail away too: on
		// t25362's first integration that cost 25% of the weight (fxcorr 0.9000
		// vs mpifxcorr 0.9039 and rising with the file's start offset), and on a
		// synthetic file starting 80 ms into the batch it halved it (0.5 vs
		// 0.912).  Only a subint lying entirely before the file start is dead.
		long long skippedframes = (-fileoffset + (long long)framebytes - 1)/(long long)framebytes;
		long long skippedblocks = (skippedframes*(long long)payloadbytes + (long long)blockbytes - 1)/(long long)blockbytes;
		if(skippedblocks >= (long long)blockspersend)
		{
			*sec = Mode::INVALID_SUBINT;
			*ns = 0;
			return 0;
		}
		lastcount += (int)skippedblocks;
		fileoffset = 0;
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
	// LBA additionally skips the ASCII header in front of the payload
	input.seekg(fileoffset + ((kind == KIND_LBA) ? headerbytes : 0), ios::beg);
	if(!input.good())
	{
		// past end of file: nothing left for this subint
		*sec = Mode::INVALID_SUBINT;
		*ns = 0;
		return 0;
	}
	input.read(reinterpret_cast<char *>(buffer), sendbytes);
	int got = input.gcount();
	// lastfileoffset (the corrected position actually read from) is what maps
	// a frame index onto the file, so that is what identifies a gap
	checkFrameContinuity(buffer, got, lastfileoffset);	// P12: gaps and their fix
	return got;
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
