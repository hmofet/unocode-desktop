#!/bin/sh
# UnoCode Desktop build. Compiles the editor core in core/ - which this repo is
# the home of - plus the host shim in host/, against SDL2.  The core's three
# foundations (unoui, unojs, fb) still come from the pinned upstream/unodos
# submodule, which is consumed read-only.
#
#   ./build.sh              native build           -> build/unocode
#   ./build.sh --gate       core tests + native build + the headless render gate
#   ./build.sh --lsp        just the language-server test (needs clangd)
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

# ---- the LSP client (UCD-22) ------------------------------------------------
# Against a REAL server, because a cooperative fake would get right by
# construction the three things that are actually hard: a frame split across
# reads, a request the server sends back and blocks on, and a death.  clangd is
# the one asserted on because it is packaged everywhere; pyright is exercised
# too when it happens to be installed.
#
# SKIPPED, loudly, where no server is present.  A language-server test that
# passed because it never started one would be worse than not having it.
lsp_test() {
    if ! command -v clangd >/dev/null 2>&1; then
        echo "lsp_test: SKIPPED - no clangd on PATH" >&2
        return 0
    fi
    rm -rf build/lsp_ws && mkdir -p build/lsp_ws
    # A file with a real error in it, and an ASTRAL character before the error
    # on its own line.  The emoji is the whole point of the second line: it is
    # one code point, TWO UTF-16 units, two cells and four bytes, so the four
    # ways this codebase counts a column all give different answers there and a
    # conversion done in the wrong unit stops being invisible.
    $PY - build/lsp_ws/MAIN.C <<'EOF'
import io, sys
E = "\U0001F642"
io.open(sys.argv[1], "w", encoding="utf-8").write(
    "int main(void) {\n"
    "    char *s = \"" + E + E + "\"; int y = bad_name;\n"
    "    return 0\n"
    "}\n")
EOF
    ( cd build/lsp_ws && ../../build/unocode --shot ../lsp.ppm \
        --open MAIN.C --type '// edited' --save --lsp 6000 . ) \
        > build/lsp.log 2>&1 || true
    $PY - build/lsp.log build/lsp.ppm <<'EOF'
import sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()

def want(needle, why):
    assert needle in log, "%s (looked for %r)" % (why, needle)

# --- UCD-22: the transport ------------------------------------------------
want('state=ready',
     "clangd never reached ready - it did not start, or never answered initialize")
want('"method":"initialize"',       "the client never sent initialize")
want('"result":{"capabilities"',    "clangd never returned its capabilities")
want('"method":"initialized"',      "the client never confirmed initialization")
want('"method":"textDocument/didOpen"',   "the open document was never sent")
want('"method":"textDocument/didChange"', "the edit was never sent")
want('"method":"textDocument/didSave"',   "the save was never sent")
# The point of full sync: the change carries the TEXT, and it is the EDITED
# text.  A didChange that shipped the pre-edit buffer satisfies every
# assertion above and is useless.
assert '// edited' in log.split('didChange', 1)[1][:4000], \
    "didChange did not carry the text that was typed"
want('publishDiagnostics', "clangd never diagnosed the file - check the URI")

# --- UCD-23: what reached the editor --------------------------------------
probs = [l for l in log.split('\n') if l.startswith('lsp# ')]
assert probs, "clangd diagnosed the file but nothing reached the Problems model"

# THE COLUMN UNIT.  "bad_name" is the 29th CHARACTER of line 2 and also the
# 31st UTF-16 unit and the 35th byte, so this one number distinguishes a
# correct conversion from both plausible wrong ones.  Written out rather than
# computed, because a test that derives the expected value the same way the
# code does agrees with the code and not with reality.
bad = [p for p in probs if 'bad_name' in p]
assert bad, "the undeclared identifier was not reported: %r" % probs
where = bad[0].split()[1]                      # MAIN.C:2:29-37
assert where.endswith(':2:29-37'), (
    "wrong column for an error after an astral character: %s\n"
    "  29 = characters (right), 31 = UTF-16 units, 35 = bytes" % where)

# --- UCD-23: what reached the screen --------------------------------------
raw = open(sys.argv[2], 'rb').read()
f, i = [], 2
while len(f) < 3:
    while raw[i:i+1].isspace():
        i += 1
    j = i
    while not raw[j:j+1].isspace():
        j += 1
    f.append(int(raw[i:j]))
    i = j
i += 1
w, h, _ = f
px = raw[i:]
ERR = (244, 71, 71)                            # UC_C_ERROR_FG, dark default
rows = {}
for y in range(h):
    xs = [x for x in range(w)
          if (px[(y*w+x)*3], px[(y*w+x)*3+1], px[(y*w+x)*3+2]) == ERR]
    if xs:
        rows[y] = (len(xs), min(xs), max(xs))
assert rows, "nothing on screen is error-coloured: no squiggle, no marks"
# A squiggle is a WIDE run somewhere in the middle of the window; the overview
# ruler is a narrow run pinned to the right edge. Both, or the feature is half
# drawn and a screenshot that merely has red in it would not say so.
squig = [y for y, (n, lo, hi) in rows.items() if n >= 8 and hi < w - 40]
ruler = [y for y, (n, lo, hi) in rows.items() if lo > w - 20]
assert squig, "no squiggle in the text: %r" % sorted(rows.items())[:6]
assert ruler, "no marks on the overview ruler: %r" % sorted(rows.items())[:6]
print("lsp: clangd initialized, opened, saw an edit; %d problem(s) placed at "
      "CHARACTER columns, squiggled on %d row(s), %d ruler mark row(s)"
      % (len(probs), len(squig), len(ruler)))
EOF
}


