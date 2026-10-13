#include "Shaders.h"

namespace radar::shaders
{
const char* const kVersion = "#version 410 core\n";

//===========================================================================
// The library. No #version, no main.
//===========================================================================
const char* const kCommon = R"(
const float PI  = 3.14159265358979;
const float TAU = 6.28318530717959;
const float LN2 = 0.693147180559945;

//= mirrored in World.cpp, Pcg(). Integer only: the same on every GPU.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float hash01( uint v )
{
	return float( pcg( v ) ) * ( 1.0 / 4294967296.0 );
}

//Exponentially distributed, mean 1: the power of a Rayleigh-amplitude return,
//which is what sea clutter, rain and receiver noise look like pulse to pulse.
//hash01 < 1, so the log's argument is never 0.
float speckle( uint v )
{
	return -log( 1.0 - hash01( v ) );
}

//Value noise on an integer lattice that repeats every 256 cells, so a drift
//reduced on the CPU into one period never jumps.
float lattice( ivec2 c, uint seed )
{
	uvec2 w = uvec2( c & 255 );
	return hash01( pcg( w.x + pcg( w.y + seed ) ) );
}

float valueNoise( vec2 p, uint seed )
{
	vec2 i  = floor( p );
	vec2 f  = p - i;
	vec2 s  = f * f * ( 3.0 - 2.0 * f );
	ivec2 c = ivec2( i );
	float a = lattice( c, seed );
	float b = lattice( c + ivec2( 1, 0 ), seed );
	float d = lattice( c + ivec2( 0, 1 ), seed );
	float e = lattice( c + ivec2( 1, 1 ), seed );
	return mix( mix( a, b, s.x ), mix( d, e, s.x ), s.y );
}

//---------------------------------------------------------------------------
// sin, cos and atan with a STATED accuracy. GLSL 4.10 (4.7.1) bounds +, -, *
// and fma (correctly rounded), / (2.5 ulp), exp (3 + 2|x| ulp) and log, and
// promises nothing at all for sin, cos or atan -- and Apple's software
// renderer, which is what a GPU-less CI runner gets, took sin( x ) / x far
// enough off to widen a sinc^4 beam by 3% (--arc found it). These use only
// the operations the spec bounds: a range reduction and a Taylor series.
//
//   sine:   x = k pi + y, |y| <= pi/2, then y to y^11: truncation y^13 / 13!
//           <= 6e-8 at pi/2, about an ulp; the reduction loses k ulp of pi.
//   atan:   t = min / max in [0, 1]; past tan( pi/12 ) it is folded by
//           atan t = pi/6 + atan( ( t sqrt3 - 1 ) / ( t + sqrt3 ) ), so
//           |t| <= 0.268 and the series to t^13 is good to 3e-10.
//---------------------------------------------------------------------------
float sine( float x )
{
	float k  = floor( x / PI + 0.5 );
	//pi in two parts (Cody & Waite), so k pi is subtracted nearly exactly.
	float y  = ( x - k * 3.140625 ) - k * 9.67653589793e-4;
	float y2 = y * y;
	float s  = y * ( 1.0 + y2 * ( -1.0 / 6.0 + y2 * ( 1.0 / 120.0 + y2 * ( -1.0 / 5040.0 + y2 * ( 1.0 / 362880.0 + y2 * ( -1.0 / 39916800.0 ) ) ) ) ) );
	return mod( k, 2.0 ) != 0.0 ? -s : s;
}

float cosine( float x )
{
	return sine( x + 0.5 * PI );
}

float atanUnit( float t )
{
	bool fold = t > 0.267949192;
	if( fold )
		t = ( t * 1.73205081 - 1.0 ) / ( t + 1.73205081 );
	float t2 = t * t;
	float a  = t * ( 1.0 + t2 * ( -1.0 / 3.0 + t2 * ( 1.0 / 5.0 + t2 * ( -1.0 / 7.0 + t2 * ( 1.0 / 9.0 + t2 * ( -1.0 / 11.0 + t2 * ( 1.0 / 13.0 ) ) ) ) ) ) );
	return fold ? a + PI / 6.0 : a;
}

//The bearing of u, clockwise from north (+y), in (-pi, pi]: atan( u.x, u.y ).
//u must not be 0.
float bearingOf( vec2 u )
{
	float ax = abs( u.x ), ay = abs( u.y );
	float a  = ax <= ay ? atanUnit( ax / ay ) : 0.5 * PI - atanUnit( ay / ax );
	if( u.y < 0.0 )
		a = PI - a;
	return u.x < 0.0 ? -a : a;
}

//---------------------------------------------------------------------------
// The antenna's TWO-WAY power pattern at `off` radians from boresight: 1 on
// the axis, exactly 0.5 at +-BeamHalf, so the arc a point target paints has a
// half-power width of 2 BeamHalf = Beamwidth, whatever Sidelobes is.
//
//   Gaussian:       exp( -ln2 (off / BeamHalf)^2 )
//   uniform aperture: one-way power sinc^2, two-way sinc^4, scaled by SincK
//                   so sinc^4 = 0.5 at BeamHalf too
//
// Both are 0.5 at +-BeamHalf and above it inside, below it outside, so any
// mix of them has the same half-power width. OneWayForTest takes the square
// root -- the one-way pattern, sqrt 2 wider: --arc's negative control.
//---------------------------------------------------------------------------
uniform float BeamHalf;
uniform float SincK;
uniform float Sidelobes;
uniform int OneWayForTest;

float beam( float off )
{
	float q  = off / BeamHalf;
	float g  = exp( -LN2 * q * q );
	float x  = SincK * off;
	float sn = abs( x ) < 1e-4 ? 1.0 : sine( x ) / x;
	float s2 = sn * sn;
	float p  = mix( g, s2 * s2, Sidelobes );
	return OneWayForTest == 1 ? sqrt( p ) : p;
}
)";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//===========================================================================
// 0. The beam, tabulated: beam( m bins ) for m = -Half .. Half, by the same
// function the point targets use, rebuilt only when the beam changes. The
// surface's convolution reads it instead of an exp and a sin per tap.
//===========================================================================
const char* const kKernelFragment = R"(
uniform int Half;
uniform int Bearings;
out vec4 FragColor;

