#include <iostream>
#include <string>

#include "svg_report.h"
#include "svg_squisher.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool has_diagnostic(const svg_squisher::ConversionResult& result,
                    const std::string& code) {
  for (const svg_squisher::Diagnostic& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string diagnostic_message(const svg_squisher::ConversionResult& result,
                               const std::string& code) {
  for (const svg_squisher::Diagnostic& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return diagnostic.message;
  }
  return "";
}

std::string gradient_reference_chain(std::size_t count) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\"><defs>"
      "<linearGradient id=\"g0\"><stop stop-color=\"red\"/></linearGradient>";
  for (std::size_t index = 1; index < count; ++index) {
    svg += "<linearGradient id=\"g" + std::to_string(index) +
           "\" href=\"#g" + std::to_string(index - 1) + "\"/>";
  }
  svg += "</defs><rect width=\"20\" height=\"20\" fill=\"url(#g" +
         std::to_string(count - 1) + ")\"/></svg>";
  return svg;
}

}  // namespace

int main() {
  using svg_squisher::ConversionPolicy;
  using svg_squisher::Options;
  using svg_squisher::SvgSquisher;

  const std::string dashed =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<path d=\"M1 10H19\" fill=\"none\" stroke=\"black\" "
      "stroke-width=\"2\" stroke-dasharray=\"2 2\"/>"
      "</svg>";
  SvgSquisher squisher;

  const auto preserved = squisher.convert_string(dashed);
  expect(preserved.success && !has_diagnostic(preserved, "live-stroke-retained"),
         "preserve-appearance remains the compatible default");
  expect(preserved.svg.find("stroke-dasharray=\"2 2\"") != std::string::npos,
         "preserve-appearance keeps a dashed live stroke");

  Options filled;
  filled.conversion_policy = ConversionPolicy::FilledPaths;
  const auto compatible_filled = squisher.convert_string(dashed, filled);
  expect(compatible_filled.success &&
         has_diagnostic(compatible_filled, "live-stroke-retained"),
         "compatible filled-path conversion diagnoses its live-stroke fallback");
  expect(diagnostic_message(compatible_filled, "live-stroke-retained").find(
             "retained the stroke as live SVG stroke attributes") != std::string::npos,
         "the filled-path fallback warning states the compatible-mode action");

  filled.strict = true;
  const auto strict_filled = squisher.convert_string(dashed, filled);
  expect(!strict_filled.success && strict_filled.svg.empty() &&
         strict_filled.error.find("live-stroke-retained") != std::string::npos,
         "strict filled-path conversion rejects a retained live stroke before output");

  const std::string solid =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<path d=\"M1 10H19\" fill=\"none\" stroke=\"black\" stroke-width=\"2\"/>"
      "</svg>";
  const auto strict_solid = squisher.convert_string(solid, filled);
  expect(strict_solid.success && !has_diagnostic(strict_solid, "live-stroke-retained"),
         "strict filled-path conversion accepts a stroke that can be outlined");
  expect(strict_solid.svg.find(" stroke=") == std::string::npos,
         "filled-path output serializes a supported solid stroke as fill geometry");

  const std::string zero_length_butt =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<path d=\"M10 10L10 10\" fill=\"none\" stroke=\"black\" "
      "stroke-width=\"6\" stroke-linecap=\"butt\"/>"
      "</svg>";
  const auto strict_zero_length_butt =
      squisher.convert_string(zero_length_butt, filled);
  expect(strict_zero_length_butt.success &&
             strict_zero_length_butt.stats.output_paths == 0 &&
             !has_diagnostic(strict_zero_length_butt, "live-stroke-retained"),
         "strict filled-path conversion treats a zero-length butt stroke as empty");

  const std::string patterned =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><line x2=\"4\" y2=\"4\" "
      "stroke=\"black\"/></pattern></defs><rect width=\"20\" height=\"20\" "
      "fill=\"url(#p)\"/></svg>";
  filled.strict = false;
  const auto compatible_pattern = squisher.convert_string(patterned, filled);
  expect(compatible_pattern.success &&
         has_diagnostic(compatible_pattern, "live-stroke-retained"),
         "filled-path conversion diagnoses live strokes copied through paint definitions");
  filled.strict = true;
  const auto strict_pattern = squisher.convert_string(patterned, filled);
  expect(!strict_pattern.success && strict_pattern.svg.empty() &&
         strict_pattern.error.find("live-stroke-retained") != std::string::npos,
         "strict filled-path conversion rejects a live stroke in a retained definition");

  const std::string fill_only_pattern =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><rect width=\"4\" height=\"4\" "
      "fill=\"blue\"/></pattern></defs><rect width=\"20\" height=\"20\" "
      "fill=\"url(#p)\"/></svg>";
  const auto strict_fill_only_pattern = squisher.convert_string(fill_only_pattern, filled);
  expect(strict_fill_only_pattern.success &&
         !has_diagnostic(strict_fill_only_pattern, "live-stroke-retained"),
         "strict filled-path conversion accepts a retained fill-only pattern");

  const std::string non_painting_pattern_stroke =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><line x2=\"4\" y2=\"4\" "
      "stroke=\"black\" stroke-opacity=\"0\"/></pattern></defs>"
      "<rect width=\"20\" height=\"20\" fill=\"url(#p)\"/></svg>";
  const auto strict_non_painting_stroke =
      squisher.convert_string(non_painting_pattern_stroke, filled);
  expect(strict_non_painting_stroke.success &&
         !has_diagnostic(strict_non_painting_stroke, "live-stroke-retained"),
         "strict filled-path conversion ignores a non-painting retained stroke");

  const std::string zero_geometry_pattern =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><circle r=\"0\" fill=\"none\" "
      "stroke=\"black\"/></pattern></defs><rect width=\"20\" height=\"20\" "
      "fill=\"url(#p)\"/></svg>";
  const auto strict_zero_geometry = squisher.convert_string(zero_geometry_pattern, filled);
  expect(strict_zero_geometry.success &&
         !has_diagnostic(strict_zero_geometry, "live-stroke-retained"),
         "strict filled-path conversion ignores a retained stroke with no geometry");

  const std::string compact_points_pattern =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><polyline points=\"0-4 4-0\" "
      "fill=\"none\" stroke=\"black\"/></pattern></defs>"
      "<rect width=\"20\" height=\"20\" fill=\"url(#p)\"/></svg>";
  const auto strict_compact_points =
      squisher.convert_string(compact_points_pattern, filled);
  expect(!strict_compact_points.success &&
         has_diagnostic(strict_compact_points, "live-stroke-retained"),
         "strict retained-stroke detection accepts compact points grammar");

  const std::string empty_use_pattern =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><g id=\"empty\"/><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><use href=\"#empty\" stroke=\"red\" "
      "stroke-width=\"2\"/></pattern></defs><rect width=\"20\" height=\"20\" "
      "fill=\"url(#p)\"/></svg>";
  const auto strict_empty_use = squisher.convert_string(empty_use_pattern, filled);
  expect(strict_empty_use.success &&
         !has_diagnostic(strict_empty_use, "live-stroke-retained"),
         "strict filled-path conversion resolves an empty local use without a false live stroke");

  const std::string zero_scale_pattern =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><g transform=\"scale(0)\"><line x2=\"4\" "
      "stroke=\"red\" stroke-width=\"2\"/></g></pattern></defs>"
      "<rect width=\"20\" height=\"20\" fill=\"url(#p)\"/></svg>";
  const auto strict_zero_scale = squisher.convert_string(zero_scale_pattern, filled);
  expect(strict_zero_scale.success &&
         !has_diagnostic(strict_zero_scale, "live-stroke-retained"),
         "strict filled-path conversion ignores strokes below a composed zero-scale transform");

  const std::string scaled_tiny_stroke =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\">"
      "<defs><pattern id=\"p\" width=\"4\" height=\"4\" "
      "patternUnits=\"userSpaceOnUse\"><g transform=\"scale(1000000000000)\">"
      "<path d=\"M0 0L1e-12 0\" fill=\"none\" stroke=\"red\" "
      "stroke-width=\"1e-12\"/></g></pattern></defs><rect width=\"20\" height=\"20\" "
      "fill=\"url(#p)\"/></svg>";
  const auto strict_scaled_tiny_stroke = squisher.convert_string(scaled_tiny_stroke, filled);
  expect(!strict_scaled_tiny_stroke.success &&
         has_diagnostic(strict_scaled_tiny_stroke, "live-stroke-retained"),
         "strict filled-path conversion detects tiny source geometry and width scaled to visible output");

  const std::string inherited_use_stroke =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">"
      "<defs><path id=\"mark\" d=\"M2 2L8 8\" fill=\"none\"/>"
      "<pattern id=\"pat\" width=\"10\" height=\"10\" patternUnits=\"userSpaceOnUse\">"
      "<use href=\"#mark\" stroke=\"red\" stroke-width=\"2\"/></pattern></defs>"
      "<rect width=\"10\" height=\"10\" fill=\"url(#pat)\"/></svg>";
  filled.strict = false;
  const auto compatible_inherited_use_stroke =
      squisher.convert_string(inherited_use_stroke, filled);
  expect(compatible_inherited_use_stroke.success &&
         has_diagnostic(compatible_inherited_use_stroke, "live-stroke-retained"),
         "filled-path conversion resolves stroke inherited by referenced use geometry");
  const std::size_t mark_start = compatible_inherited_use_stroke.svg.find("<path id=\"mark\"");
  const std::size_t mark_end = compatible_inherited_use_stroke.svg.find("/>", mark_start);
  expect(compatible_inherited_use_stroke.svg.find("stroke=\"red\"") != std::string::npos &&
         mark_start != std::string::npos && mark_end != std::string::npos &&
         compatible_inherited_use_stroke.svg.find("stroke=", mark_start) > mark_end,
         "materialized use targets keep use-supplied inherited stroke paint");
  filled.strict = true;
  const auto strict_inherited_use_stroke = squisher.convert_string(inherited_use_stroke, filled);
  expect(!strict_inherited_use_stroke.success &&
         has_diagnostic(strict_inherited_use_stroke, "live-stroke-retained"),
         "strict filled-path conversion rejects use-inherited live stroke geometry");

  const std::string external_definition_paint =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">"
      "<defs><pattern id=\"p\" width=\"10\" height=\"10\" "
      "patternUnits=\"userSpaceOnUse\"><rect width=\"10\" height=\"10\" "
      "fill=\"url(https://evil.invalid/paint.svg#p)\"/></pattern></defs>"
      "<rect width=\"10\" height=\"10\" fill=\"url(#p)\"/></svg>";
  filled.strict = false;
  const auto compatible_external_definition_paint =
      squisher.convert_string(external_definition_paint, filled);
  expect(compatible_external_definition_paint.success &&
         compatible_external_definition_paint.svg.find("evil.invalid") == std::string::npos &&
         compatible_external_definition_paint.svg.find("fill=\"none\"") != std::string::npos,
         "sanitized retained definitions replace external paint with an explicit none fallback");

  const std::string escaped_external_definition_paint = R"SVG(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">
      <defs><pattern id="escaped" width="10" height="10"
        patternUnits="userSpaceOnUse"><rect width="10" height="10"
        fill="\75\72\6c(http://127.0.0.1:47931/retained.svg)"/></pattern></defs>
      <rect width="10" height="10" fill="url(#escaped)"/>
    </svg>
  )SVG";
  const auto compatible_escaped_definition_paint =
      squisher.convert_string(escaped_external_definition_paint, filled);
  expect(compatible_escaped_definition_paint.success &&
         has_diagnostic(
           compatible_escaped_definition_paint, "unsupported-external-reference") &&
         compatible_escaped_definition_paint.svg.find("127.0.0.1") ==
           std::string::npos &&
         compatible_escaped_definition_paint.svg.find("fill=\"none\"") !=
           std::string::npos,
         "retained definitions remove escaped external URL paints before serialization");
  filled.strict = true;

  constexpr std::size_t chain_size = 1000;
  const auto long_definition_chain =
      squisher.convert_string(gradient_reference_chain(chain_size), filled);
  expect(long_definition_chain.success &&
         long_definition_chain.svg.find("id=\"g0\"") != std::string::npos &&
         long_definition_chain.svg.find("id=\"g999\"") != std::string::npos,
         "retained-definition closure processes a long dependency chain once per id");

  svg_squisher::BatchResult batch;
  const std::string report = svg_squisher::render_json_report(batch, filled);
  expect(report.find("\"conversionPolicy\":\"filled-paths\"") != std::string::npos,
         "reports record the selected conversion policy exactly");
  expect(std::string(svg_squisher::conversion_policy_name(
             ConversionPolicy::PreserveAppearance)) == "preserve-appearance",
         "the public policy name is stable for API integrations");

  if (failures != 0) return 1;
  std::cout << "output policy tests passed\n";
  return 0;
}
