import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import test from "node:test";

import {
  maximumChangedPixelRatio,
  resolveBenchmarkFont,
} from "../benchmarks/benchmark.mjs";

test("visual difference preserves the worst individual render ratio", () => {
  const ratio = maximumChangedPixelRatio([
    { dimensionsMatch: true, changed: 1, pixels: 32 * 32 },
    { dimensionsMatch: true, changed: 1, pixels: 512 * 512 },
  ]);

  assert.equal(ratio, 1 / (32 * 32));
  assert.equal(
    maximumChangedPixelRatio([{ dimensionsMatch: false, changed: 0, pixels: 0 }]),
    1,
  );
});

test("automatic font selection resolves the configured portable font", () => {
  const work = mkdtempSync(join(tmpdir(), "svg-squisher-benchmark-test-"));
  const font = join(work, "benchmark.ttf");
  try {
    writeFileSync(font, "font fixture");
    assert.equal(resolveBenchmarkFont("auto", { SVG_SQUISHER_FONT: font }), resolve(font));
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
});
