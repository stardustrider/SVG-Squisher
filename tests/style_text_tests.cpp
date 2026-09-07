#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "svg_squisher.h"
#include "svg_style.h"
#include "svg_text.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string svg(const std::string& body) {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
         "viewBox=\"0 0 100 100\">" + body + "</svg>";
}

std::string convert(const std::string& source,
                    const std::optional<std::string>& font = std::nullopt) {
  svg_squisher::Options options;
  options.font_path = font;
  return svg_squisher::SvgSquisher{}.squish_string(source, options);
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t cursor = 0;
  while ((cursor = text.find(needle, cursor)) != std::string::npos) {
    ++count;
    cursor += needle.size();
  }
  return count;
}

std::string concatenated_path_data(const std::string& output) {
  std::string paths;
  std::size_t cursor = 0;
  constexpr const char* marker = "<path d=\"";
  while ((cursor = output.find(marker, cursor)) != std::string::npos) {
    cursor += std::char_traits<char>::length(marker);
    const std::size_t end = output.find('"', cursor);
    if (end == std::string::npos) break;
    paths.append(output, cursor, end - cursor);
    cursor = end + 1;
  }
  return paths;
}

void test_css_cascade() {
  const std::string inline_output = convert(svg(
    "<rect width=\"100\" height=\"100\" fill=\"red\" style=\"fill: blue\"/>"));
  expect(inline_output.find("fill=\"blue\"") != std::string::npos,
         "inline style overrides a presentation attribute");

  const std::string specificity_output = convert(svg(
    "<style>.shape { fill: green; } #target { fill: purple; }</style>"
    "<rect id=\"target\" class=\"shape\" width=\"10\" height=\"10\" fill=\"red\"/>"));
  expect(specificity_output.find("fill=\"purple\"") != std::string::npos,
         "ID specificity wins regardless of presentation attributes and class rules");

  const std::string important_output = convert(svg(
    "<style>.shape { fill: green ! important; }</style>"
    "<rect class=\"shape\" width=\"10\" height=\"10\" style=\"fill: blue\"/>"));
  expect(important_output.find("fill=\"green\"") != std::string::npos,
         "important author rule overrides a non-important inline declaration");
}

void test_opacity_and_hidden_content() {
  const std::string alpha_output = convert(svg(
    "<rect width=\"40\" height=\"40\" fill=\"red\" fill-opacity=\"0.25\"/>"
    "<path d=\"M0 60L40 60\" fill=\"none\" stroke=\"blue\" "
    "stroke-width=\"4\" stroke-opacity=\"0.4\"/>"));
  expect(alpha_output.find("fill-opacity=\"0.25\"") != std::string::npos,
         "fill opacity survives conversion");
  expect(alpha_output.find("fill-opacity=\"0.4\"") != std::string::npos ||
         alpha_output.find("stroke-opacity=\"0.4\"") != std::string::npos,
         "stroke opacity survives live or outlined stroke conversion");

  const std::string display_output = convert(svg(
    "<g display=\"none\"><rect width=\"100\" height=\"100\" fill=\"red\"/></g>"
    "<rect width=\"10\" height=\"10\" fill=\"green\"/>"));
  expect(count_occurrences(display_output, "<path ") == 1 &&
         display_output.find("fill=\"green\"") != std::string::npos,
         "display none removes the entire hidden subtree");

  const std::string visibility_output = convert(svg(
    "<g visibility=\"hidden\">"
    "<rect width=\"20\" height=\"20\" fill=\"red\"/>"
    "<rect x=\"30\" width=\"20\" height=\"20\" fill=\"green\" visibility=\"visible\"/>"
    "</g>"));
  expect(count_occurrences(visibility_output, "<path ") == 1 &&
         visibility_output.find("fill=\"green\"") != std::string::npos,
         "visibility remains inheritable and descendants can opt back into rendering");
}

