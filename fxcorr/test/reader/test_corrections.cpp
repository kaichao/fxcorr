// Unit tests for layer 2 of the reader (applications/fxcorr-f/src/corrections.h)
//
// The ledger answers "how much is missing before time t" -- a question about the
// time axis, answered from gaps that were found in the file.  The two defects
// this layer exists for are both about that question being asked wrongly:
//
//   * B5 -- a gap noticed inside one subint's buffer but sitting after that
//     subint's end must not shorten its read (fxcorr/test/gaps/run_boundary.sh);
//   * B6 -- a gap that follows a filler run sits that many slots too late if the
//     filler ahead of it is not taken off (fxcorr/test/gaps/run_filler.sh).
//
// Both are cheap to state here and were expensive to find out there, which is
// the point of the layer (reader-model.md 7.2).
//
// Build and run (no dependencies):
//     g++ -std=c++11 -Wall -I<repo>/applications/fxcorr-f/src test_corrections.cpp -o /tmp/test_corrections
//     /tmp/test_corrections
#include "corrections.h"
#include "frametimeline.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace corrections;

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

static void checkeq(long long got, long long want, const std::string &what)
{
	checks++;
	if(got != want)
	{
		failures++;
		printf("FAIL: %s (got %lld, want %lld)\n", what.c_str(), got, want);
	}
}

static void test_no_gap()
{
	Ledger l;
	l.setOrigin(0, 100);
	check(l.empty(), "no gap: ledger starts empty");
	long long t = 42;
	checkeq(l.gapShift(t), 0, "no gap: nothing missing");
	checkeq(t, 42, "no gap: the query point does not move");
	checkeq(l.fillerFrames(), 0, "no gap: no filler either");
}

// B5: the gap sits at [100,103) on the time axis, whichever subint noticed it.
static void test_gap_before_after()
{
	Ledger l;
	l.setOrigin(0, 100);
	// the frame after the gap sits at file offset 10000, three frames lost,
	// no filler anywhere: slot 100
	check(l.noteGap(10000, 3, 0), "B5: gap recorded");

	long long t = 99;
	checkeq(l.gapShift(t), 0, "B5: a gap still ahead of the query does not apply");
	checkeq(t, 99, "B5: ... and does not move it");

	t = 100;
	checkeq(l.gapShift(t), 3, "B5: the gap applies from its own start");
	checkeq(t, 103, "B5: a query inside the hole lands after it");

	t = 102;
	checkeq(l.gapShift(t), 3, "B5: still inside the hole");
	checkeq(t, 103, "B5: ... still moved to its end");

	t = 103;
	checkeq(l.gapShift(t), 3, "B5: past the gap the frames are still missing");
	checkeq(t, 103, "B5: ... but nothing moves");
}

// B6: filler frames have bytes but no slot, so they are not part of the file
// offset the gap's slot is worked out from.
static void test_filler_before_gap()
{
	Ledger l;
	l.setOrigin(0, 100);
	// 105 frames into the file (offset 10500) with 5 filler frames ahead of
	// that point: on the time axis the gap starts at slot 100, not 105
	check(l.noteGap(10500, 3, 5), "B6: gap recorded");
	long long t = 100;
	checkeq(l.gapShift(t), 3, "B6: the gap starts 5 slots earlier");
	t = 99;
	checkeq(l.gapShift(t), 0, "B6: and not at 105");

	// the same gap worked out without the filler term would start at 105: a
	// query at 100..104 would then miss the gap entirely
	Ledger wrong;
	wrong.setOrigin(0, 100);
	wrong.noteGap(10500, 3, 0);
	long long u = 100;
	checkeq(wrong.gapShift(u), 0, "B6: without the filler term the gap is late (the defect)");
}

// A file that starts after the batch does (A class): the two origins differ by
// anchorbytes, and the synthetic tests with no start offset cannot see it.
static void test_start_offset()
{
	// the file's origin sits 50 frames before the batch's, so a file offset of
	// 5000 -- frame 50 of the file -- is time slot 100
	Ledger l;
	l.setOrigin(-5000, 100);
	check(l.noteGap(5000, 2, 0), "A: gap recorded");
	long long t = 100;
	checkeq(l.gapShift(t), 2, "A: the file's frame 50 is time slot 100");
	checkeq(t, 102, "A: query moved out of the hole");
	t = 99;
	checkeq(l.gapShift(t), 0, "A: nothing missing before it");

	Ledger late;
	late.setOrigin(3000, 100);	// origin 30 frames the other way
	late.noteGap(5000, 2, 0);	// frame 50 of the file is then slot 20
	long long u = 20;
	checkeq(late.gapShift(u), 2, "A: the other sign works out too");
	checkeq(u, 22, "A: query moved out of the hole");
}

