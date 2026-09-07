import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { Resvg } from "@resvg/resvg-js";
import pixelmatch from "pixelmatch";
import { PNG } from "pngjs";

const repository = resolve(dirname(fileURLToPath(import.meta.url)), "..");

function usage() {
  return `Usage:
  node benchmarks/benchmark.mjs --corpus <directory> [options]

Options:
  --squisher <path>    SVG Squisher executable (default: SVG_SQUISHER_BIN or build/svg_squisher)
  --svgo <path>        Optional SVGO executable; invoked as: svgo input.svg -o output.svg
  --usvg <path>        Optional usvg executable; invoked as: usvg input.svg output.svg
  --output <path>      JSON report path (default: benchmark-report.json)
  --iterations <n>     Measured conversions per input (default: 5)
  --warmups <n>        Warm-up conversions per input (default: 1)
  --precision <0-15>   SVG Squisher output precision (default: 6)
  --font <path>        Authoritative font passed to SVG Squisher and the renderer
  --strict             Run SVG Squisher in strict mode
  --max-diff <ratio>   Fail if any rendered changed-pixel ratio exceeds this 0..1 value
  --help               Show this help
`;
}

function positiveInteger(value, option, allowZero = false) {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < (allowZero ? 0 : 1)) {
    throw new Error(`${option} requires ${allowZero ? "a non-negative" : "a positive"} integer`);
  }
  return parsed;
}

function ratio(value, option) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < 0 || parsed > 1) {
    throw new Error(`${option} requires a number from 0 to 1`);
  }
  return parsed;
}

function parseArguments(argv) {
  const options = {
    squisher: process.env.SVG_SQUISHER_BIN || join(repository, "build", "svg_squisher"),
    output: resolve("benchmark-report.json"),
    iterations: 5,
    warmups: 1,
    precision: 6,
    strict: false,
    maxDiff: null,
  };

  const valueFor = (index, option) => {
    if (index + 1 >= argv.length) throw new Error(`${option} requires a value`);
    return argv[index + 1];
  };

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--help") options.help = true;
    else if (argument === "--strict") options.strict = true;
    else if (argument === "--corpus") options.corpus = resolve(valueFor(index++, argument));
    else if (argument === "--squisher") options.squisher = valueFor(index++, argument);
    else if (argument === "--svgo") options.svgo = valueFor(index++, argument);
    else if (argument === "--usvg") options.usvg = valueFor(index++, argument);
    else if (argument === "--output") options.output = resolve(valueFor(index++, argument));
    else if (argument === "--font") options.font = resolve(valueFor(index++, argument));
    else if (argument === "--iterations") {
      options.iterations = positiveInteger(valueFor(index++, argument), argument);
    } else if (argument === "--warmups") {
      options.warmups = positiveInteger(valueFor(index++, argument), argument, true);
    } else if (argument === "--precision") {
      options.precision = positiveInteger(valueFor(index++, argument), argument, true);
      if (options.precision > 15) throw new Error("--precision requires an integer from 0 to 15");
    } else if (argument === "--max-diff") {
      options.maxDiff = ratio(valueFor(index++, argument), argument);
    } else {
      throw new Error(`Unknown option: ${argument}`);
    }
  }
  return options;
}

function collectSvgFiles(directory) {
  const files = [];
  const visit = (current) => {
    const entries = readdirSync(current, { withFileTypes: true })
      .sort((left, right) => left.name.localeCompare(right.name, "en"));
    for (const entry of entries) {
      const path = join(current, entry.name);
      if (entry.isDirectory()) visit(path);
      else if (entry.isFile() && entry.name.toLowerCase().endsWith(".svg")) files.push(path);
    }
  };
  visit(directory);
  return files;
}

function sha256(data) {
  return createHash("sha256").update(data).digest("hex");
}

function median(values) {
  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0
    ? (sorted[middle - 1] + sorted[middle]) / 2
    : sorted[middle];
}

