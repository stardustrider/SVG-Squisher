# Benchmarking

The benchmark harness measures conversion results instead of assuming that path conversion makes every SVG smaller. It records input and output bytes, emitted path and command counts, median wall-clock time, peak resident memory where `/usr/bin/time` exposes it, output hashes across repeated runs, and rendered changed-pixel ratios at 32 and 512 pixels on light and dark backgrounds.

Build SVG Squisher and install the JavaScript test dependencies, then run a representative corpus:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
npm ci
npm run benchmark -- \
  --corpus tests/visual/fixtures \
  --squisher "$PWD/build/svg_squisher" \
  --font auto \
  --strict \
  --iterations 7 \
  --output benchmark-report.json
```

Use the same machine, build type, corpus, font bytes, precision, warm-up count, and iteration count when comparing changes. Close unrelated heavy processes when memory and latency deltas matter. The JSON report records the platform and configuration but does not normalize hardware differences.

The canonical command uses `--font auto` so strict conversion can process text on Windows, Linux, and macOS. Auto selection first uses the path in `SVG_SQUISHER_FONT`, then resolves Arial or DejaVu Sans from the known platform locations. The report records the concrete resolved path. For comparable measurements across machines, set `SVG_SQUISHER_FONT` to the same font bytes or pass a path directly:

```sh
npm run benchmark -- \
  --corpus ./icons \
  --font ./fonts/ProjectSans-Regular.ttf \
  --strict
```

Pass installed SVGO and usvg executables to collect normalization and optimization baselines in the same report:

```sh
npm run benchmark -- \
  --corpus ./icons \
  --svgo ./node_modules/.bin/svgo \
  --usvg /usr/local/bin/usvg
```

SVGO is invoked as `svgo input.svg -o output.svg`; usvg is invoked as `usvg input.svg output.svg`. Baseline failures are recorded per file and make the run fail. Tool versions are intentionally managed outside the harness so a benchmark job can pin them in its own lockfile or container.

Use `--max-diff 0.001` to turn the rendered changed-pixel ratio into a corpus-wide acceptance threshold. Each file reports and gates the maximum ratio from its four renders, so a 512 px result cannot dilute a regression visible at 32 px. The dedicated visual regression suite remains the better release gate because it stores independently chosen tolerances for each semantic fixture. A benchmark report is evidence for a particular corpus and machine, not a general SVG compatibility score.

Peak memory is `null` on platforms where the operating system does not expose child-process maximum resident memory through `/usr/bin/time`. Render comparison uses bundled resvg with system-font loading disabled. Inputs that depend on undeclared external resources should be made self-contained before benchmarking.
