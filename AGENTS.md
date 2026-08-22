# AGENTS.md: working on UnoCode Desktop

Read this before you start. `ROADMAP.md` says WHAT to do next; this says HOW to
do it here, and records the things that have already cost somebody a cycle.

## TL;DR

1. **Take the lowest unclaimed ID in the highest unfinished tier** of
   `ROADMAP.md`. Claim it by editing that file, in its own commit, first.
2. **Never patch `upstream/`.** It is a pinned submodule of `hmofet/unodos`,
   consumed read-only. Core changes are commits in THAT repo (see §4).
3. **`./build.sh --gate` must be green when you finish** - and you must check
   its **exit status**, not its output (§3).
4. **Verify the thing you claimed, not the build.** A green gate says the code
   compiles; it does not say the feature works (§5).

## 1. Where the code is

```
core/           the EDITOR. Canonical here; UnoDOS vendors it. See
                core/README.md before you touch it or the sync.
  uc_*.c          workbench, document model, editor, terminal, themes,
                  settings, keybindings, grammars, extension host
  unocode.h       the private header
  UNOCODE.md      the contract: formats, the `vscode` subset, the deviations
  tools/          the core's own tests (JSONC parser, regex engine). Pure
                  logic, so they run on any build host in a second
  ext/            the sample extensions - the worked example of the manifest
host/           the SDL2 + OS shim. This is your code.
  main.c          window, event loop, input translation, HiDPI, headless modes
  host_fs.c       the uno_fs_* seam: volumes, case-insensitive paths.  The
                  long-name alias table it used to carry is GONE - UCD-11
                  widened the seam, which is what it was there to work around
  host_proc.c     child processes: forkpty / ConPTY (UCD-14)
  host_secret.c   the secret store: DPAPI / Keychain / a 0600 file (UCD-48)
  host_clip.c     the OS clipboard, mirrored onto the core's (UCD-02)
  host_win.c      title, pointer shapes, window geometry + session (07/08/09)
  host_dialog.c   what happens after the native Open dialog closes (UCD-06)
  host_pick_*.c   the dialogs themselves, per platform
  host_shell.c    the pc64_shell_* hooks the core expects (the eighth,
                  pc64_shell_pick, lives in host_dialog.c with its dialog)
  compat/         headers that shadow upstream ones via #include_next, ONLY
                  where there is no other way (see uno_appdesc.h)
tools/          the tests. One per seam, each linking the unit under test.
sources.sh      the compile list, shared by build.sh and build-mac.sh
upstream/unodos the editor core, PINNED. Read-only. See §4.
```

The core is consumed **unmodified**: ~23k lines of editor against ~1.5k of
host. If a task seems to need an edit under `upstream/`, it is an upstream
request, not a local patch.

## 2. Building

**Nothing builds on `amanuensis`** - no gcc, no SDL2, no mingw. Everything
builds on **`quill`** (`ssh quill`), which has gcc, SDL2, the mingw cross
toolchain, Xvfb and QEMU. `~/bin/ucsync` tars the working tree over and builds
it; it prints `BUILD-EXIT:` because a grep over the log will hide a stage that
aborted (§3).

```
./build.sh              native            -> build/unocode
./build.sh --gate       native + tests + the headless render check
./build.sh --test       the tests alone
./build.sh --windows    mingw cross       -> build/win/unocode.exe
./build-mac.sh          Universal Binary 2 -> build/mac/UnoCode.app
```

macOS builds on **`mba`**, which is a build SERVER, not a machine to ssh into
and run `./build-mac.sh` on: codesign needs a GUI-session login keychain that a
plain ssh cannot reach.

```
ssh mba build unocode-desktop <branch> --github -y
```

**Both platform pickers are in `sources.sh` unconditionally** and both compile
everywhere - each is wrapped in its own `#ifdef`, so the inapplicable one
produces an empty object. Do not make the source list platform-conditional:
that is a second place for the builds to disagree, which is the bug
`sources.sh` exists to prevent.

## 3. The gate, and how it lies to you

`./build.sh --gate` builds, runs four test suites, renders the workbench
headlessly, and asserts a UTF-8 round trip through a real save.

**Check its EXIT STATUS.** A `./build.sh --gate 2>&1 | grep -E "error|PASSED"`
looks thorough and is not: `set -e` aborts the script at the failing stage, the
stages that already passed still printed `PASSED`, and the grep shows you those.
That hid a startup crash for two builds on 2026-08-21.

The gate has **hands as well as eyes**. Use them rather than inventing another
harness:

```
unocode --shot <out.ppm>    render headlessly and exit
unocode --open <file>       open a file in a tab
unocode --type <utf8>       feed text in as typing
unocode --keys <LRUDHEBX>   arrows / Home / End / Backspace / Delete
unocode --save              save the active editor
```

