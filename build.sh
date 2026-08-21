#!/bin/sh
# UnoCode Desktop build. Compiles the editor core in core/ - which this repo is
# the home of - plus the host shim in host/, against SDL2.  The core's three
# foundations (unoui, unojs, fb) still come from the pinned upstream/unodos
# submodule, which is consumed read-only.
#
#   ./build.sh              native build           -> build/unocode
#   ./build.sh --gate       core tests + native build + the headless render gate
#   ./build.sh --windows    mingw cross build      -> build/win/unocode.exe
#                           (SDL2_MINGW points at an extracted SDL2-devel-
#                            x.y.z-mingw tree; default /work/unodesk/SDL2-2.30.9)
#
# upstream/ is consumed read-only; if this script ever needs to patch a file
# under it, the change belongs in hmofet/unodos as a commit there (AGENTS.md
# section 4).  core/ is ours and is edited normally.
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
    cp -r core/ext/. "$out/EXT/"
}

# ---- BearSSL, compiled once and kept -----------------------------------------
# 286 files of third-party C that never change between our builds.  Putting
# them on the main compile line would add roughly a minute to EVERY build, so
# they become an archive that is rebuilt only when it is missing.  Delete
# build/bearssl*.a to force one.
#
# Compiled WITHOUT $WARN: it is upstream's code, not ours, and turning our
# warnings-as-errors on somebody else's library only teaches us to ignore
# them.  pc64 makes the same call for the same files.
bearssl_archive() {
    _out="$1"; _cc="$2"; _ar="$3"
    [ -f "$_out" ] && return 0
    echo "compiling BearSSL (once; delete $_out to redo)..."
    _od="${_out%.a}.objs"
    rm -rf "$_od" && mkdir -p "$_od"
    # shellcheck disable=SC2086
    for _c in $BSSL; do
        $_cc -O2 -I"$U/pc64/bearssl/inc" -I"$U/pc64/bearssl/src" \
             -c -o "$_od/$(basename "$_c" .c).o" "$_c"
    done
    $_ar rcs "$_out" "$_od"/*.o
    rm -rf "$_od"
}

# ---- native ----------------------------------------------------------------
build_native() {
    mkdir -p build
    # shellcheck disable=SC2086
    bearssl_archive build/bearssl.a "$CC" "${AR:-ar}"
    $CC -O2 -g $WARN $DEFS $INC $(sdl2-config --cflags) \
        $HOST $UC $UNOUI $UNOJS $FB $TLS \
        -o build/unocode $(sdl2-config --libs) build/bearssl.a -lm $NETLIBS $PTYLIBS
    stage_res build/res
    echo "built: build/unocode"
}

# ---- windows cross ---------------------------------------------------------
build_windows() {
    SDL2_MINGW="${SDL2_MINGW:-/work/unodesk/SDL2-2.30.9}"
    T="$SDL2_MINGW/x86_64-w64-mingw32"
    mkdir -p build/win
    # shellcheck disable=SC2086
    bearssl_archive build/bearssl-win.a x86_64-w64-mingw32-gcc \
        x86_64-w64-mingw32-ar
    x86_64-w64-mingw32-gcc -O2 -g $WARN $DEFS $INC \
        -I"$T/include/SDL2" -Dmain=SDL_main \
        $HOST $UC $UNOUI $UNOJS $FB $TLS \
        -o build/win/unocode.exe build/bearssl-win.a \
        -L"$T/lib" -lmingw32 -lSDL2main -lSDL2 -mwindows -lm -lole32 -luuid \
        -lws2_32 -lbcrypt -lcrypt32
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
# host_fs.c's contract, driven directly: the WIDENED listing seam (UCD-11).
# It has to be tested at the seam, not through a screenshot - what a listing
# does with a name too long for the caller's slot is invisible from outside.
fs_test() {
    mkdir -p build
    rm -rf build/fs_test_ws
    $CC -O1 -g $WARN tools/fs_test.c host/host_fs.c -o build/fs_test
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

# The CORE's own tests: the JSONC parser and the regex engine, which every
# theme, keybinding, snippet, manifest and grammar in the product is read by.
# They live in core/tools because the core is canonical here (core/README.md),
# and they run FIRST because a broken parser is not worth linking a binary for.
core_test() {
    sh core/tools/test.sh
}

# host_net.c's contract (UCD-45), including the check that only a real server
# can make: that an untrusted certificate is REFUSED.  It needs openssl and
# python3 to stand one up, and skips rather than fails without them.
net_test() {
    sh tools/net_test.sh
}

# uc_http.c's framing (UCD-46): status lines, headers, chunked decoding and
# SSE assembly, driven ONE BYTE AT A TIME straight into the state machine.
#
# There is no local server, and that is a consequence rather than a gap: the
# seam validates every certificate against the bundled roots with no way to
# turn it off, so a throwaway server is correctly unreachable.  Framing is pure
# logic and this tests it harder than a cooperative server could.  The socket
# path is covered by net_test's live check and by `http_test --live`.
#
# It #includes uc_http.c to reach feed(), so uc_http.c must NOT also be on the
# command line - the seam under test is internal.
http_test() {
    mkdir -p build
    # shellcheck disable=SC2086
    $CC -O1 -g $WARN $DEFS $INC tools/http_test.c \
        core/uc_json.c core/uc_util.c host/host_net.c $TLS \
        -o build/http_test build/bearssl.a -lm $NETLIBS
    ./build/http_test
}

# uc_secret.h against the real platform store (UCD-48).  On this build host
# that is the 0600 file; the test asserts the MODE, because "only this user
# can read it" is permission bits and permission bits are checkable.
secret_test() {
    mkdir -p build
    rm -rf build/secret_ws && mkdir -p build/secret_ws
    # shellcheck disable=SC2086
    $CC -O1 -g $WARN -Icore -Ihost tools/secret_test.c host/host_secret.c \
        -o build/secret_test
    ./build/secret_test build/secret_ws
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
    --test)    core_test; utf8_test; fs_test; clip_test; dialog_test
               secret_test ;;
    --gate)    core_test; build_native; utf8_test; fs_test; clip_test
               dialog_test; secret_test; net_test; http_test; gate; utf8_gate ;;
    *)         build_native ;;
esac
