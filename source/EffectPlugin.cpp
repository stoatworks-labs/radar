#include "Radar.h"

/**
    The effect: the clip is the sea. Its bright parts are the reflectivity,
    laid under the scope in range and bearing, and the sweep paints them as
    echoes.

    See SourcePlugin.cpp for why this file is listed in its own target.
*/
namespace
{
class RadarEffect : public radar::RadarPlugin
{
public:
	RadarEffect() :
		RadarPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< RadarEffect >, // Create method
	"RA02",                       // Plugin unique ID of maximum length 4
	"SW Radar Over",              // Plugin name
	2,                            // API major version number
	1,                            // API minor version number
	0,                            // Plugin major version number
	1,                            // Plugin minor version number
	FF_EFFECT,                    // Plugin type
	"The clip as a radar sees it: its bright parts become echoes at their range and bearing, painted by the sweep "
	"onto a long-persistence phosphor, smeared by the beam and the pulse. Threshold picks what reflects; Mix brings "
	"the clip back.",
	"Radar FFGL effect"           // About
);

extern "C" const char* RadarEffectBuildStamp()
{
	return "radar " RADAR_VERSION " effect, built " __DATE__ " " __TIME__;
}
