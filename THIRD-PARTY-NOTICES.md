# Third-party notices

UnoCode Desktop itself is under the Mozilla Public License 2.0 (see
[LICENSE](LICENSE)). It builds against, and its binaries bundle, the components
below. Every one is redistributable; this file provides the attribution some of
them require.

## The editor core, toolkit and engines

| Component | Where | Licence |
|---|---|---|
| UnoCode, unoui, unojs, the software framebuffer and the TrueType text engine | `upstream/unodos` (git submodule) | Mozilla Public License 2.0 |

The submodule is consumed unmodified. It is the same licence as this repository,
which is why the combination is straightforward: MPL is file-level copyleft, so
changes to those files go back under MPL, while `host/` remains separately
licensed under the same terms.

## Libraries

| Component | Used for | Licence |
|---|---|---|
| [SDL2](https://libsdl.org) 2.30.x | window, input, timing, presenting the framebuffer | zlib licence |
| `stb_truetype.h` | glyph rasterisation, vendored inside the submodule | public domain / MIT dual |

SDL2 is linked dynamically. Windows builds ship `SDL2.dll` beside the
executable; macOS builds embed `SDL2.framework` in the app bundle; Linux builds
link the system SDL2. The zlib licence permits this and asks only that the
origin not be misrepresented, which this notice satisfies.

## Fonts

The build stages four TrueType faces from the submodule into `res/`, under the
8.3 names the text engine looks for. All four are open-licensed and
redistributable.

| Bundled as | Face | Author | Licence |
|---|---|---|---|
| `CHICAGO.TTF` | ChiKareGo 2 | Giles Booth | Creative Commons Attribution (CC BY) |
| `SANS.TTF` | DejaVu Sans | DejaVu project | Bitstream Vera / DejaVu (permissive) |
| `MONO.TTF` | DejaVu Sans Mono | DejaVu project | Bitstream Vera / DejaVu (permissive) |
| `UBUNTU.TTF` | Ubuntu | Canonical | Ubuntu Font Licence 1.0 |

**ChiKareGo 2 is CC BY and this is its attribution.** It is by Giles Booth, a
free-culture homage to Apple's *Chicago* typeface. It is not Apple's Chicago,
which is proprietary and is not bundled here. Source: the BitFontMaker2 gallery,
<http://www.pentacom.jp/pentacom/bitfontmaker2/gallery/?id=3780>.

## Not bundled

The benchmark in [BENCHMARK.md](BENCHMARK.md) measures Visual Studio Code, which
is Microsoft's product and is neither included nor redistributed here. It is
named descriptively; no affiliation or endorsement is implied.
