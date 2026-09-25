#pragma once

#include <cstdint>

/**
    The sweep: which bearing bins the antenna crossed this frame.

    The antenna's position is kept as `b`, a double, in BINS: bin k's centre is
    at b = k, so the antenna points exactly at bin k's centre when b == k. It
    is unwrapped (it never wraps at 2 pi) and advances by rate x dt every
    frame, dt being the host's real elapsed time. A double resolves 1e-7 of a
    bin after three days at 120 rpm; a float would not resolve a whole
    rotation after an hour, which is why none of this is float.

    A frame paints the bins whose centres the antenna CROSSED since the last
    frame: moving up from b0 to b1 that is every integer k in (b0, b1], moving
    down every k in [b1, b0). A half-open interval per frame, and consecutive
    frames' intervals abut exactly, so every bin is painted once per rotation
    whatever the frame rate: no gaps, no double paints, and no rasteriser
    involved -- it is integer arithmetic on doubles, done once per frame on the
    CPU. The shader is handed the first bin and a count.

    That is the wedge, analytically: the swept sector between the antenna's
    bearing at the last frame and at this one, rasterised onto the bearing
    grid by the one rule (a bin is painted when its centre is crossed) that
    tiles a rotation exactly. The alternative -- a line at the antenna's
    bearing each frame -- leaves gaps wherever the antenna moves more than a
    bin per frame (`ratest --sweep`'s negative control).

    Each painted bin also gets its AGE: how long before the end of the frame
    the antenna crossed it, (b1 - k) / (b1 - b0) x dt. The phosphor decays
    for exactly that long, so the trail behind the sweep is the same smooth
    ramp at 24 fps as at 240.
*/
namespace radar::sweep
{
struct Wedge
{
	int64_t first = 0;///< the lowest crossed bin, unwrapped (may be negative)
	int count     = 0;///< how many bins, first .. first + count - 1
	/// Age of bin (first + i) at the end of the frame: (b1Rel - i) * ageScale.
	double b1Rel    = 0.0;
	double ageScale = 0.0;
};

inline int64_t floorToInt( double x )
{
	const int64_t i = static_cast< int64_t >( x );
	return ( static_cast< double >( i ) > x ) ? i - 1 : i;
}

inline int64_t ceilToInt( double x )
{
	const int64_t i = static_cast< int64_t >( x );
	return ( static_cast< double >( i ) < x ) ? i + 1 : i;
}

/// The bins crossed moving from b0 to b1 over dt seconds, at most `bins` of
/// them (a frame that covers more than a rotation paints each bin once, with
/// the latest crossing).
inline Wedge Crossed( double b0, double b1, double dt, int bins )
{
	Wedge w;
	if( b1 == b0 )
		return w;
	int64_t first, last;
	if( b1 > b0 )
	{
		first = floorToInt( b0 ) + 1;
		last  = floorToInt( b1 );
		if( last - first + 1 > bins )
			first = last - bins + 1;
	}
	else
	{
		first = ceilToInt( b1 );
		last  = ceilToInt( b0 ) - 1;
		if( last - first + 1 > bins )
			last = first + bins - 1;
	}
	if( last < first )
		return w;
	w.first    = first;
	w.count    = static_cast< int >( last - first + 1 );
	w.b1Rel    = b1 - static_cast< double >( first );
	w.ageScale = dt / ( b1 - b0 );
	return w;
}

/// The negative control: a LINE per frame, the one bin nearest the antenna's
/// bearing at the end of the frame. Gaps wherever it moves more than a bin.
inline Wedge LineAt( double b0, double b1, double dt )
{
	Wedge w;
	if( b1 == b0 )
		return w;
	w.first    = floorToInt( b1 + 0.5 );
	w.count    = 1;
	w.b1Rel    = b1 - static_cast< double >( w.first );
	w.ageScale = dt / ( b1 - b0 );
	return w;
}

/// k mod bins, non-negative.
inline int Column( int64_t k, int bins )
{
	const int64_t m = k % bins;
	return static_cast< int >( m < 0 ? m + bins : m );
}

} // namespace radar::sweep
