#include "xmac.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace std;

XmacEngine::XmacEngine(Configuration *conf, int cindex) :
	config(conf), configindex(cindex),
	freqtablelength(0), numbaselines(0), numdatastreams(0),
	numBufferedFFTs(0), blockspersend(0), xmacstridelength(0),
	threadresultlength(0), threadcrosscorrs(0), baselineweight(0), conjbuf(0)
{
	freqtablelength = config->getFreqTableLength();
	numbaselines = config->getNumBaselines();
	numdatastreams = config->getNumDataStreams();
	numBufferedFFTs = config->getNumBufferedFFTs(configindex);
	blockspersend = config->getBlocksPerSend(configindex);
	xmacstridelength = config->getXmacStrideLength(configindex);
	threadresultlength = config->getMaxThreadResultLength();

	threadcrosscorrs = vectorAlloc_cf32(threadresultlength);

	// conjbuf sized for the widest freq in the table
	int maxnchan = 0;
	for(int i=0;i<freqtablelength;i++)
		maxnchan = max(maxnchan, config->getFNumChannels(i));
	conjbuf = vectorAlloc_cf32(maxnchan);

	// baselineweight[f][j][p] (single pulsar bin), allocated per numpolproducts
	baselineweight = new f32**[freqtablelength];
	for(int i=0;i<freqtablelength;i++)
	{
		baselineweight[i] = new f32*[numbaselines];
		for(int j=0;j<numbaselines;j++)
		{
			int localfreqindex = config->getBLocalFreqIndex(configindex, j, i);
			if(localfreqindex >= 0)
				baselineweight[i][j] = new f32[config->getBNumPolProducts(configindex, j, localfreqindex)];
			else
				baselineweight[i][j] = 0;
		}
	}
}

XmacEngine::~XmacEngine()
{
	vectorFree(threadcrosscorrs);
	vectorFree(conjbuf);
	for(int i=0;i<freqtablelength;i++)
	{
		for(int j=0;j<numbaselines;j++)
			delete [] baselineweight[i][j];
		delete [] baselineweight[i];
	}
	delete [] baselineweight;
}

void XmacEngine::zeroSubint()
{
	// core.cpp:722-759 (single bin, no shift decorr)
	vectorZero_cf32(threadcrosscorrs, threadresultlength);
	for(int i=0;i<freqtablelength;i++)
	{
		if(config->isFrequencyUsed(configindex, i))
		{
			for(int j=0;j<numbaselines;j++)
			{
				int localfreqindex = config->getBLocalFreqIndex(configindex, j, i);
				if(localfreqindex >= 0)
					vectorZero_f32(baselineweight[i][j], config->getBNumPolProducts(configindex, j, localfreqindex));
			}
		}
	}
}

void XmacEngine::xmacBatch(int fftloop, const vector<vector<SpReader *> > &readers)
{
	int status;

	// core.cpp:867-982, non-pulsar branch; resultindex accumulation mirrors
	// Configuration::populateResultLengths() so threadcrosscorrs offsets agree
	// with getThreadResultFreqOffset/getThreadResultBaselineOffset.
	int resultindex = 0;
	for(int f=0;f<freqtablelength;f++)
	{
		if(!config->isFrequencyUsed(configindex, f))
			continue;

		int freqchannels = config->getFNumChannels(f);
		int xmacpasses = config->getNumXmacStrides(configindex, f);
		for(int x=0;x<xmacpasses;x++)
		{
			int xmacstart = x*xmacstridelength;
			int xmacstrideremain = min(freqchannels-xmacstart, xmacstridelength);
			if(xmacstrideremain < 0)
				continue;

			for(int j=0;j<numbaselines;j++)
			{
				int localfreqindex = config->getBLocalFreqIndex(configindex, j, f);
				if(localfreqindex >= 0)
				{
					int ds1index = config->getBOrderedDataStream1Index(configindex, j);
					int ds2index = config->getBOrderedDataStream2Index(configindex, j);

					for(int fftsubloop=0;fftsubloop<numBufferedFFTs;fftsubloop++)
					{
						int i = fftloop*numBufferedFFTs + fftsubloop;
						if(i >= blockspersend)
							break; //may not have to fully complete last fftloop

						for(int p=0;p<config->getBNumPolProducts(configindex, j, localfreqindex);p++)
						{
							int band1 = config->getBDataStream1BandIndex(configindex, j, localfreqindex, p);
							int band2 = config->getBDataStream2BandIndex(configindex, j, localfreqindex, p);
							const SpReader *r1 = readers[ds1index][band1];
							const SpReader *r2 = readers[ds2index][band2];

							// vis1 = getFreqs(...)[xmacstart]; vis2 = conjugated counterpart
							const cf32 *vis1 = r1->spectra() + (long long)i*r1->numChannels() + xmacstart;
							const cf32 *vis2 = r2->spectra() + (long long)i*r2->numChannels() + xmacstart;

							status = vectorConj_cf32(vis2, conjbuf, xmacstrideremain);
							if(status != vecNoErr)
								cerr << "Error conjugating vis2, baseline " << j << ", status " << status << endl;

							status = vectorAddProduct_cf32(vis1, conjbuf, &(threadcrosscorrs[resultindex+p*xmacstridelength]), xmacstrideremain);
							if(status != vecNoErr)
								cerr << "Error trying to xmac baseline " << j << " frequency " << localfreqindex << " polarisation product " << p << ", status " << status << endl;
						}
					}
					// core.cpp:970-977 (non-pulsar): advance to next baseline's xmac slice
					resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*xmacstridelength;
				}
			}
		}
	}
}