void main()
{
	int m     = int( gl_FragCoord.x ) - Half;
	FragColor = vec4( beam( float( m ) * TAU / float( Bearings ) ), 0.0, 0.0, 1.0 );
}
)";

//===========================================================================
// 1. The synthetic map (source only). Rebuilt when Range, Land or Seed move.
// A square over the scope in scope units; its value is the reflectivity of
// the surface there. Land is fBm in KM, so Range zooms it: 64 km cells down to
// a few hundred metres, as many octaves as the scale needs. The coast returns
// hardest (a cliff face), the interior less, the sea nothing.
//===========================================================================
const char* const kMapFragment = R"(
uniform float RangeKm;
uniform float LandAmount;
uniform uint MapSeed;
uniform float MapSize;
out vec4 FragColor;

void main()
{
	if( LandAmount <= 0.0 )
	{
		FragColor = vec4( 0.0 );
		return;
	}
	vec2 u  = gl_FragCoord.xy / MapSize * 2.0 - 1.0;
	vec2 km = u * RangeKm;

	float h = 0.0, amp = 0.5, norm = 0.0, cell = 48.0;
	for( int o = 0; o < 12; ++o )
	{
		if( o >= 3 && cell < RangeKm / 300.0 )
			break;
		h += amp * valueNoise( km / cell, MapSeed + uint( o ) * 1013u );
		norm += amp;
		amp *= 0.62;
		cell *= 0.5;
	}
	h /= norm;
	//Own ship is at sea, with open water round it: the land is pushed down
	//within ~6 km, in km so that Range still zooms one map.
	h -= 0.3 * exp( -dot( km, km ) / 36.0 );

	float thr   = mix( 0.66, 0.40, LandAmount );
	float fw    = max( fwidth( h ), 1e-5 );
	float land  = smoothstep( thr - fw, thr + fw, h );
	float grain = valueNoise( km / 0.4, MapSeed + 77u ) * 0.6 + valueNoise( km / 0.13, MapSeed + 78u ) * 0.4;
	//R: what the surface returns, if the radar can see it; G: how much land
	//is there, for the shadow the reflect pass casts behind it.
	FragColor = vec4( land * ( 0.25 + 0.75 * grain ), land, 0.0, 1.0 );
}
)";

