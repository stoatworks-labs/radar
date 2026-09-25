#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is universal on purpose. An arm64-only bundle builds and tests
# perfectly well here and then fails to load in an Intel Resolume, and the
# build log calls it a success either way -- so the architecture is checked
# with lipo, never with the log.
#
#     tools/verify.sh [BUILD_DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }

#---------------------------------------------------------------------------
# GLSL reserved words, declared as identifiers.
#
# glslc catches these, but glslc is optional -- so the trap most likely to be
# walked into gets its own grep that always runs. The list is GLSL 4.10's
# keywords reserved for future use (3.6) plus the ones already bitten:
# `packed` passed Apple's compiler and glslc and failed only Mesa (atrac).
# It looks for a declaration, not a mention, so prose may still use them.
#---------------------------------------------------------------------------
reserved_words() {
	local words="common partition active asm class union enum typedef template this resource goto inline noinline
	             public static extern external interface long short half fixed unsigned superp input output filter
	             sizeof cast namespace using packed sample patch layout flat smooth noperspective precise"
	local bad=0 word
	for word in $words; do
		if grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)]" \
		            source/Shaders.cpp >/dev/null 2>&1; then
			printf '   "%s" is declared as an identifier and is a GLSL reserved word\n' "$word"
			bad=$(( bad + 1 ))
		fi
	done
	[ "$bad" -eq 0 ] && printf '   none of the reserved words is used as an identifier\n'
	return "$bad"
}

step "GLSL reserved words"
reserved_words || fail "a GLSL reserved word is used as an identifier"

#---------------------------------------------------------------------------
step "GLSL: no sin, cos, tan or atan"
#---------------------------------------------------------------------------
# GLSL 4.10 promises no accuracy for them, and Apple's software renderer's
# sin widened the sinc^4 beam by 3%. The shaders use sine/cosine/bearingOf,
# built from the operations the spec bounds.
if grep -nE '(^|[^A-Za-z_])(sin|cos|tan|atan|asin|acos)[[:space:]]*\(' source/Shaders.cpp | grep -vE '^[0-9]+:[[:space:]]*//' ; then
	fail "an unbounded trig built-in is back in the GLSL"
fi
echo "   none"

#---------------------------------------------------------------------------
step "Shaders"
#---------------------------------------------------------------------------
tools/glslc.sh || fail "a shader does not compile"

#---------------------------------------------------------------------------
step "Browser demo's shaders"
#---------------------------------------------------------------------------
# demo/shaders.js carries copies of source/Shaders.cpp for the page at
# radar-demo.stoatworks-labs.com. A copy that drifts still paints a plausible
# scope, so the drift has to fail here instead: rerun
# `python3 demo/tools/check_shaders.py --write` after changing a shader. It
# checks the shaders only: the CPU half in demo/plugin.js (the sweep, the
# per-column timing, the sea, the conversions) is a hand port nothing but a
# reader checks.
if [[ -f demo/tools/check_shaders.py ]]; then
	log="$( mktemp )"
	if python3 demo/tools/check_shaders.py >"$log" 2>&1; then
		echo "   $( tail -1 "$log" )"
	else
		tail -14 "$log"
		fail "the demo's shaders have drifted from source/Shaders.cpp"
	fi
	rm -f "$log"
else
	echo "   skipped: no demo/"
fi

#---------------------------------------------------------------------------
step "Submodule"
#---------------------------------------------------------------------------
if [[ ! -f external/ffgl/CMakeLists.txt ]]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
fi
pin="$(git -C external/ffgl rev-parse --short=7 HEAD)"
[[ "$pin" == "b1afaf9" ]] || fail "FFGL SDK at $pin, not the fleet's b1afaf9"
echo "ok   FFGL SDK pinned at $pin"

#---------------------------------------------------------------------------
step "Build (universal)"
#---------------------------------------------------------------------------
log="$( mktemp )"
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >"$log" 2>&1 || { cat "$log"; fail "configure"; }
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >"$log" 2>&1 || { tail -40 "$log"; fail "build"; }
# The SDK's own FFGLUtilities.cpp warns about RAND_MAX; anything else is ours.
if grep -E 'warning:' "$log" | grep -v 'external/ffgl' | grep -q .; then
	grep -E 'warning:' "$log" | grep -v 'external/ffgl' | sed 's/^/   /'
	fail "a warning in this repo's code"
fi
rm -f "$log"
echo "ok   built, no warnings outside the SDK"
RATEST="$BUILD/ratest"

