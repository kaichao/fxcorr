// Unit tests for layer 1 of the reader (applications/fxcorr-f/src/frametimeline.h)
//
// These are the algorithms that decide where a file's frames land on the time
// axis.  They used to live inside DataReader, spread over three scan loops and
// tied to datastream members, so the only way to test them was to run a
// correlator over synthetic files and diff the products -- which is how the
// B3/B5/C6 class of state-machine bugs got through.  v4-plan.md phase C lifts
// them out; this file is the test surface that comes with it.
//
// Build and run (no dependencies -- that is the point of the header):
//     g++ -std=c++11 -Wall -I<repo>/applications/fxcorr-f/src test_timeline.cpp -o /tmp/test_timeline
//     /tmp/test_timeline
//
// Frame layout under test is vdifio's word layout (word 1 low 24 bits carry the
// frame number), and the two filler shapes are the ones 4.8 lists: an all-zero
// header, and the invalid bit (word 0 bit 31) set.
#include "frametimeline.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace frametimeline;

static int checks = 0;
static int failures = 0;

static void check(bool ok, const std::string &what)
{
	checks++;
	if(!ok)
	{
		failures++;
		printf("FAIL: %s\n", what.c_str());
	}
}

// A synthetic stretch of VDIF frames: headers we can set, and a payload byte
// carrying the frame number so a placement can be read back.
struct Stretch
{
	std::vector<unsigned char> bytes;
	int framebytes;
	int n;

	Stretch(int framebytes_, int n_) : framebytes(framebytes_), n(n_)
	{
		bytes.assign((size_t)framebytes*n, 0);
	}

	unsigned char *at(int i) { return &bytes[(size_t)i*framebytes]; }

	// data frame: seconds field non-zero so frame number 0 is not read as filler
	void data(int i, long long frameno)
	{
		unsigned char *p = at(i);
		memset(p, 0, framebytes);
		p[0] = 1;
		p[4] = (unsigned char)(frameno & 0xFF);
		p[5] = (unsigned char)((frameno >> 8) & 0xFF);
		p[6] = (unsigned char)((frameno >> 16) & 0xFF);
		p[8] = (unsigned char)(frameno & 0xFF);	// payload tag
	}

	void filler_zero(int i)		// all-zero header (t25362's BA thread 2)
	{
		memset(at(i), 0, framebytes);
	}

	void filler_invalid(int i)	// invalid bit set
	{
		memset(at(i), 0, framebytes);
		at(i)[3] = 0x80;	// word 0 bit 31
	}
};

static const int FB = 32;	// frame bytes
static const long long FPS = 1000;

static void test_walk_continuous()
{
	Stretch s(FB, 35);
	for(int i=0;i<35;i++)
		s.data(i, 100 + i);
	FrameRun r = walkFrameChain(s.bytes.data(), 35, FB, 4096, FPS, -1);
	check(r.first == 100 && r.last == 134, "continuous: first/last frame numbers");
	check(r.frames == 35, "continuous: data frame count");
	check(r.missing == 0 && r.gaps.empty(), "continuous: no gaps");
	check(r.fillers.empty(), "continuous: no filler");
}

static void test_walk_gap()
{
	// 100..109 then 113..120: three frames lost
	Stretch s(FB, 18);
	for(int i=0;i<10;i++)
		s.data(i, 100 + i);
	for(int i=10;i<18;i++)
		s.data(i, 113 + (i - 10));
	FrameRun r = walkFrameChain(s.bytes.data(), 18, FB, 1000, FPS, -1);
	check(r.gaps.size() == 1, "gap: one discontinuity");
	if(r.gaps.size() == 1)
	{
		const FrameGap &g = r.gaps[0];
		check(g.index == 10, "gap: sits after the frame at index 9");
		check(g.offset == 1000 + 10*FB, "gap: file offset is the frame after it");
		check(g.after == 109 && g.frame == 113, "gap: frame numbers around it");
		check(g.step == 4 && g.missing == 3, "gap: 3 frames missing");
	}
	check(r.missing == 3, "gap: missing total");
}

static void test_walk_wrap()
{
	// the counter restarts once per second: 999 -> 0 is a step of 1
	Stretch s(FB, 4);
	s.data(0, 998);
	s.data(1, 999);
	s.data(2, 0);
	s.data(3, 1);
	FrameRun r = walkFrameChain(s.bytes.data(), 4, FB, 0, FPS, -1);
	check(r.gaps.empty() && r.missing == 0, "wrap: second boundary is not a gap");
}

static void test_walk_repeat()
{
	// the same frame number twice is not a gap either (nothing was lost)
	Stretch s(FB, 3);
	s.data(0, 10);
	s.data(1, 10);
	s.data(2, 11);
	FrameRun r = walkFrameChain(s.bytes.data(), 3, FB, 0, FPS, -1);
	check(r.gaps.empty() && r.missing == 0, "repeat: step 0 loses nothing");
}

