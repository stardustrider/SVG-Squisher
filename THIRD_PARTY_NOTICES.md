# Third-party notices

SVG Squisher's default build compiles the libraries below into the executable and static core library. Their source revisions are pinned in `CMakeLists.txt`. This file and the corresponding license copies are included in release archives.

## pugixml

- Source: <https://github.com/zeux/pugixml>
- Pinned revision: `ee86beb30e4973f5feffe3ce63bfa4fbadf72f38`
- License: MIT; see [third_party/licenses/pugixml.txt](third_party/licenses/pugixml.txt)
- Copyright © 2006-2025 Arseny Kapoulkine

## FreeType

- Source: <https://gitlab.freedesktop.org/freetype/freetype>
- Pinned revision: `42608f77f20749dd6ddc9e0536788eaad70ea4b5`
- Version at the pinned revision: 2.13.3
- License selection for SVG Squisher distributions: FreeType License; see [third_party/licenses/freetype.txt](third_party/licenses/freetype.txt)
- Upstream license overview and compiled component notices: `third_party/licenses/freetype-overview.txt`, `freetype-bdf.txt`, `freetype-pcf.txt`, `freetype-fthash.txt`, and `freetype-zlib.txt`

Portions of this software are copyright © 2024 The FreeType Project (<https://www.freetype.org>). All rights reserved.

This software is based in part on the work of the FreeType Team.

## HarfBuzz

- Source: <https://github.com/harfbuzz/harfbuzz>
- Pinned revision: `b42511e071162fe76102f613a6ccc009726c99af`
- Version at the pinned revision: 12.3.2
- License: Old MIT; see [third_party/licenses/harfbuzz.txt](third_party/licenses/harfbuzz.txt)
- Microsoft Universal Shaping Engine data notice: `third_party/licenses/harfbuzz-ms-use.txt`

HarfBuzz includes files with their own compatible license notices. The copied upstream `COPYING` file directs readers to those individual source files for the full copyright record.

## JavaScript development dependencies

The packages in `package.json` are used only by the local and CI visual-regression harness. They are not linked into SVG Squisher release binaries:

- `@resvg/resvg-js`
- `pixelmatch`
- `pngjs`

Their exact installed versions and transitive dependency records are captured by `package-lock.json`.