//===========================================================================
// 2. Reflectivity, polar: one texel per (bearing bin, range bin), written
// only for the bearing band this frame's beam can reach. The value is the
// MEAN of the surface over the pulse's footprint in range, [r - L, r]: the
// echo at delay r comes from everything between r - L and r, so the surface
// is smeared outward by the pulse, and a uniform surface stays uniform.
//
// Two channels: R the surface (the map, or the clip), G the sprites (radar#5).
// A sprite is an extended target like the coast: its mask, over Sprite
// Threshold, is reflectivity 1 where the sprite is, resampled here in range
// and bearing and averaged over the same pulse footprint, then convolved with
// the beam in the paint. So the radar smears it exactly as it smears land,
// and the sweep paints it. Sprites cast no shadow and are not shadowed: they
// are contacts, like the source's ships, not land.
//===========================================================================
const char* const kReflectFragment = R"(
uniform sampler2D MapTexture;
uniform int IsEffect;
uniform int Bearings;
uniform int Ranges;
uniform float PulseExtent;///< c tau / 2, in scope units
uniform vec2 ClipCentre;  ///< the scope's centre, clip pixels
uniform float ClipRadius; ///< the scope's radius, clip pixels
uniform vec2 ClipRaster;
uniform vec2 MaxUV;
uniform float Threshold;
uniform float ShadowScale;///< the source's land shadow: 1 / its length, per scope unit
uniform float MirrorForTest;///< 1; -1 only in --over's negative control
uniform int SurfaceOn;      ///< 0: the map or the clip returns nothing (Land 0, Threshold 1)

uniform sampler2D SpriteMask;///< the sprite's mask, rows bottom first, 0..1
uniform int SpriteCount;     ///< the sprites that can touch this band (culled on the CPU)
uniform vec4 SpriteBox[ 32 ];///< centre x, y (scope units), half-width, half-height
uniform vec4 SpriteTurn[ 32 ];///< .xy the sprite's up, ( sin psi, cos psi ): ( 0, 1 ) upright
uniform vec4 SpriteReach[ 32 ];///< its centre's range, its bounding radius, its bearing, the half-angle it subtends
uniform float SpriteThreshold;
out vec4 FragColor;

//The source's radar shadow: what the beam has already passed through land to
//reach. The coast facing the radar returns hardest and the land behind it
//fades within a couple of km, which is why a radar coastline is a bright
//edge and not a filled shape.
float visibility( float r, float theta )
{
	if( IsEffect == 1 )
		return 1.0;
	vec2 dir   = vec2( sine( theta ), cosine( theta ) );
	float land = 0.0;
	for( int j = 0; j < 32; ++j )
		land += texture( MapTexture, 0.5 + 0.5 * dir * r * ( float( j ) + 0.5 ) / 32.0 ).g;
	return exp( -land * r / 32.0 * ShadowScale );
}

float surfaceAt( float r, float theta )
{
	vec2 u = r * vec2( sine( theta ), cosine( theta ) );
	if( IsEffect == 0 )
		return texture( MapTexture, 0.5 + 0.5 * u ).r;

	//Over: the clip IS the surface. North is up, bearings run clockwise.
	vec2 px = ClipCentre + vec2( MirrorForTest * u.x, u.y ) * ClipRadius;
	if( px.x < 0.0 || px.y < 0.0 || px.x >= ClipRaster.x || px.y >= ClipRaster.y )
		return 0.0;
	//The brightest channel, not the luma: a saturated blue ring is as
	//reflective as a white one (luma would give it 0.07 and drop it under any
	//threshold). Times alpha, so a transparent area reflects nothing.
	vec4 c      = texture( MapTexture, px / ClipRaster * MaxUV );
	float value = max( c.r, max( c.g, c.b ) ) * c.a;
	return clamp( ( value - Threshold ) / max( 1.0 - Threshold, 1e-3 ), 0.0, 1.0 );
}

//Whether sprite i's mask is over Sprite Threshold at u (scope units).
bool spriteAt( int i, vec2 u )
{
	vec4 box = SpriteBox[ i ];
	vec2 up  = SpriteTurn[ i ].xy;
	vec2 d   = u - box.xy;
	//Into the sprite's own frame: right is ( cos psi, -sin psi ), up ( sin psi, cos psi ).
	vec2 st = vec2( d.x * up.y - d.y * up.x, dot( d, up ) ) / ( 2.0 * box.zw ) + 0.5;
	if( st.x < 0.0 || st.y < 0.0 || st.x > 1.0 || st.y > 1.0 )
		return false;
	return texture( SpriteMask, st ).r > SpriteThreshold;
}

