#!/bin/sh
# UnoCode Desktop, macOS. Produces a UNIVERSAL BINARY 2 (arm64 + x86_64) and
# wraps it in a .app bundle.
#
#   ./build-mac.sh              -> build/mac/UnoCode.app  (unsigned)
#   ./build-mac.sh --sign       -> the same, code-signed (needs a GUI-session
#                                  login keychain: run it through mba's build
#                                  daemon, not a bare ssh - see CLAUDE.md)
#
# SDL2 comes from the OFFICIAL macOS framework, which upstream ships as a fat
# binary containing both slices. Homebrew's SDL2 is single-architecture and
# therefore cannot produce a UB2 - do not substitute it.
set -e
cd "$(dirname "$0")"

U=upstream/unodos
SDL_VER="${SDL_VER:-2.30.9}"
SDL_DIR="${SDL_DIR:-$HOME/.cache/unocode-desktop/sdl}"
FW="$SDL_DIR/SDL2.framework"
APP=build/mac/UnoCode.app

# ---- which identity can this machine actually sign with? -------------------
# Distribution needs a "Developer ID Application" certificate, which is an
# Apple Developer Program enrolment, not something a build script can conjure.
# Rather than failing the build on a machine that only has a development cert,
# pick the best available and SAY which one, so nobody mistakes a locally
# signed build for a distributable one.
uc_sign_identity() {
    if [ -n "$SIGN_ID" ]; then echo "$SIGN_ID"; return; fi
    for want in "Developer ID Application" "Apple Distribution" \
                "3rd Party Mac Developer Application" "Apple Development"; do
        if security find-identity -v -p codesigning 2>/dev/null | grep -q "$want"; then
            security find-identity -v -p codesigning | grep "$want" | head -1 \
                | sed 's/.*"\(.*\)"/\1/'
            return
        fi
    done
    echo "-"                       # ad-hoc: runs locally, distributes nowhere
}

uc_sign_note() {
    case "$1" in
      "Developer ID"*) echo "note: Developer ID - can be notarized and distributed directly." ;;
      "Apple Distribution"*|"3rd Party"*) echo "note: store identity - for App Store submission, not direct download." ;;
      "Apple Development"*) echo "note: DEVELOPMENT identity. Runs on this machine and registered devices. NOT distributable: notarization needs a Developer ID Application certificate (see ROADMAP UCD-32)." ;;
      "-") echo "note: AD-HOC signature. Local use only." ;;
    esac
}

# ---- SDL2.framework (universal, cached) ------------------------------------
if [ ! -d "$FW" ]; then
    mkdir -p "$SDL_DIR"
    echo "fetching SDL2 $SDL_VER framework..."
    curl -sSL -o "$SDL_DIR/sdl.dmg" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.dmg"
    MP=$(hdiutil attach -nobrowse -readonly "$SDL_DIR/sdl.dmg" | awk '/\/Volumes\//{print $NF; exit}')
    cp -R "$MP/SDL2.framework" "$SDL_DIR/"
    hdiutil detach -quiet "$MP"
fi
# refuse to build a "universal" binary against a thin dependency
if ! lipo -archs "$FW/Versions/A/SDL2" | grep -q arm64 || \
   ! lipo -archs "$FW/Versions/A/SDL2" | grep -q x86_64; then
    echo "SDL2.framework is not universal: $(lipo -archs "$FW/Versions/A/SDL2")" >&2
    exit 1
fi

# ---- sources ---------------------------------------------------------------
# sources.sh, shared with build.sh.  This block used to be a COPY of that one,
# under a comment claiming they were identical; they were not, and the way that
# surfaced was a wall of undefined symbols from ld, on another machine, long
# after the Linux and Windows builds had gone green.
. "$(dirname "$0")/sources.sh"

if [ ! -f "$U/pc64/build/font_data.h" ]; then
    ( cd "$U/ps2" && python3 mkfont_c.py )
    mkdir -p "$U/pc64/build"
    cp "$U/ps2/build/font_data.h" "$U/pc64/build/font_data.h"
fi

# ---- the bundle ------------------------------------------------------------
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"

# One clang invocation, two -arch flags: clang builds both slices and lipos them
# together itself, which is what makes this a Universal Binary 2 rather than two
# binaries in a trench coat.
#
# That is also why BearSSL is compiled here as SOURCE rather than linked from
# the cached archive build.sh keeps: an archive holds one architecture, and
# this command is producing two at once.  The build METHOD differs from
# build.sh; the source LIST does not, because both take $BSSL from sources.sh.
# That is the split sources.sh exists to enforce - the lists must not diverge,
# how each script consumes them may.
#
# $WARN is deliberately not applied to $BSSL either, and cannot be here: it is
# one invocation, so BearSSL rides on our warning flags.  It is warning-clean
# at -Wall on both slices today; if that ever stops being true, split this the
# way build.sh does rather than weakening $WARN for our own code.
# shellcheck disable=SC2086
clang -O2 -g -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
      $WARN $DEFS $INC -F"$SDL_DIR" -I"$FW/Headers" \
      $HOST $UC $UNOUI $UNOJS $FB $TLS $BSSL \
      -o "$APP/Contents/MacOS/UnoCode" \
      -F"$SDL_DIR" -framework SDL2 \
      -framework AppKit -framework Foundation -lobjc \
      -Wl,-rpath,@executable_path/../Frameworks -lm

cp -R "$FW" "$APP/Contents/Frameworks/"

# resources: the four TTFs under the names pc64_font looks for, + sample exts
R="$APP/Contents/Resources/res"
mkdir -p "$R"
cp "$U/pc64/fonts/Sans.ttf"       "$R/SANS.TTF"
cp "$U/pc64/fonts/Mono.ttf"       "$R/MONO.TTF"
cp "$U/pc64/fonts/Ubuntu.ttf"     "$R/UBUNTU.TTF"
cp "$U/pc64/fonts/ChiKareGo2.ttf" "$R/CHICAGO.TTF"
rm -rf "$R/EXT" && mkdir -p "$R/EXT"
cp -r core/ext/. "$R/EXT/"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key>              <string>UnoCode</string>
  <key>CFBundleDisplayName</key>       <string>UnoCode</string>
  <key>CFBundleIdentifier</key>        <string>com.arinbakht.unocode</string>
  <key>CFBundleVersion</key>           <string>0.1.0</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>CFBundleExecutable</key>        <string>UnoCode</string>
  <key>LSMinimumSystemVersion</key>    <string>11.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
</dict></plist>
PLIST

# ---- verify ----------------------------------------------------------------
echo "archs: $(lipo -archs "$APP/Contents/MacOS/UnoCode")"
lipo -archs "$APP/Contents/MacOS/UnoCode" | grep -q arm64
lipo -archs "$APP/Contents/MacOS/UnoCode" | grep -q x86_64

if [ "${1:-}" = "--sign" ]; then
    ID=$(uc_sign_identity)
    echo "signing as: $ID"
    if [ "$ID" = "-" ]; then
        codesign --force --deep --sign - "$APP"
    else
        # --options runtime (hardened) is only meaningful for notarized
        # distribution, and only a Developer ID identity can be notarized.
        case "$ID" in
            "Developer ID"*) codesign --force --deep --options runtime \
                                      --timestamp --sign "$ID" "$APP" ;;
            *)               codesign --force --deep --sign "$ID" "$APP" ;;
        esac
    fi
    codesign --verify --strict --verbose=2 "$APP"
    uc_sign_note "$ID"
fi

echo "built: $APP"
