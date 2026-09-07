# Releasing SVG Squisher

The release workflow publishes only a tag whose name exactly matches `v` plus the CMake project version. For example, `project(svg_squisher VERSION 0.2.0 ...)` must be released as `v0.2.0`.

## Before tagging

1. Update the CMake project version and any release-facing documentation in the same reviewed commit.
2. Run a Release build and every CTest locally.
3. Run the sanitizer suite with `SVG_SQUISHER_ENABLE_SANITIZERS=ON` on GCC or Clang.
4. Run `npm ci` and `npm run test:visual` with `SVG_SQUISHER_BIN` pointing to that Release executable.
5. Confirm the support matrix describes any newly supported or newly bounded SVG behavior.
6. Confirm every compiled dependency appears in `THIRD_PARTY_NOTICES.md` with its corresponding license copy.
7. Review the comparison report for representative production assets. A smaller byte count does not substitute for visual fidelity.

## Publish

Create and push the exact version tag after the target commit is on `main`:

```sh
git tag -a v0.2.0 -m "SVG Squisher 0.2.0"
git push origin v0.2.0
```

The workflow then:

1. validates the tag against `CMakeLists.txt`;
2. builds Release on Ubuntu 24.04 x86-64, Windows Server 2025 x86-64, macOS 15 arm64, and macOS 15 Intel;
3. runs the complete CTest suite on every target;
4. stages the executable, README, project license, third-party notices, and dependency license copies;
5. creates an archive whose name includes version, platform baseline, and architecture;
6. removes the staging directory, extracts the archive, and uses that extracted executable for a strict fixture conversion;
7. uploads the four archives, generates `SHA256SUMS.txt`, and creates the GitHub Release.

If a matrix build, test, package, extraction, or conversion fails, the publish job does not run.

## After publish

Download the release assets from GitHub and verify their names and checksums against `SHA256SUMS.txt`. The workflow smoke test proves that each archive was internally usable on its build runner. It does not replace a clean-machine test for the actual deployment environment.

Do not rename an archive to imply a lower OS baseline or a different architecture. Add that target to the workflow and validate it first.
