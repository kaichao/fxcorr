/***************************************************************************
 *   difxmonitor.cpp - DifxMessage-format status emission (fxcorr P1)      *
 *                                                                         *
 *   XML formats are copied byte-for-byte from the difxmessage library     *
 *   (difxmessageinit.c XML template, difxsend.c bodies) so that fxcorr    *
 *   messages are indistinguishable from mpifxcorr ones on the wire.       *
 ***************************************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/time.h>

#include "difxmonitor.h"

#define MAX_GROUP_SIZE 16

// internal to the difxmessage library (difxmessageinternal.h, not
// installed); same value here
#define DIFX_MESSAGE_FILENAME_TAG_LENGTH (DIFX_MESSAGE_FILENAME_LENGTH + 32)

// same send gap as difxsend.c: two multicasts closer than this get spaced
// out to avoid lost messages
static const int MIN_SEND_GAP = 20;

// replaces illegal XML string characters, byte-for-byte the same as
// difxsend.c expandEntityReferences
static int expandEntityReferences(char *dest, const char *src, int maxLength)
{
	int i, j;

	for(i = j = 0; src[i]; ++i)
	{
		if(j >= maxLength-7)
			return -1;

		if(src[i] == '>')
		{
			strcpy(dest+j, "&gt;");
			j += 4;
		}
		else if(src[i] == '<')
		{
			strcpy(dest+j, "&lt;");
			j += 4;
		}
		else if(src[i] == '&')
		{
			strcpy(dest+j, "&amp;");
			j += 5;
		}
		else if(src[i] == '"')
		{
			strcpy(dest+j, "&quot;");
			j += 6;
		}
		else if(src[i] == '\'')
		{
			strcpy(dest+j, "&apos;");
			j += 6;
		}
		else if(src[i] < 32)	/* ascii chars < 32 are not allowed */
		{
			sprintf(dest+j, "[[%3d]]", src[i]);
			j += 7;
		}
		else
		{
			dest[j] = src[i];
			++j;
		}
	}

	dest[j] = 0;

	return j - i;
}

DifxMonitor::DifxMonitor(int mpiId, const std::string &identifier,
                         const std::string &inputFilename,
                         const std::string &containerPrefix)
	: mpiId_(mpiId), identifier_(identifier), containerPrefix_(containerPrefix),
	  hostPort_(-1), staPort_(-1), seqNumber_(0)
{
	// published hostname, as difxMessageInitFull
	char hostname[DIFX_MESSAGE_PARAM_LENGTH];
	gethostname(hostname, DIFX_MESSAGE_PARAM_LENGTH);
	hostname[DIFX_MESSAGE_PARAM_LENGTH-1] = 0;
	hostname_ = hostname;

	// <input> tag inside Status/Alert bodies, same as
	// difxMessageSetInputFilename
	if(inputFilename.size() > 0)
	{
		char tag[DIFX_MESSAGE_FILENAME_TAG_LENGTH];
		snprintf(tag, sizeof(tag), "<input>%s</input>", inputFilename.c_str());
		inputTag_ = tag;
	}

	// host-mode multicast group/port, same env vars as difxMessageInit;
	// a missing group or port leaves hostPort_ = -1 (silent, upstream
	// semantics)
	const char *envstr = getenv("DIFX_MESSAGE_GROUP");
	if(envstr != 0)
	{
		if((int)strlen(envstr) < MAX_GROUP_SIZE)
			hostGroup_ = envstr;
	}
	envstr = getenv("DIFX_MESSAGE_PORT");
	if(envstr != 0 && hostGroup_.size() > 0)
		hostPort_ = atoi(envstr);

	// binary STA channel, same env vars as difxMessageInitBinary
	envstr = getenv("DIFX_BINARY_GROUP");
	if(envstr != 0)
	{
		if((int)strlen(envstr) < MAX_GROUP_SIZE)
			staGroup_ = envstr;
	}
	envstr = getenv("DIFX_BINARY_PORT");
	if(envstr != 0 && staGroup_.size() > 0)
		staPort_ = atoi(envstr);

	// container mode: truncate this process's log files now (idempotent
	// re-run overwrites the previous run of the same process), then all
	// sends append
	if(containerPrefix_.size() > 0)
	{
		FILE *file = fopen((containerPrefix_ + ".xml").c_str(), "w");
		if(file != 0)
			fclose(file);
		file = fopen((containerPrefix_ + ".sta").c_str(), "w");
		if(file != 0)
			fclose(file);
	}
}