void test_references_and_root_transform() {
  const std::string symbol_output = convert(svg(
    "<defs><symbol id=\"tile\" viewBox=\"0 0 10 10\">"
    "<rect width=\"10\" height=\"10\" fill=\"red\"/>"
    "</symbol></defs><use href=\"#tile\" x=\"5\" y=\"7\" width=\"20\" height=\"20\"/>"));
  expect(count_occurrences(symbol_output, "<path ") == 1 &&
         symbol_output.find("fill=\"red\"") != std::string::npos,
         "a symbol referenced by use is traversed");
  expect(concatenated_path_data(symbol_output).find("25") != std::string::npos,
         "symbol viewBox is mapped into the use viewport");

  const std::string cyclic_output = convert(svg("<use id=\"loop\" href=\"#loop\"/>"));
  expect(count_occurrences(cyclic_output, "<path ") == 0,
         "cyclic use references terminate without emitting recursive geometry");

  const std::string transformed_output = convert(
    "<svg xmlns=\"http://www.w3.org/2000/svg\" transform=\"translate(5 7)\">"
    "<rect width=\"10\" height=\"10\"/></svg>");
  expect(concatenated_path_data(transformed_output).find("M5,7") != std::string::npos,
         "the SVG root transform participates in traversal");
}

void test_case_sensitive_quoted_paint_reference() {
  const std::string output = convert(svg(
    "<defs><linearGradient id=\"BrandGradient\">"
    "<stop offset=\"0\" stop-color=\"red\"/>"
    "<stop offset=\"1\" stop-color=\"blue\"/>"
    "</linearGradient></defs>"
    "<rect width=\"100\" height=\"100\" fill=\"url('#BrandGradient')\"/>"));

  expect(output.find("id=\"BrandGradient\"") != std::string::npos,
         "quoted local paint references retain the referenced definition");
  expect(output.find("fill=\"url(&apos;#BrandGradient&apos;)\"") != std::string::npos ||
             output.find("fill=\"url('#BrandGradient')\"") != std::string::npos,
         "paint-server IDs remain case-sensitive when serialized");
}

void test_stroke_and_open_fill_fidelity() {
  const std::string malformed = convert(svg(
    "<path d=\"M0 0 X\" fill=\"red\"/><rect width=\"10\" height=\"10\" fill=\"green\"/>"));
  expect(count_occurrences(malformed, "<path ") == 1 &&
         malformed.find("fill=\"green\"") != std::string::npos,
         "invalid path data is skipped while compatible conversion continues");

  const std::string dashed_circle = convert(svg(
    "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"none\" stroke=\"red\" "
    "stroke-width=\"5\" stroke-dasharray=\"6 6\"/>"));
  expect(count_occurrences(dashed_circle, "<path ") == 1 &&
         dashed_circle.find("stroke=\"red\"") != std::string::npos &&
         dashed_circle.find("stroke-dasharray=\"6 6\"") != std::string::npos,
         "a dashed circle remains a live dashed stroke");

  const std::string inherited_fill = convert(svg(
    "<g fill=\"red\"><path d=\"M10 10L90 10L90 90\" stroke=\"blue\" "
    "stroke-width=\"4\"/></g>"));
  expect(inherited_fill.find("fill=\"red\"") != std::string::npos,
         "an open path retains its inherited fill");
}

