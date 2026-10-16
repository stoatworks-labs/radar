#pragma once

#include <cstdint>
#include <vector>

/**
    The sprites' motion (radar#5), on one of two paths:

    - **Closing** (v0.2.0's, the default): each one comes in from the rim on a
      hashed bearing to the centre, then a new one starts at the rim.
    - **Transit**: the radar is heading north through objects that are not
      moving. Each sprite comes into range at the top of the scope, at a
      hashed point across it, travels straight down (south) along that chord,
      and leaves at the bottom rim; then a new one comes in at the top.

    Like the source's contacts, they live in SCOPE units (1 = the scope's
    radius), so Range zooms the coast and leaves them alone. Only the sprites
    move: the coast, the clutter, the rain and the source's own contacts are
    the same on either path.

    **Where a Transit track runs.** The entry point is uniform across the
    middle 85% of the scope's width (|x| <= kTransitBand). Uniform in x is
    what still objects scattered evenly over the sea would give a radar going
    north through them: with every slot starting again as it leaves, a slot
    spends time on a chord in proportion to its length, so the scope fills
    evenly. The band keeps the shortest track at about half the diameter
    (2 sqrt( 1 - 0.85^2 ) = 1.05 radii): a track hugging the rim would be a
    brief scrap of a sprite half outside the scope.

    **Deterministic.** A slot's every random choice is a PCG hash of (seed,
    slot, generation), and its progress is integrated in double from the
    host's real elapsed time, the antenna's clock -- so the same seed and the
    same clock give the same sprites, and `ratest` can render any frame twice.

    **Exact at any frame rate.** Speed wanders (Stability below 1) as
    v ( 1 + a w( age ) ), w a sum of two sines; the distance a frame adds is
    the closed-form integral of that over the frame, not v x dt at its start,
    so 24 fps and 240 fps put a sprite in the same place at the same time. The
    weave is a function of the distance travelled, so a sprite that is not
    moving does not sway: in bearing on Closing, sideways (across the track)
    on Transit, where it fades to nothing at the rim so the track stays inside
    the scope.

    **A control never moves a sprite by itself.** Speed, Speed Spread and
    Stability set how fast the distance grows, never the distance: moving a
    slider changes where they are going, not where they are. Path is LATCHED
    per generation: a sprite in flight finishes on the path it started on and
    the next one takes the new path. (Seed is the exception: it re-hashes
    every bearing and entry point.)

    **Starting.** When slots are made -- the first frame, or Sprite Count
    raised -- up to Start With of them are put on screen at once, part-way
    along their tracks, staggered by the golden ratio (v0.2.0's start, which
    the default, "all of them", keeps). The rest are LATECOMERS: each waits a
    hashed distance out beyond the rim, counted down at Sprite Speed, and
    comes in at the rim (the top, on Transit) when it runs out. The window
    (ArrivalWindow()) is 85% of the shortest track at the fastest any sprite
    can go, so at steady controls every latecomer is on the scope before the
    first of them can leave. It is split into as many equal parts as there are
    latecomers, and the j-th comes in at a hashed point of the j-th part: an
    irregular trickle, never a clump and never a long gap, whose last arrival
    is always in the final part (pure uniform draws left seed 11's last of
    nine at 58% of the window). A wait counts down at Sprite Speed, so Sprite
    Speed 0 holds the latecomers back as it holds the rest still, and a
    faster Speed brings them in sooner.
*/
namespace radar::swarm
{
enum class Path
{
	Closing,///< rim to centre
	Transit ///< top of the scope to the bottom, straight down
};

/// Transit's entry points are uniform over |x| <= this (scope radii).
constexpr double kTransitBand = 0.85;
/// Transit's sideways weave at Stability 0, scope radii. No more than
/// 1 - kTransitBand, which is what keeps a weaving track inside the scope.
constexpr double kTransitWeave = 0.15;
/// The latecomers arrive within this share of the quickest crossing.
constexpr double kArrivalShare = 0.85;
/// "All of them": Start With at its default.
constexpr int kStartAll = 1 << 30;

struct Settings
{
	int count          = 0;
	uint32_t seed      = 0;
	double speed       = 0.0;///< scope radii per second, the base
	double spread      = 0.0;///< 0..1: each sprite's own speed, 1/3x to 3x
	double stability   = 1.0;///< 0..1: 1 steady, 0 speed wanders and the track weaves
	Path path          = Path::Closing;///< for sprites that start from now on
	int startWith      = kStartAll;    ///< how many new slots are on screen at once
};

/// One sprite this frame.
struct Placed
{
	double x = 0.0, y = 0.0;   ///< its centre, scope units, x east, y north
	double range   = 0.0;      ///< |centre|, 1 at the rim, 0 at the centre
	double bearing = 0.0;      ///< of its centre, radians clockwise from north, [0, 2 pi)
	double progress = 0.0;     ///< the fraction of its track behind it, 0..1
	uint32_t slot = 0, generation = 0;
	Path path = Path::Closing; ///< the path this generation is on
};

class Swarm
{
public:
	/// Advance by dt seconds of host time, keeping `settings.count` slots.
	void Step( double dt, const Settings& settings );
	/// The sprites on the scope: every slot but the latecomers still waiting.
	std::vector< Placed > Sprites() const;
	size_t Size() const
	{
		return slots.size();
	}
	/// Slots still waiting to come in.
	int Waiting() const;

