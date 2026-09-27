#include "fxcorrpath.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <limits.h>

using namespace std;

string FxcorrPath::workdir_;
string FxcorrPath::roots_[ROOT_COUNT];
bool FxcorrPath::resolved_ = false;

// Absolutise against the process cwd.  The path need not exist yet - roots are
// created by the orchestration layer, not by the tools (v5-plan.md Q19) - so
// realpath() is not usable here.
static string absolutise(const string &path)
{
	if(!path.empty() && path[0] == '/')
		return path;

	char buf[PATH_MAX];
	if(getcwd(buf, sizeof(buf)) == NULL)
		return path;

	string full(buf);
	if(full.empty() || full[full.size() - 1] != '/')
		full += '/';
	full += path;

	// drop a trailing "/." so that "." and "./" resolve to the same string as
	// the cwd itself - roots end up in meta/roots/<batch>.json and get compared
	// verbatim, so the spelling has to be stable
	if(full.size() >= 2 && full.compare(full.size() - 2, 2, "/.") == 0)
		full.erase(full.size() - 2);
	return full;
}

const char *FxcorrPath::envname(Root r)
{
	switch(r)
	{
	case ROOT_RAW:		return "FXCORR_RAW_ROOT";
	case ROOT_FENGINE:	return "FXCORR_FENGINE_ROOT";
	case ROOT_VIS:		return "FXCORR_VIS_ROOT";
	case ROOT_PRODUCT:	return "FXCORR_PRODUCT_ROOT";
	default:		return "";
	}
}

const char *FxcorrPath::canonname(Root r)
{
	switch(r)
	{
	case ROOT_RAW:		return "raw";
	case ROOT_FENGINE:	return "fengine";
	case ROOT_VIS:		return "vis";
	case ROOT_PRODUCT:	return "product";
	default:		return "";
	}
}

void FxcorrPath::init(const string &wd)
{
	workdir_ = absolutise(wd.empty() ? string(".") : wd);
	resolved_ = false;
}

const string &FxcorrPath::workdir()
{
	return workdir_;
}

const string &FxcorrPath::root(Root r)
{
	if(!resolved_)
	{
		for(int i = 0; i < ROOT_COUNT; i++)
		{
			const char *env = getenv(envname((Root)i));
			if(env != NULL && env[0] != '\0')
				roots_[i] = absolutise(env);
			else
				roots_[i] = workdir_ + "/" + canonname((Root)i);
		}
		resolved_ = true;
	}
	return roots_[r];
}

string FxcorrPath::under(const string &base, const string &path)
{
	if(path.empty() || path[0] == '/')
		return path;
	if(!base.empty() && base[base.size() - 1] == '/')
		return base + path;
	return base + "/" + path;
}

string FxcorrPath::dirname(const string &path)
{
	size_t pos = path.find_last_of('/');
	if(pos == string::npos)
		return ".";
	if(pos == 0)
		return "/";
	return path.substr(0, pos);
}

const char *FxcorrPath::jsonname(Root r)
{
	// keys written by fxcorr/roots.sh fxcorr_write_roots()
	switch(r)
	{
	case ROOT_RAW:		return "raw";
	case ROOT_FENGINE:	return "fengine";
	case ROOT_VIS:		return "vis";
	case ROOT_PRODUCT:	return "product";
	default:		return "";
	}
}

// Minimal "key": "value" extractor - the roots file is written by our own
// script and holds nothing else, so a JSON library would be overkill.
static string jsonField(const string &json, const string &key)
{
	string pat = "\"" + key + "\"";
	size_t p = json.find(pat);
	if(p == string::npos)
		return "";
	p = json.find(':', p + pat.size());
	if(p == string::npos)
		return "";
	p = json.find('"', p);
	if(p == string::npos)
		return "";
	size_t e = json.find('"', p + 1);
	if(e == string::npos)
		return "";
	return json.substr(p + 1, e - p - 1);
}

bool FxcorrPath::checkRoots(const string &batchid, const Root *used, int nused, const char *progname)
{
	string path = workdir_ + "/meta/roots/" + batchid + ".json";
	ifstream in(path.c_str());
	if(!in.is_open())
		return true;	// no record: single-tool run or test, nothing to honour

	stringstream ss;
	ss << in.rdbuf();
	string json = ss.str();

	for(int i = 0; i < nused; i++)
	{
		string want = jsonField(json, jsonname(used[i]));
		if(want.empty())
			continue;
		if(want != root(used[i]))
		{
			cerr << progname << ": " << envname(used[i]) << " disagrees with " << path << endl
			     << "  recorded " << want << endl
			     << "  resolved " << root(used[i]) << endl
			     << "  (the batch was laid out with different roots; rerun with those,"
			     << " or remove the record to start a new layout)" << endl;
			return false;
		}
	}
	return true;
}

void FxcorrPath::print(const char *progname)
{
	const char *p = getenv("FXCORR_PRINT_ROOTS");
	if(p == NULL || strcmp(p, "1") != 0)
		return;

	cerr << progname << ": FXCORR_WORKDIR resolved to " << workdir_ << endl;
	for(int i = 0; i < ROOT_COUNT; i++)
	{
		const char *env = getenv(envname((Root)i));
		bool fromenv = (env != NULL && env[0] != '\0');
		cerr << progname << ": " << envname((Root)i) << " = " << root((Root)i)
		     << (fromenv ? "  [env]" : "  [workdir]") << endl;
	}
}