//Whether sprite i's bounding circle is within reach of the footprint
//[r - L, r] at bearing theta: one test, before any tap is taken.
bool spriteReaches( int i, float r, float theta )
{
	vec4 reach = SpriteReach[ i ];
	if( r < reach.x - reach.y || r - PulseExtent > reach.x + reach.y )
		return false;
	float off = theta - reach.z;
	off -= TAU * floor( off / TAU + 0.5 );
	return abs( off ) <= reach.w;
}

//The sprites' mean over the pulse's footprint [r - L, r] at bearing theta.
//Overlapping sprites are one target, not two: the union at each tap, not
//the sum. Most texels of the band are in reach of no sprite and cost a test
//per sprite; the taps are taken only where one can be hit.
float spritesOver( float r, float theta, int taps )
{
	int first = -1;
	bool more = false;
	for( int i = 0; i < SpriteCount; ++i )
		if( spriteReaches( i, r, theta ) )
		{
			if( first < 0 )
				first = i;
			else
				more = true;
		}
	if( first < 0 )
		return 0.0;
	vec2 dir  = vec2( sine( theta ), cosine( theta ) );
	float sum = 0.0;
	for( int j = 0; j < taps; ++j )
	{
		float rj = r - PulseExtent * ( float( j ) + 0.5 ) / float( taps );
		if( rj <= 0.0 )
			continue;
		vec2 u   = rj * dir;
		bool hit = spriteAt( first, u );
		if( more )
			for( int i = first + 1; i < SpriteCount && !hit; ++i )
				hit = spriteReaches( i, r, theta ) && spriteAt( i, u );
		if( hit )
			sum += 1.0;
	}
	return sum / float( taps );
}

void main()
{
	ivec2 t       = ivec2( gl_FragCoord.xy );
	float theta   = ( float( t.x ) + 0.5 ) * TAU / float( Bearings );
	float r       = ( float( t.y ) + 0.5 ) / float( Ranges );
	int taps      = clamp( int( ceil( PulseExtent * float( Ranges ) ) ) + 1, 1, 24 );
	float surface = 0.0;
	if( SurfaceOn == 1 )
	{
		float sum = 0.0;
		for( int j = 0; j < taps; ++j )
		{
			float rj = r - PulseExtent * ( float( j ) + 0.5 ) / float( taps );
			if( rj > 0.0 )
				sum += surfaceAt( rj, theta );
		}
		surface = sum / float( taps ) * visibility( r, theta );
	}
	FragColor = vec4( surface, SpriteCount > 0 ? spritesOver( r, theta, taps ) : 0.0, 0.0, 1.0 );
}
)";

//===========================================================================
// 3. Paint. Only the bins the antenna crossed this frame (the wedge: FirstCol
// and Count, integers, from the CPU's double-precision antenna), drawn into a
// scratch buffer and copied back over the phosphor's same columns.
//
// A texel holds its two components, (flash, afterglow), AS THEY WERE THE
// MOMENT ITS COLUMN WAS LAST PAINTED. Nothing decays on the GPU between
// paints: the CPU knows, in double, when every column was crossed, and hands
// the composite a per-column fade -- exp( -age / tau ) -- and this pass a
// per-column Carry, the decay from the column's last crossing to this one.
// So a paint is S' = S x Carry + echo x (Flash, 1), and the time between the
// crossing and the end of the frame is the composite's fade, not an age here.
//
// The texel is FOUR components: (flash, afterglow) of everything, then
// (flash, afterglow) of the sprites' share of it, decayed alike, so the
// composite can give the sprites their own colour (Sprite Hue, Saturation).
// The sprites' share of the video is the share of P they put in: the video
// is one tube's response, 1 - exp( -P ), and is split, never added twice.
//
// The echo, for the texel at antenna bearing phi and delay range r:
//
//   P = G [ r^(n-4) ((surface + sprites) (x) beam + clutter + rain) + R_t^(n-4) points + r^n noise ] + strobe
//
// n is the STC exponent, the 4 is the radar equation (RangeExponent, which
// only --r4's negative control ever changes). The point targets are painted
// analytically: sigma x beam( their bearing - phi ) x the fraction of this
// range bin their pulse, [R_t, R_t + L], covers. The video is 1 - exp( -P ).
//===========================================================================
const char* const kPaintFragment = R"(
uniform sampler2D Previous;
uniform sampler2D Reflect;
uniform sampler2D Carry;
uniform sampler2D Kernel;
uniform int KernelHalf;
uniform int SurfaceOn;///< the reflect band was written this frame (surface or sprites)
uniform int Bearings;
uniform int Ranges;