std::string DifxMonitor::xmlMessage(const std::string &type, const std::string &body)
{
	// difxmessageinit.c difxMessageXMLFormat, byte-for-byte
	char message[DIFX_MESSAGE_LENGTH];
	int size = snprintf(message, DIFX_MESSAGE_LENGTH,

		"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		"<difxMessage>"
		  "<header>"
		    "<from>%s</from>"
		    "<mpiProcessId>%d</mpiProcessId>"
		    "<identifier>%s</identifier>"
		    "<type>%s</type>"
		  "</header>"
		  "<body>"
		    "<seqNumber>%d</seqNumber>"
		    "%s"
		  "</body>"
		"</difxMessage>",

		hostname_.c_str(),
		mpiId_,
		identifier_.c_str(),
		type.c_str(),
		seqNumber_++,
		body.c_str());

	if(size >= DIFX_MESSAGE_LENGTH)
	{
		fprintf(stderr, "DifxMonitor: message overflow (%d >= %d)\n", size, DIFX_MESSAGE_LENGTH);
		return std::string();
	}

	return std::string(message);
}

void DifxMonitor::send(const std::string &message)
{
	if(message.size() == 0)
		return;

	if(containerPrefix_.size() > 0)
	{
		// container mode: append to the per-process log file; each
		// entry is one complete, self-contained XML document (same
		// bytes as the multicast packet)
		FILE *file = fopen((containerPrefix_ + ".xml").c_str(), "a");
		if(file != 0)
		{
			fputs(message.c_str(), file);
			fclose(file);
		}
		else
			fprintf(stderr, "DifxMonitor: cannot open %s.xml for appending\n", containerPrefix_.c_str());
	}
	else if(hostPort_ >= 0)
	{
		// host mode: multicast, with the same minimum send gap as
		// difxMessageSend2
		static int first = 1;
		static struct timeval tv0;
		struct timeval tv;

		if(first)
		{
			first = 0;
			gettimeofday(&tv0, 0);
		}
		else
		{
			int dt;

			gettimeofday(&tv, 0);
			dt = 1000000*(tv.tv_sec - tv0.tv_sec) + (tv.tv_usec - tv0.tv_usec);
			if(dt < MIN_SEND_GAP && dt > 0)
			{
				struct timespec ts;

				ts.tv_sec = 0;
				ts.tv_nsec = 1000*(MIN_SEND_GAP-dt);

				nanosleep(&ts, 0);
			}
		}

		MulticastSend(hostGroup_.c_str(), hostPort_, message.c_str(), (int)message.size());
	}
	// else: no transport configured, silent no-op
}

void DifxMonitor::status(enum DifxState state, const std::string &message,
                         double visMJD, int numdatastreams, const float *weight,
                         double mjdStart, double mjdStop)
{
	char messageExpanded[DIFX_MESSAGE_LENGTH];
	char body[DIFX_MESSAGE_LENGTH];
	char weightstr[DIFX_MESSAGE_LENGTH];
	int i, n;
	int size;

	size = expandEntityReferences(messageExpanded, message.c_str(), DIFX_MESSAGE_LENGTH);
	if(size < 0)
	{
		fprintf(stderr, "DifxMonitor: status message body overflow in entity replacement (>= %d)\n", DIFX_MESSAGE_LENGTH);
		return;
	}

	// difxsend.c difxMessageSendDifxStatus(3): weight entries < 0 are
	// skipped, at most 20 datastreams are emitted
	weightstr[0] = 0;
	n = 0;

	if(numdatastreams > 20)
		numdatastreams = 20;

	for(i = 0; i < numdatastreams && weight != 0; ++i)
	{
		if(weight[i] >= 0)
		{
			n += snprintf(weightstr+n, DIFX_MESSAGE_LENGTH-n,
				"<weight ant=\"%d\" wt=\"%5.3f\"/>",
				i, weight[i]);
			if(n >= DIFX_MESSAGE_LENGTH)
			{
				fprintf(stderr, "DifxMonitor: status message weightstr overflow (%d >= %d)\n", n, DIFX_MESSAGE_LENGTH);
				return;
			}
		}
	}

	if(mjdStop > 0.0)
	{
		size = snprintf(body, DIFX_MESSAGE_LENGTH,

			"<difxStatus>"
			  "%s"
			  "<state>%s</state>"
			  "<message>%s</message>"
			  "<visibilityMJD>%9.7f</visibilityMJD>"
			  "<jobstartMJD>%9.7f</jobstartMJD>"
			  "<jobstopMJD>%9.7f</jobstopMJD>"
			  "%s"
			"</difxStatus>",

			inputTag_.c_str(),
			DifxStateStrings[state],
			messageExpanded,
			visMJD, mjdStart, mjdStop,
			weightstr);
	}
	else
	{
		size = snprintf(body, DIFX_MESSAGE_LENGTH,

			"<difxStatus>"
			  "%s"
			  "<state>%s</state>"
			  "<message>%s</message>"
			  "<visibilityMJD>%9.7f</visibilityMJD>"
			  "%s"
			"</difxStatus>",

			inputTag_.c_str(),
			DifxStateStrings[state],
			messageExpanded,
			visMJD,
			weightstr);
	}

	if(size >= DIFX_MESSAGE_LENGTH)
	{
		fprintf(stderr, "DifxMonitor: status message body overflow (%d >= %d)\n", size, DIFX_MESSAGE_LENGTH);
		return;
	}

	send(xmlMessage(DifxMessageTypeStrings[DIFX_MESSAGE_STATUS], body));
}

