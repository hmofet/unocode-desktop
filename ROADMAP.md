# UnoCode Desktop: the road to VS Code parity

Ordered by what a **day-to-day user** hits first, not by what is architecturally
interesting. A task near the top is one that makes someone put the editor down
today; a task near the bottom is one they would miss in their second month.

Each task is sized to **one agent session**. If a task turns out to need more,
split it and say so here rather than growing it.

## Where things stand (2026-08-21)

**Tier 0 is finished.** UCD-01 through UCD-10 are done; UnoCode Desktop is a
daily driver on Windows, Linux and macOS. What exists:

- The unmodified core builds and runs on **Windows, Linux and macOS** (the mac
  build is a Universal Binary 2; both slices render byte-identical frames).
- `./build.sh --gate` builds, runs four test suites, renders the workbench
  headlessly and asserts a UTF-8 round trip through a real save. That is the
  check every task below must leave green. **Check its exit status, not its
  output**: a grep over the log will happily hide a stage that aborted.
- The editor is [benchmarked](BENCHMARK.md) against VS Code and wins on startup,
  memory, process count and disk by large multiples.

**Start with UCD-45.** Tier A was inserted ahead of Tier 1 on 2026-08-21: the
assistant is what the product is going to be shown doing, and UCD-44 has
already moved the editor into this repository so that work happens in one place
instead of two. UCD-11 waits - it buys long filenames on the desktop and
nothing at all on the device.

**The editor now lives here.** `core/` is canonical and UnoDOS vendors it;
`upstream/` is still pinned and read-only but is down to unoui, unojs and fb.
Read [core/README.md](core/README.md) before touching either side of that.

### What Tier 0 turned up

Three things worth knowing before Tier 1 starts, none of them tasks yet:

- **CJK and emoji draw as .notdef boxes.** UTF-8 works end to end - typed,
  stored, moved through by character, saved byte-exact, and laid out at the
  right width - but the four bundled faces carry no glyphs for those blocks.
  Latin-1, Greek, Cyrillic and box drawing all render. This is a
  font-SHIPPING decision (a CJK face is 5-20 MB, which is most of the "136x
  smaller on disk" the benchmark claims) and not a code defect. Whoever takes
  it should decide whether to ship one, subset one, or fall back to a system
  font.
- **Three Tier 0 tasks turned out to have upstream halves**, which the
  original entries did not mark: UCD-03 (the whole document model, column
  arithmetic and glyph cache), and UCD-06/07/08/09, which need queries only
  the subsystem can answer. Expect this: "host-side" is a guess until the
  seam is read.
- **UCD-04's 2x path is UNVERIFIED.** Every other Tier 0 acceptance criterion
  was exercised and watched: long names listed and saved back, the clipboard
  round-tripping through a real X clipboard, UTF-8 typed and re-read
  byte-exact, the native picker opening and re-rooting, the close prompt
  refusing to lose work, the pointer changing shape, geometry restoring to
  the pixel, settings landing in HOME and surviving a restart. HiDPI is the
  exception: neither machine available here has a 2x display, so what is
  confirmed is that the code computes 100% and changes nothing at 1x. The
  arithmetic and the resize path want a Retina Mac or a 150% Windows display
  before anyone calls that task closed.
- **`--type`, `--keys` and `--save`** now join `--shot` as the gate's hands.
  A screenshot can say the workbench painted; only typing, moving and saving
  can say that what went in came back out. Later tasks should use them rather
  than inventing another harness.

Two known issues that are deliberately **not** yet tasks, recorded so nobody
rediscovers them as bugs:

- **Idle CPU is ~1.2%** because the SDL event loop wakes every 15 ms whether or
  not anything happened. Deferred on purpose: the right time to tune the frame
  loop is after the feature set stops changing, since what the loop has to do
  per frame is exactly what is being changed by the work below.
- **A Developer ID certificate is missing.** The mac build signs with an Apple
  Development identity, which cannot be notarized or distributed. Obtaining one
  is an Apple Developer Program enrolment, not a code change (see UCD-32).

## How to use this list

Read [AGENTS.md](AGENTS.md) first: which machine builds what, how to run the
gate so it cannot lie to you, how to drive the GUI to verify something visual,
and the two-repo dance an `[UPSTREAM]` task needs.


- **Take the lowest unclaimed ID in the highest unfinished tier.** Tiers are
  strictly ordered; within a tier, order is a strong suggestion.
- **Claim by editing this file** (set `Status: claimed (<agent/date>)`) in its own
  commit, before you start.
- **`[UPSTREAM]` tasks do not belong to this repo.** Nothing under `upstream/`
  is ever patched here. They are changes to `hmofet/unodos`, filed and landed
  under that repo's `AGENTS.md` process, then consumed here by bumping the
  submodule. Doing one of these means working in two repos, in that order.
  **Since UCD-44 this is a much smaller set**: the editor itself is `core/` and
  ours, so only unoui, unojs and fb changes are `[UPSTREAM]` now.
- **Done means the gate is green**: `./build.sh --gate` still renders a
  workbench, and whatever check the task names below passes too.

Status values: `open`, `claimed (<who>, <date>)`, `done (<commit>)`.

---

# Tier 0: it is not a daily driver until these land  -  DONE

These were the reasons someone would try UnoCode Desktop for an hour and go
back to VS Code. All ten landed on 2026-08-21; the entries are kept for the
reasoning, and each records the commit that closed it.

### UCD-01: long filenames, via a FAT-style alias table
**Status:** done (28914dd) · **Size:** M

Today `host_fs.c` withholds any name longer than 15 bytes from listings, because
the core's listing seam hands out `char[16]` buffers. On a real project most
files are therefore **invisible**. This is the single biggest blocker.

Do it host-side first, the way FAT itself solved it: keep a per-directory table
mapping a generated short alias (`COMPON~1.TSX`) to the true name, hand the alias
across the seam, and resolve aliases back in `resolve()`. Every file becomes
openable and saveable immediately, with only the *display* truncated. UCD-11 then
removes the truncation properly.

- **Where:** `host/host_fs.c` (`list_into`, `resolve`, a new alias table).
- **Done when:** a directory containing `VeryLongComponentName.tsx` lists it,
  opens it, edits it and saves it back to the original filename; aliases are
  stable within a session; the table is bounded and its cap is logged, not
  silently hit.

### UCD-02: system clipboard
**Status:** done (c760b22) · **Size:** S

Ctrl+C / Ctrl+X / Ctrl+V move text inside the editor but do not reach the OS
clipboard, so you cannot paste a URL in or copy a snippet out. This is the most
frequently used feature in any editor.

- **Where:** `host/` (SDL_GetClipboardText / SDL_SetClipboardText); find where
  the core's copy/cut/paste commands land and give the host a seam to serve
  them. If the core has no such seam, that is a `[UPSTREAM]` sub-request, so
  check `uc_cmd.c` first and report which it is.
- **Done when:** copy from a browser pastes into UnoCode and vice versa, on both
  Windows and Linux; multi-line and empty clipboards behave.

### UCD-03: UTF-8 input and rendering
**Status:** done (77a4161; upstream af891f91) · **Size:** M

`on_text()` in `main.c` drops every byte >= 0x80, so accented characters, curly
quotes, box drawing and emoji cannot be typed, and a file containing them may
render wrongly. Users outside ASCII cannot use the editor at all.

Establish first whether the core's `ch` is a codepoint or a byte (read
`uc_edit_char` and the document model), then carry UTF-8 end to end: input,
storage, cursor movement by codepoint, and rasterisation (stb_truetype is
already codepoint-based).

- **Where:** `host/main.c` `on_text`; expect a `[UPSTREAM]` component in the
  document model. Report the split before writing code.
- **Done when:** a file of mixed Latin-1, CJK and emoji opens, renders,
  round-trips through a save unchanged, and arrow keys move by character.

### UCD-04: HiDPI / Retina rendering
**Status:** done (26ec1e6) · **Size:** M

`SDL_WINDOW_ALLOW_HIGHDPI` is set but the framebuffer is sized in **points**,
not pixels, so on any Retina or 150%-scaled display the editor is upscaled and
blurry. Every Mac is a Retina Mac; this blocks the mac build from looking
credible in a screenshot.

Size `fb` to the drawable size (`SDL_GetRendererOutputSize`), keep input in
points, and drive the UI scale (`uno_font_set_ui_scale`) from the ratio so text
is rendered at native resolution rather than magnified.