uniform int FirstCol;
uniform uint FirstSerial;

uniform vec2 Weights;///< (Flash, 1)
uniform int CountForTest;

uniform int Window;
uniform int Stride;

uniform float GainLinear;
uniform float Stc;
uniform float RangeExponent;
uniform float PulseExtent;
uniform vec4 Targets[ 32 ];
uniform int TargetCount;

uniform float ClutterLevel;
uniform float ClutterKm;
uniform float NoiseLevel;
uniform float RainLevel;
uniform float RangeKm;
uniform vec2 RainOffset;
uniform uint Seed;
uniform float StrobeLevel;
uniform float SpriteLevel;///< a sprite's reflectivity, against the coast's 0.25..1

out vec4 FragColor;

float rainAt( vec2 km )
{
	vec2 p  = ( km + RainOffset ) / 8.0;
	float n = 0.6 * valueNoise( p, Seed + 501u ) + 0.3 * valueNoise( p * 2.3, Seed + 502u )
	          + 0.1 * valueNoise( p * 5.1, Seed + 503u );
	return smoothstep( 0.60, 0.85, n );
}

//( the video, the sprites' share of it )
vec2 echo( ivec2 t, int offset )
{
	float phi  = ( float( t.x ) + 0.5 ) * TAU / float( Bearings );
	float r    = ( float( t.y ) + 0.5 ) / float( Ranges );
	float lo   = float( t.y ) / float( Ranges );
	float hi   = float( t.y + 1 ) / float( Ranges );
	float rMin = 0.5 / float( Ranges );
	float rr   = max( r, rMin );

	//The surface and the sprites, convolved in bearing with the beam,
	//normalised: a wider beam blurs the coast, it does not brighten it.
	vec2 acc   = vec2( 0.0 );
	float wsum = 0.0;
	if( SurfaceOn == 1 )
		for( int m = -Window; m <= Window; m += Stride )
		{
			int col = ( t.x + m + 4 * Bearings ) % Bearings;
			float w = texelFetch( Kernel, ivec2( m + KernelHalf, 0 ), 0 ).r;
			acc += w * texelFetch( Reflect, ivec2( col, t.y ), 0 ).rg;
			wsum += w;
		}
	float surface = wsum > 0.0 ? acc.x / wsum : 0.0;
	float sprite  = wsum > 0.0 ? acc.y / wsum * SpriteLevel : 0.0;

	//Point targets, analytic in both directions.
	float points = 0.0;
	for( int i = 0; i < TargetCount; ++i )
	{
		vec4 tg = Targets[ i ];
		float over = max( 0.0, min( hi, tg.y + PulseExtent ) - max( lo, tg.y ) ) * float( Ranges );
		if( over <= 0.0 )
			continue;
		float d = tg.x - phi;
		d -= TAU * floor( d / TAU + 0.5 );
		points += tg.z * beam( d ) * over * pow( max( tg.y, rMin ), Stc - RangeExponent );
	}

	uint serial = FirstSerial + uint( offset );
	uint cell   = pcg( serial ^ pcg( uint( t.y ) * 2654435761u + Seed ) );
	float km    = r * RangeKm;

	//Sea clutter: area-extensive (the cell grows as r) and falling with range
	//in km, speckled pulse to pulse.
	float clutter = ClutterLevel > 0.0 ? ClutterLevel * rr * exp( -km / ClutterKm ) * speckle( cell ) : 0.0;
	float rain    = 0.0;
	if( RainLevel > 0.0 )
		rain = RainLevel * rr * rr * rainAt( km * vec2( sine( phi ), cosine( phi ) ) ) * speckle( cell + 0x9e3779b9u );
	float noise = NoiseLevel > 0.0 ? NoiseLevel * speckle( cell + 0x3c6ef372u ) * pow( rr, Stc ) : 0.0;

	float law  = pow( rr, Stc - RangeExponent );
	float p    = GainLinear * ( law * ( surface + sprite + clutter + rain ) + points + noise );
	if( StrobeLevel > 0.0 )
		p += StrobeLevel * ( 0.4 + 0.6 * speckle( cell + 0xdaa66d2bu ) );
	//1 - exp( -p ), without the float cancellation: exp( -1e-5 ) is 0.99999 to
	//one float ulp (6e-8), which leaves a faint echo 0.6% of quantisation
	//noise. The series is exact to 4e-10 relative below 0.01.
	float video = p < 0.01 ? p * ( 1.0 - p * ( 0.5 - p * ( 1.0 / 6.0 ) ) ) : 1.0 - exp( -p );
	float mine  = GainLinear * law * sprite;
	return vec2( video, p > 0.0 && mine > 0.0 ? video * min( mine / p, 1.0 ) : 0.0 );
}