# ---- completions from a language server (UCD-24) ----------------------------
# A second run, on its own fixture: the question here is what is IN the list and
# in what order, which no screenshot can answer, so the editor prints the list
# and this reads it.
#
# The fixture is a struct member access, because it is the one completion whose
# answer the word scraper CANNOT fake: `px`, `py` and `weight` are the fields of
# the type of `p`, and knowing that requires having parsed the file.
sug_test() {
    if ! command -v clangd >/dev/null 2>&1; then
        echo "sug_test: SKIPPED - no clangd on PATH" >&2
        return 0
    fi
    rm -rf build/sug_ws && mkdir -p build/sug_ws
    cat > build/sug_ws/SUG.C <<'CEOF'
struct Point { int px; int py; double weight; };
static int helper_fn(int a) { return a; }
int main(void) {
    struct Point p;
    p.
    return helper_fn(0);
}
CEOF
    # five Downs and an End puts the caret just after the `.` on line 5
    ( cd build/sug_ws && ../../build/unocode --shot ../sug.ppm \
        --open SUG.C --keys 'DDDDE' --lsp 6000 --suggest . ) \
        > build/sug.log 2>&1 || true
    $PY - build/sug.log <<'EOF'
import sys
log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
rows = [l for l in log.split('\n') if l.startswith('sug# ')]
head = [l for l in log.split('\n') if l.startswith('sug: ')]
assert head, "the suggestion widget never reported: %r" % log[-400:]
assert 'source=server' in head[0], (
    "the completions came from the WORD SCRAPER, not from clangd: %s\n"
    "  (a local list here would contain px/py/weight too, scraped from the "
    "struct declaration three lines up - which is exactly why the source "
    "matters and the labels alone would not have caught it)" % head[0])

labels = [r[5:].split()[0] for r in rows]
for want in ('px', 'py', 'weight'):
    assert want in labels, "clangd did not offer %r: %r" % (want, labels)
# ONLY the members. A word-scraped list would also carry `helper_fn`, `main`,
# `Point` and `struct`, so their absence is the proof that the answer came from
# something that understands the language.
for unwanted in ('helper_fn', 'main', 'Point', 'struct'):
    assert unwanted not in labels, (
        "%r is not a member of Point but was offered: %r" % (unwanted, labels))
# The server's ORDER, not the fuzzy matcher's. clangd returns declaration
# order here; a re-sort by prefix score would be free to reorder them, and the
# prefix is empty so every score would tie.
assert labels[:3] == ['px', 'py', 'weight'], \
    "the server's order was not preserved: %r" % labels
# Kind and detail, both of which the word scraper has no way to know.
assert 'property' in rows[0], "field kind was lost: %r" % rows[0]
assert rows[0].rstrip().endswith('int'), \
    "the field's type did not come through as detail: %r" % rows[0]
print("lsp: clangd completed a struct member - %d item(s), server order, "
      "kinds and types intact" % len(rows))
EOF
}


