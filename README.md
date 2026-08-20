# UnoCode Desktop

The [UnoCode](https://github.com/hmofet/unodos) editor - UnoDOS's VS Code-class
workbench - running as a native desktop application on Windows, macOS and
Linux, over SDL2.

The editor core (~11k lines of C), the unoui toolkit, the unojs extension host
and the TrueType text engine come from the pinned `upstream/unodos` submodule
**unmodified**: the same translation units that boot on a bare-metal PC (and
the PS2, and a Dreamcast) compile here against a ~900-line host shim. The core
reaches the outside world only through the ~30 functions it declares in
`unocode.h` - filesystem, fonts, a clock, five shell hooks - and `host/`
provides exactly those, on top of SDL2 and the host OS, the way the pc64 shell
provides them on top of UEFI.

What that buys, concretely: VS Code's file formats (settings, keybindings,
themes, TextMate grammars, snippets, extension manifests) in a binary around a
megabyte that starts in tens of milliseconds and idles around two repaints a
second.

## Layout

```
host/               the shim: SDL2 shell, filesystem seam, shell hooks
upstream/unodos     pinned submodule - consumed read-only, never patched here
build.sh            native + mingw cross builds, headless render gate
sample/             a small demo workspace
```

The rule that keeps this repo honest: **nothing under `upstream/` is ever
edited here.** A change the port needs in the core is an upstream request in
the unodos repo (per its `AGENTS.md`), not a patch in this one.

## Build

```
git submodule update --init
./build.sh              # Linux/mac native -> build/unocode
./build.sh --gate       # + headless render check (no display needed)
./build.sh --windows    # mingw cross -> build/win/unocode.exe
```

Needs `gcc`, `python3`, and SDL2 dev headers (`libsdl2-dev` /
`brew install sdl2`). The Windows cross build needs `mingw-w64` and an
extracted SDL2 mingw devel tree (`SDL2_MINGW=...`).

## Run

```
build/unocode [folder]
```

Ctrl+Shift+P opens the command palette; Ctrl+` the terminal. The volume map:
`WORK` is the folder being edited, `APP` the read-only bundled resources
(fonts, sample extensions), `HOME` the per-user data directory.

## Performance

Against VS Code 1.126.0 on the same machine, same folder, both watched from
outside with neither instrumented: **4.1x faster** to a usable window than a
clean-profile VS Code, **13x faster** than the VS Code that actually opens on
this machine, **23x less** memory, **136x smaller** on disk, one process
against eight. Full method, the traces, and the one dimension where UnoCode
loses: [BENCHMARK.md](BENCHMARK.md).

## Status: phase 0

Working: the full workbench (activity bar, explorer, tabbed editors, minimap,
find, command palette, settings, themes, syntax highlighting, the integrated
terminal's builtins, declarative + JS extensions). Deliberately not yet:
long filenames beyond 15 characters (withheld from listings, not truncated),
file deletion, process spawning from the terminal, UTF-8 input, clipboard
integration with the host.
