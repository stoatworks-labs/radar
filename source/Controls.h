#pragma once

#include "StoatworksAboutLinks.h"

#include <vector>

/**
    The host's parameters, and what they mean in physical units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` could
    widen it. The conversions live in Controls.cpp, one function per control.
    Option, boolean and event parameters hold the element value itself;
    Contacts and Seed are real integers (`FF_TYPE_INTEGER`).

    Units: km, seconds, microseconds, degrees, decibels. Range inside the
    shaders is NORMALISED: 1 is the edge of the scope, whatever Range says.
*/
namespace radar
{
/**
    Every control either plugin has, by what it is -- NOT by the index a host
    sees. Each plugin declares its own subset, densely, in the order of
    `HostOrder()`, with the About block LAST in both.

    Why not boreal's shape (a shared prefix, the Over group after the About
    block): the day a user guide is written the About block grows a "User
    guide" button, and everything declared after it moves up one index. Here
    nothing is declared after it. And why not flyback's (both plugins declare
    every control, the other's inert): an inert control is a slider that does
    nothing, which the gate has to be told about and a user does not. So each
    plugin declares only what it uses, and `HostOrder()` is the one table.
*/
enum ParamId : unsigned int
{
	// -- Antenna -------------------------------------------------------------
	PT_RPM = 0,
	PT_BEAMWIDTH,
	PT_SIDELOBES,
	PT_DIRECTION,

	// -- Transmitter ---------------------------------------------------------
	PT_PULSE,
	PT_RANGE,
	PT_GAIN,
	PT_STC,

	// -- Returns -------------------------------------------------------------
	PT_CLUTTER,
	PT_NOISE,
	PT_CONTACTS,  ///< source only
	PT_RAIN,      ///< source only
	PT_LAND,      ///< source only
	PT_SEED,      ///< source only
	PT_THRESHOLD, ///< Over only: the clip's luma below which nothing echoes

	// -- Audio ---------------------------------------------------------------
	PT_AUDIO,
	PT_AUDIO_STROBE,
	PT_AUDIO_CONTACTS,///< source only

	// -- Scope ---------------------------------------------------------------
	PT_PERSISTENCE,
	PT_FLASH,
	PT_PHOSPHOR,
	PT_RINGS,
	PT_BEARING_MARKS,
	PT_HEADING_LINE,
	PT_SCOPE_SIZE,
	PT_MIX,///< Over only

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_TEXT,
	PT_COUNT = PT_ABOUT_TEXT + 1 + stoatworks::about::kButtonCount
};

/// The ids a plugin declares, in the order the host sees them. Host index i
/// is `HostOrder( effect )[ i ]`. The About block is last in both.
const std::vector< unsigned int >& HostOrder( bool effect );

/// The group each control is shown under.
const char* GroupOf( unsigned int id );

enum class Direction
{
	Clockwise = 0,
	Anticlockwise,
	Count
};

enum class Phosphor
{
	P7 = 0,///< blue-white flash, yellow-green afterglow: the classic PPI
	P19,   ///< orange, long
	Green, ///< a green P1/P39-like screen
	Count
};

/// The polar grid everything is painted into: bearing across, range up. Fixed,
/// never the host's raster -- which is why a resize cannot touch the phosphor.
constexpr int kBearings = 2048;
constexpr int kRanges   = 1024;
/// The synthetic map (source), a square over the scope in scope units.
constexpr int kMapSize = 1024;
/// At most this many point targets are painted analytically.
constexpr int kMaxTargets = 32;

/// Metres of range per microsecond of pulse, two-way: c/2.
constexpr double kMetresPerMicrosecond = 149.896229;

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------

/// Antenna revolutions per minute: 0 at the bottom (stopped), then 1 to 120,
/// geometrically.
double RpmFromParam( float v );
float ParamFromRpm( double rpm );
/// The painted arc's half-power width, degrees, 0.5 to 20, geometric. This is
/// the TWO-WAY width (transmit x receive): what a point target paints.
double BeamwidthFromParam( float v );
float ParamFromBeamwidth( double degrees );
/// 0 = a Gaussian beam (no sidelobes) .. 1 = a uniform aperture's sinc^4.
double SidelobesFromParam( float v );
/// Pulse length, microseconds, 0.05 to 20, geometric.
double PulseFromParam( float v );
float ParamFromPulse( double microseconds );
/// The radius of the scope, km, 0.5 to 200, geometric.
double RangeFromParam( float v );
float ParamFromRange( double km );
/// Receiver gain, dB, -30 to +50.
double GainDbFromParam( float v );
float ParamFromGainDb( double db );
/// The STC exponent n: the gain rises as R^n. 0 to 4, linear.
double StcFromParam( float v );
/// Sea clutter, 0..1.
double ClutterFromParam( float v );
/// Receiver noise, 0..1.
double NoiseFromParam( float v );
/// Contacts is an integer parameter, 0..16 (real value).
/// Rain, 0..1.
double RainFromParam( float v );
/// Land, 0..1: open sea .. an archipelago.
double LandFromParam( float v );
/// The clip's luma threshold, 0..1.
double ThresholdFromParam( float v );
/// The afterglow's time constant, seconds, 0.1 to 30, geometric.
double PersistenceFromParam( float v );
float ParamFromPersistence( double seconds );
/// The flash's strength against the afterglow's (which is 1), 0 to 4, linear.
double FlashFromParam( float v );
/// The flash's time constant, seconds, by phosphor.
double FlashTauFor( Phosphor phosphor );

int OptionIndex( float value, int count );

} // namespace radar