**`--type` must run after a frame has painted.** `uc_edit_reveal()` scrolls the
caret into view against the editor's rect, and before the first paint that rect
is empty - so it concludes one column fits and scrolls the typed line off to
the left. The text is in the buffer and in the file, and simply not on screen,
which reads exactly like a rendering bug. `shot_mode()` paints once first.

## 4. Work that crosses into `hmofet/unodos`

Two different things go by this name now, and they run in opposite directions.

**Outbound - a core change reaching UnoDOS.** This is the common case and it
needs nothing of you here beyond a green gate: commit to `core/`, and the OS
picks it up at its next sync (`pc64/tools/sync_unocode.py` there). **A sync is
gated by pc64's gate, not by ours** - see �4.1, which is now the reason the
flip is safe rather than a caveat on it.

**Inbound - an `[UPSTREAM]` task.** Only unoui, unojs and fb changes are these
now, which is a much smaller set than before the flip: an event loop in unojs,
a toolkit widget, a rendering fix. Working on one means two repos, in this
order:

1. In `upstream/unodos`: branch off `master`, rebase onto it first, commit,
   **run its own gate** (§4.1), push.
2. Here: bump the submodule and commit that with the host-side half.

Read `upstream/unodos/AGENTS.md` before you commit there - it has its own
ownership registry and commit rules, and `master` must stay green.

### 4.1 Run the pc64 gate, not just this one

```
cd upstream/unodos/pc64 && QUICK=1 sh tools/gate.sh    # builds only
cd upstream/unodos/pc64 && sh tools/gate.sh            # + the QEMU suite
```

**This repo's gate cannot catch a pc64 break.** `sources.sh` compiles the
editor module and its foundations, not the kernel - so `pc64_modload.c`,
`pc64_uui.c` and everything else kernel-side is never touched here. Adding a
kernel export on 2026-08-21 built and gated perfectly green in this repo and
did not compile in pc64 at all.

**A modified `pc64/tls_test/pinned_key.h` is CORRECT state, not noise.** A
fresh clone regenerates the TLS test cert and re-pins it; reverting that breaks
the TLS gate with `BR_ERR 27`. Leave it uncommitted.

## 5. Verifying, when the thing you built is visual

A green gate is not a verified feature. Every Tier 0 task was watched doing the
thing it was written to do, and two real bugs were found that way that no test
had caught.

**On `amanuensis` you cannot drive the GUI.** The session is normally RDP, and
RDP blocks `SetForegroundWindow`, so synthetic keystrokes never reach the app.
You CAN screenshot it:

```
powershell -ExecutionPolicy Bypass -File "%USERPROFILE%\.claude\tools\cc-capture.ps1" -Out shot.png -Window UnoCode
```

**To drive it, use quill**: Xvfb + openbox + xdotool. openbox matters - without
a window manager `xdotool windowactivate` fails and modal dialogs misbehave.

```
Xvfb :99 -screen 0 1280x800x24 &   openbox &   export DISPLAY=:99
xdotool search --name UnoCode | head -1        # the window id
xdotool key --window $W --clearmodifiers ctrl+k
ffmpeg -f x11grab -draw_mouse 1 -video_size 1280x800 -i :99 -frames:v 1 s.png
```

`-draw_mouse 1` captures the actual **cursor shape**, which is how UCD-08 was
verified. This same setup drove Ctrl+K Ctrl+O to a real zenity dialog and
clicked Save in the close prompt.

## 6. Traps already paid for

- **`realpath()` into a caller's buffer needs `PATH_MAX`** (4 KB on glibc). A
  1 KB one aborts the process through `_FORTIFY`. Use the allocating form.
- **`uno_fs_pref_vol()` is also the core's initial WORKSPACE.** Moving it to
  HOME for UCD-05 silently moved the workspace off the command-line folder.
  `boot_app()` now states `uc_open_folder(0, "")` explicitly.
- **The core's clipboard is 32 KB and truncates.** Take the core-side signature
  AFTER `uc_clip_set`, or a 100 KB OS clipboard gets replaced by our stub.
- **`uno_app_main` and `pc64_shell_theme` have no prototypes** on the module
  side; an implicit declaration truncates the returned pointer to 32 bits and
  segfaults at boot. The build uses
  `-Werror=implicit-function-declaration` for exactly this.
- **Mach-O rejects `section(".unodesc")`.** Shimmed in
  `host/compat/uno_appdesc.h` on ALL platforms so they cannot diverge again;
  UCD-43 fixes it upstream.
- **Build scripts need the exec bit set in git** (`git update-index --chmod=+x`)
  or mba's fresh checkout fails `rc=126`. **CRLF shebangs** fail on Linux;
  `.gitattributes` pins `.sh/.c/.h/.py/.md` to LF.
- **`mba` has no Developer ID**, only an Apple Development identity: it signs
  but cannot notarize or distribute (UCD-32).

## 7. Commit hygiene

One task per branch, small commits, and a message that says **why** rather than
what - the diff already says what. Record the trap you hit, if you hit one:
every entry in §6 is there because a commit message explained it.