void test_utf8_and_text_runs() {
  const std::vector<char32_t> decoded = svg_squisher::decode_utf8("A\xc3\xa9\xf0\x9f\x98\x80");
  expect(decoded == std::vector<char32_t>({U'A', U'\u00e9', U'\U0001f600'}),
         "UTF-8 decoding maps multi-byte sequences to Unicode scalar values");
  expect(svg_squisher::decode_utf8("\xed\xa0\x80") ==
           std::vector<char32_t>({0xfffd, 0xfffd, 0xfffd}),
         "invalid UTF-8 never produces a surrogate scalar");

  const std::optional<std::string> font = svg_squisher::discover_default_font();
  expect(font.has_value(), "a default test font is available");
  if (!font) return;

  const std::string plain = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">ABC</text>"), font));
  const std::string spanned = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">A<tspan>B</tspan>C</text>"), font));
  expect(!plain.empty() && spanned == plain,
         "text and tspan glyphs are emitted in document order");

  const std::string normalized_space = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">A B C</text>"), font));
  const std::string split_space = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">  A  <tspan> B </tspan> C  </text>"), font));
  expect(split_space == normalized_space,
         "default SVG whitespace collapses consistently across span boundaries");

  const std::string offset = concatenated_path_data(convert(svg(
    "<text x=\"10\" dx=\"5\" y=\"50\" font-size=\"30\">A</text>"), font));
  const std::string positioned = concatenated_path_data(convert(svg(
    "<text x=\"15\" y=\"50\" font-size=\"30\">A</text>"), font));
  expect(offset == positioned, "the first dx offset is applied exactly once");

  const std::string parent_positions = concatenated_path_data(convert(svg(
    "<text x=\"10 60\" y=\"40 70\" dx=\"2 3\" dy=\"4 5\" "
    "font-size=\"30\">A<tspan>B</tspan></text>"), font));
  const std::string direct_positions = concatenated_path_data(convert(svg(
    "<text x=\"10 60\" y=\"40 70\" dx=\"2 3\" dy=\"4 5\" "
    "font-size=\"30\">AB</text>"), font));
  expect(parent_positions == direct_positions,
         "parent x, y, dx, and dy lists continue across a nested tspan character");

  const std::string nested_override = concatenated_path_data(convert(svg(
    "<text x=\"10 60 80\" y=\"50\" font-size=\"30\">A"
    "<tspan x=\"40\">B</tspan>C</text>"), font));
  const std::string explicit_override = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">A"
    "<tspan x=\"40\">B</tspan><tspan x=\"80\">C</tspan></text>"), font));
  expect(nested_override == explicit_override,
         "a child override still consumes the matching parent-list character slot");

  const std::string parent_offsets = concatenated_path_data(convert(svg(
    "<text x=\"10\" dx=\"5 20\" y=\"50\" font-size=\"30\">A"
    "<tspan>B</tspan></text>"), font));
  const std::string explicit_offsets = concatenated_path_data(convert(svg(
    "<text x=\"10\" dx=\"5\" y=\"50\" font-size=\"30\">A"
    "<tspan dx=\"20\">B</tspan></text>"), font));
  expect(parent_offsets == explicit_offsets,
         "a parent dx list continues across a nested tspan without double application");

  const std::string empty_span = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">A"
    "<tspan x=\"90\"></tspan>B</text>"), font));
  const std::string neutral_empty_span = concatenated_path_data(convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\">A<tspan></tspan>B</text>"), font));
  expect(empty_span == neutral_empty_span,
         "an empty positioned tspan does not move the shared text cursor");

  const std::string stroke_text = convert(svg(
    "<text x=\"10\" y=\"50\" font-size=\"30\" fill=\"none\" stroke=\"red\">A</text>"), font);
  expect(stroke_text.find("stroke=\"red\"") != std::string::npos,
         "stroke-only text remains visible");

  svg_squisher::StyleState requested_family;
  requested_family.font_family = "a-family-that-must-not-override-the-cli";
  expect(svg_squisher::resolve_text_font_path(requested_family, font, true) == font,
         "an explicit command-line font is authoritative");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
    {"css-cascade", test_css_cascade},
    {"opacity-hidden-content", test_opacity_and_hidden_content},
    {"references-root-transform", test_references_and_root_transform},
    {"case-sensitive-quoted-paint-reference", test_case_sensitive_quoted_paint_reference},
    {"stroke-open-fill", test_stroke_and_open_fill_fidelity},
    {"utf8-text-runs", test_utf8_and_text_runs},
  };

  for (const auto& [name, test] : tests) {
    try {
      test();
    } catch (const std::exception& error) {
      std::cerr << "FAIL: " << name << " threw: " << error.what() << '\n';
      ++failures;
    }
  }

  if (failures != 0) return 1;
  std::cout << "style/text fidelity tests passed\n";
  return 0;
}
