#pragma once

#include <string>

/**
    The GLSL, as text.

    `kCommon` is a LIBRARY, not a shader: no #version, no main. Every pass is
    assembled as kVersion + kCommon + its body, and so is the harness's probe
    of the beam pattern (`ratest --arc` reads the pattern out of the painted
    phosphor, not out of a copy), so a check runs the text the plugin runs.
    Pieces stay under MSVC's ~16 KB literal cap; tools/glslc.sh reassembles
    them for glslc.

    Passes, in order:

      0. kernel     (on change) the beam pattern tabulated per bin offset

      1. map        (source only, on change) the synthetic sea and land, a
                    square over the scope in scope units, km inside
      2. reflect    the reflectivity in range x bearing, for the bearing band
                    the beam can reach from this frame's wedge: a polar
                    resample of the map (source) or the clip (Over),
                    averaged over the pulse's range footprint
      3. paint      the wedge's columns only, into a scratch buffer copied
                    back: the last paint carried forward by its decay, plus
                    this frame's echo -- the band convolved with the beam
                    pattern, the point targets analytically, clutter, rain,
                    noise, STC, the radar equation
      4. composite  polar -> the screen: every texel faded by its column's
                    age (timed on the CPU in double), the two components in
                    their colours, the rings and marks, the clip (Over)
*/
namespace radar::shaders
{
extern const char* const kVersion;
extern const char* const kCommon;

extern const char* const kQuadVertex;
extern const char* const kKernelFragment;
extern const char* const kMapFragment;
extern const char* const kReflectFragment;
extern const char* const kPaintFragment;
extern const char* const kCompositeFragment;

/// kVersion + kCommon + the pieces, in order.
std::string Assemble( const char* a, const char* b = nullptr );

} // namespace radar::shaders
