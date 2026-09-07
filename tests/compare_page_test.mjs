import assert from "node:assert/strict";
import { readFile, stat } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..");
const pagePath = path.join(repositoryRoot, "compare.html");
const page = await readFile(pagePath, "utf8");

test("comparison page has valid inline JavaScript", () => {
  const start = page.indexOf("<script>");
  const end = page.lastIndexOf("</script>");
  assert.notEqual(start, -1, "expected an inline script");
  assert.ok(end > start, "expected a closing script tag");
  assert.doesNotThrow(() => new vm.Script(page.slice(start + "<script>".length, end)));
});

test("dynamic content avoids markup injection sinks", () => {
  for (const sink of ["innerHTML", "outerHTML", "insertAdjacentHTML", "document.write("]) {
    assert.equal(page.includes(sink), false, `compare.html must not use ${sink}`);
  }
  assert.match(page, /\.textContent\s*=/u);
  assert.match(page, /\.replaceChildren\(/u);
});

test("every form control has an explicit label", () => {
  const controls = [...page.matchAll(/<(?:input|select)\b[^>]*\bid="([^"]+)"[^>]*>/gu)]
    .map((match) => match[1]);
  assert.ok(controls.length >= 10, "expected the complete review toolbar");
  for (const id of controls) {
    assert.match(page, new RegExp(`<label[^>]+for="${id}"`, "u"), `missing label for #${id}`);
  }
});

test("control IDs are unique and report state is query-addressable", () => {
  const ids = [...page.matchAll(/\bid="([^"]+)"/gu)].map((match) => match[1]);
  assert.equal(new Set(ids).size, ids.length, "HTML IDs must be unique");
  for (const key of ["input", "output", "manifest", "size", "search", "status", "sort", "mode", "background", "opacity", "zoom"]) {
    assert.match(page, new RegExp(`${key}:`, "u"), `missing URL state for ${key}`);
  }
  assert.match(page, /new AbortController\(\)/u);
  assert.match(page, /loadId !== activeLoadId/u);
});

test("inspection controls provide actual-size synchronized pan and reset", () => {
  assert.match(page, /id="zoomLevel"/u);
  assert.match(page, /value="100">100% actual size/u);
  assert.match(page, /id="resetView"/u);
  assert.match(page, /applyRowViewport/u);
  assert.match(page, /pointerdown/u);
  assert.match(page, /setPointerCapture/u);
  assert.match(page, /ArrowLeft/u);
  assert.match(page, /prefers-reduced-motion: reduce/u);
});

test("visual discrepancy is bounded, same-origin, and exposed to review state", () => {
  assert.match(page, /PIXEL_COMPARE_SIZE\s*=\s*128/u);
  assert.match(page, /window\.location\.origin/u);
  assert.match(page, /getImageData/u);
  assert.match(page, /sharedScale/u);
  assert.match(page, /shared actual-size scaling/u);
  assert.match(page, /visualMismatch/u);
  assert.match(page, /pixel mismatch/u);
});

test("image listeners are attached before browser loading can start", () => {
  const start = page.indexOf("function createFigure(");
  const end = page.indexOf("function createDetails(", start);
  const createFigure = page.slice(start, end);
  const loadListener = createFigure.indexOf('image.addEventListener("load"');
  const errorListener = createFigure.indexOf('image.addEventListener("error"');
  const sourceAssignment = createFigure.indexOf("image.src = cacheBustedUrl");
  assert.ok(loadListener >= 0 && loadListener < sourceAssignment);
  assert.ok(errorListener >= 0 && errorListener < sourceAssignment);
});

test("manifest 404s become missing sides without discarding their valid pair", () => {
  assert.match(page, /error\.httpStatus\s*=\s*response\.status/u);
  assert.match(page, /error\?\.httpStatus\s*===\s*404/u);
  assert.match(page, /sideState\.availability\s*=\s*"missing"/u);
  assert.match(page, /createPlaceholder\(`\$\{sideLabel\(side\)\} file missing`\)/u);
});

test("sample corpus covers encoded names, exact changes, and missing pairs", async () => {
  const inputDirectory = path.join(repositoryRoot, "examples", "compare", "input");
  const outputDirectory = path.join(repositoryRoot, "examples", "compare", "output");
  await stat(path.join(inputDirectory, "a space.svg"));
  await stat(path.join(outputDirectory, "a space.svg"));
  await stat(path.join(inputDirectory, "naïve.svg"));
  await stat(path.join(outputDirectory, "naïve.svg"));
  await stat(path.join(inputDirectory, "glyph warning.svg"));
  await stat(path.join(outputDirectory, "glyph warning.svg"));
  await stat(path.join(inputDirectory, "input-only.svg"));
  await assert.rejects(stat(path.join(outputDirectory, "input-only.svg")));
  await stat(path.join(outputDirectory, "output-only.svg"));
  await assert.rejects(stat(path.join(inputDirectory, "output-only.svg")));

  const exactInput = await readFile(path.join(inputDirectory, "a space.svg"));
  const exactOutput = await readFile(path.join(outputDirectory, "a space.svg"));
  assert.deepEqual(exactInput, exactOutput);
  const changedInput = await readFile(path.join(inputDirectory, "changed.svg"));
  const changedOutput = await readFile(path.join(outputDirectory, "changed.svg"));
  assert.notDeepEqual(changedInput, changedOutput);
});

test("sample manifest resolves file paths from its own report location", async () => {
  const manifestPath = path.join(repositoryRoot, "examples", "compare", "sample-report.json");
  const report = JSON.parse(await readFile(manifestPath, "utf8"));
  assert.equal(report.schemaVersion, 1);
  assert.match(report.generatorVersion, /^\d+\.\d+\.\d+(?:[-+].*)?$/u);
  assert.equal(report.pathBase, "page");
  assert.ok(Array.isArray(report.files));
  assert.equal(report.options.continueOnError, true);
  assert.equal(report.options.allowInPlace, false);
  assert.equal(report.options.precision, 3);

  for (const entry of report.files) {
    assert.ok(Array.isArray(entry.fonts), `${entry.input} must report its font paths`);
    assert.ok(Array.isArray(entry.fontIdentities), `${entry.input} must report font identities`);
    assert.ok(Number.isInteger(entry.missingGlyphs), `${entry.input} must report missing glyphs`);
    assert.ok(Number.isInteger(entry.inputPathCommands), `${entry.input} must report input path commands`);
    assert.ok(Number.isInteger(entry.outputPathCommands), `${entry.input} must report output path commands`);
    const inputPath = path.resolve(path.dirname(manifestPath), entry.input);
    assert.equal((await stat(inputPath)).size, entry.inputBytes, `${entry.input} byte count drifted`);
    if (entry.status === "converted") {
      const outputPath = path.resolve(path.dirname(manifestPath), entry.output);
      assert.equal((await stat(outputPath)).size, entry.outputBytes, `${entry.output} byte count drifted`);
    }
  }

  const glyphWarning = report.files.find((entry) => entry.missingGlyphs > 0);
  assert.ok(glyphWarning, "sample report must exercise the missing-glyph attention state");
  assert.ok(glyphWarning.fonts.length > 0, "missing-glyph example must show the selected font path");
  assert.equal(glyphWarning.fontIdentities.length, glyphWarning.fonts.length);
  assert.equal(glyphWarning.fontIdentities[0].path, glyphWarning.fonts[0]);
  assert.equal(glyphWarning.fontIdentities[0].bytes, null);
  assert.equal(glyphWarning.fontIdentities[0].sha256, null);
  assert.match(glyphWarning.fontIdentities[0].error, /not bundled/u);
  const warningCount = report.files.flatMap((entry) => entry.diagnostics)
    .filter((diagnostic) => diagnostic.severity === "warning").length;
  assert.equal(report.summary.warnings, warningCount);
  assert.match(page, /value="glyphs">Missing glyphs/u);
  assert.match(page, /reportedMissingGlyphs/u);
  assert.match(page, /reportedFonts/u);
  assert.match(page, /reportedFontIdentities/u);
  assert.match(page, /SHA-256/u);
  assert.match(page, /Font identity unavailable/u);
  assert.match(page, /flags\.fontIdentityWarning/u);
  assert.match(page, /buildManifestRows\(report, manifestUrl\)/u);
  assert.match(page, /entryFromManifestPath\(record\.input, manifestUrl\)/u);
});

test("report schema defines complete SHA-256 font identities", async () => {
  const schemaPath = path.join(repositoryRoot, "docs", "report-schema-v1.json");
  const schema = JSON.parse(await readFile(schemaPath, "utf8"));
  assert.ok(schema.$defs.file.required.includes("fontIdentities"));
  assert.deepEqual(schema.$defs.fontIdentity.required, ["path", "bytes", "sha256", "error"]);
  assert.equal(schema.$defs.fontIdentity.properties.sha256.pattern, "^[0-9a-f]{64}$");
  assert.deepEqual(schema.$defs.fontIdentity.properties.error.type, ["string", "null"]);
});
