#!/bin/sh
# UnoCode Desktop build. Compiles the UNMODIFIED editor core out of the pinned
# upstream/unodos submodule plus the host shim in host/, against SDL2.
#
#   ./build.sh              native build           -> build/unocode
#   ./build.sh --gate       native build + the headless render gate
#   ./build.sh --windows    mingw cross build      -> build/win/unocode.exe
#                           (SDL2_MINGW points at an extracted SDL2-devel-
#                            x.y.z-mingw tree; default /work/unodesk/SDL2-2.30.9)
#
# The core is consumed read-only; if this script ever needs to patch a file
# under upstream/, the port has failed and the fix is an upstream request.
set -e
cd "$(dirname "$0")"

U=upstream/unodos
CC="${CC:-gcc}"
PY="${PY:-python3}"

# ---- the compile list ------------------------------------------------------
# In sources.sh, shared with build-mac.sh.  It used to be duplicated here and
# there; the copies drifted, and the mac build died at the linker.
. ./sources.sh

# fb.c's 8x8 bitmap font table is generated, not committed
if [ ! -f "$U/pc64/build/font_data.h" ]; then
    ( cd "$U/ps2" && $PY mkfont_c.py )
    mkdir -p "$U/pc64/build"
    cp "$U/ps2/build/font_data.h" "$U/pc64/build/font_data.h"
fi

# ---- resources -------------------------------------------------------------
# res/ ships beside the binary: the four TTFs under the names pc64_font looks
# for, and the sample extensions so the Extensions view has something to show.
stage_res() {
    out="$1"
    mkdir -p "$out"
    cp "$U/pc64/fonts/Sans.ttf"       "$out/SANS.TTF"
    cp "$U/pc64/fonts/Mono.ttf"       "$out/MONO.TTF"
    cp "$U/pc64/fonts/Ubuntu.ttf"     "$out/UBUNTU.TTF"
    cp "$U/pc64/fonts/ChiKareGo2.ttf" "$out/CHICAGO.TTF"
    rm -rf "$out/EXT"
    mkdir -p "$out/EXT"
    cp -r "$U/pc64/unocode/ext/." "$out/EXT/"
}

# ---- native ----------------------------------------------------------------
build_native() {
    mkdir -p build
    # shellcheck disable=SC2086
    $CC -O2 -g $WARN $DEFS $INC $(sdl2-config --cflags) \
        $HOST $UC $UNOUI $UNOJS $FB \
        -o build/unocode $(sdl2-config --libs) -lm
    stage_res build/res
    echo "built: build/unocode"
}

# ---- windows cross ---------------------------------------------------------
build_windows() {
    SDL2_MINGW="${SDL2_MINGW:-/work/unodesk/SDL2-2.30.9}"
    T="$SDL2_MINGW/x86_64-w64-mingw32"
    mkdir -p build/win
    # shellcheck disable=SC2086
    x86_64-w64-mingw32-gcc -O2 -g $WARN $DEFS $INC \
        -I"$T/include/SDL2" -Dmain=SDL_main \
        $HOST $UC $UNOUI $UNOJS $FB \
        -o build/win/unocode.exe \
        -L"$T/lib" -lmingw32 -lSDL2main -lSDL2 -mwindows -lm -lole32 -luuid
    cp "$T/../x86_64-w64-mingw32/bin/SDL2.dll" build/win/ 2>/dev/null || \
        cp "$T/bin/SDL2.dll" build/win/
    stage_res build/win/res
    echo "built: build/win/unocode.exe"
}

# ---- the gate --------------------------------------------------------------
# Headless render: boot the workbench against a scratch workspace and demand a
# plausibly-painted frame. Runs on a box with no display at all.
gate() {
    rm -rf build/gate && mkdir -p build/gate/ws
    printf 'int main(void) { return 0; }\n' > build/gate/ws/HELLO.C
    printf '# gate workspace\n'             > build/gate/ws/README.md
    ( cd build/gate/ws && ../../../build/unocode --shot ../shot.ppm . )
    $PY - build/gate/shot.ppm <<'EOF'
import sys
p = open(sys.argv[1], 'rb').read()
hdr = p.split(b'\n', 3)
w, h = map(int, hdr[1].split())
px = hdr[3]
colours = set()
for i in range(0, min(len(px), w * h * 3), 997 * 3):
    colours.add(px[i:i+3])
assert w >= 700 and h >= 460, (w, h)
assert len(colours) >= 8, "workbench painted fewer than 8 distinct colours: %d" % len(colours)
print("gate: %dx%d, %d sampled colours - looks like a workbench" % (w, h, len(colours)))
EOF
}