void main()
{
	ivec2 t    = ivec2( gl_FragCoord.xy );
	int offset = ( t.x - FirstCol + Bearings ) % Bearings;
	vec4 s     = texelFetch( Previous, t, 0 ) * texelFetch( Carry, ivec2( t.x, 0 ), 0 ).rgrg;
	if( CountForTest == 1 )
		s.xy += vec2( 1.0 );
	else
	{
		vec2 e = echo( t, offset );
		s.xy += e.x * Weights;
		s.zw += e.y * Weights;
	}
	//One NaN would survive every paint for the life of the instance
	//(vectrix's trap), so none gets in.
	if( isnan( s.x ) || isinf( s.x ) )
		s.x = 0.0;
	if( isnan( s.y ) || isinf( s.y ) )
		s.y = 0.0;
	if( isnan( s.z ) || isinf( s.z ) )
		s.z = 0.0;
	if( isnan( s.w ) || isinf( s.w ) )
		s.w = 0.0;
	FragColor = s;
}
)";

//===========================================================================
// 4. Composite: the polar phosphor onto the screen. Each pixel's range and
// bearing from the scope's centre (north up, clockwise) read the four
// nearest texels, each FADED by its own column's age -- by hand, because the
// fade jumps between two columns painted a rotation apart and a hardware
// bilinear of the unfaded values would smear last rotation's paint into
// this one's. The two components glow in their own colours (the sprites'
// share in the phosphor's colours turned by Sprite Hue and Saturation); the rings, the
// bearing marks and the heading line are drawn one pixel wide; the tube's
// response 1 - exp( -light ) saturates the bright ones. The source is
// transparent outside the scope; the Over effect mixes the scope with the clip.
//===========================================================================
const char* const kCompositeFragment = R"(
uniform sampler2D Phosphor;
uniform sampler2D Fade;
uniform sampler2D InputTexture;
uniform int Bearings;
uniform int Ranges;
uniform vec2 Raster;
uniform vec2 ViewOrigin;
uniform vec2 Centre;
uniform float RadiusPx;
uniform vec2 MaxUV;
uniform vec3 FlashColour;
uniform vec3 AfterColour;
uniform vec3 OverlayColour;
uniform vec3 FaceColour;
uniform float RingSpacing;
uniform float RingLevel;
uniform float MarkLevel;
uniform int HeadingLine;
uniform int IsEffect;
uniform float MixAmount;
uniform int PolarViewForTest;
uniform ivec2 PolarOrigin;
uniform int PolarSpritesForTest;///< the polar view shows the sprites' share, not the whole
uniform int SpriteTint;         ///< 0: the sprites are in the phosphor's own colours
uniform vec3 SpriteFlashColour;
uniform vec3 SpriteAfterColour;
uniform int SpriteNoFadeForTest;///< --sprite-beam's negative control: the share never fades
out vec4 FragColor;

//One texel, faded to now: (flash, afterglow, the sprites' flash, their
//afterglow). `col` may be up to one rotation either side.
vec4 glowAt( int col, int row )
{
	int c     = ( col + 2 * Bearings ) % Bearings;
	int r     = clamp( row, 0, Ranges - 1 );
	vec2 f    = texelFetch( Fade, ivec2( c, 0 ), 0 ).rg;
	vec4 fade = SpriteNoFadeForTest == 1 ? vec4( f, 1.0, 1.0 ) : f.xyxy;
	return texelFetch( Phosphor, ivec2( c, r ), 0 ) * fade;
}

