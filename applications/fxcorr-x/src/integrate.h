#ifndef INTEGRATE_H
#define INTEGRATE_H

#include <string>
#include <vector>

#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/visibility.h>

class DifxMonitor;

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
 *
 * When a monitor is attached, each completed integration also emits a
 * DIFX_STATE_RUNNING DifxMessage (algo-plan.md P1, upstream
 * Visibility::multicastweights visibility.cpp:1100-1146).
 */
class Integrator {
public:
	/**
	 * @param configindex configuration index of the batch's scan
	 * @param difxdir  vis/<experiment>.difx/ from batch.json, metadata only;
	 *                 SWIN writes go to the .input OUTPUT FILENAME directory
	 *                 (created if missing)
	 * @param eseconds total correlation length of this batch, seconds
	 * @param scan     0 in V1 (single scan)
	 * @param startsec seconds of the batch start relative to the scan start
	 * @param startns  nanoseconds remainder of the batch start
	 * @param monitor  optional DifxMessage emitter for RUNNING status
	 */
	Integrator(Configuration *config, int configindex, const std::string &difxdir, int eseconds,
		int scan, int startsec, int startns, DifxMonitor *monitor = 0);
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
	// visibility.cpp multicastweights 1100-1146: per-station band-averaged
	// autocorr weights + integration-centre MJD, sent as RUNNING
	void sendRunning();

	Configuration *config;
	int configindex;
	Visibility *vis_;
	char *todiskbuffer_;
	DifxMonitor *monitor_;
};

#endif
