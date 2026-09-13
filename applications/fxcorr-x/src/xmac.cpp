#include "xmac.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace std;

XmacEngine::XmacEngine(Configuration *conf, int cindex, Model *m, int sc) :
	config(conf), model(m), scan(sc), configindex(cindex),
	numphasecentres(model->getNumPhaseCentres(scan)),
	freqtablelength(0), numbaselines(0), numdatastreams(0),
	numBufferedFFTs(0), blockspersend(0), xmacstridelength(0),
	threadresultlength(0), threadcrosscorrs(0), baselineweight(0), baselineshiftdecorr(0), conjbuf(0),
	maxchan(0), maxrotatestrideplussteplength(0),
	chanfreqs(0), rotator(0), rotated(0), argument(0), shifterrorcount(0)
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
	maxchan = config->getMaxNumChannels();
	conjbuf = vectorAlloc_cf32(maxchan);

	// multi phase centre scratchspace (core.cpp:416-433), sized over all
	// configs exactly like the upstream allocation
	{
		int slen = config->getRotateStrideLength(0);
		maxrotatestrideplussteplength = slen + maxchan/slen;
		for(int i=1;i<config->getNumConfigs();i++)
		{
			slen = config->getRotateStrideLength(i);
			int strideplussteplen = slen + maxchan/slen;
			if(strideplussteplen > maxrotatestrideplussteplength)
				maxrotatestrideplussteplength = strideplussteplen;
		}
		chanfreqs = vectorAlloc_f64(maxrotatestrideplussteplength);
		rotator = vectorAlloc_cf32(maxrotatestrideplussteplength);
		rotated = vectorAlloc_cf32(maxchan);
		argument = vectorAlloc_f32(3*maxrotatestrideplussteplength);
	}

	// baselineweight[f][j][p] (single pulsar bin), allocated per numpolproducts
	baselineweight = new f32**[freqtablelength];
	baselineshiftdecorr = new f32**[freqtablelength];
	for(int i=0;i<freqtablelength;i++)
	{
		baselineweight[i] = new f32*[numbaselines];
		if(config->isFrequencyUsed(configindex, i) && config->getMaxPhaseCentres(configindex) > 1)
		{
			// allocateConfigSpecificThreadArrays (core.cpp:2135-2148)
			baselineshiftdecorr[i] = new f32*[numbaselines];
			for(int j=0;j<numbaselines;j++)
			{
				int localfreqindex = config->getBLocalFreqIndex(configindex, j, i);
				if(localfreqindex >= 0)
					baselineshiftdecorr[i][j] = vectorAlloc_f32(config->getMaxPhaseCentres(configindex));
				else
					baselineshiftdecorr[i][j] = 0;
			}
		}
		else
			baselineshiftdecorr[i] = 0;
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
	vectorFree(chanfreqs);
	vectorFree(rotator);
	vectorFree(rotated);
	vectorFree(argument);
	for(int i=0;i<freqtablelength;i++)
	{
		for(int j=0;j<numbaselines;j++)
		{
			delete [] baselineweight[i][j];
			if(baselineshiftdecorr[i] && baselineshiftdecorr[i][j])
				vectorFree(baselineshiftdecorr[i][j]);
		}
		delete [] baselineweight[i];
		delete [] baselineshiftdecorr[i];
	}
	delete [] baselineweight;
	delete [] baselineshiftdecorr;
}

