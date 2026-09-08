# Releasing SVG Squisher

The release workflow publishes only a tag whose name exactly matches `v` plus the CMake project version and whose commit is an ancestor of `origin/main`. For example, `project(svg_squisher VERSION 0.2.0 ...)` must be released as `v0.2.0` from a commit already on `main`.

## Before tagging

1. Update the CMake project version and any release-facing documentation in the same reviewed commit.
2. Run a Release build and every CTest locally.
3. Run the sanitizer suite with `SVG_SQUISHER_ENABLE_SANITIZERS=ON` on GCC or Clang.
4. Run `npm ci`, `npm test`, and `npm run test:visual` with `SVG_SQUISHER_BIN` pointing to that Release executable.
5. Confirm the support matrix describes any newly supported or newly bounded SVG behavior.
6. Confirm every compiled dependency appears in `THIRD_PARTY_NOTICES.md` with its corresponding license copy.
7. Review the comparison report for representative production assets. A smaller byte count does not substitute for visual fidelity.

## Apple release credentials

The two macOS jobs require all six repository Actions secrets below. A missing secret fails packaging; the workflow never falls back to an unsigned macOS asset.

| Secret | Value |
| --- | --- |
| `APPLE_DEVELOPER_ID_APPLICATION_CERTIFICATE_BASE64` | Base64-encoded Developer ID Application `.p12` certificate and private key. |
| `APPLE_DEVELOPER_ID_APPLICATION_CERTIFICATE_PASSWORD` | Password used when the `.p12` was exported. |
| `APPLE_DEVELOPER_ID_APPLICATION_IDENTITY` | Full signing identity, such as `Developer ID Application: Example, Inc. (TEAMID)`. |
| `APPLE_NOTARY_KEY_ID` | App Store Connect API key ID. |
| `APPLE_NOTARY_ISSUER_ID` | App Store Connect API issuer ID. |
| `APPLE_NOTARY_PRIVATE_KEY_BASE64` | Base64-encoded `AuthKey_<KEY_ID>.p8` private key authorized for notarization. |

Create the two encoded values without line wrapping with `openssl base64 -A -in DeveloperIDApplication.p12` and `openssl base64 -A -in AuthKey_<KEY_ID>.p8`. Keep the source credential files outside the repository. Configure tag protection and restrict release-secret administration to release maintainers.

## Publish

Create and push the exact version tag after the target commit is on `main`:

```sh
git tag -a v0.2.0 -m "SVG Squisher 0.2.0"
git push origin v0.2.0
```

The workflow then:

1. verifies that the tagged commit belongs to `origin/main` and that the tag matches `CMakeLists.txt`;
2. invokes the complete build workflow against the tag: four warnings-as-errors native builds and CTest runs, Linux ASan/UBSan tests, a bounded libFuzzer corpus run, JavaScript harness tests, and visual regression tests;
3. rebuilds and runs CTest on each release platform before packaging;
4. signs the macOS executable with hardened runtime and a timestamp, creates and signs a disk image, submits that finished image to Apple, requires an `Accepted` result, and staples and validates the ticket;
5. stages the executable, README, project license, third-party notices, and dependency license copies;
6. extracts or mounts every finished asset, verifies macOS signatures and Gatekeeper assessment, and performs a strict conversion with the packaged executable;
7. uploads the four tested assets, generates `SHA256SUMS.txt`, and creates the GitHub Release.

If verification, a quality gate, signing, notarization, packaging, extraction, mounting, or conversion fails, the publish job does not run.

## After publish

Download the release assets from GitHub and verify their names and checksums against `SHA256SUMS.txt`. The workflow smoke test proves that each asset was internally usable on its build runner. It does not replace a clean-machine test for the actual deployment environment.

Do not rename an asset to imply a lower OS baseline or a different architecture. Add that target to the workflow and validate it first.
