# SVG support and conversion contract

SVG Squisher targets static, icon-oriented artwork that can be represented as a small set of explicit paths. It is not a browser renderer and does not claim full SVG 2 conformance.

The table uses these terms:

- **Supported**: the converter has direct implementation and automated regression coverage.
- **Partial**: a useful subset is implemented; read the boundary before relying on it.
- **Diagnosed**: the compatibility profile warns and continues with the supported remainder; strict mode rejects the file.
- **Unsupported**: the semantic is not represented. The scanner diagnoses unknown elements and unsupported inline or embedded CSS properties, but uncommon attribute syntax may not receive a dedicated diagnostic, so strict success is not a proof that every SVG feature was understood.

## Profiles

| Profile | Output behavior | Diagnostics | Destination behavior |
| --- | --- | --- | --- |
| Compatible enforcement (default) | Converts supported content and skips or simplifies known unsupported content. | Warnings go to standard error and the JSON report. | A successful supported remainder is written atomically. |
| Strict enforcement (`--strict`) | Runs capability preflight and a final diagnostic gate before serialization. | Any detected warning makes the file fail. | The destination is not created or replaced for a rejected file. |
| Preserve appearance policy (default) | `--conversion-policy preserve-appearance` permits live SVG strokes when that preserves behavior the outline builder cannot reproduce. | No policy warning is needed solely because a live stroke is retained. | Output is path-based but can use stroke attributes. |
| Filled paths policy | `--conversion-policy filled-paths` requests fill-only geometry. Supported solid strokes become outlines. | A stroke that cannot be outlined receives `live-stroke-retained`; compatible enforcement retains it and states that action, while strict enforcement rejects it. | Strict mode never writes a fallback containing the diagnosed live stroke. |
| Monochrome (`--fill`) | Replaces emitted fill and stroke paints with the requested value. | Other profile rules still apply. | Opacity and geometry remain; background cleanup is not implied. |
| Background cleanup (`--remove-background`) | Applies an icon-specific large-background heuristic after conversion. | No claim is made that a removed shape was semantically a background. | Opt in only when reviewed output is acceptable. |

Directory runs are lexicographically ordered by input path. By default they continue after failures; `--fail-fast` stops after the first failed file. `--no-overwrite` records existing destinations as skipped. Directory scans do not follow symlinks, and an SVG symlink is reported as skipped. The user-selected output root is resolved once and may itself be a symlink or reparse point. Below that resolved root, destination directories are created one component at a time; an existing symlink or reparse component is rejected as a failed file. The parent chain and temporary file are checked again immediately before atomic installation. The writer remains path based, so hostile concurrent replacement between its final validation and filesystem rename/replace call is outside its portable guarantee. In recursive mode, an existing output subtree nested below the input is excluded from enumeration, including files from earlier runs. Any failed or skipped entry makes the CLI return 1.

## Elements and geometry