- **Where:** `host/main.c` (window/texture sizing, the resize path).
- **Done when:** on a 2x display, glyph edges are crisp at 1:1 and the UI is the
  same physical size as at 1x; dragging between a 1x and a 2x monitor re-lays out.

### UCD-05: settings live in the user's home, not in their project
**Status:** done (26ec1e6) · **Size:** S

`uno_fs_pref_vol()` returns volume 0 (the workspace), so `UNOCODE\SETTINGS.JSN`
is written **into the folder being edited**. That pollutes every repo it touches
and will end up in someone's pull request.

Point the config volume at `HOME` (already registered), keeping workspace-scoped
files (`TASKS.JSN`, `LAUNCH.JSN`) where they belong.

- **Where:** `host/host_fs.c` (`uno_fs_pref_vol`), `host/main.c` volume order.
  Check `uc_cfg.c` for how the config volume is chosen before moving anything.
- **Done when:** settings and keybindings persist across restarts, live under
  `%APPDATA%\UnoCode` / `~/.unocode`, and a freshly opened project gains no new
  files.

### UCD-06: open a folder and a file from the GUI
**Status:** done (26ec1e6; upstream 20eea5dc) · **Size:** M

The folder can only be chosen as a command-line argument. A desktop application
must have File > Open Folder, File > Open File, and a recent-folders list.

Use a native picker per platform (`IFileDialog` on Windows, `NSOpenPanel` on
mac, portal/zenity on Linux) rather than an in-app one, because users expect
their own file manager's places and bookmarks.

- **Where:** new `host/host_dialog_{win,mac,linux}.c`, wired to commands.
- **Done when:** all three platforms open a picker, changing folders re-roots the
  explorer without restarting, and the recent list survives a restart.

### UCD-07: window title, dirty state, and close confirmation
**Status:** done (26ec1e6; upstream 20eea5dc) · **Size:** S

The title is always "UnoCode": it never names the file or folder, never shows
the dirty dot, and closing the window discards unsaved work **silently**. That
last one loses data, which makes this a Tier 0 item rather than a polish one.

- **Where:** `host/main.c` (`SDL_SetWindowTitle`, the `SDL_QUIT` path).
- **Done when:** the title reads `file - folder`, gains a marker when dirty, and
  quitting with unsaved editors prompts save / discard / cancel.

### UCD-08: mouse cursor shapes
**Status:** done (26ec1e6; upstream 20eea5dc) · **Size:** S

The pointer is an arrow everywhere: over text, over the two splitters, over the
scrollbar. It reads as a toy immediately, and the splitters look non-draggable.

