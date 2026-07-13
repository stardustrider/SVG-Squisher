# SVG Squisher

SVG Squisher is a native C++ SVG conversion tool for simplifying icon-style SVGs into clean output SVGs. It supports path and primitive conversion, text-to-path rendering, stroke flattening, transform handling, and batch processing for folders of SVG files.

<a href="https://www.buymeacoffee.com/thestarduster" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174"></a>

## Features

- Native C++17 executable
- `pugixml` for SVG/XML parsing
- `FreeType` for text-to-path conversion
- Single-file and directory conversion
- Optional flattened output fill override
- Support for common SVG content:
  - `path`
  - `rect`
  - `circle`
  - `ellipse`
  - `line`
  - `polyline`
  - `polygon`
  - `text`
  - `tspan`
  - `use`
- Style inheritance, transforms, stroke handling, and icon-oriented cleanup heuristics

## Build

This project uses CMake and fetches `pugixml` and `FreeType` during configure.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Usage

Convert a single file:

```powershell
.\build\Release\svg_squisher.exe input.svg output.svg
```

Convert a directory:

```powershell
.\build\Release\svg_squisher.exe input-folder output-folder
```

Flatten all output to a single fill color:

```powershell
.\build\Release\svg_squisher.exe input-folder output-folder --fill white
```

Provide an explicit font for text conversion:

```powershell
.\build\Release\svg_squisher.exe input.svg output.svg --font C:\Windows\Fonts\arial.ttf
```

## Visual Compare

An input-vs-output comparison page is included at `compare.html`.

![SVG Squisher Compare interface listing SVG files in a left column with matching input and output preview cards side by side for each file.](assets/img_readme_compare.png) 
*Screenshot includes Unreal Engine icons and interface elements. Portions of the materials used are trademarks and/or copyrighted works of Epic Games, Inc. All rights reserved by Epic. This material is not official and is not endorsed by Epic.*

By default it compares:

- `svgs/`
- `svgs-out/`

Serve the repo with a static file server, then open the compare page:

```powershell
python -m http.server 8080
```

Then browse to:

```text
http://127.0.0.1:8080/compare.html
```

You can also override the folders with query parameters:

```text
http://127.0.0.1:8080/compare.html?input=svgs&output=svgs-out
```

The preview size is also configurable from the page or via query parameter:

```text
http://127.0.0.1:8080/compare.html?input=svgs&output=svgs-out&size=180
```

## Repository Layout

- `src/`
  - converter pipeline and native modules
- `CMakeLists.txt`
  - build configuration
- `.github/workflows/build.yml`
  - CI build workflow
- `.github/workflows/release.yml`
  - tag-driven release workflow for packaged binaries

## Releases

Push a version tag to automatically build and publish release binaries on GitHub:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The release workflow publishes:

- `svg-squisher-windows-x64.zip`
- `svg-squisher-linux-x64.tar.gz`

## Status

The project is organized into focused native modules for:

- style resolution
- computed style analysis
- transforms
- paint handling
- geometry
- path processing
- shape conversion
- stroke generation
- text conversion
- DOM traversal
- postprocessing
- SVG output serialization

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