| Feature | Status | Boundary |
| --- | --- | --- |
| `svg` root | Supported | Requires an XML document whose document element is `svg`. Width, height, viewBox, preserveAspectRatio, root transform, id, role, aria-label, aria-labelledby, aria-describedby, aria-hidden, focusable, lang/xml:lang, and direct `title`/`desc` metadata receive explicit handling. Other root metadata is not retained. |
| Structural nesting | Bounded | At most 256 source element levels are accepted, counting the root `svg` as level 1. A deeper document fails conversion during an iterative guard, before XPath queries, recursive diagnostics, reference lookup, or geometry traversal. |
| `path` | Supported | Commands `M/L/H/V/C/S/Q/T/A/Z`, relative forms, repeated coordinate groups, and compact numeric syntax enter the shared parser. Malformed data is diagnosed and skipped without a parser stall. |
| `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon` | Supported | Shapes become path data. Rounded rectangles are supported by the shape converter. |
| Container groups | Supported | `g` and ordinary nested containers contribute inherited style and transforms. Group compositing remains limited as described below. |
| `defs` | Partial | Definition children are not painted directly. Referenced local geometry can be expanded; referenced paint definitions needed by emitted URL paints are copied through the sanitizer. Unused definitions are omitted. |
| Local `use` | Supported within limits | Fragment references to local geometry/groups expand with cycle detection. Both `href` and `xlink:href` are accepted. Expansion is bounded to 64 reference hops, 256 combined source/reference element levels, 16,384 expanded element visits, and 8,192 actually emitted output paths. Reference, depth, and expanded-node limits are preflight checks. The output-path limit is enforced during traversal so hidden and fill-only elements use their actual emission count. Compatible mode diagnoses and truncates the bounded remainder; strict mode rejects before serialization. |
| `symbol` through `use` | Partial | Explicit `use` width/height win, then explicit symbol dimensions, then omitted or `auto` dimensions resolve against the root user-space viewport (the root viewBox size, otherwise finite root width/height, with SVG's 300 by 150 initial size for an omitted or unsupported root dimension). ViewBox and common preserveAspectRatio alignment are applied. The default/hidden symbol viewport clip is omitted only when all generated fill geometry is proven contained; otherwise compatible mode emits `unsupported-symbol-viewport-clipping` and leaves overflow visible, while strict mode rejects. `overflow="visible"` opts into unclipped output. A symbol nested inside another symbol must set explicit instance or symbol dimensions when it cannot use the root viewport fallback. Source `x`/`y` on a referenced symbol receives `unsupported-symbol-position`: compatible mode ignores it and uses the instance position from `use`, while strict mode rejects. |
| Nested `svg` | Diagnosed | Nested viewport and preserveAspectRatio semantics are not fully resolved. |
| `image`, `foreignObject` | Diagnosed | Raster, HTML, and other embedded document content is not converted. |
| `script` | Diagnosed | Active content receives `unsupported-script`, is never copied to output definitions, and makes strict conversion fail even when unreferenced. |
| `switch` | Diagnosed | Conditional processing is not evaluated. |
| Animation elements | Diagnosed | Static output does not represent animation or its runtime state. |
| Unknown elements | Diagnosed | Elements outside the scanner's known SVG set receive `unsupported-element` and do not become geometry. Known definition/filter elements still rely on the feature-specific boundaries below. |

Invalid XML, a missing `svg` root, more than 256 nested element levels, I/O errors, and an out-of-range precision are conversion errors rather than compatibility warnings.

## Transforms and coordinates

| Feature | Status | Boundary |
| --- | --- | --- |
| `matrix`, `translate`, `scale`, `rotate`, `skewX`, `skewY` | Supported | Transform lists are composed through the element tree. Geometry is baked when safe; transforms remain live where stroke or URL-paint behavior requires it. |
| Root `viewBox` | Supported | Exactly four general SVG numbers are required. Origins are preserved verbatim and emitted geometry remains in the source user coordinate system. Width and height must be non-negative; a zero dimension is valid and disables rendering. |
| Unitless and `px` lengths | Supported | Numeric parsing uses SVG user-space values. |
| Percent, font-relative, viewport-relative, and physical units | Diagnosed | These units are unsupported. Each diagnostic states the compatible action for its context: coordinates generally use 0, an invalid required geometry extent drops that geometry, text position lists are ignored while the current cursor continues, and supported CSS length properties use their documented converter default. |
| Nested viewport transforms | Diagnosed | Full nested viewport establishment is not implemented. |
| Arc transforms | Partial | Common icon transforms are supported, but arbitrary affine arc behavior should remain under visual regression for the target corpus. |

## Paint, strokes, and compositing

| Feature | Status | Boundary |
| --- | --- | --- |
| Solid fill and stroke | Supported | Paint values are emitted on explicit paths. `none` is respected. |
| `fill-rule` | Supported | Nonzero and evenodd behavior is retained on emitted fill paths. |
| `opacity`, `fill-opacity`, `stroke-opacity` on painted elements | Supported | Fill and stroke alpha remain distinct. When one source element becomes separate fill and stroke paths, an output wrapper applies element opacity once to their combined rendering. |
| Container and text expansion opacity | Diagnosed when layered | Local opacity between 0 and 1 on a container, link, text, or `tspan` expansion that produces multiple painted layers is distributed in compatible output and can change overlap compositing. It receives `group-opacity-flattened`; strict mode rejects it. Opacity around one painted element remains supported. |
| `use` instance opacity | Diagnosed when layered | Opacity between 0 and 1 on a `use` that may paint multiple referenced layers is distributed in compatible output and can change overlap compositing. It receives `use-opacity-flattened`; strict mode rejects it. Zero opacity and opacity around one painted layer do not need a compositing warning. |
| Solid strokes | Partial | Ordinary strokes become filled outline geometry. Caps, joins, miter limits, curves, and closed shapes have regression coverage, but the outline builder is an approximation rather than a browser stroker. |
| Dashed strokes | Partial | Under `preserve-appearance`, dashed geometry remains a live SVG stroke so dash behavior and transforms can be retained. Under `filled-paths`, compatible enforcement emits `live-stroke-retained` and keeps the stroke, while `--strict` rejects the fallback before serialization. |
| Gradient URL paints | Partial | Referenced definitions are copied through a safe element/attribute subset and relevant transforms can remain live. Complex paint-server inheritance and coordinate-space combinations require visual verification. |
| Patterns and local URL paint servers | Partial | Referenced definition XML is copied rather than evaluated. Supported styles are materialized first; `style`/`script`, active or unsupported child elements, event handlers, and external resource links are then removed. Visual equivalence depends on the retained safe definition and coordinate spaces, so target-renderer verification is required. |
| External paint/resource URLs | Diagnosed | CSS escapes, comments, ASCII casing, and surrounding whitespace are normalized before URL classification. Every `url(...)` target must be a provable local `#fragment`; an external or malformed target receives `unsupported-external-reference`. Compatible serialization replaces unsafe paints with `none` and removes other unsafe resource attributes, while strict mode rejects the file. Escaped local fragments remain supported and retain their referenced definition. |
| `clip-path`, `mask`, `filter` | Diagnosed | These effects are not preserved. |
| Markers | Diagnosed | Marker geometry is not expanded. Marker elements and common marker attributes are capability warnings. |
| `vector-effect`, `paint-order` | Diagnosed | Their rendering semantics are not represented by the output model. |
| `currentColor` | Diagnosed | Compatible output retains the literal paint but does not preserve the source `color` cascade, so the output renderer resolves it. Strict mode rejects the warning. |
| Blend modes and isolation | Unsupported | Group-level compositing beyond ordinary source-over paths is outside the model. |
| `stroke-dashoffset` | Diagnosed | The offset is not represented in the emitted path model. Attribute, inline-CSS, and embedded-CSS forms are capability warnings. |
| Less common paint/stroke properties | Unsupported | Unsupported declarations in inline or embedded CSS are diagnosed; uncommon presentation attributes may not receive a dedicated warning. |

## Styles

| Feature | Status | Boundary |
| --- | --- | --- |
| Presentation attributes | Supported | Supported paint, stroke, visibility, and text properties participate at author origin. |
| Inline `style` | Partial | Supported declarations participate in the cascade and outrank selector-based author declarations when importance is equal. Unsupported property names receive the same `unsupported-css-property` diagnostic as embedded rules and are ignored in compatible output. Invalid supported numeric declarations receive the converter default named by their diagnostic. |
| Embedded `style` rules | Partial | Type, class, ID, universal, and compound simple selectors are supported, including comma-separated rule expansion, specificity, source order, and `!important`. |
| Descendant/child/sibling, attribute, and pseudo selectors | Diagnosed | Rules using combinators, attribute selectors, or pseudo-classes are not resolved. |
| External stylesheets | Unsupported | External resources are not fetched. |
| CSS variables, `calc()`, media queries, and at-rules | Unsupported | Unsupported inline and embedded property names are diagnosed, but values and at-rule parsing are not a complete CSS validation pass. |
| `display: none` | Supported | The element subtree is not emitted. |
| `visibility: hidden/collapse` | Supported | Hidden painted content is not emitted while inherited visibility is resolved. |
| Inheritance keywords | Partial | `inherit`, `initial`, and `unset` are handled for the supported property set. CSS-wide behavior outside that set is not modeled. |

The supported property set is: `fill`, `fill-opacity`, `stroke`, `stroke-opacity`, `stroke-width`, `stroke-dasharray`, `stroke-linecap`, `stroke-linejoin`, `stroke-miterlimit`, `fill-rule`, `opacity`, `display`, `visibility`, `font-size`, `font-family`, `font-weight`, `font-style`, `text-anchor`, and `letter-spacing`.

## Text

| Feature | Status | Boundary |
| --- | --- | --- |
| UTF-8 input | Supported | Input is decoded to Unicode scalar values; malformed sequences use the replacement character. |
| Glyph shaping | Supported within one text run | HarfBuzz shapes ligatures and connected scripts such as Arabic. Script and direction are inferred for the run. |
| Glyph outlines | Supported | FreeType outlines become SVG path data. Bitmap-only glyphs do not produce outline geometry. |
| `text` and `tspan` document order | Supported | Text and spans are consumed in source order with SVG default whitespace collapsing and `xml:space` handling. |
| Fill and stroke text | Supported | Outlined text can retain supported fill/stroke paint semantics. |
| `font-size`, `font-family`, `font-weight`, `font-style`, `letter-spacing`, `text-anchor` | Partial | These enter the supported style model. Middle/end anchoring measures descendant runs with each run's resolved font metrics and preserves shaping across transparent `tspan` boundaries. A multi-value `x`/`y` list or descendant absolute `x`/`y` creates chunk membership that is not fully represented across element boundaries; compatible mode emits `unsupported-text-anchor-chunk-positioning`, preserves logical position-list consumption, and applies node-local anchor shifts, while strict mode rejects. Platform font discovery is deliberately narrow. |
| `x`, `y`, `dx`, `dy` lists | Partial | Valid position lists are consumed by logical character in tree order across direct text and nested `tspan` content. A child value overrides its ancestor for that character while still consuming the ancestor slot. An invalid or unsupported-unit list is ignored, so the current cursor or applicable ancestor position continues. Full mapping inside multi-codepoint shaped clusters is not implemented. |
| Authoritative `--font` | Supported | The supplied font wins over requested SVG font family/style. Face index 0 is used for font collections. |
| Automatic system font selection | Diagnosed | A short platform-specific search is used when no authoritative font is supplied. Results can differ by machine, so text with implicit selection receives a warning and strict mode requires `--font`. |
| Complex paragraph bidi | Diagnosed | Run direction inference is not a full Unicode bidi paragraph implementation. Explicit direction and unicode-bidi layout properties are capability warnings. |
| `textPath` | Diagnosed | Text-on-path placement is not implemented. |
| Vertical text | Diagnosed | Vertical writing modes and glyph orientation are outside the text model. |
| Font fallback | Unsupported | Missing glyphs are not automatically taken from another font. They produce a warning and increment the report's `missingGlyphs` count. |
| Variable-font axes and OpenType feature controls | Unsupported | Default shaping behavior is used; SVG/CSS feature and variation controls are not exposed. |

For deterministic text conversion, provide `--font` and keep the exact font file under the same licensing and build controls as the input assets. The converter shapes from an owned byte snapshot and each JSON file record includes the matching path in `fonts`, byte size and SHA-256 in `fontIdentities`, and `missingGlyphs`. An unreadable or invalid snapshot fails conversion, and every successfully shaped snapshot is reported from the exact owned bytes used for shaping. The report does not embed the font.

## Output document

Output contains a new `svg` root, sanitized retained paint definitions, and emitted `path` elements. Supported root and direct title/description identity/language metadata is retained; IDs and classes on converted graphic elements, editor metadata, scripts, event handlers, external resource URLs, arbitrary data attributes, and unreferenced definitions are intentionally absent. This makes the result easier to audit but means SVG Squisher is a converter, not a lossless XML round trip.

File writes use a temporary sibling and replacement operation. This prevents a failed conversion or incomplete write from truncating the current destination. Directory destinations additionally use the descendant-link checks described above. Atomic replacement still depends on the destination filesystem's rename/replace guarantees.

The CLI rejects input/output aliases by default, including equivalent existing paths, and requires `--in-place` before replacing an input. A directory conversion report must remain outside the input and output trees; file conversions reject a report path that aliases either operand.

The automated visual suite renders both source and output on light and dark backgrounds. Passing its fixture thresholds is evidence for those cases and renderer settings only; add representative fixtures for every production corpus feature that matters.
