/***************************************************************************
 *   sta_ctrl.c - P9 STA test utility                                       *
 *                                                                         *
 *   Two modes, both thin wrappers around the installed libdifxmessage:    *
 *                                                                         *
 *     sta_ctrl send <name> <value> [mpiDest]                              *
 *       Sends a difxmessage parameter to a running mpifxcorr (default     *
 *       destination DIFX_MESSAGE_ALLMPIFXCORR).  This is how upstream     *
 *       enables the STA dumps at runtime: dumpsta=true, dumpkurtosis=     *
 *       true, stachannels=N (mpifxcorr.cpp:88-97).                        *
 *                                                                         *
 *     sta_ctrl recv <outfile> [maxmsgs]                                   *
 *       Joins the BINARY_STA multicast group (DIFX_BINARY_GROUP/PORT)     *
 *       and appends every received record verbatim to <outfile>, i.e.     *
 *       the same record-stream layout as fxcorr-f's container-mode       *
 *       meta/difxmsg/<...>.sta files, so cmp_sta.py parses both sides     *
 *       identically.  The capture ends after a 2 s quiet window or after  *
 *       maxmsgs records.                                                  *
 *                                                                         *
 *   Build (test machine): cc -O2 -o sta_ctrl sta_ctrl.c -ldifxmessage     *
 *   DIFX_BINARY_GROUP/PORT come from setup.bash (224.2.2.1:50202).        *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include <difxmessage.h>

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s send <name> <value> [mpiDest] | %s recv <outfile> [maxmsgs]\n", prog, prog);
}

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		usage(argv[0]);
		return 2;
	}

	if(strcmp(argv[1], "send") == 0)
	{
		if(argc < 4)
		{
			usage(argv[0]);
			return 2;
		}
		int dest = DIFX_MESSAGE_ALLMPIFXCORR;
		if(argc >= 5)
			dest = atoi(argv[4]);

		difxMessageInit(-1, 0);
		if(difxMessageSendDifxParameter(argv[2], argv[3], dest) < 0)
		{
			fprintf(stderr, "sta_ctrl: send of %s=%s failed\n", argv[2], argv[3]);
			return 1;
		}
		printf("sent %s=%s to mpiDestination %d\n", argv[2], argv[3], dest);
		return 0;
	}

	if(strcmp(argv[1], "recv") == 0)
	{
		if(argc < 3)
		{
			usage(argv[0]);
			return 2;
		}
		int maxmsgs = 0;
		if(argc >= 4)
			maxmsgs = atoi(argv[3]);

		difxMessageInitBinary();
		int sock = difxMessageBinaryOpen(BINARY_STA);
		if(sock < 0)
		{
			fprintf(stderr, "sta_ctrl: cannot open BINARY_STA socket (DIFX_BINARY_GROUP/PORT set?)\n");
			return 1;
		}

		FILE *out = fopen(argv[2], "wb");
		if(out == 0)
		{
			fprintf(stderr, "sta_ctrl: cannot open %s\n", argv[2]);
			return 1;
		}

		char buf[1 << 16];
		char from[64];
		int nmsgs = 0;
		while(maxmsgs == 0 || nmsgs < maxmsgs)
		{
			/* 2 s quiet window ends the capture */
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(sock, &fds);
			struct timeval tv = { 2, 0 };
			int ready = select(sock + 1, &fds, 0, 0, &tv);
			if(ready <= 0)
				break;

			int n = difxMessageBinaryRecv(sock, buf, sizeof(buf), from);
			if(n <= 0)
				continue;
			fwrite(buf, 1, n, out);
			nmsgs++;
		}

		fclose(out);
		difxMessageBinaryClose(sock);
		printf("captured %d records into %s\n", nmsgs, argv[2]);
		return 0;
	}

	usage(argv[0]);
	return 2;
}