void XmacEngine::accumulateWeights(int fftloop, const vector<vector<SpReader *> > &readers)
{
	// core.cpp:1005-1052, non-pulsar branch
	for(int fftsubloop=0;fftsubloop<numBufferedFFTs;fftsubloop++)
	{
		int i = fftloop*numBufferedFFTs + fftsubloop;
		if(i >= blockspersend)
			break; //may not have to fully complete last fftloop

		for(int f=0;f<freqtablelength;f++)
		{
			if(config->isFrequencyUsed(configindex, f))
			{
				for(int j=0;j<numbaselines;j++)
				{
					int localfreqindex = config->getBLocalFreqIndex(configindex, j, f);
					if(localfreqindex >= 0)
					{
						int ds1index = config->getBOrderedDataStream1Index(configindex, j);
						int ds2index = config->getBOrderedDataStream2Index(configindex, j);
						for(int p=0;p<config->getBNumPolProducts(configindex, j, localfreqindex);p++)
						{
							int ds1recordbandindex = config->getBDataStream1RecordBandIndex(configindex, j, localfreqindex, p);
							int ds2recordbandindex = config->getBDataStream2RecordBandIndex(configindex, j, localfreqindex, p);

							if(ds1recordbandindex < 0 || ds2recordbandindex < 0)
							{
								cerr << "Error: one of the record band indices could not be found: ds1recordbandindex = " << ds1recordbandindex << " ds2recordbandindex = " << ds2recordbandindex << endl;
							}
							else
							{
								f32 weight1 = readers[ds1index][ds1recordbandindex]->weights()[i];
								f32 weight2 = readers[ds2index][ds2recordbandindex]->weights()[i];
								baselineweight[f][j][p] += weight1*weight2;
							}
						}
					}
				}
			}
		}
	}
}

void XmacEngine::uvshiftAndAverage(double nsoffset, double nswidth, cf32 *subintresults)
{
	// core.cpp:1530-1563 with threadid==0 (startfreq/startbaseline both 0,
	// so every used (freq, baseline) pair is processed exactly once)
	for(int f=0;f<freqtablelength;f++)
	{
		if(config->isFrequencyUsed(configindex, f))
		{
			for(int i=0;i<numbaselines;i++)
				uvshiftAndAverageBaselineFreq(nsoffset, nswidth, f, i, subintresults);
		}
	}

	// clear the cross-corr results for the next averaging period (core.cpp:1560-1563)
	vectorZero_cf32(threadcrosscorrs, threadresultlength);
}

