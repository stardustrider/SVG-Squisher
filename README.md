# SVG Squisher

SVG Squisher is a native C++ converter for turning icon-oriented SVGs into explicit, reviewable path output. It handles single files and deterministic directory batches, reports unsupported semantics before they become silent surprises, and includes a browser comparison surface for inspecting every result.

The default compatibility profile completes supported work and records warnings. `--strict` turns every detected compatibility warning into a failed conversion and leaves the destination untouched. See the [support matrix](docs/SUPPORT.md) before using SVG Squisher as a general-purpose SVG renderer.

<a href="https://www.buymeacoffee.com/thestarduster" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174"></a>

## What it does

- Converts `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, and `polygon` elements.
- Expands local `use` references, including bounded `symbol` instances.
- Resolves presentation attributes, inline styles, and supported simple CSS selectors with specificity and `!important`.
- Converts text to outlines with FreeType and shapes text runs with HarfBuzz.
- Flattens ordinary strokes to filled outlines while retaining live dashed strokes when that better preserves dash behavior.
- Preserves per-element opacity as one compositing unit across generated fill/stroke paths, distinct fill and stroke opacity, root viewport/language/identity metadata, `title`, `desc`, and common accessibility attributes.
- Writes through a checked sibling temporary file before replacing a destination.
- Processes directory entries in stable order with recursion, overwrite, fail-fast, and per-file JSON reporting controls.
- Provides visual regression tests and a responsive side-by-side, overlay, and blink comparison page.

Path conversion can increase file size, especially when text or strokes become geometry. Treat the byte counts in the report as measurements, not a promise that every output will be smaller.

## Build and test

SVG Squisher requires CMake 3.20 or newer and a C++17 compiler. The default build fetches pinned pugixml, FreeType, and HarfBuzz sources, so the first configure needs network access.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build/svg_squisher` with a single-config Unix generator and `build/Release/svg_squisher.exe` with the default Visual Studio generator.

To use installed dependencies instead of fetching them:

```sh
cmake -S . -B build -DSVG_SQUISHER_USE_SYSTEM_DEPS=ON
```

The package names expected by CMake are `pugixml`, `Freetype`, and `harfbuzz`. Install the executable, static core library, public header, and notices with:

```sh
cmake --install build --config Release --prefix ./stage
```

## Command line

Convert one file:

```sh
./build/svg_squisher input.svg output.svg
```

Convert the immediate SVG files in a directory and write a report:

```sh
./build/svg_squisher icons converted --report conversion-report.json
```

Include nested directories, keep processing after individual failures, and reject detected unsupported semantics:

```sh
./build/svg_squisher icons converted \
  --recursive \
  --strict \
  --report conversion-report.json
```

Use a fixed font file whenever text output must be repeatable across machines:

```sh
./build/svg_squisher label.svg label-paths.svg \
  --font ./fonts/ProjectSans-Regular.ttf \
  --precision 6
```

Options may appear before or after the two paths. Use `--` before dash-prefixed path names to end option parsing.

| Option | Behavior |
| --- | --- |
| `--fill <color>` | Recolors emitted fills and strokes without invoking cleanup. |
| `--font <path>` | Uses one authoritative font file for all text in the conversion. |
| `--conversion-policy <preserve-appearance\|filled-paths>` | Selects whether unsupported stroke outlines stay live for fidelity or count as a filled-path fallback. The default is `preserve-appearance`. |
| `--precision <0-15>` | Sets generated coordinate precision; the default is 4. |
| `--remove-background` | Applies the opt-in icon background heuristic. It can remove intentional large dark or gradient geometry, so review the result. |
| `--strict` | Fails before writing output when the capability scan emits any warning. |
| `--in-place` | Explicitly permits the input and output to resolve to the same path. Replacement still uses the checked temporary-file write. |
| `--recursive` | Includes regular SVG files below nested input directories and preserves their relative paths. Directory scans never follow symlinks; SVG symlink entries are reported as skipped. The selected output root may itself be a link, but links and reparse points beneath its resolved location are rejected as failed destinations. |
| `--fail-fast` | Stops a directory run after its first failed file. The default is to continue. |
| `--no-overwrite` | Skips existing destinations. A run containing skips returns a nonzero exit status. |
| `--overwrite` | Replaces existing destinations; this is the default. |
| `--report <path>` | Writes the schema-versioned JSON result for a file or directory run. |
| `--` | Treats all remaining arguments as paths, including names beginning with `-`. |
| `-h`, `--help` | Prints command help. |
| `--version` | Prints the project version. |

