#!/bin/sh
# Signed macOS deliverable, for mba's build daemon (see CLAUDE.md).
#
#   ssh mba build unocode-desktop --github
#
# Run it through the daemon, not a bare ssh: codesign needs mba's GUI-session
# login keychain, which an ssh session cannot reach ("no identity found").
set -e
cd "$(dirname "$0")/../.."          # repo root

# The daemon's fresh checkout may not have brought the submodule; the core
# lives there, so make sure of it rather than failing three minutes in.
git submodule update --init --recursive

./build-mac.sh --sign

VER=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" \
      build/mac/UnoCode.app/Contents/Info.plist)
OUT="packaging/mac/UnoCode-$VER.dmg"
rm -f "$OUT"

STAGE=$(mktemp -d)
cp -R build/mac/UnoCode.app "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "UnoCode $VER" -srcfolder "$STAGE" \
               -ov -format UDZO "$OUT"
rm -rf "$STAGE"

# Sign the dmg with the same identity build-mac.sh chose for the .app: the best
# one this machine has. See build-mac.sh's uc_sign_identity for why this is a
# search rather than a constant.
ID="$SIGN_ID"
if [ -z "$ID" ]; then
    for want in "Developer ID Application" "Apple Distribution" \
                "3rd Party Mac Developer Application" "Apple Development"; do
        found=$(security find-identity -v -p codesigning 2>/dev/null \
                | grep "$want" | head -1 | sed 's/.*"\(.*\)"/\1/')
        if [ -n "$found" ]; then ID="$found"; break; fi
    done
fi
[ -n "$ID" ] || ID="-"
codesign --force --sign "$ID" "$OUT"
codesign --verify --strict --verbose=2 "$OUT"
echo "built: $OUT"
echo "archs: $(lipo -archs build/mac/UnoCode.app/Contents/MacOS/UnoCode)"
