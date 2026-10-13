#ifndef FRAMETIMELINE_H
#define FRAMETIMELINE_H

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

/**
 * @file frametimeline.h
 * @brief Layer 1 of the reader rework (v4-plan.md phase C): the frame/time-axis
 *        algorithms, as pure functions.
 *
 * A raw VDIF file hands over frames in *file* order, which is not *time* order:
 * a recording interruption leaves either frames missing (the file is shorter
 * than the time axis) or filler frames behind (bytes but no time slot), and both
 * push the two orders apart.  Everything the reader does about that is a
 * question about frame numbers and file offsets alone, and this header holds
 * those questions as functions of their inputs only -- no datastream members,
 * no file I/O, no logging -- so they can be tested on synthetic frame sequences
 * (fxcorr/test/reader/test_timeline.cpp) instead of through a correlator run.
 *
 * The two questions:
 *
 *   * walkFrameChain -- given a stretch of frames, where does the frame-number
 *     chain break, and how many frames are missing at each break?  (Data frames
 *     only: filler carries no number and is reported separately, since it says
 *     the opposite thing about the read position.)
 *   * placeFrames -- given a stretch of frames read for one subint, which time
 *     slot does each one belong in?  Filler is dropped, a frame-number jump
 *     advances the destination by the jump, and the slots nobody claims are
 *     reported as holes.  The caller that only wants to know *how much* input a
 *     subint needs calls this with a null destination (dry run) -- the same
 *     walk decides both, which is the point: two copies of that judgement drift
 *     apart silently (B2, reader-model.md 4.8).
 *
 * Types: `unsigned char`, not the `u8` macro, so this header compiles on its
 * own (the macro comes from DiFX's architecture.h and expands to unsigned char;
 * the two are the same type, so callers pass either).
 */
namespace frametimeline {

// Frame number of frame i of a raw VDIF buffer: word 1 of the VDIF header, low
// 24 bits (word layout per vdifio's vdif_header, not the vlbi.org spec).
inline long long vdifFrameNumber(const unsigned char *buffer, int i, int framebytes)
{
	const unsigned char *p = buffer + (long long)i*framebytes;
	unsigned int w1 = (unsigned int)p[4] | ((unsigned int)p[5] << 8)
	                  | ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
	return (long long)(w1 & 0xFFFFFF);
}

// Is frame i a filler -- a frame the recorder wrote where a recording
// interruption left it without data?  Two shapes occur: the VDIF invalid bit
// (word 0 bit 31) set, or a completely zero header (which is what t25362's BA
// thread 2 uses -- seconds 0, frame number 0, frame length 0, so the frame
// also claims a length of 0 bytes).  Such a frame occupies its bytes in the
// file but no slot on the time axis: after a run of them the frame number
// picks up from the last real frame plus the frames actually lost, not plus
// the filler count.  Counting them as missing frames instead (which the
// frame-number step alone cannot distinguish) inflates the correction by
// roughly a whole second's worth of frames per run boundary.
inline bool vdifIsFiller(const unsigned char *buffer, int i, int framebytes)
{
	const unsigned char *p = buffer + (long long)i*framebytes;
	unsigned int w0 = (unsigned int)p[0] | ((unsigned int)p[1] << 8)
	                  | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);

	// vdifio's FILL_PATTERN (vdifmux.c:30-32): a recorder that had no data for
	// a frame leaves this word behind -- at the frame's END in some
	// implementations, at its START in others.  vdifmux tests exactly those
	// two places (vdifmux.c:598/606) and skips the frame either way: the end
	// test skips it whole, the start test only eight bytes and then walks
	// until it re-syncs, which on a frame made of the pattern lands on the
	// same place.  Measured with the two forms side by side: mpifxcorr's
	// output is byte-identical for a zero-header run and for either pattern,
	// so fxcorr treats both as "bytes, no time slot" like any other filler
	// (fxcorr/test/gaps/run_pattern.sh).  A data frame whose payload happens
	// to carry the word would be dropped -- the same 2^-32 exposure upstream
	// has always had.
	//
	// Only when the frame is long enough: a short frame (LBA/Mark5 straddling
	// is not this path, but keep the test safe) cannot hold a trailing word.
	static const unsigned int FILLPATTERN = 0x11223344u;
	if(w0 == FILLPATTERN)
		return true;
	if(framebytes >= 4)
	{
		const unsigned char *t = p + framebytes - 4;
		unsigned int wt = (unsigned int)t[0] | ((unsigned int)t[1] << 8)
		                  | ((unsigned int)t[2] << 16) | ((unsigned int)t[3] << 24);
		if(wt == FILLPATTERN)
			return true;
	}

	unsigned int w1 = (unsigned int)p[4] | ((unsigned int)p[5] << 8)
	                  | ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
	return (w0 & 0x3FFFFFFFu) == 0 && (w1 & 0xFFFFFFu) == 0;
}

