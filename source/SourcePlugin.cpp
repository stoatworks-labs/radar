#include "Radar.h"

/**
    The source: a PPI scope over a synthetic sea, no input.

    Listed directly in the RadarSource target, not in radar_core: both plugins
    share the class and not the `CFFGLPluginInfo` below, and putting either
    registration in the shared library would register both plugins into both
    bundles. The core is an OBJECT library because this registers itself from a
    file-scope constructor nothing references (see CMakeLists.txt).

    `SW Radar` is eight characters; the FFGL name field is char[ 16 ] and not
    null-terminated. `oxbow probe` reads it back the way a host does.
*/
namespace
{
class RadarSource : public radar::RadarPlugin
{
public:
	RadarSource() :
		RadarPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< RadarSource >, // Create method
	"RA01",                       // Plugin unique ID of maximum length 4
	"SW Radar",                   // Plugin name
	2,                            // API major version number
	1,                            // API minor version number
	0,                            // Plugin major version number
	1,                            // Plugin minor version number
	FF_SOURCE,                    // Plugin type
	"A plan-position radar scope. The antenna turns, the sweep paints each echo onto a long-persistence phosphor "
	"at its range and bearing, and the picture is the sea convolved with the radar: arcs as wide as the beam, "
	"streaks as long as the pulse, clutter near the centre, and the tracks of moving contacts in their fading paints.",
	"Radar FFGL source"           // About
);

extern "C" const char* RadarSourceBuildStamp()
{
	return "radar " RADAR_VERSION " source, built " __DATE__ " " __TIME__;
}
