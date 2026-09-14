#include "datareader.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT

using namespace std;

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
	headerbytes(0), bytesperns(0.0)
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
		anchorbytes = fileSummary.firstFrameOffset
			+ (long long)((double)(batchstartabsns - filefirstabsns)*framespersecond/1.0e9 + 0.5)*framebytes;

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
		anchorbytes += (long long)((double)(batchstartabsns - filefirstabsns)*framespersecond/1.0e9 + 0.5)*framebytes;
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
		anchorbytes = (long long)((double)(batchstartabsns - filefirstabsns)*bytesperns + 0.5);
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
	return input.gcount();
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
}
