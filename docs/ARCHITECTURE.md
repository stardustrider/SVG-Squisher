# Architecture

SVG Squisher keeps the command line, conversion model, and output policy separate so the same behavior can be exercised through CTest and embedded through the C++ API.

## Conversion pipeline

1. **Input and XML** — `svg_squisher.cpp` reads bytes and pugixml parses one UTF-8 SVG document. File writes happen only after conversion succeeds.
2. **Structural guard** — `svg_diagnostics.cpp` iteratively verifies that the document has at most 256 element levels, including the root. This runs before XPath queries and every recursive diagnostic, lookup, and traversal pass.
3. **Capability preflight** — `svg_diagnostics.cpp` scans known semantics that the path model cannot faithfully represent. Inline and embedded CSS declarations share one supported-property classifier. Diagnostics carry a severity, stable code, message, and element label. Strict mode stops at this boundary.
4. **Style resolution** — `svg_style.cpp` parses the supported declaration set, simple selectors, specificity, source order, inheritance, inline styles, and importance. `svg_computed_style.cpp` turns string properties into rendering decisions.
5. **DOM traversal** — `svg_traversal.cpp` walks elements in document order, composes transforms, suppresses hidden subtrees, and expands local references with cycle, reference-depth, combined-depth, expanded-node, and output-path guards. Capability preflight checks reference, combined-depth, and expanded-node growth. Traversal counts actual emitted paths, and the final strict gate rejects an exhausted output budget before serialization.
6. **Geometry** — `svg_shape.cpp` normalizes primitives. `svg_path_data.cpp` parses path data once into absolute `PathSegment` values, resolving relative, shorthand, smooth, and repeated commands. Validation, command counts, transforms, bounds, flattening, and stroke generation consume that representation; its compact serializer elides repeatable command letters and numeric separators only where SVG grammar remains unambiguous.
7. **Stroke conversion** — `svg_stroke.cpp` creates fill outlines for supported solid strokes from the normalized path representation. The selected `ConversionPolicy` either permits a live stroke for appearance preservation or diagnoses that fallback as incompatible with filled-path output. Strict enforcement rejects the diagnostic before serialization.
8. **Text conversion** — `svg_text.cpp` selects an authoritative or discovered font, shapes each run with HarfBuzz, and reads glyph outlines through FreeType.
9. **Postprocessing policy** — `svg_postprocess.cpp` applies background removal only when explicitly requested. Recoloring is an independent serialization option.
10. **Serialization and I/O** — `svg_output.cpp` emits the path-oriented document, materializes supported definition styles, removes active/unsupported definition content and external resources, and performs checked temporary-file replacement. Directory writes resolve the selected output root, reject descendant symlink/reparse parents, and revalidate containment before installation. `svg_report.cpp` emits schema-versioned JSON using the same result objects returned by the library.

## Core data contract

`PathEntry` is the boundary between traversal and serialization. It carries path data, a remaining transform when required, separate fill/stroke paints and alpha, stroke metrics for live strokes, fill rule, and emission flags. It deliberately cannot represent arbitrary SVG subtrees, filters, masks, or group compositing. Capability diagnostics must identify those boundaries before a caller treats the output as faithful.

The public header exposes:

- `ConversionPolicy` and `Options` for appearance-preserving or filled-path output, batch behavior, overwrite, precision, font, recolor, cleanup, and strict enforcement;
- `Diagnostic` and `ConversionStats` for inspectable byte, element, path, path-command, font, and missing-glyph results;
- `ConversionResult`, `FileConversionResult`, and `BatchResult` for string, file, and directory work;
- throwing convenience wrappers for callers that only need success/failure.

The CLI is a small adapter over these objects. A behavior needed by another interface belongs in the core result model rather than in terminal-output parsing.

## Determinism

Directory inputs are sorted by generic path before conversion. Numeric output uses the selected fixed precision. Serialization uses one stable element order. These controls make the same build, options, input, and font suitable for golden and visual regression tests.

Text has an extra dependency: deterministic outlines require identical font bytes and compatible FreeType/HarfBuzz builds. Automatic OS font discovery is a convenience path and is not reproducible across platforms. Each file result records the actual font paths used, their byte size and SHA-256 identity when available, and a missing-glyph count.

## Error boundary

Parsing, structural-limit, font-loading, conversion, and I/O exceptions become failed result objects at the public API boundary. In a batch, each failure is retained and processing continues unless `continue_on_error` is false. A strict rejection is a normal failed conversion with its preflight diagnostics preserved.

Output is written to a unique sibling temporary file, checked after close, then renamed or replaced. This ordering keeps an existing destination intact when conversion or writing fails.

## Build targets

- `svg_squisher_core`: static C++17 conversion library.
- `SVG::Squisher`: in-tree alias for consumers using `add_subdirectory`.
- `svg_squisher`: command-line executable.
- CTest executables for parser/stroke, style/text, and public core behavior.
- `svg_squisher_fuzz`: optional Clang libFuzzer target when `SVG_SQUISHER_BUILD_FUZZER=ON`.

The default dependency mode fetches immutable revisions of pugixml, FreeType, and HarfBuzz. `SVG_SQUISHER_USE_SYSTEM_DEPS=ON` switches to installed CMake packages. Sanitizers are opt in through `SVG_SQUISHER_ENABLE_SANITIZERS=ON` and require GCC or Clang.

## Release baselines

Release archive names encode the runner OS baseline and CPU architecture that built and exercised the binary:

| Archive suffix | Architecture | Build/test baseline |
| --- | --- | --- |
| `linux-ubuntu-24.04-x86_64` | x86-64 | Ubuntu 24.04 |
| `windows-2025-x86_64` | x86-64 | Windows Server 2025, static MSVC runtime selection |
| `macos-15-arm64` | Apple silicon | macOS 15 deployment target |
| `macos-15-x86_64` | Intel x86-64 | macOS 15 deployment target |

The labels describe verified build environments. They do not imply that a binary was tested on an older OS, another libc, another architecture, or through emulation. A new target should be added only with a build job, CTest execution, an extracted-archive conversion smoke test, and an explicit archive label.