# ---- hover (UCD-25) ---------------------------------------------------------
# Four runs, because "a hover appeared" is three separate claims.
#
#   1. It says the right thing about a symbol.
#   2. It does NOT appear where there is nothing to say - a popup that opens
#      over whitespace is the behaviour that makes people turn hovers off - and
#      not at all for a file no server serves.
#   3. It is actually PAINTED. Every assertion in (1) reads a state flag and a
#      string, and a popup whose painter was clipped out of existence would
#      satisfy all of them. The control run is identical but for --hover, so
#      the pixels that differ between the two ARE the popup.
hov_test() {
    if ! command -v clangd >/dev/null 2>&1; then
        echo "hov_test: SKIPPED - no clangd on PATH" >&2
        return 0
    fi
    rm -rf build/hov_ws && mkdir -p build/hov_ws
    cat > build/hov_ws/HOV.C <<'CEOF'
/** Adds one to a number. */
static int helper_fn(int a) { return a + 1; }
int main(void) {
    return helper_fn(2);
}
CEOF
    printf 'alpha beta\n' > build/hov_ws/NOTES.TXT
    # three Downs, End, then ten Lefts lands the caret inside `helper_fn`
    ( cd build/hov_ws && ../../build/unocode --shot ../hov.ppm \
        --open HOV.C --keys 'DDDELLLLLLLLLL' --lsp 6000 --hover . ) \
        > build/hov.log 2>&1 || true
    # the CONTROL: same file, same keys, same waits, no --hover
    ( cd build/hov_ws && ../../build/unocode --shot ../hovctl.ppm \
        --open HOV.C --keys 'DDDELLLLLLLLLL' --lsp 6000 . ) \
        > build/hovctl.log 2>&1 || true
    # the caret left on a line with nothing to say about it
    ( cd build/hov_ws && ../../build/unocode --shot ../hov2.ppm \
        --open HOV.C --keys 'DDD' --lsp 6000 --hover . ) \
        > build/hov2.log 2>&1 || true
    # a file no server serves at all
    ( cd build/hov_ws && ../../build/unocode --shot ../hov3.ppm \
        --open NOTES.TXT --lsp 2000 --hover . ) \
        > build/hov3.log 2>&1 || true
    $PY - build/hov.log build/hov2.log build/hov3.log \
          build/hov.ppm build/hovctl.ppm <<'EOF'
import sys
on   = open(sys.argv[1], encoding='utf-8', errors='replace').read()
off  = open(sys.argv[2], encoding='utf-8', errors='replace').read()
none = open(sys.argv[3], encoding='utf-8', errors='replace').read()

assert 'hov: shown' in on, \
    "no hover over an identifier clangd knows: %s" % on[-500:]
body = '\n'.join(l[5:] for l in on.split('\n') if l.startswith('hov| '))
assert 'helper_fn' in body, "the hover did not name the symbol: %r" % body
assert 'int a' in body, "the signature did not come through: %r" % body
# The DOC COMMENT is the part that proves this came from something that parsed
# the file rather than from the identifier itself.
assert 'Adds one to a number' in body, \
    "clangd's doc comment was lost: %r" % body
# Markdown is stripped, not rendered. A fence left in would show as ``` on its
# own line, which is worse than plain text.
assert '```' not in body, "a markdown fence survived into the popup: %r" % body

assert 'hov: nothing' in off, \
    "a hover opened where there is no symbol - the dwell would fire on every " \
    "blank line the pointer crossed: %s" % off[-300:]
assert 'hov: nothing' in none, \
    "a hover opened for a file no language server serves: %s" % none[-300:]

def ppm(path):
    raw = open(path, 'rb').read()
    f, i = [], 2
    while len(f) < 3:
        while raw[i:i+1].isspace():
            i += 1
        j = i
        while not raw[j:j+1].isspace():
            j += 1
        f.append(int(raw[i:j]))
        i = j
    return f[0], f[1], raw[i+1:]

w, h, a = ppm(sys.argv[4])
w2, h2, b = ppm(sys.argv[5])
assert (w, h) == (w2, h2), "the two runs rendered at different sizes"
rows = {}
for y in range(h):
    xs = [x for x in range(w)
          if a[(y*w+x)*3:(y*w+x)*3+3] != b[(y*w+x)*3:(y*w+x)*3+3]]
    if xs:
        rows[y] = (len(xs), min(xs), max(xs))
assert rows, ("the hover reported itself shown but changed NOTHING on screen - "
              "the popup is not being painted")
ys = sorted(rows)
# One contiguous block, not scattered pixels: a popup is a box.
assert ys[-1] - ys[0] + 1 == len(ys), \
    "the difference is not one contiguous block: rows %r" % ys[:20]
box_h = len(ys)
box_w = max(r[2] for r in rows.values()) - min(r[1] for r in rows.values()) + 1
assert box_h >= 30 and box_w >= 120, \
    "the popup is too small to be the hover box: %dx%d" % (box_w, box_h)
print("lsp: clangd hovered a symbol (signature + doc comment, markdown "
      "stripped), painted a %dx%d popup, and stayed quiet on whitespace "
      "and on plaintext" % (box_w, box_h))
EOF
}

case "${1:-}" in
    --windows) build_windows ;;
    --test)    core_test; utf8_test; fs_test; clip_test; dialog_test
               secret_test ;;
    --lsp)     build_native; lsp_test; sug_test; hov_test ;;
    --gate)    core_test; build_native; utf8_test; fs_test; clip_test
               dialog_test; secret_test; net_test; http_test; gate; utf8_gate
               lsp_test; sug_test; hov_test ;;
    *)         build_native ;;
esac