#---------------------------------------------------------------------------
step "Bundles"
#---------------------------------------------------------------------------
# Two bundles, one per registration: FFGL resolves one plugMain per binary.
check_bundle() {
	local bundle="$1" executable="$2" identifier="$3" id="$4" name="$5" kind="$6"
	local binary="$bundle/Contents/MacOS/$executable"
	[[ -f "$binary" ]] || fail "no binary at $binary"

	local arches
	arches="$(lipo -archs "$binary")"
	[[ "$arches" == *arm64* ]]  || fail "$executable: no arm64 slice (got: $arches)"
	[[ "$arches" == *x86_64* ]] || fail "$executable: no x86_64 slice (got: $arches)"

	# Captured, then matched from a herestring -- never `nm ... | grep -q`:
	# under pipefail the grep's early exit SIGPIPEs nm and fails the pipeline.
	local symbols
	symbols=$( nm -gU "$binary" 2>/dev/null || true )
	grep -q '_plugMain' <<<"$symbols" || fail "$executable: plugMain not exported"
	echo "ok   $executable: $arches, plugMain exported"

	local plist="$bundle/Contents/Info.plist"
	[[ -f "$plist" ]] || fail "no Info.plist in $bundle"
	read_plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || true; }
	local declared
	declared="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"
	[[ "$( read_plist CFBundleIdentifier )" == "$identifier" ]] || fail "$executable: bundle id is '$( read_plist CFBundleIdentifier )'"
	[[ "$( read_plist CFBundleExecutable )" == "$executable" ]] || fail "$executable: CFBundleExecutable is wrong"
	[[ "$( read_plist CFBundlePackageType )" == "BNDL" ]] || fail "$executable: CFBundlePackageType is not BNDL"
	[[ "$( read_plist CFBundleVersion )" == "$declared" ]] || fail "$executable: plist version != CMakeLists $declared"
	grep -q "versionFallback = \"v$declared\"" source/StoatworksAbout.h \
		|| fail "StoatworksAbout.h's versionFallback is not v$declared"
	echo "ok   $identifier, BNDL, v$declared -- plist, CMakeLists and About agree"

	codesign --force --sign - --timestamp=none "$bundle" >/dev/null 2>&1 || fail "$bundle could not be ad-hoc signed"
	codesign --verify --deep --strict "$bundle" >/dev/null 2>&1 || fail "$bundle's ad-hoc signature does not verify"
	echo "ok   ad-hoc signed and verified"

	# What a host reads: id, name, type. The FFGL name field is char[16] and
	# not null-terminated, so only something reading it back as a host does
	# notices a truncation.
	local OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
	if [[ -x "$OXBOW" ]]; then
		local probe
		probe="$( "$OXBOW" probe "$bundle" 2>&1 || true )"
		printf '%s\n' "$probe" | sed -n '1,3p' | sed 's/^/   /'
		grep -q "id:          $id" <<<"$probe" || fail "oxbow did not read the id $id"
		grep -q "name:        $name\$" <<<"$probe" || fail "oxbow did not read the name '$name'"
		grep -q "type:        $kind" <<<"$probe" || fail "oxbow did not read the type as $kind"
		local selftest
		selftest="$( "$OXBOW" selftest "$bundle" 2>&1 || true )"
		grep -q "selftest:    PASS" <<<"$selftest" || fail "oxbow selftest failed on $bundle"
		echo "ok   a host reads $name $id $kind, and oxbow selftest renders 120 frames through it"
	else
		echo "   skipped: no oxbow at $OXBOW (set OXBOW=...)"
	fi
}

check_bundle "$BUILD/Radar.bundle" "Radar" "com.stoatworks.ffgl.radar" RA01 "SW Radar" source
check_bundle "$BUILD/Radar Over.bundle" "Radar Over" "com.stoatworks.ffgl.radar.over" RA02 "SW Radar Over" effect

#---------------------------------------------------------------------------
step "Checks (this Mac's GPU, 1280x720 and 320x180)"
#---------------------------------------------------------------------------
# Every claim the README makes, in the order the README makes them.
CHECKS="arc pulse sweep persist r4 over-check prime resize clock state"
for check in $CHECKS sweep-law cues names; do
	if out=$( "$RATEST" --$check 2>&1 ); then
		printf '   ok   ratest --%s: %s\n' "$check" "$( printf '%s\n' "$out" | grep -c '^  ok' ) checks"
	else
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
		fail "ratest --$check"
	fi
done