void XmacEngine::zeroSubint()
{
	// core.cpp:722-759 (single bin)
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
			if(numphasecentres > 1)
			{
				// core.cpp:745-757
				for(int j=0;j<numbaselines;j++)
				{
					int localfreqindex = config->getBLocalFreqIndex(configindex, j, i);
					if(localfreqindex >= 0)
						vectorZero_f32(baselineshiftdecorr[i][j], numphasecentres);
				}
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

void XmacEngine::uvshiftAndAverage(double offsetsec, double nsoffset, double nswidth, cf32 *subintresults)
{
	// core.cpp:1530-1563 with threadid==0 (startfreq/startbaseline both 0,
	// so every used (freq, baseline) pair is processed exactly once)
	for(int f=0;f<freqtablelength;f++)
	{
		if(config->isFrequencyUsed(configindex, f))
		{
			for(int i=0;i<numbaselines;i++)
				uvshiftAndAverageBaselineFreq(offsetsec, nsoffset, nswidth, f, i, subintresults);
		}
	}

	// clear the cross-corr results for the next averaging period (core.cpp:1560-1563)
	vectorZero_cf32(threadcrosscorrs, threadresultlength);
}

void XmacEngine::uvshiftAndAverageBaselineFreq(double offsetsec, double nsoffset, double nswidth, int freqindex, int baseline, cf32 *subintresults)
{
	int localfreqindex, targetfreqindex, freqchannels, targetfreqchannels;
	int channelinc, targetchannelinc, stridestoaverage, averagesperstride, averagelength, numstrides;
	int xmacstrideremain, threadindex, threadstart, coreindex, coreoffset, coredest;
	int rotatestridelen, rotatesperstride, rotatorlength, status;
	double bandwidth, bandwidthoftarget, channelbandwidth, outchannelplacementpreavg;
	double stepbandwidth, lofrequency, applieddelay, delaywindow, edgeturns, turns;
	double maxphasechange, timesmeardecorr, delaydecorr;
	double pointingcentredelay1approx[2], pointingcentredelay2approx[2];
	double **phasecentredelay1 = 0, **phasecentredelay2 = 0, **differentialdelay = 0;

	applieddelay = 0.0;
	delaywindow = config->getFNumChannels(freqindex)/(config->getFreqTableBandwidth(freqindex)); //max lag (plus and minus)
	localfreqindex = config->getBLocalFreqIndex(configindex, baseline, freqindex);
	rotatestridelen = config->getRotateStrideLength(configindex);

	if(localfreqindex < 0)
		return;

	// allocate space for the phase centre delays if necessary, and calculate pointing centre delays (core.cpp:1636-1672)
	if(numphasecentres > 1)
	{
		phasecentredelay1 = new double*[numphasecentres];
		phasecentredelay2 = new double*[numphasecentres];
		differentialdelay = new double*[numphasecentres];
		for(int i=0;i<numphasecentres;i++)
		{
			phasecentredelay1[i] = new double[2];
			phasecentredelay2[i] = new double[2];
			differentialdelay[i] = new double[2];
		}

		int antenna1index = config->getDModelFileIndex(configindex, config->getBDataStream1Index(configindex, baseline));
		int antenna2index = config->getDModelFileIndex(configindex, config->getBDataStream2Index(configindex, baseline));

		// get the pointing centre interpolator, validity range aribitrarily set to 1us (approximately the tangent)
		model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna1index, 0, 1, pointingcentredelay1approx);
		model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna2index, 0, 1, pointingcentredelay2approx);
		for(int s=0;s<numphasecentres;s++)
		{
			model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna1index, s+1, 1, phasecentredelay1[s]);
			model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna2index, s+1, 1, phasecentredelay2[s]);

			// work out the correct delay (and rate of delay) for this phase centre
			double applieddelay1 = phasecentredelay1[s][1] - pointingcentredelay1approx[1];
			double applieddelay2 = phasecentredelay2[s][1] - pointingcentredelay2approx[1];
			// make correction for geometric rate over the shifted sample range
			applieddelay1 += applieddelay1*pointingcentredelay1approx[0];
			applieddelay2 += applieddelay2*pointingcentredelay2approx[0];
			differentialdelay[s][1] = applieddelay2 - applieddelay1;
			differentialdelay[s][0] = phasecentredelay2[s][0] + pointingcentredelay1approx[0] - (phasecentredelay1[s][0] + pointingcentredelay2approx[0]);
		}
	}

	freqchannels = config->getFNumChannels(freqindex);
	channelinc = config->getFChannelsToAverage(freqindex);
	bandwidth = config->getFreqTableBandwidth(freqindex);
	targetfreqindex = config->getBTargetFreqIndex(configindex, baseline, localfreqindex);
	targetfreqchannels = config->getFNumChannels(targetfreqindex);
	targetchannelinc = config->getFChannelsToAverage(targetfreqindex);
	bandwidthoftarget = config->getFreqTableBandwidth(targetfreqindex);
	lofrequency = config->getFreqTableFreq(freqindex);
	stridestoaverage = channelinc/xmacstridelength;
	rotatesperstride = xmacstridelength/rotatestridelen;
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

	// fill chanfreqs for the rotator (core.cpp:1697-1717)
	rotatorlength = rotatestridelen+numstrides*rotatesperstride;
	stepbandwidth = rotatestridelen*channelbandwidth;
	if(numphasecentres > 1)
	{
		if(config->getFreqTableLowerSideband(freqindex))
		{
			for(int c=0;c<rotatestridelen;c++)
				chanfreqs[c] = -(rotatestridelen-(c+1))*channelbandwidth;
			for(int c=0;c<numstrides*rotatesperstride;c++)
				chanfreqs[rotatestridelen+c] = -(numstrides*rotatesperstride-(c+1))*stepbandwidth;
		}
		else
		{
			for(int c=0;c<rotatestridelen;c++)
				chanfreqs[c] = c*channelbandwidth;
			for(int c=0;c<numstrides*rotatesperstride;c++)
				chanfreqs[rotatestridelen+c] = c*stepbandwidth;
		}
	}

	// core.cpp:1725-1727: destination index into the core results array
	coreindex = config->getCoreResultBaselineOffset(configindex, freqindex, baseline);
	if(coreindex < 0)
		cerr << "Baseline " << baseline << " input freqId " << freqindex << " with destination freqId " << targetfreqindex << " is not mapped to any results[] array index!" << endl;

	// collect spectra data from threadcrosscorrs, do the multi-phasecenter rotation (if necessary),
	// spectral averaging (if necessary) and concatenation to the subint results (core.cpp:1729-1879)
	for(int s=0;s<numphasecentres;s++)
	{
		if(numphasecentres > 1)
		{
			// work out the correct rotator for this frequency and phase centre (core.cpp:1732-1768)
			applieddelay = differentialdelay[s][1];
			if(fabs(applieddelay) > 1.0e-20)
			{
				edgeturns = applieddelay*lofrequency;
				edgeturns -= floor(edgeturns);
				for(int r=0;r<rotatestridelen;r++)
				{
					turns = applieddelay*chanfreqs[r] + edgeturns;
					argument[r] = (turns-floor(turns))*TWO_PI;
				}
				for(int r=rotatestridelen;r<rotatorlength;r++)
				{
					turns = applieddelay*chanfreqs[r];
					argument[r] = (turns-floor(turns))*TWO_PI;
				}
				status = vectorSinCos_f32(argument, &(argument[rotatorlength]), &(argument[2*rotatorlength]), rotatorlength);
				if(status != vecNoErr)
					cerr << "Error in phase shift, sin/cos!!! " << status << endl;
				status = vectorRealToComplex_f32(&(argument[2*rotatorlength]), &(argument[rotatorlength]), rotator, rotatorlength);
				if(status != vecNoErr)
					cerr << "Error in phase shift, real to complex!!! " << status << endl;
			}
		}

		// core.cpp:1770: threadresults offset for this freq/baseline
		threadstart = config->getThreadResultFreqOffset(configindex, freqindex) + config->getThreadResultBaselineOffset(configindex, freqindex, baseline);

		for(int x=0;x<config->getNumXmacStrides(configindex, freqindex);x++)
		{
			threadindex = threadstart + x*config->getCompleteStrideLength(configindex, freqindex);
			xmacstrideremain = min(freqchannels-x*xmacstridelength, xmacstridelength);

			for(int k=0;k<config->getBNumPolProducts(configindex, baseline, localfreqindex);k++)
			{
				coreoffset = (k*targetfreqchannels + x*xmacstridelength)/targetchannelinc;

				const cf32 *srcpointer;
				if(numphasecentres > 1 && fabs(applieddelay) > 1.0e-20)
				{
					// rotate into the scratch buffer (core.cpp:1789-1808)
					srcpointer = &(threadcrosscorrs[threadindex]);
					for(int r=0;r<rotatesperstride;r++)
					{
						status = vectorMul_cf32(rotator, &(srcpointer[r*rotatestridelen]), &(rotated[r*rotatestridelen]), rotatestridelen);
						if(status != vecNoErr)
							cerr << "Error in phase shift, multiplication1!!! " << status << endl;
						status = vectorMulC_cf32_I(rotator[rotatestridelen+r+x*rotatesperstride], &(rotated[r*rotatestridelen]), rotatestridelen);
						if(status != vecNoErr)
							cerr << "Error in phase shift, multiplication2!!! " << status << endl;
					}
					srcpointer = rotated;
				}
				else
					srcpointer = &(threadcrosscorrs[threadindex]);

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

		// stride to next phase centre output area (core.cpp:1878); we stride by 'targetfreqchannels'>='freqchannels'
		// since current freq may be nested within wider freq
		coreindex += config->getBNumPolProducts(configindex, baseline, localfreqindex)*targetfreqchannels/targetchannelinc;
	}

	// calculate the decorrelation for each freq/baseline/source (core.cpp:1886-1923)
	if(numphasecentres > 1)
	{
		for(int s=0;s<numphasecentres;s++)
		{
			timesmeardecorr = 1.0;
			delaydecorr = 1.0;
			if(fabs(differentialdelay[s][0]) > 1e-18)
			{
				maxphasechange = TWO_PI*differentialdelay[s][0]*(nswidth/1000.0)*config->getFreqTableFreq(freqindex);
				timesmeardecorr = sin(maxphasechange/2.0) / (maxphasechange/2.0);
				if(timesmeardecorr < 0.0)
				{
					// use Brian Kernighan's bit counting trick to see if shifterrorcount is a power of two,
					// print only the first few and then increasingly less
					if(shifterrorcount < 10 || (shifterrorcount & (shifterrorcount-1)) == 0)
						cerr << "UV shift integration time far too long for baseline " << baseline << ", source " << s << "; no correlation! (errorcount now " << shifterrorcount << ")" << endl;
					timesmeardecorr = 0;
					shifterrorcount++;
				}
			}
			if(fabs(differentialdelay[s][1]) > 1e-18)
			{
				delaydecorr = 1.0 - fabs(differentialdelay[s][1] / delaywindow);
				if(delaydecorr < 0.0)
				{
					if(shifterrorcount < 10 || (shifterrorcount & (shifterrorcount-1)) == 0)
						cerr << "FFT window is not wide enough for baseline " << baseline << ", source " << s << "; no correlation! (errorcount now " << shifterrorcount << ")" << endl;
					delaydecorr = 0;
					shifterrorcount++;
				}
			}
			baselineshiftdecorr[freqindex][baseline][s] += nswidth*timesmeardecorr*delaydecorr;
		}
	}

	// free the phasecentredelay vectors if necessary (core.cpp:1925-1937)
	if(phasecentredelay1)
	{
		for(int i=0;i<numphasecentres;i++)
		{
			delete [] phasecentredelay1[i];
			delete [] phasecentredelay2[i];
			delete [] differentialdelay[i];
		}
		delete [] phasecentredelay1;
		delete [] phasecentredelay2;
		delete [] differentialdelay;
	}
}

void XmacEngine::copyBaselineWeights(f32 *floatresults)
{
	// core.cpp:1070-1107, without locks
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
			if(numphasecentres > 1)
			{
				// shift-decorr section (core.cpp:1090-1105), one f32 per phase centre
				for(int i=0;i<numbaselines;i++)
				{
					int localfreqindex = config->getBLocalFreqIndex(configindex, i, f);
					if(localfreqindex >= 0)
					{
						int resultindex = config->getCoreResultBShiftDecorrOffset(configindex, f, i)*2;
						for(int s=0;s<numphasecentres;s++)
						{
							floatresults[resultindex] += baselineshiftdecorr[f][i][s];
							resultindex++;
						}
					}
				}
			}
		}
	}
}
