#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
    A sprite file as the radar sees it: a MASK, one value per texel, 0..1.

    radar#5 asked for "sprites as detected objects": an image the scope paints
    as its contacts. A radar sees reflectivity, not colour, so what the plugin
    keeps of the picture is one number per texel, and Sprite Threshold cuts it:

    - an image with real transparency (any alpha below 255) gives its ALPHA;
    - an opaque one (a JPG, or a PNG with no alpha) gives its BRIGHTEST
      CHANNEL, the Over effect's rule for the clip and for the same reason: a
      saturated blue shape has a luma of 0.07 and would never echo.

    The mask is box-filtered down to at most kMaxMaskSide texels on its longer
    side, which keeps a 4000-pixel PNG from aliasing at 40 pixels on the
    screen (and the texture tiny). Box filtering keeps the mean, so a hard
    alpha edge becomes a coverage ramp and a threshold of 0.5 finds the edge
    where the picture had it.

    Rows are stored BOTTOM FIRST, GL's order: texel ( x, y ) with y = 0 is the
    bottom row of the picture.

    **The picture's colour** (since v0.2.0, for Sprite Colour's *Picture*):
    the mean of its stored RGB, each pixel weighted by its mask value (alpha,
    or the brightest channel), so a transparent or dark pixel counts for as
    little as it is a target. Computed once, at load, from the full-size
    picture; a threshold-free weighting, because Sprite Threshold moves live
    and the colour does not. Encoded values, not linear light: the plugin
    writes its output with no transfer curve, so these are the units the
    phosphor's colours are already in. A mean whose HSV saturation is under
    kGreySaturation is grey, and has no colour to give.

    This is the only file that sees stb_image or the file system. On Windows
    the host's UTF-8 path is widened to UTF-16 for `_wfopen`: a narrow `fopen`
    reads the bytes in the ANSI code page and cannot open a folder with a
    non-ASCII name (downpour's and jackpot's trap).
*/
namespace radar::sprite
{
constexpr int kMaxMaskSide = 128;
/// Larger pictures are refused before they are decoded: 8192^2 x 4 bytes is
/// already 256 MB, and nothing about a radar contact needs it.
constexpr int kMaxImageSide = 8192;
/// Files are read whole; anything past this is not a sprite.
constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;
/// A mean colour with less HSV saturation, ( max - min ) / max, than this is grey.
constexpr double kGreySaturation = 0.05;

struct Mask
{
	int width  = 0;
	int height = 0;
	std::vector< float > value;///< width x height, rows bottom first, 0..1
	bool fromAlpha = false;    ///< alpha, or (false) the brightest channel
	float colour[ 3 ] = { 0.0f, 0.0f, 0.0f };///< the picture's mean RGB, weighted by the mask
	bool coloured     = false;///< that mean is not grey (the blip has none)
};

struct Loaded
{
	bool ok = false;
	Mask mask;
	int sourceWidth = 0, sourceHeight = 0, sourceChannels = 0;
	std::string why;///< when !ok: what went wrong, for the log
};

/// The built-in sprite, used when no file is set or the file fails: a radar
/// blip, 64 x 64, a cone 1 at the centre and 0 at the inscribed circle. So
/// Sprite Threshold T leaves a disc Sprite Size x (1 - T) across, which is
/// what `ratest --sprite-threshold` measures.
const Mask& BuiltIn();

/// Whether a mean colour is far enough from grey to tint anything.
bool IsColoured( const float colour[ 3 ] );

/// The picture's bytes, decoded and reduced to a mask. Never throws.
Loaded Decode( const unsigned char* bytes, size_t size );

/// Read a file by its UTF-8 path, whole, into `out`.
bool ReadFileBytes( const std::string& utf8Path, std::vector< unsigned char >& out, std::string& why );

/// Read and decode. `utf8Path` is what the host handed SetTextParameter.
Loaded LoadFile( const std::string& utf8Path );

/// Strict UTF-8 to UTF-16 (what `_wfopen` takes on Windows): overlong forms,
/// surrogates, code points past U+10FFFF and truncated sequences are refused,
/// never guessed at. Portable, so the harness checks it on macOS
/// (`ratest --sprite-utf16`); only Windows calls it for real.
bool Utf8ToUtf16( const std::string& in, std::u16string& out );

/// --sprite-utf16's negative control, the wrong model: every byte widened to
/// one code unit, which is what a narrow fopen amounts to for a path that is
/// not ASCII (the bytes read in a single-byte code page).
std::u16string WidenBytesForTest( const std::string& in );

/// The box filter that reduces a full-size mask, exposed for the harness:
/// area-weighted, so the mean is kept.
Mask Reduce( const std::vector< float >& value, int width, int height, int maxSide );

} // namespace radar::sprite
