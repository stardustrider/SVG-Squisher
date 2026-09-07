import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { Resvg } from "@resvg/resvg-js";
import pixelmatch from "pixelmatch";
import { PNG } from "pngjs";

const here = dirname(fileURLToPath(import.meta.url));
const repository = resolve(here, "../..");
const binary = resolve(process.env.SVG_SQUISHER_BIN || join(repository, "build/svg_squisher"));
const work = mkdtempSync(join(tmpdir(), "svg-squisher-visual-"));
const diffDirectory = resolve(process.env.SVG_SQUISHER_DIFF_DIR || join(work, "diffs"));

// Exact-preservation fixtures allow at most 0.05% changed pixels. Element opacity
// gets 0.10%, dashed stroke approximation 0.60%, and curve fallback 2.00%.
// Text compares the same explicit font file in both renders and allows 0.40% for
// FreeType/HarfBuzz versus resvg rasterization and hinting differences.
const cases = [
  { name: "cascade.svg", maxRatio: 0.0005 },
  { name: "fill-opacity.svg", maxRatio: 0.0005 },
  { name: "element-opacity.svg", maxRatio: 0.001 },
  { name: "implicit-moveto.svg", maxRatio: 0.0005 },
  { name: "root-transform.svg", maxRatio: 0.0005 },
  { name: "symbol-use.svg", maxRatio: 0.0005 },
  { name: "pattern-css.svg", maxRatio: 0.0005 },
  { name: "gradient-viewbox.svg", maxRatio: 0.0005 },
  { name: "dashed-circle.svg", maxRatio: 0.006 },
  { name: "stroked-curve.svg", maxRatio: 0.02 },
  { name: "text-shaping.svg", maxRatio: 0.004, requiresFont: true },
];

const fontCandidates = [
  { path: "C:/Windows/Fonts/arial.ttf", family: "Arial" },
  { path: "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", family: "DejaVu Sans" },
  { path: "/usr/share/fonts/TTF/DejaVuSans.ttf", family: "DejaVu Sans" },
  { path: "/System/Library/Fonts/Supplemental/Arial.ttf", family: "Arial" },
];
const testFont = fontCandidates.find((candidate) => existsSync(candidate.path));
if (!testFont) throw new Error("Visual text coverage requires Arial or DejaVu Sans");

const renderWidths = [32, 512];

function render(svg, background, width) {
  return PNG.sync.read(new Resvg(svg, {
    background,
    fitTo: { mode: "width", value: width },
    font: {
      loadSystemFonts: false,
      fontFiles: [testFont.path],
      defaultFontFamily: testFont.family,
      sansSerifFamily: testFont.family,
    },
  }).render().asPng());
}

let failed = false;
try {
  mkdirSync(diffDirectory, { recursive: true });
  for (const testCase of cases) {
    const inputPath = join(here, "fixtures", testCase.name);
    const outputPath = join(work, testCase.name);
    const arguments_ = [inputPath, outputPath, "--strict", "--precision", "6"];
    if (testCase.requiresFont) arguments_.push("--font", testFont.path);
    execFileSync(binary, arguments_, {
      encoding: "utf8",
      stdio: "pipe",
      timeout: 10_000,
    });

    const inputSvg = readFileSync(inputPath);
    const outputSvg = readFileSync(outputPath);
    for (const width of renderWidths) {
      for (const background of ["#ffffff", "#17191d"]) {
        const input = render(inputSvg, background, width);
        const output = render(outputSvg, background, width);
        if (input.width !== output.width || input.height !== output.height) {
          throw new Error(`${testCase.name}: rendered dimensions changed`);
        }

        const diff = new PNG({ width: input.width, height: input.height });
        const changed = pixelmatch(
          input.data,
          output.data,
          diff.data,
          input.width,
          input.height,
          { threshold: 0.1, includeAA: false },
        );
        const ratio = changed / (input.width * input.height);
        const label = `${width}px-${background === "#ffffff" ? "light" : "dark"}`;
        console.log(`${testCase.name} (${label}): ${(ratio * 100).toFixed(4)}% changed`);
        if (ratio > testCase.maxRatio) {
          failed = true;
          writeFileSync(
            join(diffDirectory, `${basename(testCase.name, ".svg")}-${label}.png`),
            PNG.sync.write(diff),
          );
          console.error(
            `${testCase.name} (${label}) exceeded ${(testCase.maxRatio * 100).toFixed(2)}% tolerance`,
          );
        }
      }
    }
  }
} finally {
  if (!failed && !process.env.SVG_SQUISHER_KEEP_VISUAL_OUTPUT) rmSync(work, { recursive: true, force: true });
}

if (failed) {
  console.error(`Visual regression diffs: ${diffDirectory}`);
  process.exitCode = 1;
}
