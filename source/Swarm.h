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
    Stability's surge set how fast the distance grows, never the distance:
    moving a slider changes where they are going, not where they are. What
    would place a sprite is LATCHED per generation, when it comes in: its Path
    (a sprite in flight finishes on the path it started on) and its weave's
    share of Stability (a jump of Stability from 1 to 0 would otherwise throw
    a Transit sprite 0.15 radii sideways in one frame, and a Closing one 0.35
    rad round in bearing, which v0.2.0 did). The next generation takes the
    controls as they are. (Seed is the exception: it re-hashes every bearing
    and entry point.)

    **Starting: a fill.** When slots are made -- the first frame, or Sprite
    Count raised, at once or a step at a time as a slider sends it -- a FILL
    starts unless one is still running: one window, ArrivalWindow(), 85% of
    the shortest track at the fastest any sprite can go, counted down at
    Sprite Speed from the moment the fill started. Up to Start With of the new
    slots go on the scope at once, staggered by the golden ratio; the rest are
    LATECOMERS, each waiting a hashed distance out beyond the rim and coming
    in at the rim (the top, on Transit) when it runs out.

    - The latecomers share what is left of the fill's window: every time one
      is added, all those still waiting are spread over it again, the j-th of
      m (by slot) at a hashed point of the j-th m-th. An irregular trickle,
      never a clump and never a long gap, the last always in the final part
      -- whether Sprite Count jumped to 12 or was dragged there one step a
      frame. (Pure uniform draws left seed 11's last of nine at 58% of the
      window; stratifying each rise on its own clumped a drag.)
    - **Nobody leaves before the last latecomer is in.** A latecomer that
      arrives at t >= 0 cannot cover the shortest track before the window's
      end. And while Start With is below "all of them", the ones put on at
      once are kept to the first stretch of their track, at most
      L - 0.85 x the shortest track along it (the outer 15% of the radius
      closing), so they cannot leave before the window's end either. At Start
      With's default (kStartAll) they are spread along the whole track as in
      v0.2.0, which is fine, because then nothing waits.
    - A wait counts down at Sprite Speed, so Sprite Speed 0 holds the
      latecomers back as it holds the rest still, and a faster Speed brings
      them in sooner. The promise holds at steady Spread and Stability.
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
/// "All of them": Start With at its default and top (Sprite Count's top too).
/// At this or above, every new slot goes on at once, spread along its track.
constexpr int kStartAll = 32;

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
	/// --sprite-path's negative controls: the ones on at once staggered along
	/// the whole track even with latecomers to come; the weave read live from
	/// Stability every frame; each rise's latecomers stratified on their own.
	void SetStaggerWholeForTest( bool on )
	{
		staggerWholeForTest = on;
	}
	void SetWeaveLiveForTest( bool on )
	{
		weaveLiveForTest = on;
	}
	void SetBatchOnlyForTest( bool on )
	{
		batchOnlyForTest = on;
	}

private:
	struct Slot
	{
		uint32_t generation = 0;
		double age          = 0.0;///< seconds since this generation started
		double travelled    = 0.0;///< scope radii along its track
		double wait         = 0.0;///< a latecomer's distance still to come, at Sprite Speed; > 0: not on the scope yet
		Path path           = Path::Closing;///< latched when this generation started
		double loose        = 0.0;///< 1 - Stability when this generation started: its weave's share
	};

	double entryOf( uint32_t seed, uint32_t slot, uint32_t generation ) const;
	double lengthOf( Path path, uint32_t seed, uint32_t slot, uint32_t generation ) const;
	/// A generation starts: latch what places it.
	static void startGeneration( Slot& s, const Settings& settings );
	/// Every latecomer still waiting, spread over what is left of the fill.
	void spreadWaits( const Settings& settings );

	std::vector< Slot > slots;
	Settings last;
	double fillLeft = 0.0;///< what is left of the current fill's window, scope radii at Sprite Speed
	bool perFrameForTest     = false;
	bool northForTest        = false;
	bool wideForTest         = false;
	bool naiveForTest        = false;
	bool staggerWholeForTest = false;
	bool weaveLiveForTest    = false;
	bool batchOnlyForTest    = false;
};

} // namespace radar::swarm