A successful file conversion returns 0. Input and output paths that resolve to the same location are rejected unless `--in-place` is present. Invalid arguments, failed conversions, strict rejections, directory failures, and directory skips return 1. Diagnostics use stable codes such as `unsupported-clip-path` and identify the affected element when possible.

## Compatibility and strict conversion

The compatibility profile is the default. It skips malformed path data and content that cannot enter the path model, emits warnings to standard error and the JSON report, and writes the supported remainder. Numeric and unit diagnostics state the exact compatible action, such as a substituted default, an ignored text position list, or dropped geometry. This is useful for reviewing a mixed corpus without losing all successful files because one file needs attention.

The conversion policy is a separate choice. `--conversion-policy preserve-appearance` is the default and keeps a live stroke when outlining it would lose behavior, including dashed strokes. `--conversion-policy filled-paths` requests fill-only geometry. Supported solid strokes become outlines; when an outline cannot be produced, compatible conversion emits `live-stroke-retained` and states that it retained SVG stroke attributes. Combining `filled-paths` with `--strict` rejects that fallback before output is serialized.

`--strict` uses the same converter after a capability preflight. If preflight emits a warning, conversion fails and no destination file is replaced. Strict mode covers the known boundaries listed in [docs/SUPPORT.md](docs/SUPPORT.md); it is not a claim of full SVG 2 conformance.

The preflight classifies unsupported declarations in inline `style` attributes and embedded stylesheets with the same `unsupported-css-property` diagnostic. URL classification decodes CSS escapes and handles comments, casing, quotes, and whitespace before accepting only a local `#fragment`; external or malformed `url(...)` targets receive `unsupported-external-reference`. Compatible output removes them, while strict conversion rejects the file. Local opacity on a container, link, or text expansion that produces multiple painted layers receives a compositing warning because compatible output distributes that opacity; opacity around one painted element remains supported. Independently of the selected profile, input is limited to 256 nested element levels including the root `svg`; deeper documents fail before recursive scans or conversion begin. Expanded `use` traversal has the same 256-level combined depth limit, a 64-reference limit, a 16,384-element visit budget, and an exact 8,192-output-path budget. Reference, depth, and expanded-node limits are checked during preflight. The output-path limit is enforced while paths are produced, so fill-only and hidden elements are counted according to actual emission. Compatible mode truncates bounded output with a diagnostic; strict mode rejects it before serialization.

## Reproducible text

`--font` is authoritative: CSS `font-family`, `font-weight`, and `font-style` cannot replace it. Reproducible output requires the same font bytes, converter version, platform architecture, options, and input. Font files are loaded into owned byte snapshots used by FreeType and HarfBuzz, and each report entry records the matching byte size and SHA-256 in `fontIdentities` alongside the path in `fonts`. An unreadable or invalid snapshot fails conversion; every successfully shaped snapshot is reported from the exact owned bytes used for shaping. The report identifies those bytes without embedding or redistributing them.

Without `--font`, SVG Squisher searches a small set of common system font locations and may choose different files on different computers. Text using implicit font selection receives a warning, so strict text conversion requires `--font`. HarfBuzz provides glyph shaping for ligatures and connected scripts, with script and direction inferred per text run. Parent `x`, `y`, `dx`, and `dy` lists are consumed in logical tree order across nested `tspan` content. The current text model does not implement `textPath`, full paragraph bidi layout, vertical text, automatic multi-font fallback, variable-font axis selection, or complete SVG 2 per-character positioning inside multi-codepoint shaped clusters. Font collections use their first face.

## JSON report

Reports use schema version 1, described by [docs/report-schema-v1.json](docs/report-schema-v1.json). They identify the generator and `generatorVersion`, contain the exact options including `conversionPolicy`, and include aggregate totals plus one entry per attempted file with:

- input and output paths;
- `converted`, `failed`, or `skipped` status;
- an error string when applicable;
- input/output byte counts, source/output path-command counts, and element/output-path counts;
- actual font paths, byte sizes, SHA-256 identities, and the number of missing Unicode glyphs;
- structured warning and error diagnostics.

Paths use `"pathBase": "page"`: the converter records UTF-8 input and output paths relative to the report file's parent, and the comparison page rebases them from the manifest URL. Keep the report and referenced files in the same relative layout when moving a review corpus. For directory conversions, place the report outside both the input and output trees so it cannot collide with a source or generated file.

## Visual comparison

Serve the repository and open the included report example:

```sh
python3 -m http.server 8080
```

Open:

```text
http://127.0.0.1:8080/compare.html?manifest=examples/compare/sample-report.json
```

The example includes matching, changed, Unicode/space-containing, missing, and failed records. The page can also inspect server directory listings:

```text
http://127.0.0.1:8080/compare.html?input=svgs&output=svgs-out
```