static void test_walk_filler()
{
	// filler sits between data frames without breaking the chain, both shapes
	Stretch s(FB, 8);
	s.data(0, 10);
	s.data(1, 11);
	s.filler_zero(2);
	s.filler_zero(3);
	s.filler_invalid(4);
	s.data(5, 12);
	s.data(6, 13);
	s.data(7, 14);
	FrameRun r = walkFrameChain(s.bytes.data(), 8, FB, 0, FPS, -1);
	check(r.fillers.size() == 3, "filler: both shapes counted");
	check(r.fillers.size() == 3 && r.fillers[0] == 2 && r.fillers[1] == 3 && r.fillers[2] == 4,
	      "filler: positions reported");
	check(r.gaps.empty() && r.missing == 0, "filler: chain runs unbroken across it");
	check(r.first == 10 && r.last == 14 && r.frames == 5, "filler: data frames only");
}

static void test_walk_chain_seed()
{
	// a gap straddling the seam between two stretches: the first frame of this
	// one is 105 while the chain stood at 100
	Stretch s(FB, 3);
	s.data(0, 105);
	s.data(1, 106);
	s.data(2, 107);
	FrameRun r = walkFrameChain(s.bytes.data(), 3, FB, 0, FPS, 100);
	check(r.gaps.size() == 1, "seed: seam gap found");
	if(r.gaps.size() == 1)
	{
		check(r.gaps[0].index == 0 && r.gaps[0].after == 100 && r.gaps[0].frame == 105,
		      "seed: gap at the very first frame");
		check(r.gaps[0].missing == 4, "seed: 4 frames missing across the seam");
	}
}

static void test_walk_all_filler()
{
	Stretch s(FB, 5);
	for(int i=0;i<5;i++)
		s.filler_zero(i);
	FrameRun r = walkFrameChain(s.bytes.data(), 5, FB, 0, FPS, 77);
	check(r.first == -1 && r.last == -1, "all-filler: no data frame to report");
	check(r.frames == 0 && r.fillers.size() == 5, "all-filler: counted as filler");
	check(r.gaps.empty(), "all-filler: no gap inside");
}

static void test_place_plain()
{
	const int slots = 8;
	Stretch s(FB, 12);
	for(int i=0;i<12;i++)
		s.data(i, i);		// frame number == slot
	unsigned char dst[FB*slots];
	memset(dst, 0xEE, sizeof(dst));
	SlotPlacement p = placeFrames(s.bytes.data(), 12, FB, FPS, 0, dst, slots);
	check(p.placed == slots, "plain: all slots filled");
	check(p.firstslot == 0, "plain: starts at slot 0");
	check(p.holes.empty(), "plain: no holes");
	check(p.usedframes == slots, "plain: stops once the slots fill up");
	bool same = true;
	for(int d=0; d<slots; d++)
		if(dst[d*FB + 8] != (unsigned char)d)
			same = false;
	check(same, "plain: slot d holds frame d");
}

static void test_place_offset()
{
	// the subint's start lies before the first frame the file holds: the first
	// data frame belongs at slot 5, and 0..4 are holes
	const int slots = 8;
	Stretch s(FB, 12);
	for(int i=0;i<12;i++)
		s.data(i, 5 + i);
	unsigned char dst[FB*slots];
	memset(dst, 0xEE, sizeof(dst));
	SlotPlacement p = placeFrames(s.bytes.data(), 12, FB, FPS, 0, dst, slots);
	check(p.firstslot == 5, "offset: first frame lands at slot 5");
	check(p.holes.size() == 1 && p.holes[0].first == 0 && p.holes[0].second == 5,
	      "offset: head reported as a hole");
	check(p.placed == slots, "offset: the buffer is still slots long");
	check(dst[5*FB + 8] == 5, "offset: frame 5 sits in slot 5");
}

static void test_place_gap()
{
	// frames 0..3 then 7..: three slots vacated in the middle, later frames move up
	const int slots = 10;
	Stretch s(FB, 8);
	for(int i=0;i<4;i++)
		s.data(i, i);
	for(int i=4;i<8;i++)
		s.data(i, i + 3);	// 7,8,9,10
	unsigned char dst[FB*slots];
	memset(dst, 0xEE, sizeof(dst));
	SlotPlacement p = placeFrames(s.bytes.data(), 8, FB, FPS, 0, dst, slots);
	check(p.firstslot == 0, "gap: starts at slot 0");
	check(p.holes.size() == 1, "gap: one hole, nothing was lost from the tail");
	if(p.holes.size() == 1)
		check(p.holes[0].first == 4 && p.holes[0].second == 7, "gap: [4,7) vacated");
	check(p.placed == slots, "gap: the slots behind the gap are covered");
	check(dst[7*FB + 8] == 7, "gap: frame 7 placed in slot 7");
}