void XmacEngine::uvshiftAndAverageBaselineFreq(double nsoffset, double nswidth, int freqindex, int baseline, cf32 *subintresults)
{
	int localfreqindex, targetfreqindex, freqchannels, targetfreqchannels;
	int channelinc, targetchannelinc, stridestoaverage, averagesperstride, averagelength, numstrides;
	int xmacstrideremain, threadindex, threadstart, coreindex, coreoffset, coredest;
	double bandwidth, bandwidthoftarget, channelbandwidth, outchannelplacementpreavg;

	localfreqindex = config->getBLocalFreqIndex(configindex, baseline, freqindex);
	if(localfreqindex < 0)
		return;

	freqchannels = config->getFNumChannels(freqindex);
	channelinc = config->getFChannelsToAverage(freqindex);
	bandwidth = config->getFreqTableBandwidth(freqindex);
	targetfreqindex = config->getBTargetFreqIndex(configindex, baseline, localfreqindex);
	targetfreqchannels = config->getFNumChannels(targetfreqindex);
	targetchannelinc = config->getFChannelsToAverage(targetfreqindex);
	bandwidthoftarget = config->getFreqTableBandwidth(targetfreqindex);
	stridestoaverage = channelinc/xmacstridelength;
	if(stridestoaverage == 0)
		stridestoaverage = 1;
	averagesperstride = xmacstridelength/channelinc;
	if(averagesperstride == 0)
		averagesperstride = 1;
	averagelength = xmacstridelength/averagesperstride;
	numstrides = freqchannels/xmacstridelength;
	channelbandwidth = bandwidth/double(freqchannels);
	outchannelplacementpreavg = fabs(config->getFreqTableFreqLowedge(freqindex) - config->getFreqTableFreqLowedge(targetfreqindex)) / channelbandwidth;

	assert(targetfreqchannels == (int)(0.5 + (bandwidthoftarget / bandwidth)*freqchannels));
	assert(targetchannelinc == channelinc);

	// core.cpp:1725-1727: destination index into the core results array
	coreindex = config->getCoreResultBaselineOffset(configindex, freqindex, baseline);
	if(coreindex < 0)
		cerr << "Baseline " << baseline << " input freqId " << freqindex << " with destination freqId " << targetfreqindex << " is not mapped to any results[] array index!" << endl;

	// core.cpp:1770: threadresults offset for this freq/baseline
	threadstart = config->getThreadResultFreqOffset(configindex, freqindex) + config->getThreadResultBaselineOffset(configindex, freqindex, baseline);

	// single phase centre: no rotator, srcpointer comes straight from threadcrosscorrs
	for(int x=0;x<config->getNumXmacStrides(configindex, freqindex);x++)
	{
		threadindex = threadstart + x*config->getCompleteStrideLength(configindex, freqindex);
		xmacstrideremain = min(freqchannels-x*xmacstridelength, xmacstridelength);

		for(int k=0;k<config->getBNumPolProducts(configindex, baseline, localfreqindex);k++)
		{
			coreoffset = (k*targetfreqchannels + x*xmacstridelength)/targetchannelinc;

			const cf32 *srcpointer = &(threadcrosscorrs[threadindex]);

			// spectrally average (or not) and accumulate into the subint results
			coredest = coreindex+coreoffset;
			if(channelinc == 1) //this frequency is not averaged
			{
				vectorAdd_cf32_I(srcpointer, &(subintresults[coredest]), xmacstrideremain);
			}
			else //this frequency *is* averaged - core.cpp:1829-1853
			{
				outchannelplacementpreavg = fabs(config->getFreqTableFreqLowedge(freqindex) - config->getFreqTableFreqLowedge(targetfreqindex)) / channelbandwidth;
				int virtualplacement = outchannelplacementpreavg + x*xmacstridelength;
				const cf32* psrc = srcpointer;
				const cf32* pend = psrc + xmacstrideremain;
				while (psrc < pend)
				{
					int valuesinbin = virtualplacement % averagelength;
					if(valuesinbin == 0)
						valuesinbin = averagelength;
					if(valuesinbin > xmacstrideremain)
						valuesinbin = xmacstrideremain;
					cf32 meanresult;
					vectorSum_cf32(psrc, valuesinbin, &meanresult, vecAlgHintFast);
					subintresults[coredest].re += meanresult.re/(stridestoaverage*averagelength);
					subintresults[coredest].im += meanresult.im/(stridestoaverage*averagelength);
					psrc += valuesinbin;
					virtualplacement += valuesinbin;
					coredest++;
				}
			}
			//advance to next xmac channel group
			threadindex += xmacstridelength;
		}
	}

	// single phase centre: core.cpp:1878's stride across phase centre output
	// areas collapses to a no-op (one iteration), skip it
}

void XmacEngine::copyBaselineWeights(f32 *floatresults)
{
	// core.cpp:1070-1107, without locks and without the multi phase centre
	// shift-decorr section
	for(int f=0;f<freqtablelength;f++)
	{
		if(config->isFrequencyUsed(configindex, f))
		{
			for(int i=0;i<numbaselines;i++)
			{
				int localfreqindex = config->getBLocalFreqIndex(configindex, i, f);
				if(localfreqindex >= 0)
				{
					int resultindex = config->getCoreResultBWeightOffset(configindex, f, i)*2;
					for(int j=0;j<config->getBNumPolProducts(configindex, i, localfreqindex);j++)
					{
						floatresults[resultindex] += baselineweight[f][i][j];
						resultindex++;
					}
				}
			}
		}
	}
}
