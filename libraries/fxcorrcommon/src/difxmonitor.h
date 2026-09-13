/***************************************************************************
 *   difxmonitor.h - DifxMessage-format status emission for fxcorr tools   *
 *                                                                         *
 *   P1 of fxcorr/algo-plan.md: the fxcorr-f/x tools emit status/alert/    *
 *   diagnostic messages in the upstream DifxMessage XML format.  One      *
 *   XML-generation path is shared by both transport modes, so host        *
 *   multicast packets and container log files are byte-identical:         *
 *                                                                         *
 *     host mode (default):     multicast to DIFX_MESSAGE_GROUP/PORT       *
 *     container mode:          append to <containerPrefix>.xml            *
 *       (selected by FXCORR_RUN_MODE=container)                           *
 *                                                                         *
 *   With no transport configured (no DIFX_MESSAGE_PORT, and either no     *
 *   FXCORR_RUN_MODE=container or an empty containerPrefix) every send is  *
 *   a silent no-op, matching difxmessage's difxMessageSend2 semantics.    *
 ***************************************************************************/

#ifndef __DIFXMONITOR_H__
#define __DIFXMONITOR_H__

#include <string>

#include <difxmessage.h>	// DifxState, DifxMessageSTARecord, DIFX_ALERT_LEVEL_*

// Thin sender for the fxcorr batch tools.  The caller decides when to
// send (see algo-plan.md P1 for the schedule); this class only formats
// and transports.  In host mode the multicast group/port come from the
// DIFX_MESSAGE_GROUP / DIFX_MESSAGE_PORT environment variables (same as
// difxMessageInit); a missing port disables sending.
class DifxMonitor
{
public:
	// mpiId: 0 for the manager role (fxcorr-x), dsindex+1 for the
	// datastream/core role (fxcorr-f), matching mpifxcorr's process
	// numbering.  identifier: experiment name (.input basename, same
	// as mpifxcorr's generateIdentifier).  inputFilename: added as an
	// <input> tag inside Status/Alert bodies, like upstream's
	// difxMessageSetInputFilename.  containerPrefix: base name (no
	// extension) of the meta/difxmsg/ log file in container mode;
	// empty when not used.
	DifxMonitor(int mpiId, const std::string &identifier,
	            const std::string &inputFilename,
	            const std::string &containerPrefix);

	// Status message (DIFX_MESSAGE_STATUS).  When mjdStop > 0 the
	// <jobstartMJD>/<jobstopMJD> pair is included (upstream
	// difxMessageSendDifxStatus3, used for RUNNING); otherwise the
	// plain form (difxMessageSendDifxStatus, used for STARTING/DONE
	// etc).  weight may be null; entries < 0 are skipped, and at most
	// the first 20 datastreams are emitted (upstream behaviour).
	void status(enum DifxState state, const std::string &message,
	            double visMJD, int numdatastreams, const float *weight,
	            double mjdStart, double mjdStop);

	// Alert message (DIFX_MESSAGE_ALERT); severity is a
	// DIFX_ALERT_LEVEL_* value (the XML carries the integer, like
	// upstream).
	void alert(const std::string &message, int severity);

	// Diagnostic messages (DIFX_MESSAGE_DIAGNOSTIC), upstream
	// datastream.cpp:631-634 counterparts.
	void diagnosticDataConsumed(long long bytes);
	void diagnosticInputDatarate(double bytespersec);

	// BINARY_STA record to DIFX_BINARY_GROUP/PORT in host mode, or
	// appended raw to <containerPrefix>.sta in container mode.
	void staSend(const DifxMessageSTARecord *record, int nbytes);

private:
	std::string xmlMessage(const std::string &type, const std::string &body);
	void send(const std::string &message);

	int mpiId_;
	std::string identifier_;
	std::string inputTag_;		// "<input>...</input>" or empty
	std::string hostname_;
	std::string containerPrefix_;	// non-empty in container mode

	std::string hostGroup_;
	int hostPort_;
	std::string staGroup_;
	int staPort_;

	int seqNumber_;
};

#endif
