#ifndef INTEGRATE_H
#define INTEGRATE_H

#include <string>
#include <vector>

#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/visibility.h>

/**
 * @class Integrator
 * @brief Single-Visibility long-term integration for fxcorr-x: the serial
 *        replacement of FxManager's visbuffer ring + write thread
 *        (fxmanager.cpp:168-185, 650-698).
 *
 * Subint results are accumulated with Visibility::addData(); when an
 * integration period completes, writedata() emits the SWIN record and
 * increment() moves the Visibility to the next period.  Autocorrelations
 * come from the stations' autocorr.bin files (one record per subint).
 */
class Integrator {
public:
	/**
	 * @param configindex configuration index of the batch's scan
	 * @param difxdir  vis/<experiment>.difx/ (created if missing; writeSWIN appends there)
	 * @param eseconds total correlation length of this batch, seconds
	 * @param scan     0 in V1 (single scan)
	 * @param startsec seconds of the batch start relative to the scan start
	 * @param startns  nanoseconds remainder of the batch start
	 */
	Integrator(Configuration *config, int configindex, const std::string &difxdir, int eseconds,
		int scan, int startsec, int startns);
	~Integrator();

	Visibility *visibility() const { return vis_; }

	/**
	 * Accumulates one subint into the Visibility; when the integration period
	 * completes, writes the SWIN record and advances.
	 * @return true if an integration was completed (and written)
	 */
	bool addSubint(cf32 *subintresults);

	/**
	 * Reads one subint of autocorrelations from every station's autocorr.bin
	 * (acblocks averaging-batch records per subint, each averaged by
	 * fxcorr-f) and accumulates them into the autocorr / acweight sections
	 * of subintresults (core.cpp:1260-1370).
	 * @param autocorrFiles one autocorr.bin path per datastream (station)
	 */
	void addAutocorrs(int subint, const std::vector<std::string> &autocorrFiles, cf32 *subintresults);

private:
	Configuration *config;
	int configindex;
	Visibility *vis_;
	char *todiskbuffer_;
};

#endif