Use its search and status filters, sort by attention or byte change, choose side-by-side/overlay/blink review, switch the preview background, and inspect declared size, viewBox, intrinsic browser size, counts, byte deltas, and diagnostics. Directory sides load independently, so a missing output folder does not hide valid inputs.

![SVG Squisher Compare interface listing SVG files in a left column with matching input and output preview cards side by side for each file.](assets/img_readme_compare.png)

*Screenshot includes Unreal Engine icons and interface elements. Portions of the materials used are trademarks and/or copyrighted works of Epic Games, Inc. All rights reserved by Epic. This material is not official and is not endorsed by Epic.*

## C++ API

Add the repository as a CMake subdirectory and link the core target:

```cmake
add_subdirectory(path/to/SVG-Squisher)
target_link_libraries(my_tool PRIVATE SVG::Squisher)
```

After `cmake --install`, consumers can instead use the installed package:

```cmake
find_package(SvgSquisher CONFIG REQUIRED)
target_link_libraries(my_tool PRIVATE SVG::Squisher)
```

Use the result-bearing API when diagnostics or statistics matter:

```cpp
#include <svg_squisher.h>

svg_squisher::SvgSquisher converter;
svg_squisher::Options options;
options.conversion_policy = svg_squisher::ConversionPolicy::FilledPaths;
options.strict = true;
options.precision = 6;

svg_squisher::ConversionResult result = converter.convert_string(source, options);
if (!result.success) {
  // result.error and result.diagnostics explain the failure.
}
```

`squish_string` and `squish_file` remain convenience wrappers that throw `std::runtime_error` on failure. `squish_directory_with_result` returns stable per-file results and totals. File conversion rejects equivalent input and output paths by default; set `options.allow_in_place = true` only when intentional checked replacement is required.

## Quality checks

The CTest suite covers path parsing, bounded references, style/text behavior, stroke regression cases, the library API, CLI help/version, an actual file conversion, and a consumer built against the installed CMake package. `node --test tests/compare_page_test.mjs` checks comparison-page syntax, injection-safe DOM construction, accessibility labels, URL state, and the example corpus. Run sanitizers with Clang or GCC:

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSVG_SQUISHER_ENABLE_SANITIZERS=ON \
  -DBUILD_TESTING=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Configure the same Clang build with `-DSVG_SQUISHER_BUILD_FUZZER=ON` to produce `svg_squisher_fuzz`; pass it a directory of SVG seeds using normal libFuzzer arguments.

The visual suite renders each input and converted output at 32 px icon size and 512 px inspection size on light and dark backgrounds, then enforces fixture-specific pixel-difference limits. Its corpus includes deterministic same-font text shaping and element-opacity compositing:

```sh
npm ci
SVG_SQUISHER_BIN="$PWD/build/svg_squisher" npm run test:visual
```

PowerShell equivalent:

```powershell
npm ci
$env:SVG_SQUISHER_BIN = "$PWD\build\Release\svg_squisher.exe"
npm run test:visual
```

Set `SVG_SQUISHER_DIFF_DIR` to retain failure images in a chosen directory. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for module boundaries and [docs/RELEASING.md](docs/RELEASING.md) for the release gate.

For measured optimization work, the benchmark harness records byte size, path complexity, median runtime, peak resident memory where available, deterministic output hashes, and rendered pixel difference. It can run optional SVGO and usvg baselines against the same corpus:

```sh
npm run benchmark -- \
  --corpus tests/visual/fixtures \
  --squisher "$PWD/build/svg_squisher" \
  --font auto \
  --strict \
  --output benchmark-report.json
```

See [docs/BENCHMARKING.md](docs/BENCHMARKING.md) for controlled-run guidance, baseline command contracts, and visual error budgets.

## Release targets

Tagged releases build and test four assets whose names state the tested platform baseline and architecture:

- `svg-squisher-vX.Y.Z-linux-ubuntu-24.04-x86_64.tar.gz`
- `svg-squisher-vX.Y.Z-windows-2025-x86_64.zip`
- `svg-squisher-vX.Y.Z-macos-15-arm64.dmg`
- `svg-squisher-vX.Y.Z-macos-15-x86_64.dmg`

Each asset contains the executable, README, project license, and third-party notices. The macOS executables and disk images require Developer ID signatures, successful Apple notarization, and stapled tickets. The workflow extracts or mounts every finished asset and converts a strict fixture before publishing it. These names identify the build and test baseline; compatibility with an older operating system is not asserted.

## License

SVG Squisher is licensed under the MIT License. See [LICENSE](LICENSE). Bundled dependency acknowledgements and license copies are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [third_party/licenses](third_party/licenses).