static void test_place_filler()
{
	// two filler frames in the middle of nine frames: they take no slot, so the
	// frames behind them move up and the subint comes up one frame short
	const int slots = 8;
	Stretch s(FB, 9);
	s.data(0, 0);
	s.data(1, 1);
	s.filler_zero(2);
	s.filler_zero(3);
	for(int i=4;i<9;i++)
		s.data(i, i - 2);	// 2..6
	unsigned char dst[FB*slots];
	memset(dst, 0xEE, sizeof(dst));
	SlotPlacement p = placeFrames(s.bytes.data(), 9, FB, FPS, 0, dst, slots);
	check(p.placed == 7, "filler: seven data frames, seven slots");
	check(p.holes.size() == 1 && p.holes[0].first == 7 && p.holes[0].second == 8,
	      "filler: tail left without data");
	check(dst[2*FB + 8] == 2, "filler: frame 2 sits in slot 2");
}

static void test_place_whole_invalid()
{
	// the gap reaches past the whole buffer: none of it is this subint's data
	const int slots = 8;
	Stretch s(FB, 5);
	for(int i=0;i<5;i++)
		s.data(i, 100 + i);	// way past slot 8
	unsigned char dst[FB*slots];
	memset(dst, 0xEE, sizeof(dst));
	SlotPlacement p = placeFrames(s.bytes.data(), 5, FB, FPS, 0, dst, slots);
	check(p.placed == 0, "past-the-end: nothing placed");
	check(p.firstslot == 100, "past-the-end: first frame's own slot");
	check(p.holes.size() == 1 && p.holes[0].first == 0 && p.holes[0].second == slots,
	      "past-the-end: every slot is a hole");
	check(p.usedframes == 5, "past-the-end: the whole stretch walked");
	bool zeroed = true;
	for(int i=0;i<FB*slots;i++)
		if(dst[i] != 0)
			zeroed = false;
	check(zeroed, "past-the-end: destination zeroed");
}

static void test_place_tail()
{
	// fewer frames than slots: everything placed, the tail is a hole
	const int slots = 10;
	Stretch s(FB, 4);
	for(int i=0;i<4;i++)
		s.data(i, i);
	unsigned char dst[FB*slots];
	memset(dst, 0xEE, sizeof(dst));
	SlotPlacement p = placeFrames(s.bytes.data(), 4, FB, FPS, 0, dst, slots);
	check(p.placed == 4, "tail: four slots filled");
	check(p.holes.size() == 1 && p.holes[0].first == 4 && p.holes[0].second == slots,
	      "tail: [4,slots) reported");
	check(p.usedframes == 4, "tail: source exhausted");
}

// The dry run is how readSubint finds out how much input a subint needs, and it
// has to answer exactly what the real placement does -- that is the whole point
// of B2's shared walk (reader-model.md 4.8).
static void test_place_dryrun_matches()
{
	const int slots = 10;
	Stretch s(FB, 16);
	s.data(0, 0);
	s.data(1, 1);
	s.filler_zero(2);
	s.filler_invalid(3);
	for(int i=4;i<9;i++)
		s.data(i, i + 1);	// 5..9: one slot vacated at 2
	for(int i=9;i<16;i++)
		s.data(i, i + 6);	// 15..21: more slots vacated

	unsigned char dstA[FB*slots], dstB[FB*slots];
	memset(dstA, 0xEE, sizeof(dstA));
	memset(dstB, 0xEE, sizeof(dstB));
	SlotPlacement real = placeFrames(s.bytes.data(), 16, FB, FPS, 0, dstA, slots);
	SlotPlacement dry = placeFrames(s.bytes.data(), 16, FB, FPS, 0, NULL, slots);

	check(dry.placed == real.placed, "dry run: same number of slots");
	check(dry.usedframes == real.usedframes, "dry run: same frames consumed");
	check(dry.firstslot == real.firstslot, "dry run: same first slot");
	check(dry.holes.size() == real.holes.size(), "dry run: same holes");
	bool sameholes = dry.holes.size() == real.holes.size();
	for(size_t i=0; sameholes && i<dry.holes.size(); i++)
		if(dry.holes[i] != real.holes[i])
			sameholes = false;
	check(sameholes, "dry run: identical hole ranges");

	bool untouched = true;
	for(int i=0;i<FB*slots;i++)
		if(dstB[i] != 0xEE)
			untouched = false;
	check(untouched, "dry run: destination untouched");
}

int main()
{
	test_walk_continuous();
	test_walk_gap();
	test_walk_wrap();
	test_walk_repeat();
	test_walk_filler();
	test_walk_chain_seed();
	test_walk_all_filler();
	test_place_plain();
	test_place_offset();
	test_place_gap();
	test_place_filler();
	test_place_whole_invalid();
	test_place_tail();
	test_place_dryrun_matches();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