function executableInvocation(tool, input, output, options) {
  if (tool.name === "svg-squisher") {
    const args = [input, output, "--precision", String(options.precision)];
    if (options.strict) args.push("--strict");
    if (options.font) args.push("--font", options.font);
    return args;
  }
  if (tool.name === "svgo") return [input, "-o", output];
  return [input, output];
}

function measuredSpawn(executable, args) {
  const started = process.hrtime.bigint();
  let command = executable;
  let commandArgs = args;
  let memoryParser = null;

  if (existsSync("/usr/bin/time") && process.platform === "linux") {
    command = "/usr/bin/time";
    commandArgs = ["-f", "SVG_SQUISHER_BENCH_MAX_RSS_KB=%M", executable, ...args];
    memoryParser = (stderr) => {
      const match = stderr.match(/SVG_SQUISHER_BENCH_MAX_RSS_KB=(\d+)/);
      return match ? Number(match[1]) * 1024 : null;
    };
  } else if (existsSync("/usr/bin/time") && process.platform === "darwin") {
    command = "/usr/bin/time";
    commandArgs = ["-l", executable, ...args];
    memoryParser = (stderr) => {
      const match = stderr.match(/(\d+)\s+maximum resident set size/);
      return match ? Number(match[1]) : null;
    };
  }

  const result = spawnSync(command, commandArgs, {
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
    timeout: 30_000,
  });
  const durationMs = Number(process.hrtime.bigint() - started) / 1_000_000;
  const stderr = result.stderr || "";
  return {
    durationMs,
    peakRssBytes: memoryParser ? memoryParser(stderr) : null,
    status: result.status,
    signal: result.signal,
    error: result.error?.message || null,
    stderr,
  };
}

function render(svg, background, width, font) {
  const fontOptions = font
    ? { loadSystemFonts: false, fontFiles: [font] }
    : { loadSystemFonts: false };
  return PNG.sync.read(new Resvg(svg, {
    background,
    fitTo: { mode: "width", value: width },
    font: fontOptions,
  }).render().asPng());
}

function visualDifference(inputSvg, outputSvg, font) {
  let changed = 0;
  let pixels = 0;
  for (const width of [32, 512]) {
    for (const background of ["#ffffff", "#17191d"]) {
      const input = render(inputSvg, background, width, font);
      const output = render(outputSvg, background, width, font);
      if (input.width !== output.width || input.height !== output.height) return 1;
      const count = pixelmatch(
        input.data,
        output.data,
        null,
        input.width,
        input.height,
        { threshold: 0.1, includeAA: false },
      );
      changed += count;
      pixels += input.width * input.height;
    }
  }
  return pixels === 0 ? 0 : changed / pixels;
}