float lineMask( float distancePx )
{
	return 1.0 - smoothstep( 0.35, 1.1, distancePx );
}

void main()
{
	vec2 p = gl_FragCoord.xy - ViewOrigin;
	if( PolarViewForTest == 1 )
	{
		//The harness's window onto the polar grid: output pixel = texel.
		ivec2 q   = ivec2( p ) + PolarOrigin;
		vec4 g    = glowAt( q.x, q.y );
		FragColor = vec4( PolarSpritesForTest == 1 ? g.zw : g.xy, 0.0, 1.0 );
		return;
	}
	vec2 u      = ( p - Centre ) / RadiusPx;
	float r     = length( u );
	float theta = r > 1e-7 ? bearingOf( u ) : 0.0;

	//Bin k's centre is at bearing ( k + 0.5 ) / Bearings of a turn, range bin
	//i's at ( i + 0.5 ) / Ranges.
	float x  = theta / TAU * float( Bearings ) - 0.5;
	float y  = r * float( Ranges ) - 0.5;
	float x0 = floor( x ), y0 = floor( y );
	vec2 f   = vec2( x - x0, y - y0 );
	int c0 = int( x0 ), r0 = int( y0 );
	vec4 s = mix( mix( glowAt( c0, r0 ), glowAt( c0 + 1, r0 ), f.x ), mix( glowAt( c0, r0 + 1 ), glowAt( c0 + 1, r0 + 1 ), f.x ), f.y );
	vec3 light = s.x * FlashColour + s.y * AfterColour;
	//The sprites' share, moved from the phosphor's colours to theirs.
	if( SpriteTint == 1 )
		light += s.z * ( SpriteFlashColour - FlashColour ) + s.w * ( SpriteAfterColour - AfterColour );

	//The graticule, in pixels.
	float overlay = 0.0;
	if( RingLevel > 0.0 )
	{
		float q    = r / RingSpacing;
		float ring = floor( q + 0.5 );
		float d    = abs( q - ring ) * RingSpacing * RadiusPx;
		if( ring >= 1.0 && ring * RingSpacing < 1.0 - 0.5 / RadiusPx )
			overlay += RingLevel * lineMask( d );
		overlay += RingLevel * lineMask( abs( 1.0 - r ) * RadiusPx );
	}
	if( MarkLevel > 0.0 && r > 0.9 && r <= 1.0 )
	{
		float deg   = degrees( theta ) + 360.0;
		float five  = floor( deg / 5.0 + 0.5 );
		float along = abs( deg - five * 5.0 ) * PI / 180.0 * r * RadiusPx;
		int i5      = int( five ) % 72;
		float len   = ( i5 % 6 == 0 ) ? 0.06 : ( ( i5 % 2 == 0 ) ? 0.035 : 0.018 );
		if( 1.0 - r < len )
			overlay += MarkLevel * lineMask( along );
	}
	if( HeadingLine == 1 && u.y > 0.0 && r < 1.0 )
		overlay += 0.8 * lineMask( abs( u.x ) * RadiusPx );

	float inside = clamp( ( 1.0 - r ) * RadiusPx + 0.5, 0.0, 1.0 );
	vec3 scope   = ( 1.0 - exp( -( light + FaceColour + overlay * OverlayColour ) ) ) * inside;

	if( IsEffect == 0 )
	{
		//Premultiplied: transparent outside the scope, so it composites round.
		FragColor = vec4( scope, inside );
		return;
	}
	//The clip's own texel, fetched, not filtered: a filtered read at a texel
	//centre is exact on a GPU and was not on the software renderer, and Mix 0
	//must hand the clip back bit for bit.
	vec4 clip = texelFetch( InputTexture, ivec2( p ), 0 );
	FragColor = vec4( mix( clip.rgb, scope, MixAmount ), mix( clip.a, 1.0, MixAmount ) );
}
)";

std::string Assemble( const char* a, const char* b )
{
	std::string s = kVersion;
	s += kCommon;
	for( const char* piece : { a, b } )
		if( piece )
			s += piece;
	return s;
}

} // namespace radar::shaders