#---------------------------------------------------------------------------
step "Software renderer (CI's, at 320x180)"
#---------------------------------------------------------------------------
# The same checks on Apple's software renderer, which is what a GPU-less CI
# runner falls back to. It is not repeatable at the last bit, and it is where
# a check that only holds on this GPU is found before CI finds it: it found
# that sin() there is not the GPU's (see AGENTS.md).
for check in $CHECKS; do
	if out=$( RATEST_RENDERER=software "$RATEST" --$check --size 320x180 2>&1 ); then
		printf '   ok   ratest --%s (software): %s\n' "$check" "$( printf '%s\n' "$out" | grep -c '^  ok' ) checks"
	else
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
		fail "ratest --$check on the software renderer -- run: RATEST_RENDERER=software $RATEST --$check --size 320x180"
	fi
done

#---------------------------------------------------------------------------
step "Offline (what CI runs)"
#---------------------------------------------------------------------------
"$RATEST" --offline >/dev/null || fail "ratest --offline"
echo "ok   ratest --offline"

#---------------------------------------------------------------------------
step "Pipe"
#---------------------------------------------------------------------------
# The fleet's --pipe frame format, which the video renders through. Two and a
# half frames in must be exactly two out and a clean exit -- a partial frame is
# the end of the stream, never a frame -- the source's --pipe --frames N must be
# N frames, a cue naming no parameter must be refused, and a reader that hangs
# up must end the run with exit 1, not SIGPIPE's silent 141, for the source's
# --pipe, the effect's --pipe and --film alike.
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp ); out=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
status=0
"$RATEST" --over --pipe --size 64x36 < "$raw" > "$out" 2>/dev/null || status=$?
got=$( wc -c < "$out" | tr -d ' ' )
[[ "$status" -eq 0 && "$got" == "$(( frame * 2 ))" ]] \
	|| fail "2.5 frames into the Over gave $got bytes out (want $(( frame * 2 ))), exit $status"
echo "ok   Over: 2.5 frames in, exactly 2 frames out, clean exit"
status=0
"$RATEST" --pipe --size 64x36 --frames 3 > "$out" 2>/dev/null || status=$?
got=$( wc -c < "$out" | tr -d ' ' )
[[ "$status" -eq 0 && "$got" == "$(( frame * 3 ))" ]] || fail "the source's --pipe --frames 3 gave $got bytes, exit $status"
echo "ok   source: --pipe --frames 3 is exactly 3 frames, clean exit"
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever ratest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
status=0
"$RATEST" --over --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1 || status=$?
[[ "$status" -eq 2 ]] || fail "a cue naming no parameter gave exit $status, not 2"
echo "ok   a cue naming no parameter is refused (exit 2)"
head -c $(( frame * 20 )) /dev/zero > "$raw"
set +e
"$RATEST" --pipe --size 64x36 2>/dev/null | head -c 1 >/dev/null
s1=${PIPESTATUS[0]}
"$RATEST" --over --pipe --size 64x36 < "$raw" 2>/dev/null | head -c 1 >/dev/null
s2=${PIPESTATUS[0]}
"$RATEST" --film 20 --size 64x36 2>/dev/null | head -c 1 >/dev/null
s3=${PIPESTATUS[0]}
set -e
[[ "$s1" -eq 1 && "$s2" -eq 1 && "$s3" -eq 1 ]] \
	|| fail "a closed stdout gave exit $s1 (source --pipe), $s2 (Over --pipe), $s3 (--film), not 1"
echo "ok   | head -c 1: exit 1 from the source's --pipe, the Over's --pipe and --film, not SIGPIPE's 141"
rm -f "$raw" "$cues" "$out"

#---------------------------------------------------------------------------
step "Negative controls"
#---------------------------------------------------------------------------
# Every check above, against a model that is deliberately wrong, required to
# FAIL. A check that cannot fail is not a check.
if out=$( "$RATEST" --negative 2>&1 ); then
	printf '%s\n' "$out" | grep -E '^negative controls' | sed 's/^/   /'
else
	printf '%s\n' "$out" | grep -E 'PASSED against' | sed 's/^/      /'
	fail "a negative control went undetected"
fi

#---------------------------------------------------------------------------
step "Mutants"
#---------------------------------------------------------------------------
tools/mutate.sh || fail "a mutant survived"

#---------------------------------------------------------------------------
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$BUILD" || fail "a dead control"

#---------------------------------------------------------------------------
step "Cost"
#---------------------------------------------------------------------------
"$RATEST" --bench

printf '\n\033[32mall green\033[0m\n'