- **Where:** `host/main.c` (SDL_CreateSystemCursor, set from a hit test the core
  can already answer, or from the host's own region test).
- **Done when:** I-beam over text, resize arrows over both splitters, arrow
  elsewhere, updated as the pointer moves.

### UCD-09: remember window geometry and the open workspace
**Status:** done (26ec1e6; upstream 20eea5dc) · **Size:** S

Every launch is a 1280x800 window at the centre of the screen with nothing open.

- **Where:** `host/` (a small host-owned state file under `HOME`).
- **Done when:** size, position, maximised state, last folder and open tabs come
  back; a geometry that no longer fits any attached display is clamped rather
  than restored off-screen.

### UCD-10: smooth and precise scrolling
**Status:** done (26ec1e6) · **Size:** S

`SDL_MOUSEWHEEL` integer notches are translated 1:1, so trackpad scrolling is
steppy and fast wheel spins overshoot. Reading code is most of what an editor is
used for.

- **Where:** `host/main.c` (prefer `preciseY`, accumulate sub-notch deltas).
- **Done when:** two-finger scrolling on a trackpad is continuous, and a fast
  wheel does not jump pages.

---

# Tier A: the assistant  -  jumped the queue

This tier was inserted on 2026-08-21, ahead of Tier 1, because writing software
inside UnoDOS with an assistant is the thing the product is going to be shown
doing. Nothing in Tier 1 blocks it, and UCD-11 in particular buys long
filenames on the desktop and buys nothing on the device, where FAT is 8.3
regardless.

### What already exists, and changes the size of this work

**UnoDOS Studio already ships a working HTTPS chat client that talks to
api.anthropic.com**, and it is built entirely from kernel exports:
`pc64_net_up` + `net_dns_query` + `tls_connect_ca` + `tls_write`/`tls_read`,
with a hand-written HTTP/1.1 POST and a JSON extractor
(`upstream/unodos/pc64/apps/studio_ai.c`, `studio_json.c`, ~760 lines together).
It supports three providers, keeps keys in `AI.CFG`, attaches the current file,
pastes a code block at the caret, and has an **offline fake-provider test
server** at `pc64/tls_test/ai_server.py` that gates the whole transport with no
key and no internet.

So the transport question is answered and the answer is not `pc64_http.c`. Read
`studio_ai.c` before starting UCD-45; most of these tasks are lifting proven
code into the core and widening it, not writing a client from scratch.

Four things Studio's client does **not** do, which is most of what is left:

- It **blocks**. The request is issued in one frame after a "thinking..."
  repaint, and a big response stalls the UI. v1 accepted that; an editor
  cannot.
- It sends **only the latest user message**. The conversation buffer exists and
  is drawn, but `build_request()` puts one user turn in the body, so the model
  has no history. Fixed in Studio too, as part of UCD-46.
- It has **no tool use**, so it can write code into a reply but cannot read a
  file, write one, or run what it wrote. That is the whole difference between a
  chat pane and an assistant.
- It is a **built-in pane**, not an extension, so nothing about it is VS Code
  compatible and no third-party extension can reach the model.

The stale model string and the missing history are **defects in Studio, not
just gaps**, and they are fixed as UCD-46's `[UPSTREAM]` half rather than left
for someone to rediscover.

### UCD-44: `[DONE]` the core moves here, and this repo becomes its home
**Status:** done · **Size:** M

The editor was developed in `hmofet/unodos` and consumed here read-only. That is
now the other way round: `core/` is canonical, UnoDOS vendors it. See
[core/README.md](core/README.md), and `pc64/UNOCODE-UPSTREAM.md` +
`pc64/tools/sync_unocode.py` in that repo.

`unoui`, `unojs` and `fb` stay upstream and are still consumed read-only, so
nothing is canonical in both places.

- **Done when:** `./build.sh --gate` is green building from `core/`, the sync
  reproduces `pc64/unocode/` byte for byte with its banners, and pc64's own
  gate is green on the vendored copy.

### UCD-45: one net + TLS seam, implemented by both platforms
**Status:** claimed (2026-08-21) · **Size:** M · **Has an `[UPSTREAM]` half**

Declare the seam the core will reach the network through - link up, resolve,
CA-validated connect, read, write, close, plus an entropy-source query - as
`core/uc_net.h`. On pc64 it is a shim over kernel exports. On the desktop it is
new: `host/host_net.c`, over BearSSL (which upstream already vendors at
`pc64/bearssl`, MIT) and platform sockets.

Keep it a **socket** seam, not a request seam. `pc64_http.c` is the browser's
client, owned by the browser lane, and its POST is form-encoded with no way to
set a header - which is why Studio went around it rather than through it.

**Shape the seam like pc64's HANDLE API, not like the one Studio uses.**
`tls.h` has two surfaces over one engine: `tls_open`/`tls_poll`/`tls_send`/
`tls_recv`/`tls_free`, which are non-blocking and pumped from wherever you
already call `net_poll()`, and the legacy module-scoped `tls_connect`/
`tls_write`/`tls_read`/`tls_close`, which block. Studio uses the legacy one,
which is exactly why it stalls the frame. Copying that shape here would make
UCD-47 a rewrite of this task instead of a use of it.

**The `[UPSTREAM]` half, and it is in the file this repo's gate cannot see.**
The five handle calls are **not in `kExports`** - `pc64_modload.c` exports
`net_poll`, `pc64_net_up`, `net_dns_query` and the four legacy `tls_*`, and
nothing else. UNOCODE.UNO is a module, so today it can only reach the blocking
API. Add `tls_open`, `tls_poll`, `tls_send`, `tls_recv`, `tls_free`,
`tls_conn_error` and `tls_open_error` to that list in `hmofet/unodos`, and
**run `pc64/tools/gate.sh` there** - a one-line kernel export is the exact
change that gated green here and did not compile in pc64 on 2026-08-21.

- **Done when:** the same core code reaches api.anthropic.com from pc64 and
  from all three desktop targets; certificate validation is not disableable
  from a config file on any of them; and the fake provider in
  `upstream/unodos/pc64/tls_test/ai_server.py` serves the desktop build too.

### UCD-46: an HTTP + JSON client in the core, and Studio's two defects
**Status:** open · **Size:** M · **Depends:** UCD-45 · **Has an `[UPSTREAM]` half**

Lift `studio_ai.c`'s request construction and `studio_json.c`'s extractor into
`core/uc_http.c`, against `uc_json.c` (which already parses JSONC and is
already gated by `core/tools/test.sh`). Then widen it past what Studio needed:
arbitrary headers, a body of any size, multi-turn message history, and
**incremental SSE decode** so a stream can be delivered as it arrives.

Streaming is the part with no precedent to copy. Studio de-chunks in place
after the whole response has landed; an SSE stream is chunked *and* still
arriving, so the decoder has to hand out events mid-transfer.
`pc64_http.c`'s progress callback is no help either: it is explicitly not
offered for chunked responses.

**The `[UPSTREAM]` half: fix Studio while you are reading it.** Two defects are
in `pc64/apps/studio_ai.c` in `hmofet/unodos`, which is that repo's own file
and not vendored, so they are commits there under its `AGENTS.md` process.
Whoever lifts this code is the person who has just understood it, which is why
they are attached to this task rather than left as a separate one.

- **The default model is `claude-sonnet-4-5`, two families stale.** Current ids
  are `claude-opus-5`, `claude-sonnet-5`, `claude-fable-5` and
  `claude-haiku-4-5-20251001` (that one carries a date suffix; the others do
  not). Default to `claude-sonnet-5` and leave Opus 5 selectable with `/model`.
  Check the ids rather than copying them from here - this list ages.
- **Only the latest user turn is sent.** `build_request()` puts one user message
  in the body. The conversation buffer exists, is bounded, drops the oldest turn
  and is drawn on screen, so the pane *looks* like it has a memory and the model
  has never seen one. Feed `msg[]` through as a real `messages` array. Both
  other providers need the same, in their own shapes.

**Do not try to make Studio and UnoCode share one compiled client in this
task.** They are separate `.UNO` modules, so sharing means either compiling the
same file into both or adding a kernel export, and the vendored core is not
reachable from the `apps/` lane today. Fix Studio in place; the sharing question
is worth revisiting once the seam is real, and the one thing to preserve for it
is that **`uc_http.c` must not include `unocode.h`** - it should need only
`uc_net.h` and `uc_json.c`, which is what would make it liftable later.

- **Done when:** a POST with custom headers and a 100 KB body round-trips, an
  SSE stream is delivered event by event rather than at the end, both are gated
  offline against `ai_server.py` with no key and no internet, and Studio holds a
  real multi-turn conversation against a current model with `tools/json_test.c`
  still green.

### UCD-47: the request must not stop the frame
**Status:** open · **Size:** M · **Depends:** UCD-46

Studio's `do_request()` runs to completion inside one frame. Drive the exchange
from the frame loop instead, as a state machine the host pumps, so the editor
stays live while a generation runs.

This is deliberately **not** UCD-21. A real event loop in unojs is still wanted
and still Tier 2; what this task needs is narrower, and the existing thenable
plus a pump between frames carries it.

- **Done when:** a 20-second generation leaves the workbench scrolling, typing
  and repainting; the request can be cancelled; and closing the view or the
  window mid-flight tears the connection down instead of leaking it.

### UCD-48: `SecretStorage`, and a key that is not sitting in a settings file
**Status:** open · **Size:** S

Studio keeps API keys in `AI.CFG` in plaintext and says so. Offer VS Code's
`SecretStorage` shape, backed by DPAPI on Windows and the Keychain on macOS.

On Linux and on pc64 there may be nothing better than a file with tight
permissions - in which case **say that in the UI** rather than implying a
secret store that is not there. An honest plaintext caveat is what Studio did
right.

- **Done when:** a key set in the UI survives a restart, never appears in
  `SETTINGS.JSN`, and the storage in use is named on screen.

### UCD-49: the assistant view
**Status:** open · **Size:** L · **Depends:** UCD-47

A native side-bar view and panel: a scrolling transcript, streamed text
appearing as it arrives, code blocks in the editor's own grammars, and a
proposed edit shown as a diff you can apply or reject.

Native because **webviews are out of scope and staying out of scope** - the
whole value here is not shipping a browser. Everything this needs, unoui and
the existing document model already draw.

- **Done when:** a generation streams into the transcript without stutter, a
  code block is syntax-coloured, and an applied edit is one undo step.

### UCD-50: a model API extensions can call
**Status:** open · **Size:** M · **Depends:** UCD-46, UCD-47

Expose the client to the extension host in `vscode.lm`'s shape, so the
assistant is an **extension** rather than a built-in pane and a third-party
extension can use the model too.

Gate it on a permission declared in `PACKAGE.JSN`. An extension host that can
reach the network is an extension host that can exfiltrate a workspace, and
`EXT\` is a folder anyone can drop a file into.

- **Done when:** an extension sends a prompt and receives a stream, an
  extension without the permission is refused with a message that says why,
  and `UNOCODE.md` documents the API and its deviations.

### UCD-51: the assistant extension, with tools
**Status:** open · **Size:** L · **Depends:** UCD-49, UCD-50

`EXT\ASSIST\`: a CommonJS extension against `vscode` plus UCD-50, with tools
for read file, write file, list directory, and run. The loop that turns a chat
pane into something that writes software.

Studio's system prompt is a good starting point and is already correct about
this platform - UnoC's subset, `uno_app_main`, the Python `uno.App` shape, the
`Canvas` methods. Take it and add the tools.

**Run** is where the two platforms differ, and the device is the easier one:
`pc64_shell_run_user()` already launches a Python app, and the terminal already
calls it, so an edit-run-read-output loop works on UnoDOS today. On the desktop
the same call is an honest refusal until **UCD-14** lands, so ship the desktop
build with the run tool absent rather than present and broken - offer only what
the platform can do.

- **Done when:** on pc64, the assistant writes a Python app, runs it, reads the
  failure, fixes it and runs it again, with each file write shown as a diff
  first; and on the desktop the same session works with the run tool absent.

---

# Tier 1: the editing features people reach for every hour

### UCD-11: `[UPSTREAM]` widen the filename seam
**Status:** open · **Size:** L

Replace `char (*names)[16]` in the listing seam with a width the platform picks
(`UC_NAME_MAX`, 256), so long names stop being an alias trick. Touches
`uno_fs_list_dir`, `uc_list_dir`, the explorer, tabs, breadcrumbs and quick open.
Land it in unodos, then bump the submodule here and delete UCD-01's alias table.

- **Done when:** the alias table is gone, names round-trip verbatim, and pc64
  still builds and passes its own gate (the FAT backend keeps 8.3 internally).

**Read [AGENTS.md](AGENTS.md) §4.1 before you start this one.** `uno_fs_list_dir`
is touched by three `.c` files, and **this repo's gate compiles exactly one of
them**: `uc_main.c`. The other two - `pc64_fs.c`, which DEFINES it, and
`pc64_modload.c`, which exports it - are kernel-side and are never built here at
any warning level. Changing that signature will therefore build clean, gate
green and run correctly in UnoCode Desktop while leaving pc64 unable to compile.
That already happened once on a one-line export (unodos `029a4f17`); this task
changes a signature used across the kernel, so it will happen harder. Run
`cd upstream/unodos/pc64 && sh tools/gate.sh` before you push upstream, not
after.

### UCD-12: workspace-wide search
**Status:** open · **Size:** M

Find in the open file exists; find across the folder does not. It is one of the
top three things anyone does in a codebase.

The pieces are all present: a file walker, the core's budgeted regex engine, and
a spare side-bar view slot.

- **Done when:** a query lists hits grouped by file with line context, clicking a
  hit opens it at that line, the walk skips ignored directories, and a huge tree
  stays responsive (report progress, allow cancel).

### UCD-13: workspace-wide replace
**Status:** open · **Size:** M · **Depends:** UCD-12

Preview per hit, replace one / all in a file / all everywhere, and a single undo
step per file.

### UCD-14: real processes in the terminal
**Status:** open · **Size:** L

`pc64_shell_run_user` currently refuses honestly. The terminal has builtins but
cannot run `git status`, a compiler, or a test suite, so the panel is decorative.

Spawn through a PTY (ConPTY on Windows, `forkpty` elsewhere), stream output into
the existing terminal buffer, forward keystrokes and Ctrl+C, and report exit
codes. This also unblocks Tier 2 and Tier 3, which both need child processes.

- **Done when:** an interactive shell runs, `git log` pages, a long build streams
  without blocking the UI, and closing the panel kills the child.

### UCD-15: tasks run real commands
**Status:** open · **Size:** S · **Depends:** UCD-14

`TASKS.JSN` is parsed in VS Code's shape already. Route it through UCD-14 and
surface failures with the failing line.

### UCD-16: multi-cursor and column selection
**Status:** open · **Size:** L

Alt+click, Ctrl+Alt+Up/Down, and "select next occurrence" (Ctrl+D). Once someone
has this in their fingers, an editor without it feels broken.

- **Done when:** every editing operation (typing, delete, indent, paste) applies
  at all cursors, and one undo step reverses the whole multi-edit.

### UCD-17: go to line, and go to symbol in file
**Status:** open · **Size:** S

Ctrl+G and Ctrl+Shift+O. The quick-open widget already exists; these are two more
modes on it, and the symbol list can come from the existing grammar's scopes
until UCD-22 replaces it with real symbols.

### UCD-18: split editors
**Status:** open · **Size:** L

Side-by-side editing of two files, or two views of one file. Common enough that
its absence is noticed on day one, expensive enough that it sits below the cheap
wins.

### UCD-19: drag and drop files onto the window
**Status:** open · **Size:** S

`SDL_DROPFILE`. Dropping a file opens it; dropping a folder opens the workspace.

### UCD-20: auto-detect indentation and line endings
**Status:** open · **Size:** S

Opening a tabs file in a spaces editor and saving it rewrites the whole file,
which shows up as a catastrophic diff. Detect per file, show it in the status
bar, and preserve on save.

---

# Tier 2: code intelligence

This is the largest single block of what people mean by "VS Code", and it is
mostly one dependency chain. UCD-21 is the foundation for all of it.

### UCD-21: real Promises and an event loop in unojs `[UPSTREAM]`
**Status:** open · **Size:** L

`showQuickPick`/`showInputBox` return thenables because there is no microtask
queue; real extensions are `async`/`await` throughout, and an LSP client needs
somewhere for responses to land. Add a microtask queue and a host-pumped event
loop, then `Promise`, `async`/`await` and `Promise.all`.

- **Done when:** unojs's own test suite passes, an `async` extension activates,
  and the documented "deliberate deviation #1" is deleted from `UNOCODE.md`.

### UCD-22: LSP transport and lifecycle
**Status:** open · **Size:** L · **Depends:** UCD-14, UCD-21

Spawn a language server, frame JSON-RPC over stdio, handle initialize/shutdown,
map documents to URIs, and send did-open/did-change/did-save. No UI yet: this
task ends with a server running and a log of real traffic.

- **Done when:** clangd and pyright both start, initialize and stay alive across
  edits, and a crashed server is restarted with a bounded backoff.

### UCD-23: diagnostics
**Status:** open · **Size:** M · **Depends:** UCD-22

Squiggles in the editor, marks in the minimap and scrollbar, a Problems panel,
and counts in the status bar (the status bar already reserves `E 0  W 0`).

### UCD-24: completions from the language server
**Status:** open · **Size:** M · **Depends:** UCD-22

Replace the word-based suggestions with real ones: detail, kind icons,
documentation, and insert text. The suggest widget already exists.

### UCD-25: hover
**Status:** open · **Size:** S · **Depends:** UCD-22

### UCD-26: go to definition, and find all references
**Status:** open · **Size:** M · **Depends:** UCD-22

Includes the navigation stack (Alt+Left / Alt+Right), which is the part people
actually feel.

### UCD-27: rename symbol, and format document
**Status:** open · **Size:** M · **Depends:** UCD-22

Format on save is the setting most people turn on first.

### UCD-28: `[UPSTREAM]` TextMate fidelity
**Status:** open · **Size:** L

The regex engine rejects lookaround and backreferences, and cross-line state is
one open rule rather than a stack, so real published grammars (TypeScript, C++)
either fail to load or mis-colour. Until this lands, "any VS Code grammar works"
is true only of simple ones.

---

# Tier 3: source control

### UCD-29: git gutter marks
**Status:** open · **Size:** M · **Depends:** UCD-14

Added / modified / deleted marks against HEAD, in the gutter and the minimap. An
editor without these reads as dated immediately.

### UCD-30: diff view
**Status:** open · **Size:** L · **Depends:** UCD-18, UCD-29

Side-by-side and inline, with per-hunk stage and revert.

### UCD-31: SCM panel
**Status:** open · **Size:** M · **Depends:** UCD-29

Changed files, stage/unstage, commit with a message, push/pull, current branch in
the status bar.

---

# Tier 4: shipping it as a product

These do not change what the editor can do, and every one of them blocks release
on at least one store.

### UCD-32: macOS app bundle, notarization and hardened runtime
**Status:** partly done (UB2 builds; see `build-mac.sh`) · **Size:** M

A signed `.app`, a notarized and stapled `.dmg`, and the hardened runtime with
whatever entitlements the file-access story needs. Mac App Store additionally
needs sandboxing, which changes how folders are opened (security-scoped
bookmarks) and therefore interacts with UCD-06 and UCD-09.

### UCD-33: MSIX package for the Microsoft Store
**Status:** open · **Size:** M

Package identity, a signed MSIX, and store metadata. Note that a packaged app has
a virtualised writable location, which interacts with UCD-05.

### UCD-34: Linux packaging
**Status:** open · **Size:** M

A `.deb` and an `.rpm` with a desktop entry, icon, MIME associations, and an
AppImage or Flatpak for distributions we do not target directly.

### UCD-35: automatic updates
**Status:** open · **Size:** M

Store builds update themselves; the direct downloads need their own channel.
Signature verification is not optional.

### UCD-36: crash reporting and anonymous diagnostics
**Status:** open · **Size:** M

Opt-in, with the privacy story written down before the code is written.

### UCD-37: accessibility
**Status:** open · **Size:** L

Screen-reader output, full keyboard reachability, high-contrast themes and
respect for the OS reduced-motion setting. Because everything is drawn into a
framebuffer, there is no accessibility tree for free: one has to be published
per platform. This is a legal requirement for public-sector sales, and it is
large, so it needs starting well before it is urgent.

---

# Tier 5: the long tail

### UCD-38: debugging (DAP)
**Status:** open · **Size:** L · **Depends:** UCD-22

`LAUNCH.JSN` already exists in VS Code's shape, and DAP is JSON-RPC like LSP, so
the transport is reused. The cost is UI: breakpoints, variables, call stack,
watch, and stepping state.

### UCD-39: settings and keybindings editors
**Status:** open · **Size:** M

Both are editable as JSON today. A searchable UI over the existing `kDefs` table
is what makes them discoverable.

### UCD-40: extension install from a folder or archive
**Status:** open · **Size:** M

Extensions load from `EXT\`, but there is no way to install one from the UI. A
real marketplace is out of scope; installing a downloaded extension is not.

### UCD-41: file watching
**Status:** open · **Size:** M

Reload on external change, and refresh the explorer when git checks out a branch
underneath it.

### UCD-42: workspace trust
**Status:** open · **Size:** S · **Depends:** UCD-14

Opening a folder that can run tasks and language servers is an execution
decision. VS Code learned this the hard way; we should not have to.

### UCD-43: `[UPSTREAM]` spell the app-descriptor section per target
**Status:** open · **Size:** S

`UNO_APP_DESC` hardcodes `section(".unodesc")`, which Mach-O rejects outright
(Apple's assembler wants `__SEGMENT,__section`), so macOS could not compile
`uc_main.c` at all. The desktop works around it with `host/compat/uno_appdesc.h`,
which shadows the macro to nothing.

Spell the section per target inside `uno_appdesc.h` so the freestanding builds
keep their descriptor block and hosted builds need no shim, then delete
`host/compat/uno_appdesc.h` here.

- **Done when:** the shim is gone, macOS still builds, and pc64's loader still
  finds descriptors in a `.UNO`.

---

## Explicitly out of scope

Not oversights. Chasing these means re-deriving Electron:

- **Webviews and an HTML-rendering extension surface.** The whole value here is
  not shipping a browser.
- **Remote development** (SSH / containers / WSL).
- **A marketplace.** Compatible file formats, yes; hosting and ranking, no.
- **Settings sync as a hosted service.**
