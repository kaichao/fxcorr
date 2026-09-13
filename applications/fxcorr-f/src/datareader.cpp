#include "datareader.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT

using namespace std;

DataReader::DataReader(Configuration *conf, int confindex, int ds, Model *mdl,
                       long long batchstartsec, int batchstartns) :
	config(conf), model(mdl), configindex(confindex), dsindex(ds),
	framebytes(0), payloadbytes(0), framespersecond(0), sendbytes(0),
	blockspersend(0), intclockseconds(0), numfiles(0), datafilenames(0),
	currentfile(-1), currentscanstartsec(0), currentscan(0),
	batchstartabsns(batchstartsec*1000000000LL + (long long)batchstartns),
	lastfileoffset(0)
{
	// V1 restriction: local VDIF files only (datasim output), one mux thread
	Configuration::dataformat format = config->getDataFormat(configindex, dsindex);
	if(format != Configuration::VDIF && format != Configuration::VDIFL)
	{
		cerr << "DataReader: unsupported data format " << format << " (V1 supports VDIF only)" << endl;
		exit(EXIT_FAILURE);
	}
	if(config->getDNumMuxThreads(configindex, dsindex) != 1)
	{
		cerr << "DataReader: V1 does not support multiplexed VDIF (nummuxthreads must be 1)" << endl;
		exit(EXIT_FAILURE);
	}

	// fixed frame parameters (vdiffile.cpp:397-401)
	payloadbytes = config->getFramePayloadBytes(configindex, dsindex);
	framespersecond = config->getFramesPerSecond(configindex, dsindex);	// nthreads==1 checked above
	framebytes = config->getFrameBytes(configindex, dsindex);
	sendbytes = config->getDataBytes(configindex, dsindex);	// frame-aligned, guard included
	blockspersend = config->getBlocksPerSend(configindex);

	// internal clock offset in whole seconds (datastream.cpp:158)
	intclockseconds = (long long)floor(config->getDClockCoeff(0, dsindex, 0)/1000000.0 + 0.5);

	numfiles = config->getDNumFiles(configindex, dsindex);
	if(numfiles < 1)
	{
		cerr << "DataReader: no data files for datastream " << dsindex << endl;
		exit(EXIT_FAILURE);
	}
	datafilenames = config->getDDataFileNames(configindex, dsindex);
}

DataReader::~DataReader()
{
	if(input.is_open())
		input.close();
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
	if(!foundok)
		return false;
	delayus1 -= (double)intclockseconds*1000000.0;
	long long firstoffsetns = ((long long)offsetns) - (long long)(delayus1*1000.0);

	// delay-corrected start relative to the scan start, in ns
	long long relstartns = (long long)offsetsec*1000000000LL + firstoffsetns;

	// byte offsets are relative to the batch start, not the scan start: the
	// file holds this batch's data (data-spec 5.2 file-per-batch layout).
	// batchstartabsns is in day-seconds (config->getStartSeconds() frame); the
	// scan-relative frame here is offset by experseconds + scanstartsec.
	long long batchrelscan = batchstartabsns
	                       - (config->getStartSeconds() + currentscanstartsec)*1000000000LL;
	long long batchrel = relstartns - batchrelscan;

	// frame-aligned start (vdiffile.cpp:429-444; V1 frame granularity is 1)
	double framed = (double)batchrel*framespersecond/1.0e9;
	long long framesin = (long long)floor(framed);
	if(framesin < 0)
		framesin = 0;	// start slightly before the batch start: begin at the first frame

	*fileoffset = framesin*(long long)framebytes;

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

	input.seekg(fileoffset, ios::beg);
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
	// data actually read (datastream.cpp:600-604)
	int fftchannels = config->getFNumChannels(config->getDRecordedFreqIndex(configindex, dsindex, 0))*2;
	if(config->getDSampling(configindex, dsindex) == Configuration::COMPLEX)
		fftchannels = config->getFNumChannels(config->getDRecordedFreqIndex(configindex, dsindex, 0));
	int blockbytes = (fftchannels*config->getDBytesPerSampleNum(configindex, dsindex))/config->getDBytesPerSampleDenom(configindex, dsindex);

	memset(flags, 0, sizeof(s32)*((blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT));
	for(int i=0;i<blockspersend;i++)
	{
		if((long long)i*blockbytes < validbytes)
			flags[i/FLAGS_PER_INT] |= 1<<(i%FLAGS_PER_INT);
	}
}
