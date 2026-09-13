#ifndef SWITCHEDPOWER_H
#define SWITCHEDPOWER_H

#include <string>
#include <iostream>
#include <fstream>
#include <mark5access.h>
#include "configuration.h"
#include "architecture.h"

class SwitchedPower
{
public:
	SwitchedPower(const Configuration * conf, int configindex, int dsindex);
	~SwitchedPower();
	void init();
	int open();
	int realloc(int n);
	int close();
	int flush();			// write to disk the existing data
	int feed(u8 *buffer, int nbytes);	// compute power for one block of raw bytes

	int datastreamId;
	std::ofstream output;		// ostream for text file being written

	int interval;			// in seconds, the accumulation period
	int frequency;			// the switched power cycle frequency	(e.g., 80 Hz for VLBA)

	unsigned int *stats;
	int nchan;
	unsigned int *counts;
	double *highOn, *highOff;	// per IF, the number of high states
	double *nOn, *nOff;		// per IF, the total number of states
	int startmjd;			// mjd of accumulation start
	int startsec;
	double startns;
	// note: the following three numbers may be improper in the sense that endns may be > 1e9 or endsec may be >= 86400
	int endmjd;
	int endsec;
	double endns;
	bool opened;
	bool failed;
	std::string filepath;
	int startMJD;
	int startSeconds;

private:
	int feedMark5Stream(mark5_stream *ms);	// take entire stream and compute power

	char formatname[64];
};

#endif
