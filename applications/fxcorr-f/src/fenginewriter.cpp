#include "fenginewriter.h"

#include <cstring>
#include <cstdlib>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT
#include <fxcorrcommon/architecture.h>	// cf32, f32

using namespace std;

// .sp / pcal.bin / autocorr.bin file layouts follow fxcorr/data-spec.md 5.3
// (host byte order; single-machine in V1)

FEngineWriter::FEngineWriter(const string &outdir, Configuration *conf, int confindex, int ds, int nsubs) :
	config(conf), configindex(confindex), dsindex(ds), nsubints(nsubs),
	nrecordedbands(0), blockspersend(0), flagwords(0), autocorrchannels(0),
	haspcal(false), pcalfile(0), autocorrfile(0)
{
	nrecordedbands = config->getDNumRecordedBands(configindex, dsindex);
	blockspersend = config->getBlocksPerSend(configindex);
	flagwords = blockspersend/FLAGS_PER_INT;
	if(blockspersend%FLAGS_PER_INT)
		flagwords++;

	// autocorrelation width after averageFrequency() = recordedbandchannels/chanstoavg;
	// chanstoavg is datastream-wide, taken from the first recorded freq
	// (same formula as configuration.cpp's streamchanstoaverage)
	int freqindex = config->getDRecordedFreqIndex(configindex, dsindex, 0);
	autocorrchannels = config->getFNumChannels(freqindex)/config->getFChannelsToAverage(freqindex);

	haspcal = (config->getDPhaseCalIntervalHz(configindex, dsindex) > 0);

	// one .sp file per recorded band
	spfiles.resize(nrecordedbands);
	spnames.resize(nrecordedbands);
	for(int j=0;j<nrecordedbands;j++)
	{
		char filename[32];
		snprintf(filename, sizeof(filename), "band_%02d.sp", j);
		spnames[j] = outdir + "/" + filename;
		spfiles[j] = fopen(spnames[j].c_str(), "wb");
		if(spfiles[j] == NULL)
		{
			cerr << "FEngineWriter: cannot open " << spnames[j] << " for writing" << endl;
			exit(EXIT_FAILURE);
		}
		writeSpHeader(j);
	}

	if(haspcal)
	{
		pcalfile = fopen((outdir + "/pcal.bin").c_str(), "wb");
		if(pcalfile == NULL)
		{
			cerr << "FEngineWriter: cannot open " << outdir << "/pcal.bin for writing" << endl;
			exit(EXIT_FAILURE);
		}
		writePcalHeader();
	}

	autocorrfile = fopen((outdir + "/autocorr.bin").c_str(), "wb");
	if(autocorrfile == NULL)
	{
		cerr << "FEngineWriter: cannot open " << outdir << "/autocorr.bin for writing" << endl;
		exit(EXIT_FAILURE);
	}
	writeAutocorrHeader();
}

FEngineWriter::~FEngineWriter()
{
	for(int j=0;j<nrecordedbands;j++)
		if(spfiles[j] != NULL)
			fclose(spfiles[j]);
	if(pcalfile != NULL)
		fclose(pcalfile);
	if(autocorrfile != NULL)
		fclose(autocorrfile);
}

void FEngineWriter::writeSpHeader(int band)
{
	// fixed 256-byte header, reserved region zero-filled
	char buf[256];
	memset(buf, 0, sizeof(buf));

	memcpy(buf + 0, "FXCSP\0", 6);
	u32 version = 1;                       memcpy(buf + 6,  &version, 4);
	u32 bandindex = band;                  memcpy(buf + 10, &bandindex, 4);
	int freqindex = config->getDRecordedFreqIndex(configindex, dsindex, band);
	char pol = config->getDRecordedBandPol(configindex, dsindex, band);
	memcpy(buf + 14, &pol, 1);
	u32 nchan = config->getFNumChannels(freqindex);        memcpy(buf + 16, &nchan, 4);
	// .input freq table stores MHz; .sp header stores Hz
	f64 bandwidth = config->getFreqTableBandwidth(freqindex)*1.0e6;   memcpy(buf + 20, &bandwidth, 8);
	f64 bandedge = config->getFreqTableFreqLowedge(freqindex)*1.0e6;  memcpy(buf + 28, &bandedge, 8);
	u32 lsb = config->getFreqTableLowerSideband(freqindex) ? 1 : 0;   memcpy(buf + 36, &lsb, 4);
	u32 cx = (config->getDSampling(configindex, dsindex) == Configuration::COMPLEX) ? 1 : 0;
	                                                                  memcpy(buf + 40, &cx, 4);
	u32 nsub = nsubints;                   memcpy(buf + 44, &nsub, 4);
	u32 subns = config->getSubintNS(configindex);  memcpy(buf + 48, &subns, 4);
	u32 bps = blockspersend;               memcpy(buf + 52, &bps, 4);
	u32 nbf = config->getNumBufferedFFTs(configindex); memcpy(buf + 56, &nbf, 4);
	u32 fw = flagwords;                    memcpy(buf + 60, &fw, 4);

	if(fwrite(buf, 1, sizeof(buf), spfiles[band]) != sizeof(buf))
	{
		cerr << "FEngineWriter: error writing header of " << spnames[band] << endl;
		exit(EXIT_FAILURE);
	}
}

