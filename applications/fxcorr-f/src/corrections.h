#ifndef CORRECTIONS_H
#define CORRECTIONS_H

#include <cstddef>
#include <vector>

/**
 * @file corrections.h
 * @brief Layer 2 of the reader rework (v4-plan.md phase C): the read-position
 *        corrections, as functions of time.
 *
 * A file hands over bytes in its own order, which is not the time axis the
 * subints are cut on.  Two kinds of damage move the two apart, in opposite
 * directions:
 *
 *   * a gap -- frames the file is missing -- leaves it *shorter* than the time
 *     axis, so a read position computed linearly from the subint's time comes
 *     out too far along by exactly the bytes of the missing frames (subtract);
 *   * a filler frame -- bytes the file has but the time axis has no slot for --
 *     leaves it *longer*, so every later position comes out too early
 *     (add).
 *
 * Both are recorded here as the recording walks the file, and both are asked
 * for by TIME: "how much is missing before time t", not "how much has been
 * noticed so far".  The difference is what B5 was: a gap noticed inside one
 * subint's buffer but sitting after that subint's end on the time axis must not
 * shorten that subint's read -- applying it the moment it is seen did exactly
 * that, and t25362's boundary subint came out 51 frames early
 * (fxcorr/test/gaps/run_boundary.sh reproduces it on synthetic data).
 *
 * Writing the correction as a function of time makes that a definition rather
 * than a bug to fix.  It also removes a conversion: a gap used to be filed by
 * file offset and turned back into a time slot on every query, from the offset,
 * the frames lost before it and the filler ahead of it -- three quantities whose
 * relationship is easy to get wrong (getting the filler term wrong is what B6
 * was: the gaps after t25362's 572-frame filler run came out 572 slots late,
 * holding the read position 63..73 frames too far along for twelve subints;
 * fxcorr/test/gaps/run_filler.sh reproduces that one).  Here the slot is worked
 * out once, when the gap is recorded, and stored.
 *
 * Pure data: no datastream members, no file I/O, no logging -- the unit tests
 * are fxcorr/test/reader/test_corrections.cpp.
 */
namespace corrections {

/** One gap on the time axis: where it starts and how many frames it ate. */
struct SlotGap
{
	long long start;	///< first slot the gap covers
	long long frames;	///< slots it covers

	SlotGap() : start(0), frames(0) {}
};

/**
 * Gaps and filler found so far, and where the counting has got to.
 *
 * The de-duplication watermarks live here because they are what keeps a frame
 * from being counted twice: consecutive subint reads overlap by their guard
 * margin, so the same filler frame (or the same gap) is seen by more than one
 * scan.  A gap is identified by the file offset of the frame right after it,
 * which stays monotonic even though the corrected read position steps back when
 * a correction grows.
 */
class Ledger
{
public:
	Ledger();

	/** The file's frame grid, used to turn a file offset into a time slot:
	 *  anchorbytes is the distance from the file's first frame to the batch
	 *  start (negative when the file starts after the batch does), framebytes
	 *  the frame size.  Set once, before anything is recorded. */
	void setOrigin(long long anchorbytes, int framebytes);

	/** A filler frame at file offset `offset`.  True when it was new: frames
	 *  already behind the watermark were counted by an earlier scan.  The
	 *  watermark itself is advanced by the scanner (advanceFillerWatermark) --
	 *  it is where the scan has got to, not where the filler is. */
	bool noteFiller(long long offset);

	/** Filler frames counted so far: the amount a read position moves forward
	 *  by, filler having bytes but no slot. */
	long long fillerFrames() const { return fillers; }

	bool fillerWatermarkValid() const { return fwatermarkvalid; }
	long long fillerWatermark() const { return fwatermark; }
	/** The file offset a scan has counted filler up to (monotonic). */
	void advanceFillerWatermark(long long upto);

	/** A gap whose first frame *after* it sits at file offset `offset`, `frames`
	 *  long, with `fillerbefore` filler frames ahead of it in the file.  True
	 *  when it was new; the slot it starts at is worked out here. */
	bool noteGap(long long offset, long long frames, long long fillerbefore);

	/** Frames lost before time slot t.  t is moved out of any gap it falls
	 *  inside: the frames before it are in the gap too, and a read has to land
	 *  after the hole rather than inside data belonging to a later slot. */
	long long gapShift(long long &t) const;

	/** Frames lost in all the gaps recorded so far. */
	long long gapFrames() const { return lost; }

	/** No gap recorded yet -- the ordinary case, and the one a read has to keep
	 *  byte for byte what it always was. */
	bool empty() const { return gaps.empty(); }

private:
	long long anchorbytes;
	int framebytes;

	std::vector<SlotGap> gaps;	///< ascending by start
	long long lost;			///< frames lost in `gaps`
	long long fillers;		///< filler frames counted

	long long gwatermark;		///< file offset of the last gap recorded
	bool gwatermarkvalid;
	long long fwatermark;		///< file offset filler has been counted up to
	bool fwatermarkvalid;
};

inline Ledger::Ledger()
	: anchorbytes(0), framebytes(0), lost(0), fillers(0),
	  gwatermark(0), gwatermarkvalid(false),
	  fwatermark(0), fwatermarkvalid(false)
{
}

inline void Ledger::setOrigin(long long ab, int fb)
{
	anchorbytes = ab;
	framebytes = fb;
}

inline bool Ledger::noteFiller(long long offset)
{
	if(fwatermarkvalid && offset < fwatermark)
		return false;	// behind the scan's watermark: counted by an earlier scan
	fillers++;
	return true;
}

inline void Ledger::advanceFillerWatermark(long long upto)
{
	if(!fwatermarkvalid || upto > fwatermark)
	{
		fwatermark = upto;
		fwatermarkvalid = true;
	}
}

inline bool Ledger::noteGap(long long offset, long long frames, long long fillerbefore)
{
	if(gwatermarkvalid && offset <= gwatermark)
		return false;	// this gap has been counted already

	gwatermark = offset;
	gwatermarkvalid = true;

	// Where the gap sits on the time axis.  The file offset counts every frame
	// the file holds; the time axis counts only the ones that carry data -- so
	// the filler ahead of the gap is subtracted (no slot) and the frames lost
	// in the earlier gaps are added on (no bytes).
	SlotGap g;
	g.start = (framebytes > 0) ? (offset - anchorbytes)/(long long)framebytes : 0;
	g.start += lost - fillerbefore;
	g.frames = frames;
	gaps.push_back(g);
	lost += frames;
	return true;
}

inline long long Ledger::gapShift(long long &t) const
{
	long long total = 0;
	for(std::size_t i=0;i<gaps.size();i++)
	{
		if(t < gaps[i].start)
			break;		// this gap and every later one start at or after t
		if(t < gaps[i].start + gaps[i].frames)
			t = gaps[i].start + gaps[i].frames;
		total += gaps[i].frames;
	}
	return total;
}

}

#endif
