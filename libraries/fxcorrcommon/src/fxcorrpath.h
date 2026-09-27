#ifndef FXCORRPATH_H
#define FXCORRPATH_H

#include <string>

/* fxcorr directory-root resolution (V5 P5).
 *
 * Paths reach fxcorr through four different mechanisms today: the apps paste
 * workdir + "/<name>", DiFX's Configuration opens whatever the .input names
 * relative to the process cwd, the .input carries whatever vex2difx wrote
 * (often absolute), and the shell scripts keep their own WORKDIR.  This class
 * is the one place that turns "which root" into a path, shared by the library
 * (Configuration/Model) and by the apps.
 *
 * Three-step fallback per root (v5-plan.md Q9):
 *   1. FXCORR_<X>_ROOT set  -> its value, absolutised against cwd
 *   2. otherwise            -> <workdir>/<dirname>
 *      (workdir is already resolved as positional arg > FXCORR_WORKDIR > "."
 *      by the calling app and handed to init())
 * and one rule that applies to every join (v5-plan.md rule 2): only relative
 * paths are prefixed - an absolute path is always used as-is, because the
 * .input of a real observation carries absolute paths and those must not be
 * rewritten.
 *
 * FXCORR_PRINT_ROOTS=1 makes print() report every resolved root and where it
 * came from - the cheapest way to answer "did my root take effect?".
 */

class FxcorrPath
{
public:
	enum Root
	{
		ROOT_RAW = 0,		// FXCORR_RAW_ROOT: DATA TABLE's FILE lines
		ROOT_FENGINE,		// FXCORR_FENGINE_ROOT: f output / x input
		ROOT_VIS,		// FXCORR_VIS_ROOT: SWIN (takes over OUTPUT FILENAME)
		ROOT_PRODUCT,		// FXCORR_PRODUCT_ROOT: final products
		ROOT_COUNT
	};

	// Called once from a tool's main(): records workdir (absolutised against
	// cwd) and drops any cached values.
	static void init(const std::string &workdir);

	// The workdir as init() recorded it, absolutised - tools use this so every
	// path they build themselves is absolute too.
	static const std::string &workdir();

	// Three-step fallback result for one root, evaluated on first use.
	static const std::string &root(Root r);

	// Prefix `path` with `base` unless it is absolute.
	static std::string under(const std::string &base, const std::string &path);

	// Directory part of a path ("/a/b/c.input" -> "/a/b"; no '/' -> ".").
	static std::string dirname(const std::string &path);

	// FXCORR_PRINT_ROOTS=1: print every root and its source, else no-op.
	static void print(const char *progname);

	// Compare the roots this tool uses against what the orchestration layer
	// recorded in meta/roots/<batchid>.json (v5-plan.md Q4/Q14).  Pass only
	// the roots the caller actually reads or writes - comparing all of them
	// would fail a tool for a change in a root it never touches.  Returns
	// false (and prints both values) on disagreement; a missing record is not
	// an error: single-tool runs and tests have none.
	static bool checkRoots(const std::string &batchid, const Root *used,
	                       int nused, const char *progname);

private:
	static const char *envname(Root r);
	static const char *canonname(Root r);
	static const char *jsonname(Root r);

	static std::string workdir_;
	static std::string roots_[ROOT_COUNT];
	static bool resolved_;
};

#endif