/**
 * True for a frame whose data the recorder marked unusable (VDIF invalid bit).
 *
 * **This is not a filler.**  A filler frame has no place on the time axis -- its
 * frame number is not even readable and the chain simply runs across it.  An
 * invalid frame is the opposite: the recorder wrote a normal frame in its
 * normal slot and only said "do not trust this data", so its frame number *is*
 * part of the chain.  Measured 2026-09-19: mpifxcorr keeps them in place
 * (output records and timestamps identical to the undamaged file, only the
 * weights drop -- reader-model.md 4.12), while treating them as filler collapses
 * the time axis by the length of the run and moves every later read position
 * (which is what fxcorr did from 2026-09-17, when the invalid bit was folded
 * into vdifIsFiller without a measurement behind it).
 *
 * Callers place such a frame in its slot like any data frame and mark that slot
 * invalid, so the data never reaches the integration.
 */
inline bool vdifIsInvalid(const unsigned char *buffer, int i, int framebytes)
{
	const unsigned char *p = buffer + (long long)i*framebytes;
	unsigned int w0 = (unsigned int)p[0] | ((unsigned int)p[1] << 8)
	                  | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	return (w0 & 0x80000000u) != 0;
}

/** One frame-number discontinuity inside a scanned stretch. */
struct FrameGap
{
	int index;		///< source index of the frame *after* the gap
	long long offset;	///< that frame's file offset
	long long after;	///< frame number before the gap
	long long frame;	///< frame number after the gap
	long long step;		///< observed step, in frames (mod frames-per-second)
	long long missing;	///< frames lost there

	FrameGap() : index(0), offset(0), after(0), frame(0), step(0), missing(0) {}
};

/**
 * What one stretch of frames says about the frame-number chain.
 *
 * Data frames and filler are both counted, but only data frames carry numbers:
 * `first`/`last` are the first and last data frame number seen (both -1 for a
 * stretch that is filler throughout, which is why they cannot be derived from
 * `frames` alone), and `fillers` lists the filler positions so a caller can
 * decide per frame whether it has already counted that one.  `invalids` lists
 * the frames whose data the recorder marked unusable: they are counted in
 * `frames` and advance the chain like data frames (they hold their slot), they
 * are only listed so a caller can mark the slot's data invalid.
 */
struct FrameRun
{
	long long first;	///< first data frame number, -1 if none
	long long last;		///< last data frame number, -1 if none
	long long frames;	///< frames on the time axis (invalid ones included)
	long long missing;	///< sum of gaps' missing
	std::vector<int> fillers;	///< filler frame indices, ascending
	std::vector<int> invalids;	///< invalid-marked frame indices, ascending
	std::vector<FrameGap> gaps;	///< discontinuities, in scan order

	FrameRun() : first(-1), last(-1), frames(0), missing(0) {}
};

/**
 * Walks the frame-number chain across `srcframes` frames of `src`.
 *
 * `chainfr` is the last data frame number seen *before* this stretch (-1 when
 * there is none), so a gap straddling the seam is caught here rather than by
 * neither side.  `baseoffset` is the file offset of the stretch's first frame,
 * which is what the caller identifies a gap by.
 *
 * A step that is not 1 means frames are missing there, except when it is 0 --
 * the same frame number twice, which is what the counter does across a second
 * boundary in some recordings and means nothing was lost.  Filler frames break
 * neither: the number picks up after a run as if the run were not there.
 */
inline FrameRun walkFrameChain(const unsigned char *src, int srcframes, int framebytes,
                               long long baseoffset, long long fps, long long chainfr)
{
	FrameRun run;
	long long pfr = chainfr;

	for(int i=0;i<srcframes;i++)
	{
		if(vdifIsFiller(src, i, framebytes))
		{
			run.fillers.push_back(i);
			continue;
		}
		long long fr = vdifFrameNumber(src, i, framebytes);
		// an invalid frame holds its slot, so it advances the chain like a data
		// frame -- only its data must not be used (vdifIsInvalid)
		if(vdifIsInvalid(src, i, framebytes))
			run.invalids.push_back(i);
		run.frames++;
		if(run.first < 0)
			run.first = fr;
		run.last = fr;

		if(pfr >= 0)
		{
			long long step = (fr - pfr + fps) % fps;
			if(step != 1)
			{
				long long m = (step == 0) ? 0 : step - 1;
				if(m > 0)
				{
					FrameGap g;
					g.index = i;
					g.offset = baseoffset + (long long)i*framebytes;
					g.after = pfr;
					g.frame = fr;
					g.step = step;
					g.missing = m;
					run.gaps.push_back(g);
					run.missing += m;
				}
			}
		}
		pfr = fr;
	}
	return run;
}

/**
 * Where a stretch of frames read for one subint lands on the time axis.
 *
 * `holes` are the slot ranges nobody claims, ascending and non-overlapping:
 * the head when the subint's start is swallowed by a gap, each jump's own
 * slots, and the tail when the stretch ran out of frames.  All of them are
 * slots the block validators have to see cleared.
 */