	/// The speed slot `slot` of generation `generation` has at Stability 1:
	/// Sprite Speed x its spread factor. For the harness.
	static double SpeedOf( const Settings& settings, uint32_t slot, uint32_t generation );
	/// Where a new slot starts: the fraction of the way along, [0, 1).
	static double StartOf( uint32_t slot );
	/// Transit: where generation `generation` of `slot` crosses, x in scope
	/// radii, uniform over |x| <= kTransitBand.
	static double EntryOf( uint32_t seed, uint32_t slot, uint32_t generation );
	/// The length of a track: 1 (Closing), or the chord 2 sqrt( 1 - x^2 ).
	static double TrackLength( Path path, uint32_t seed, uint32_t slot, uint32_t generation );
	/// The latecomers' window, in scope radii at Sprite Speed: kArrivalShare
	/// x the shortest track / ( 3^Spread x ( 1 + the surge ) ). In seconds at
	/// a steady Sprite Speed v it is this / v.
	static double ArrivalWindow( const Settings& settings );
	/// Slot `slot`'s wait as the `nth` (0-based) of `of` latecomers made
	/// together, scope radii at Sprite Speed: ArrivalWindow() x ( nth + u ) / of,
	/// u its hashed share of its part, uniform in [0, 1).
	static double WaitOf( const Settings& settings, uint32_t slot, int nth, int of );

	/// --sprite-motion's negative control: the distance a frame adds is
	/// v / 60, whatever the frame's real length -- motion tied to frames.
	void SetPerFrameForTest( bool on )
	{
		perFrameForTest = on;
	}
	/// --sprite-transit's negative control: Transit's tracks run north.
	void SetTransitNorthForTest( bool on )
	{
		northForTest = on;
	}
	/// --sprite-path's negative control: entry points across the whole width.
	void SetEntryWideForTest( bool on )
	{
		wideForTest = on;
	}
	/// --sprite-arrivals' negative control: the latecomers spread over one
	/// whole crossing at Sprite Speed, Spread and the surge ignored.
	void SetArrivalNaiveForTest( bool on )
	{
		naiveForTest = on;
	}

private:
	struct Slot
	{
		uint32_t generation = 0;
		double age          = 0.0;///< seconds since this generation started
		double travelled    = 0.0;///< scope radii along its track
		double wait         = 0.0;///< a latecomer's distance still to come, at Sprite Speed; > 0: not on the scope yet
		Path path           = Path::Closing;
	};

	double entryOf( uint32_t seed, uint32_t slot, uint32_t generation ) const;
	double lengthOf( Path path, uint32_t seed, uint32_t slot, uint32_t generation ) const;

	std::vector< Slot > slots;
	Settings last;
	bool perFrameForTest = false;
	bool northForTest    = false;
	bool wideForTest     = false;
	bool naiveForTest    = false;
};

} // namespace radar::swarm