function geometryComplexity(svg) {
  const source = svg.toString("utf8");
  const paths = [...source.matchAll(/<path\b[^>]*\bd\s*=\s*(["'])(.*?)\1/gs)];
  return {
    paths: paths.length,
    pathCommands: paths.reduce(
      (total, match) => total + (match[2].match(/[AaCcHhLlMmQqSsTtVvZz]/g)?.length || 0),
      0,
    ),
  };
}

function runTool(tool, input, output, options) {
  const durations = [];
  const peakMemory = [];
  const hashes = [];
  let lastOutput = null;

  for (let iteration = -options.warmups; iteration < options.iterations; iteration += 1) {
    rmSync(output, { force: true });
    const result = measuredSpawn(
      tool.executable,
      executableInvocation(tool, input, output, options),
    );
    if (result.status !== 0 || result.signal || result.error || !existsSync(output)) {
      const detail = result.error || result.signal || result.stderr.trim() || `exit ${result.status}`;
      return { status: "failed", error: detail };
    }
    const data = readFileSync(output);
    if (iteration >= 0) {
      durations.push(result.durationMs);
      if (result.peakRssBytes !== null) peakMemory.push(result.peakRssBytes);
      hashes.push(sha256(data));
    }
    lastOutput = data;
  }

  const inputSvg = readFileSync(input);
  return {
    status: "converted",
    inputBytes: inputSvg.length,
    outputBytes: lastOutput.length,
    byteRatio: lastOutput.length / inputSvg.length,
    ...geometryComplexity(lastOutput),
    medianDurationMs: median(durations),
    minDurationMs: Math.min(...durations),
    maxDurationMs: Math.max(...durations),
    peakRssBytes: peakMemory.length === 0 ? null : Math.max(...peakMemory),
    deterministic: new Set(hashes).size === 1,
    outputSha256: hashes.at(-1),
    changedPixelRatio: visualDifference(inputSvg, lastOutput, options.font),
    error: null,
  };
}

function summarize(files, toolName) {
  const converted = files.map((file) => file.results[toolName]).filter((result) => result.status === "converted");
  return {
    converted: converted.length,
    failed: files.length - converted.length,
    inputBytes: converted.reduce((total, result) => total + result.inputBytes, 0),
    outputBytes: converted.reduce((total, result) => total + result.outputBytes, 0),
    medianDurationMs: converted.length === 0
      ? null
      : median(converted.map((result) => result.medianDurationMs)),
    maxPeakRssBytes: converted.every((result) => result.peakRssBytes === null)
      ? null
      : Math.max(...converted.map((result) => result.peakRssBytes || 0)),
    maxChangedPixelRatio: converted.length === 0
      ? null
      : Math.max(...converted.map((result) => result.changedPixelRatio)),
    deterministic: converted.every((result) => result.deterministic),
  };
}

function main() {
  const options = parseArguments(process.argv.slice(2));
  if (options.help) {
    process.stdout.write(usage());
    return;
  }
  if (!options.corpus) throw new Error("--corpus is required");
  if (!statSync(options.corpus).isDirectory()) throw new Error("--corpus must name a directory");
  if (options.font && !existsSync(options.font)) throw new Error(`Font does not exist: ${options.font}`);

  const tools = [
    { name: "svg-squisher", executable: options.squisher },
    ...(options.svgo ? [{ name: "svgo", executable: options.svgo }] : []),
    ...(options.usvg ? [{ name: "usvg", executable: options.usvg }] : []),
  ];
  const inputs = collectSvgFiles(options.corpus);
  if (inputs.length === 0) throw new Error(`No SVG files found under ${options.corpus}`);

  const work = mkdtempSync(join(tmpdir(), "svg-squisher-benchmark-"));
  const files = [];
  try {
    for (const input of inputs) {
      const key = relative(options.corpus, input).split("\\").join("/");
      const file = { input: key, results: {} };
      for (const tool of tools) {
        const output = join(work, `${tools.indexOf(tool)}-${files.length}-${basename(input)}`);
        const result = runTool(tool, input, output, options);
        file.results[tool.name] = result;
        const ratioText = result.status === "converted"
          ? `${(result.changedPixelRatio * 100).toFixed(4)}% diff, ${result.outputBytes} bytes`
          : result.error;
        console.log(`${tool.name}: ${key}: ${result.status} (${ratioText})`);
      }
      files.push(file);
    }
  } finally {
    rmSync(work, { recursive: true, force: true });
  }

  const report = {
    schemaVersion: 1,
    generatedAt: new Date().toISOString(),
    environment: { platform: process.platform, architecture: process.arch, node: process.version },
    configuration: {
      corpus: options.corpus,
      iterations: options.iterations,
      warmups: options.warmups,
      precision: options.precision,
      strict: options.strict,
      font: options.font || null,
      renderWidths: [32, 512],
      renderBackgrounds: ["#ffffff", "#17191d"],
      maxDiff: options.maxDiff,
    },
    tools: Object.fromEntries(tools.map((tool) => [tool.name, { executable: tool.executable }])),
    summary: Object.fromEntries(tools.map((tool) => [tool.name, summarize(files, tool.name)])),
    files,
  };
  writeFileSync(options.output, `${JSON.stringify(report, null, 2)}\n`);
  console.log(`Wrote ${options.output}`);

  const failures = files.some((file) => Object.values(file.results).some((result) =>
    result.status !== "converted" || !result.deterministic ||
    (options.maxDiff !== null && result.changedPixelRatio > options.maxDiff)));
  if (failures) process.exitCode = 1;
}

try {
  main();
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