# ---- the seam test ---------------------------------------------------------
# host_fs.c's contract, driven directly: the long-name alias table (UCD-01)
# has to be tested at the seam, not through a screenshot.  Built with small
# caps so the full-table road is reached with eight names, not four thousand.
fs_test() {
    mkdir -p build
    rm -rf build/fs_test_ws
    $CC -O1 -g $WARN -DALIAS_MAX=8 -DALIAS_DIRS=8 \
        tools/fs_test.c host/host_fs.c -o build/fs_test
    ./build/fs_test build/fs_test_ws
}

# host_clip.c against a REAL clipboard.  SDL's clipboard is a video-subsystem
# service, so this one needs a display; on a headless box Xvfb is one, and
# without either the test says so and is skipped rather than passing silently.
clip_test() {
    mkdir -p build
    # shellcheck disable=SC2086
    $CC -O1 -g $WARN $INC $(sdl2-config --cflags) \
        tools/clip_test.c host/host_clip.c \
        -o build/clip_test $(sdl2-config --libs)
    if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then
        ./build/clip_test
    elif command -v xvfb-run >/dev/null 2>&1; then
        xvfb-run -a ./build/clip_test
    else
        echo "clip_test: SKIPPED - no display and no xvfb-run" >&2
    fi
}

# What happens AFTER the Open dialog closes (UCD-06).  The dialog is the OS's;
# the translation from an absolute path back to (volume, directory, name) is
# ours, and is the part that can be wrong quietly.
dialog_test() {
    mkdir -p build
    rm -rf build/dlg_ws
    # shellcheck disable=SC2086
    $CC -O1 -g $WARN $INC $(sdl2-config --cflags)         tools/dialog_test.c host/host_dialog.c host/host_fs.c         -o build/dialog_test $(sdl2-config --libs)
    ./build/dialog_test build/dlg_ws
}

# The UTF-8 decoder every text road now depends on (UCD-03).
utf8_test() {
    mkdir -p build
    # shellcheck disable=SC2086
    $CC -O1 -g $WARN -I$U/pc64 tools/utf8_test.c -o build/utf8_test
    ./build/utf8_test
}

# ---- the UTF-8 round trip --------------------------------------------------
# The one claim a screenshot cannot check: that what was typed reached the
# buffer and that saving put those same bytes back on disk.  --type and --save
# are the headless hands that make it checkable.
utf8_gate() {
    rm -rf build/u8 && mkdir -p build/u8/ws
    printf 'first line\n' > build/u8/ws/MIXED.TXT
    ( cd build/u8/ws && ../../../build/unocode \
        --open MIXED.TXT --type 'café 中文 🙂 ──' \
        --save --shot ../mixed.ppm . ) >/dev/null
    $PY - build/u8/ws/MIXED.TXT <<'EOF'
import sys
want = "café 中文 \U0001f642 ──"
raw = open(sys.argv[1], 'rb').read()
text = raw.decode('utf-8')                      # 1. it is valid UTF-8 at all
assert want in text, "typed text is not in the saved file: %r" % text
assert raw == text.encode('utf-8'), "bytes are not their own re-encoding"
print("utf8: typed %d characters (%d bytes), saved and re-read byte-exact"
      % (len(want), len(want.encode('utf-8'))))
EOF

    # And the half typing cannot reach: that a caret STEP is a character.
    # Type a line, walk Left over b, the emoji and the CJK glyph, then
    # Backspace.  Byte-stepping would leave a severed sequence behind, which
    # the decode() below refuses - the assertion is a decoder, not an eyeball.
    rm -rf build/u8/mv && mkdir -p build/u8/mv
    printf 'x\n' > build/u8/mv/MOVE.TXT
    ( cd build/u8/mv && ../../../build/unocode --open MOVE.TXT \
        --type 'aé中🙂b' --keys 'LLLB' --save --shot ../mv.ppm . ) >/dev/null
    $PY - build/u8/mv/MOVE.TXT <<'EOF'
import sys
raw = open(sys.argv[1], 'rb').read()
text = raw.decode('utf-8')            # a severed sequence dies right here
assert text.startswith('a中🙂bx'), \
    "Left x3 + Backspace removed the wrong thing: %r" % text
print("utf8: Left x3 then Backspace removed ONE whole character (%r)"
      % text.rstrip())
EOF
}

case "${1:-}" in
    --windows) build_windows ;;
    --test)    utf8_test; fs_test; clip_test; dialog_test ;;
    --gate)    build_native; utf8_test; fs_test; clip_test; dialog_test
               gate; utf8_gate ;;
    *)         build_native ;;
esac