static void test_two_gaps()
{
	Ledger l;
	l.setOrigin(0, 100);
	l.noteGap(10000, 3, 0);		// [100,103)
	l.noteGap(20000, 5, 0);		// 200 + 3 lost before = [203,208)

	long long t = 150;
	checkeq(l.gapShift(t), 3, "two gaps: only the first is ahead of 150");
	checkeq(t, 150, "two gaps: 150 is in data, nothing moves");

	t = 203;
	checkeq(l.gapShift(t), 8, "two gaps: both apply from 203");
	checkeq(t, 208, "two gaps: the second hole swallows the query");

	t = 100;
	checkeq(l.gapShift(t), 3, "two gaps: only the one the query is in applies");
	checkeq(t, 103, "two gaps: and the query lands at its end");

	checkeq(l.gapFrames(), 8, "two gaps: the total lost is kept");
}

static void test_dedup()
{
	Ledger l;
	l.setOrigin(0, 100);
	check(l.noteGap(10000, 3, 0), "dedup: first sighting counts");
	check(!l.noteGap(10000, 3, 0), "dedup: the same offset does not count twice");
	check(!l.noteGap(9000, 3, 0), "dedup: an earlier offset does not either");
	check(l.noteGap(11000, 3, 0), "dedup: a further one does");
	checkeq(l.gapFrames(), 6, "dedup: two gaps recorded, not four");
}

static void test_filler_count()
{
	Ledger l;
	l.setOrigin(0, 100);
	check(l.noteFiller(1000), "filler: first frame counted");
	checkeq(l.fillerFrames(), 1, "filler: tally");

	// the watermark is where a scan got to, not where the filler is: frames
	// behind it were counted by an earlier scan over the same bytes (the guard
	// overlap between consecutive subint reads)
	l.advanceFillerWatermark(2000);
	check(!l.noteFiller(1500), "filler: behind the watermark is not counted again");
	checkeq(l.fillerFrames(), 1, "filler: tally unchanged");
	check(l.noteFiller(2000), "filler: at the watermark a frame is new");
	checkeq(l.fillerFrames(), 2, "filler: tally grew");

	check(l.fillerWatermarkValid(), "filler: watermark set");
	checkeq(l.fillerWatermark(), 2000, "filler: watermark value");
	l.advanceFillerWatermark(1000);		// never goes back
	checkeq(l.fillerWatermark(), 2000, "filler: watermark is monotonic");
}

// The two layers together: a stretch of frames walked by layer 1, its gap handed
// to the ledger, and the correction asked for by time.
static void test_layers_together()
{
	const int FB = 32;
	const long long FPS = 1000;
	std::vector<unsigned char> bytes((size_t)FB*17, 0);
	for(int i=0;i<17;i++)
	{
		unsigned char *p = &bytes[(size_t)i*FB];
		p[0] = 1;			// seconds, so frame 0 is not read as filler
		long long fr = (i < 10) ? i : i + 3;	// 0..9 then 13..19
		p[4] = (unsigned char)(fr & 0xFF);
		p[5] = (unsigned char)((fr >> 8) & 0xFF);
	}

	frametimeline::FrameRun run = frametimeline::walkFrameChain(&bytes[0], 17, FB, 0, FPS, -1);
	check(run.gaps.size() == 1, "together: one gap found by layer 1");

	Ledger l;
	l.setOrigin(0, FB);
	for(size_t k=0;k<run.gaps.size();k++)
		l.noteGap(run.gaps[k].offset, run.gaps[k].missing, 0);

	long long t = 10;
	checkeq(l.gapShift(t), 3, "together: the gap applies from slot 10");
	checkeq(t, 13, "together: query moved past it");

	t = 9;
	checkeq(l.gapShift(t), 0, "together: not before it");
}

int main()
{
	test_no_gap();
	test_gap_before_after();
	test_filler_before_gap();
	test_start_offset();
	test_two_gaps();
	test_dedup();
	test_filler_count();
	test_layers_together();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
