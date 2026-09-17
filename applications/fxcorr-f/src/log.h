#ifndef FXCORR_LOG_H
#define FXCORR_LOG_H

#include <cstdlib>
#include <cstring>
#include <iostream>

// How much of a tool's stderr chatter to emit, selected by FXCORR_LOGLEVEL
// (error, warn, info, verbose, debug).
//
// The default, info, keeps errors, warnings and the one-line summaries that
// say how a run went; the per-event detail behind those summaries is
// interesting only when something looks wrong, so it sits at verbose.
//
// Used as
//	FXLOG(FXLOG_VERBOSE) << "detail " << x << endl;
// which checks the level before anything is formatted.

enum FxLogLevel
{
	FXLOG_ERROR = 0,
	FXLOG_WARN = 1,
	FXLOG_INFO = 2,
	FXLOG_VERBOSE = 3,
	FXLOG_DEBUG = 4
};

// Read once -- the level does not change while a tool runs.  An unrecognised
// value falls back to the default and says so rather than silently ignoring
// what is likely a typo.
inline int fxLogLevel()
{
	static int level = -1;
	if(level < 0)
	{
		level = FXLOG_INFO;
		if(const char *env = getenv("FXCORR_LOGLEVEL"))
		{
			if(strcmp(env, "error") == 0)
				level = FXLOG_ERROR;
			else if(strcmp(env, "warn") == 0)
				level = FXLOG_WARN;
			else if(strcmp(env, "info") == 0)
				level = FXLOG_INFO;
			else if(strcmp(env, "verbose") == 0)
				level = FXLOG_VERBOSE;
			else if(strcmp(env, "debug") == 0)
				level = FXLOG_DEBUG;
			else
				std::cerr << "FXCORR_LOGLEVEL: unknown level \"" << env
				          << "\" (error/warn/info/verbose/debug); using info"
				          << std::endl;
		}
	}
	return level;
}

#define FXLOG(lvl) if(fxLogLevel() >= (lvl)) std::cerr

#endif