void FEngineWriter::writePcalHeader()
{
	// magic + version + counts
	fwrite("FXCPC\0", 1, 6, pcalfile);
	u32 version = 1;  fwrite(&version, 1, 4, pcalfile);
	u32 nsub = nsubints;  fwrite(&nsub, 1, 4, pcalfile);
	u32 ntones = config->getDMaxRecordedPCalTones(configindex, dsindex);  fwrite(&ntones, 1, 4, pcalfile);
	u32 nbands = nrecordedbands;  fwrite(&nbands, 1, 4, pcalfile);

	// per-band tone tables (tones concatenated in band order)
	for(int j=0;j<nrecordedbands;j++)
	{
		u32 bandindex = j;  fwrite(&bandindex, 1, 4, pcalfile);
		int localfreqindex = config->getDLocalRecordedFreqIndex(configindex, dsindex, j);
		int ntonesband = config->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex);
		u32 nt = ntonesband;  fwrite(&nt, 1, 4, pcalfile);
		char pol = config->getDRecordedBandPol(configindex, dsindex, j);
		for(int t=0;t<ntonesband;t++)
		{
			f64 tonefreq = config->getDRecordedFreqPCalToneFreqHz(configindex, dsindex, localfreqindex, t)/1.0e6;
			fwrite(&tonefreq, 1, 8, pcalfile);
			fwrite(&pol, 1, 1, pcalfile);
		}
	}
}

void FEngineWriter::writeAutocorrHeader()
{
	fwrite("FXCAC\0", 1, 6, autocorrfile);
	u32 version = 1;  fwrite(&version, 1, 4, autocorrfile);
	u32 nsub = nsubints;  fwrite(&nsub, 1, 4, autocorrfile);
	u32 nbands = nrecordedbands;  fwrite(&nbands, 1, 4, autocorrfile);
	for(int j=0;j<nrecordedbands;j++)
	{
		u32 bandindex = j;  fwrite(&bandindex, 1, 4, autocorrfile);
		u32 nchan = autocorrchannels;  fwrite(&nchan, 1, 4, autocorrfile);
	}
}

void FEngineWriter::writeSubintHeader(int scan, int sec, int ns, Mode *mode, const s32 *validflags)
{
	for(int j=0;j<nrecordedbands;j++)
	{
		// i32 scan / sec / ns
		s32 s = scan;  fwrite(&s, 1, 4, spfiles[j]);
		s = sec;       fwrite(&s, 1, 4, spfiles[j]);
		s = ns;        fwrite(&s, 1, 4, spfiles[j]);

		// valid flags, one bit per FFT block
		fwrite(validflags, 1, sizeof(u32)*flagwords, spfiles[j]);

		// per-band data weights (getDataWeight: perbandweights or plain dataweight)
		for(int b=0;b<blockspersend;b++)
		{
			f32 w = mode->getDataWeight(j, b);
			fwrite(&w, 1, 4, spfiles[j]);
		}
	}
}

void FEngineWriter::writeSpectra(int fftloop, Mode *mode)
{
	// spectra: subloop-major within each fftloop batch (same slot reuse as
	// Core::processdata), covering the FFT indices of this batch
	int numffts = blockspersend - fftloop*config->getNumBufferedFFTs(configindex);
	if(numffts > config->getNumBufferedFFTs(configindex))
		numffts = config->getNumBufferedFFTs(configindex);

	for(int j=0;j<nrecordedbands;j++)
	{
		int freqindex = config->getDRecordedFreqIndex(configindex, dsindex, j);
		int nchan = config->getFNumChannels(freqindex);
		for(int b=0;b<numffts;b++)
		{
			const cf32 *spec = mode->getFreqs(j, b);
			fwrite(spec, sizeof(cf32), nchan, spfiles[j]);
		}
	}
}

void FEngineWriter::writePcal(Mode *mode)
{
	if(!haspcal)
		return;

	// per subint: finalise the accumulated tones, then write them band by band
	mode->finalisepcal();
	for(int j=0;j<nrecordedbands;j++)
	{
		int localfreqindex = config->getDLocalRecordedFreqIndex(configindex, dsindex, j);
		int ntonesband = config->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex);
		for(int t=0;t<ntonesband;t++)
		{
			cf32 tone = mode->getPcal(j, t);
			fwrite(&tone, sizeof(cf32), 1, pcalfile);
		}
	}
}

void FEngineWriter::writeAutocorrelation(Mode *mode)
{
	// same order as Core::averageAndSendAutocorrs: average first, then copy out
	mode->averageFrequency();
	for(int j=0;j<nrecordedbands;j++)
	{
		const cf32 *ac = mode->getAutocorrelation(false, j);
		fwrite(ac, sizeof(cf32), autocorrchannels, autocorrfile);
		f32 w = mode->getWeight(false, j);
		fwrite(&w, 1, 4, autocorrfile);
	}
}
