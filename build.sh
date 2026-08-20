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
# unocode core + its three foundations (unoui, unojs, fb+font), one theme for
# the window-metrics hook, and the host shim.
UC=$(ls $U/pc64/unocode/uc_*.c)
UNOUI="$U/unoui/unoui.c $U/unoui/unoui_input.c $U/unoui/unoui_anim.c \
       $U/unoui/unoui_wmanim.c $U/unoui/themes/theme_unodos.c"
UNOJS=$(ls $U/unojs/ujs_*.c)
FB="$U/pc64/fb.c $U/pc64/pc64_font.c"
HOST="host/main.c host/host_fs.c host/host_shell.c"

INC="-I$U/pc64 -I$U/pc64/unocode -I$U/unoui -I$U/unojs -Ihost"
DEFS="-DUNO_PC64"
WARN="-Wall -Wno-unused-parameter -Werror=implicit-function-declaration \
      -Werror=incompatible-pointer-types"

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
        -L"$T/lib" -lmingw32 -lSDL2main -lSDL2 -mwindows -lm
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

case "${1:-}" in
    --windows) build_windows ;;
    --gate)    build_native; gate ;;
    *)         build_native ;;
esac
