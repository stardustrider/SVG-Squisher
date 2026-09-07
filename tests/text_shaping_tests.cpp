#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "svg_text.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::optional<std::string> discover_complex_script_test_font() {
  const std::vector<std::string> candidates{
    "C:/Windows/Fonts/arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/System/Library/Fonts/SFArabic.ttf",
    "/System/Library/Fonts/GeezaPro.ttc",
  };
  for (const std::string& candidate : candidates) {
    if (fs::exists(candidate)) return candidate;
  }
  return std::nullopt;
}

void test_latin_shaping(const std::string& font_path) {
  const std::string text = "office";
  const svg_squisher::TextLayoutResult layout = svg_squisher::text_to_path(
    text, 0.0, 40.0, 32.0, font_path, 0.0, {}, {}, {}, {});
  expect(!layout.d.empty(), "a shaped Latin run emits outlines");
  expect(layout.glyph_count != 0 &&
           layout.glyph_count <= svg_squisher::decode_utf8(text).size(),
         "a simple Latin run produces a valid glyph sequence");
  expect(!layout.has_right_to_left_run, "Latin text is detected as left-to-right");
}

void test_complex_scripts(const std::string& font_path) {
  const svg_squisher::TextLayoutResult arabic = svg_squisher::text_to_path(
    "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85", 0.0, 40.0, 32.0, font_path, 0.0,
    {}, {}, {}, {});
  expect(!arabic.d.empty(), "an Arabic-capable font emits connected-script outlines");
  expect(arabic.glyph_count != 0 && arabic.glyph_count < 4,
         "Arabic shaping performs the lam-alef glyph substitution");
  expect(arabic.has_right_to_left_run, "Arabic script selects right-to-left shaping");
  expect(arabic.end_x > 0.0,
         "right-to-left glyph order still advances along the default SVG inline axis");

  const svg_squisher::TextLayoutResult mixed = svg_squisher::text_to_path(
    "SVG \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85", 0.0, 40.0, 32.0, font_path, 0.0,
    {}, {}, {}, {});
  expect(mixed.has_right_to_left_run,
         "mixed-script text retains a separately shaped right-to-left run");
}

void test_combining_marks(const std::string& font_path) {
  const svg_squisher::TextLayoutResult base = svg_squisher::text_to_path(
    "A", 0.0, 40.0, 32.0, font_path, 0.0, {}, {}, {}, {});
  const svg_squisher::TextLayoutResult combined = svg_squisher::text_to_path(
    "A\xcc\x81", 0.0, 40.0, 32.0, font_path, 0.0, {}, {}, {}, {});
  expect(!combined.d.empty(), "a base character with a combining mark emits outlines");
  expect(std::abs(combined.end_x - base.end_x) < 0.02,
         "a combining mark does not add an unrelated character advance");
}

void test_missing_glyph_reporting(const std::string& font_path) {
  const svg_squisher::TextLayoutResult missing = svg_squisher::text_to_path(
    "A\xf4\x8f\xbf\xbf", 0.0, 40.0, 32.0, font_path, 0.0, {}, {}, {}, {});
  expect(missing.missing_codepoints == std::vector<char32_t>{U'\U0010ffff'},
         "a missing glyph reports its Unicode scalar value");
}

void test_positioning_and_spacing(const std::string& font_path) {
  const svg_squisher::TextLayoutResult positioned = svg_squisher::text_to_path(
    "AB", 10.0, 40.0, 32.0, font_path, 0.0, {10.0, 45.0}, {}, {}, {});
  expect(positioned.end_x > 45.0,
         "an absolute x list still addresses the second logical character");

  const double normal = svg_squisher::measure_text_advance("AB", 32.0, font_path, 0.0);
  const double spaced = svg_squisher::measure_text_advance("AB", 32.0, font_path, 4.0);
  expect(std::abs((spaced - normal) - 4.0) < 0.02,
         "letter spacing is applied once between adjacent characters");
}

}  // namespace

int main() {
  const std::optional<std::string> font_path = svg_squisher::discover_default_font();
  if (!font_path) {
    std::cerr << "FAIL: no default font is available for shaping tests\n";
    return 1;
  }
  const std::optional<std::string> complex_font_path = discover_complex_script_test_font();
  if (!complex_font_path) {
    std::cerr << "FAIL: no complex-script test font is available\n";
    return 1;
  }

  try {
    test_latin_shaping(*font_path);
    test_complex_scripts(*complex_font_path);
    test_combining_marks(*font_path);
    test_missing_glyph_reporting(*font_path);
    test_positioning_and_spacing(*font_path);
  } catch (const std::exception& error) {
    std::cerr << "FAIL: shaping test threw: " << error.what() << '\n';
    return 1;
  }

  if (failures != 0) return 1;
  std::cout << "text shaping tests passed\n";
  return 0;
}