void DifxMonitor::alert(const std::string &message, int severity)
{
	char messageExpanded[DIFX_MESSAGE_LENGTH];
	char body[DIFX_MESSAGE_LENGTH];
	int size;

	size = expandEntityReferences(messageExpanded, message.c_str(), DIFX_MESSAGE_LENGTH);
	if(size < 0)
	{
		fprintf(stderr, "DifxMonitor: alert message body overflow in entity replacement (>= %d)\n", DIFX_MESSAGE_LENGTH);
		return;
	}

	// difxsend.c difxMessageSendDifxAlert
	size = snprintf(body, DIFX_MESSAGE_LENGTH,

		"<difxAlert>"
		  "%s"
		  "<alertMessage>%s</alertMessage>"
		  "<severity>%d</severity>"
		"</difxAlert>",

		inputTag_.c_str(),
		messageExpanded,
		severity);

	if(size >= DIFX_MESSAGE_LENGTH)
	{
		fprintf(stderr, "DifxMonitor: alert message body overflow (%d >= %d)\n", size, DIFX_MESSAGE_LENGTH);
		return;
	}

	send(xmlMessage(DifxMessageTypeStrings[DIFX_MESSAGE_ALERT], body));
}

void DifxMonitor::diagnosticDataConsumed(long long bytes)
{
	char body[DIFX_MESSAGE_LENGTH];
	int size;

	// difxsend.c difxMessageSendDifxDiagnosticDataConsumed
	size = snprintf(body, DIFX_MESSAGE_LENGTH,

		"<difxDiagnostic>"
		  "<diagnosticType>%s</diagnosticType>"
		  "<bytes>%lld</bytes>"
		"</difxDiagnostic>",
		DifxDiagnosticStrings[DIFX_DIAGNOSTIC_DATACONSUMED],
		bytes);

	if(size >= DIFX_MESSAGE_LENGTH)
	{
		fprintf(stderr, "DifxMonitor: diagnostic message body overflow (%d >= %d)\n", size, DIFX_MESSAGE_LENGTH);
		return;
	}

	send(xmlMessage(DifxMessageTypeStrings[DIFX_MESSAGE_DIAGNOSTIC], body));
}

void DifxMonitor::diagnosticInputDatarate(double bytespersec)
{
	char body[DIFX_MESSAGE_LENGTH];
	int size;

	// difxsend.c difxMessageSendDifxDiagnosticInputDatarate
	size = snprintf(body, DIFX_MESSAGE_LENGTH,

		"<difxDiagnostic>"
		  "<diagnosticType>%s</diagnosticType>"
		  "<bytespersec>%.3f</bytespersec>"
		"</difxDiagnostic>",
		DifxDiagnosticStrings[DIFX_DIAGNOSTIC_INPUTDATARATE],
		bytespersec);

	if(size >= DIFX_MESSAGE_LENGTH)
	{
		fprintf(stderr, "DifxMonitor: diagnostic message body overflow (%d >= %d)\n", size, DIFX_MESSAGE_LENGTH);
		return;
	}

	send(xmlMessage(DifxMessageTypeStrings[DIFX_MESSAGE_DIAGNOSTIC], body));
}

void DifxMonitor::staSend(const DifxMessageSTARecord *record, int nbytes)
{
	if(record == 0 || nbytes <= 0)
		return;

	if(containerPrefix_.size() > 0)
	{
		// container mode: raw DifxMessageSTARecord appended to the
		// per-process STA file
		FILE *file = fopen((containerPrefix_ + ".sta").c_str(), "a");
		if(file != 0)
		{
			fwrite(record, 1, nbytes, file);
			fclose(file);
		}
		else
			fprintf(stderr, "DifxMonitor: cannot open %s.sta for appending\n", containerPrefix_.c_str());
	}
	else if(staPort_ >= 0)
	{
		// same silent-failure semantics as difxsta.c difxMessageSendBinary
		MulticastSend(staGroup_.c_str(), staPort_, (const char *)record, nbytes);
	}
}
