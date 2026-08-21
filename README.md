# UnoCode Desktop

A code editor in the shape of Visual Studio Code, built out of about 23,000
lines of C, with no browser engine and no JavaScript runtime underneath it.

It starts in a quarter of a second, holds a 60,000-line file in 48 MB, runs as
one process, and installs as 6.5 MB across 14 files. It reads VS Code's colour
themes, keybindings, snippets, TextMate grammars and extension manifests as they
are written, because those file formats are the compatibility surface it was
designed around.

The editor is not new. It is [UnoCode](https://github.com/hmofet/unodos), the
workbench from UnoDOS, an operating system that boots on bare-metal PCs and on a
PlayStation 2 and a Dreamcast. **This repository is the ~900 lines that let the
same C run on Windows, macOS and Linux.** The core is consumed from a pinned
submodule, unmodified: the translation units compiled here are byte-for-byte the
ones that boot on a machine with no operating system under them.

```
                                   UnoCode Desktop      VS Code
  time to a usable window                   276 ms      1,126 ms   (3,597 ms as configured)
  memory, private bytes                      40 MB        896 MB
  processes                                       1            8
  on disk                                   6.5 MB        886 MB
```

Same machine, same folder, neither application instrumented. Method, traces and
the dimension where UnoCode *loses* are in [BENCHMARK.md](BENCHMARK.md).

## Status

Phase 0. The workbench is real and complete: activity bar, file explorer, tabbed
editors with a minimap, find, the command palette, settings, colour themes,
syntax highlighting, the integrated terminal's builtins, and an extension host
that runs both declarative and JavaScript extensions.

It is **not yet a daily driver**, and the reasons are specific and written down.
Filenames longer than 15 characters are withheld from listings rather than
truncated, so most real projects are partly invisible. There is no clipboard
integration with the host, no UTF-8 input, no HiDPI awareness, and settings are
written into the folder you are editing. Each of those is a numbered task in
[ROADMAP.md](ROADMAP.md), which is the plan for closing the distance to VS Code.

## Build

```bash
git clone --recurse-submodules https://github.com/hmofet/unocode-desktop
cd unocode-desktop
./build.sh              # Linux / macOS native  -> build/unocode
./build.sh --gate       # + a headless render check that needs no display
./build.sh --windows    # mingw cross-build     -> build/win/unocode.exe
./build-mac.sh          # macOS Universal Binary 2 (arm64 + x86_64)
```

Requirements: `gcc` or `clang`, `python3`, and SDL2 development headers
(`libsdl2-dev`, or `brew install sdl2`). The Windows cross-build needs
`mingw-w64` plus an extracted SDL2 mingw devel tree (`SDL2_MINGW=...`). The
macOS build fetches the official SDL2 framework itself, because it is the
universal one and Homebrew's is not.

## Run

```bash
build/unocode [folder]
build/unocode [folder] --open path/to/file.c
```

`Ctrl+Shift+P` opens the command palette, `Ctrl+P` jumps to a file, `` Ctrl+` ``
opens the terminal, `Ctrl+,` opens settings. Type `help` in the terminal for what
it can do.

## How it works

The editor core reaches the outside world through roughly thirty functions it
declares in `unocode.h`: a filesystem, a font, a clock, and five hooks into
whatever shell is hosting it. On UnoDOS those are provided by the operating
system. Here they are provided by `host/`, on top of SDL2 and whatever OS you
are on. That is the whole port.

```
host/main.c        SDL2 window, the two input roads, the frame loop
host/host_fs.c     the filesystem seam, speaking FAT's dialect over a real OS
host/host_shell.c  the five shell hooks and the clock
host/compat/       headers that shadow upstream where a hosted build must differ
upstream/unodos    the pinned submodule: consumed, never patched
```

The submodule is [hmofet/unodos](https://github.com/hmofet/unodos), also MPL-2.0,
pinned to an exact commit so a clone builds the core this host was tested
against rather than whatever happens to be on its main branch today. Moving to a
newer core is a deliberate act: bump the submodule, re-run the gate, commit.

Two decisions carry most of the design. The editor runs in unoui's **fullscreen
canvas** mode, so there is no simulated desktop inside the window and the OS
provides the title bar: structurally it is a native application, not an emulator
showing one. And the filesystem shim resolves paths **case-insensitively**,
because the core upper-cases everything the way FAT does, and ext4 does not.

The rule that keeps the port honest is that **nothing under `upstream/` is ever
edited here**. When the port needs something from the core, that is a request
filed against the UnoDOS repository under its own process, and those tasks are
marked `[UPSTREAM]` in the roadmap. It is why the same source still boots on a
Dreamcast.

## Contributing, or picking this up

[ROADMAP.md](ROADMAP.md) is the work queue: 43 tasks, each scoped to a single
session, each with acceptance criteria, ordered by what a day-to-day user hits
first rather than by what is interesting to build. Take the lowest unclaimed ID
in the highest unfinished tier, claim it by editing that file, and make sure
`./build.sh --gate` is still green when you are done.

[AGENTS.md](AGENTS.md) is how to work in this tree: which machine builds what
(none of it builds on Windows), how to run the gate so it cannot lie to you,
how to drive the GUI when the thing you built is visual, what the two-repo
dance for an upstream change looks like, and the traps that have already cost
somebody a cycle. Read it before the first build, not after the first surprise.

**Tier 0 is finished.** Long filenames, the system clipboard, UTF-8, HiDPI,
settings out of the project directory, native Open dialogs, the window title
and close confirmation, pointer shapes, remembered geometry and smooth
scrolling all landed on 2026-08-21. UCD-11 is next.

## Licence

Mozilla Public License 2.0, the same licence as the UnoDOS core it is built
from. See [LICENSE](LICENSE), and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
for SDL2, stb_truetype and the four bundled typefaces.