struct SlotPlacement
{
	int placed;		///< slots filled (`holes` excluded; invalid frames count)
	int usedframes;		///< source frames walked getting that far
	int firstslot;		///< slot the first frame belongs in
	std::vector<std::pair<int,int> > holes;	///< [from,to) slot ranges, ascending
	/** Slots whose frame is present but marked invalid, ascending.  Their data
	 *  was copied into the buffer like any other frame's -- callers must clear
	 *  the blocks these slots cover, or the recorder's "do not use this" is
	 *  thrown away and the run is integrated as if nothing happened. */
	std::vector<int> invalidslots;

	SlotPlacement() : placed(0), usedframes(0), firstslot(0) {}

	/** `holes` and `invalidslots` merged into one ascending, non-overlapping
	 *  range list -- what the block validators have to clear.  The two do not
	 *  overlap (a hole is a slot nobody claimed, an invalid slot one that an
	 *  invalid frame claimed), but they can touch, and the callers want one
	 *  list either way. */
	std::vector<std::pair<int,int> > invalidRanges() const
	{
		std::vector<std::pair<int,int> > out;
		size_t h = 0, v = 0;
		while(h < holes.size() || v < invalidslots.size())
		{
			int from, to;
			if(v >= invalidslots.size() ||
			   (h < holes.size() && holes[h].first <= invalidslots[v]))
			{
				from = holes[h].first;
				to = holes[h].second;
				h++;
			}
			else
			{
				from = invalidslots[v];
				to = from + 1;
				v++;
			}
			if(!out.empty() && from <= out.back().second)
			{
				if(to > out.back().second)
					out.back().second = to;
			}
			else
				out.push_back(std::make_pair(from, to));
		}
		return out;
	}
};

/**
 * Places each frame of `src` in the slot its frame number says it belongs in.
 *
 * `anchorframens` is the in-second frame number of the subint's own start
 * (locate() supplies it): the first data frame belongs wherever its own number
 * says relative to that, which is *not* slot 0 when a gap covers the start --
 * the read position then lands on the first frame after the gap and the slots
 * the gap ate belong before it.  Placing it at 0 instead would slide the whole
 * buffer back and mark the wrong slots invalid, so the hole would be integrated
 * as data.
 *
 * `dst` may be null, in which case nothing is written and only the placement is
 * computed -- that is the dry run the caller uses to find out how many input
 * frames a subint needs, through the very same walk that later places them.
 * When it is not null it must not overlap `src` (the caller passes a scratch
 * buffer when it does), and slots no frame claims are left as they are: the
 * caller zeroes the buffer first.
 */
inline SlotPlacement placeFrames(const unsigned char *src, int srcframes, int framebytes,
                                 long long fps, long long anchorframens,
                                 unsigned char *dst, int slots)
{
	SlotPlacement p;
	int d = 0;
	long long prevfr = -1;

	for(int s=0; s<srcframes; s++)
	{
		if(vdifIsFiller(src, s, framebytes))
			continue;
		d = (int)((vdifFrameNumber(src, s, framebytes) - anchorframens + fps) % fps);
		break;
	}
	p.firstslot = d;

	if(d >= slots)
	{
		// The gap reaches past this whole buffer: none of it is this subint's
		// data, and every slot is a hole.
		p.holes.push_back(std::make_pair(0, slots));
		p.usedframes = srcframes;
		if(dst)
			memset(dst, 0, (std::size_t)slots*framebytes);
		return p;
	}
	if(d > 0)
		p.holes.push_back(std::make_pair(0, d));

	for(int s=0; s<srcframes && d<slots; s++)
	{
		// frames walked so far, filler included: the caller uses this to bound
		// its bookkeeping to the stretch this subint actually needs, so that
		// bytes read past the point where the slots fill up are not counted
		// twice or charged to the wrong subint
		p.usedframes = s + 1;

		if(vdifIsFiller(src, s, framebytes))
			continue;	// no time slot: drop it, later frames move up

		// an invalid frame keeps its slot (its number advances the chain); the
		// data goes in as usual, but the slot is reported so the caller clears
		// it (vdifIsInvalid)
		bool invalid = vdifIsInvalid(src, s, framebytes);

		long long fr = vdifFrameNumber(src, s, framebytes);
		if(prevfr >= 0)
		{
			long long miss = (fr - prevfr - 1 + fps) % fps;
			if(miss > 0)
			{
				long long hole = miss;
				if(hole > slots - d)
					hole = slots - d;
				if(hole > 0)
					p.holes.push_back(std::make_pair(d, d + (int)hole));
				d += (int)miss;
				if(d >= slots)
					break;
			}
		}
		if(dst)
			memcpy(dst + (long long)d*framebytes, src + (long long)s*framebytes,
			       (std::size_t)framebytes);
		if(invalid)
			p.invalidslots.push_back(d);
		d++;
		prevfr = fr;
	}

	// everything the source did not fill (a dropped filler run at the end, or
	// frames pushed past the buffer) holds no data
	if(d < slots)
		p.holes.push_back(std::make_pair(d, slots));

	p.placed = (d > slots) ? slots : d;
	return p;
}

}

#endif
