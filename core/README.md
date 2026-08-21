# `core/` - the editor, and this repository is its home

Everything under this directory is **canonical here**. UnoDOS vendors it; it is
not vendored *from* UnoDOS. That is the opposite of how this repo started, and
the reason for the change is below.

The same arrangement already governs Duum, whose canonical home is
`hmofet/duum` and which UnoDOS carries as a generated drop. The rule there and
the rule here are the same rule.

## The rule

**Do not edit `pc64/unocode/` in the UnoDOS repository.** Those files are a
vendored copy with a banner saying so. An edit made there is lost at the next
sync, and until then it silently forks the editor away from the tree that the
Windows, macOS and Linux builds are cut from.

A change to the editor - the workbench, the document model, the extension host,
the grammars, the themes, the JSONC parser, the regex engine - is a commit
**here**, and reaches UnoDOS as a sync
(`pc64/tools/sync_unocode.py` in that repo, and `pc64/UNOCODE-UPSTREAM.md`
beside it).

## What is here, and what is deliberately not

| Here (canonical) | Why |
|---|---|
| `uc_*.c`, `unocode.h` | the editor: workbench, document model, editor, terminal, themes, settings, keybindings, grammars, extension host |
| `UNOCODE.md` | the contract - the formats, the `vscode` API subset, the stated deviations |
| `tools/test.sh`, `tools/uc_test.c` | the core's own tests. Pure logic, no framebuffer, so they run on any build host in a second |
| `ext/` | the sample extensions. They are the worked example of the manifest contract, so they belong with the contract |

| Stays in UnoDOS | Why |
|---|---|
| `unoui/`, `unojs/` | UnoDOS subsystems with many other consumers. The shell, every app and the Dreamcast port use unoui; unoweb and the browser use unojs. They are not ours to take |
| `pc64/fb.c`, `pc64/pc64_font.c` | software rendering, same reason |
| `pc64/unocode/tools/unocode_urc.py` | the QEMU/device harness. It drives a booted UnoDOS, which only that repo can do |
| `pc64/build.sh` step 3c2, `pc64/docs_shots.py` | packaging `UNOCODE.UNO` and photographing it for the manual |

We still consume those from the pinned `upstream/unodos` submodule, read-only,
exactly as before. **Nothing is canonical in both places**, which is what keeps
the two-way dependency safe: each repo is the sole author of what it sends the
other.

## Why the direction was flipped

The editor now has three shipping targets (Windows, macOS, Linux) against one
in UnoDOS, and its next block of work - networking for extensions, an event
loop, an assistant - is developed and tested far faster on a desktop than
through a QEMU boot. Keeping the source in the OS repo meant every one of those
changes was an `[UPSTREAM]` two-repo dance in the direction that hurt most:
edit there, gate there, push there, bump here, gate here.

It also removes a foot-gun. `sources.sh` compiles the editor but never the
kernel, so a core change could gate green here and not compile in pc64 at all
(it did, on unodos `029a4f17`). With the core here, that break can no longer
reach UnoDOS silently: it arrives through a sync, and a sync is gated by
`pc64/tools/gate.sh` before it lands.

## What a sync still has to prove

Read `pc64/UNOCODE-UPSTREAM.md` in the UnoDOS repo. The short version: **this
repo's gate cannot see a pc64 break.** `sources.sh` builds the editor module
and its foundations, not the kernel, so `pc64_modload.c` and `pc64_fs.c` are
never compiled here at any warning level. Before a sync is called done:

```
cd pc64 && sh tools/gate.sh                  # QUICK=1 for builds only
python3 unocode/tools/unocode_urc.py         # the 12-scene QEMU drive
```
