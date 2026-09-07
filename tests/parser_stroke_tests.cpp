#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "svg_geometry.h"
#include "svg_path.h"
#include "svg_stroke.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace {

using svg_squisher::BBox;
using svg_squisher::Matrix;
using svg_squisher::PathEntry;
using svg_squisher::StrokeSubpath;
using svg_squisher::StyleState;

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool near(double actual, double expected) {
  return std::abs(actual - expected) <= 1e-9;
}

void expect_safe_outline(const std::string& source, const std::string& description);

void expect_single_triangle(const std::optional<std::vector<StrokeSubpath>>& parsed,
                            const std::string& parser_name) {
  expect(parsed.has_value(), parser_name + " accepts valid compact moveto data");
  if (!parsed) return;
  expect(parsed->size() == 1, parser_name + " keeps implicit lines in one subpath");
  if (parsed->size() != 1) return;
  expect(parsed->front().points.size() == 3, parser_name + " emits two implicit line endpoints");
  expect(parsed->front().closed, parser_name + " preserves the close command");
}

void test_implicit_moveto() {
  const Matrix translate{1.0, 0.0, 0.0, 1.0, 5.0, 5.0};

  const auto absolute = svg_squisher::bake_path_transform(
    "M0 0 20 0 20 20Z", translate);
  expect(absolute.has_value(), "transform parser accepts absolute implicit lines");
  if (absolute) {
    expect(*absolute == "M5,5L25,5L25,25Z",
           "absolute coordinate pairs after moveto become lineto commands");
  }

  const auto relative = svg_squisher::bake_path_transform(
    "m1 2 3 4 5 6", translate);
  expect(relative.has_value(), "transform parser accepts relative implicit lines");
  if (relative) {
    expect(*relative == "M6,7L9,11L14,17",
           "relative coordinate pairs after moveto accumulate as lineto commands");
  }

  expect_single_triangle(
    svg_squisher::parse_straight_subpaths("M0 0 20 0 20 20Z"),
    "straight parser");
  expect_single_triangle(
    svg_squisher::flatten_path_subpaths("M0 0 20 0 20 20Z"),
    "flattening parser");

  const auto relative_straight = svg_squisher::parse_straight_subpaths(
    "m1 2 3 4 5 6");
  expect(relative_straight.has_value() && relative_straight->size() == 1 &&
         relative_straight->front().points.size() == 3,
         "straight parser keeps relative implicit lines in one subpath");
  if (relative_straight && relative_straight->size() == 1 &&
      relative_straight->front().points.size() == 3) {
    const auto& points = relative_straight->front().points;
    expect(near(points[0].x, 1.0) && near(points[0].y, 2.0) &&
           near(points[1].x, 4.0) && near(points[1].y, 6.0) &&
           near(points[2].x, 9.0) && near(points[2].y, 12.0),
           "straight parser accumulates relative implicit line endpoints");
  }

  const auto relative_flattened = svg_squisher::flatten_path_subpaths(
    "m1 2 3 4 5 6");
  expect(relative_flattened.has_value() && relative_flattened->size() == 1 &&
         relative_flattened->front().points.size() == 3 &&
         near(relative_flattened->front().points.back().x, 9.0) &&
         near(relative_flattened->front().points.back().y, 12.0),
         "flattening parser accumulates relative implicit line endpoints");

  const auto relative_bounds = svg_squisher::path_bbox("m1 2 3 4 5 6");
  expect(relative_bounds.has_value() && near(relative_bounds->min_x, 1.0) &&
         near(relative_bounds->min_y, 2.0) && near(relative_bounds->max_x, 9.0) &&
         near(relative_bounds->max_y, 12.0),
         "bounding-box parser accumulates relative implicit line endpoints");

  const std::string outline = svg_squisher::build_curve_fallback_outline(
    "M0 0 20 0 20 20", 2.0, "butt", "miter", 4.0);
  expect(!outline.empty(), "curve parser retains implicit line segments after moveto");
}

void test_malformed_no_progress() {
  const Matrix translate{1.0, 0.0, 0.0, 1.0, 5.0, 5.0};
  const std::vector<std::string> malformed_paths{
    "M0 0 X",
    "M0 0 @",
    "M0 0 L",
    "M0 0 Q 1",
    "M0 0 Z 1 2",
  };

  expect(!svg_squisher::path_data_is_valid("M0 0 X"),
         "path validator rejects an unknown command");
  expect(!svg_squisher::path_data_is_valid("M0 0Z1"),
         "path validator rejects parameters after closepath");
  expect(!svg_squisher::path_data_is_valid("M1e999 0"),
         "path validator rejects non-finite coordinates");
  expect(!svg_squisher::path_data_is_valid("M0 0A-1 2 0 0 1 5 5"),
         "path validator rejects negative arc radii");
  expect(!svg_squisher::path_data_is_valid("M0 0A1 2 0 2 0 5 5"),
         "path validator rejects arc flags other than zero or one");
  for (const std::string& invalid_start : {
         std::string("L10 10"), std::string("H10"),
         std::string("C1 1 2 2 3 3"), std::string("Z")}) {
    expect(!svg_squisher::path_data_is_valid(invalid_start),
           "path validator requires moveto as the first command: " + invalid_start);
  }
  expect(svg_squisher::path_data_is_valid("M0 0 20 0 20 20Z"),
         "path validator accepts implicit lineto coordinates");
  expect(svg_squisher::count_path_commands("M0 0 20 0 20 20Z") == 4,
         "semantic command count includes implicit lineto groups");

  for (const std::string& malformed : malformed_paths) {
    expect(!svg_squisher::bake_path_transform(malformed, translate).has_value(),
           "transform parser rejects malformed data: " + malformed);
    expect(!svg_squisher::parse_straight_subpaths(malformed).has_value(),
           "straight parser rejects malformed data: " + malformed);
    expect(!svg_squisher::flatten_path_subpaths(malformed).has_value(),
           "flattening parser rejects malformed data: " + malformed);
    expect(!svg_squisher::path_bbox(malformed).has_value(),
           "bounding-box parser rejects malformed data: " + malformed);
    expect(svg_squisher::convert_evenodd_to_nonzero(malformed) == malformed,
           "evenodd conversion leaves malformed data unchanged: " + malformed);
    expect(svg_squisher::build_curve_fallback_outline(
             malformed, 2.0, "butt", "miter", 4.0).empty(),
           "curve parser rejects malformed data: " + malformed);
  }
}

void test_compact_arc_flags() {
  const std::string compact = "M0 0A5 5 0 0110 20";
  const Matrix translate{1.0, 0.0, 0.0, 1.0, 2.0, 3.0};

  const auto baked = svg_squisher::bake_path_transform(compact, translate);
  expect(baked.has_value(), "transform parser accepts adjacent arc flags and endpoint");
  if (baked) {
    expect(*baked == "M2,3A5,5 0 0 1 12,23",
           "transform parser assigns compact arc tokens to the correct fields");
  }

  const auto flattened = svg_squisher::flatten_path_subpaths(compact);
  expect(flattened.has_value() && flattened->size() == 1 &&
         !flattened->front().points.empty() &&
         near(flattened->front().points.back().x, 10.0) &&
         near(flattened->front().points.back().y, 20.0),
         "geometry parser accepts compact arc flags and preserves the endpoint");

  const auto bounds = svg_squisher::path_bbox(compact);
  expect(bounds.has_value() && bounds->max_x >= 10.0 && bounds->max_y >= 20.0,
         "bounding-box parser accepts compact arc flags");

  expect_safe_outline(compact, "stroke parser with compact arc flags");

  for (const std::string& invalid : {
         std::string("M0 0A5 5 0 2110 20"),
         std::string("M0 0A5 5 0 0210 20")}) {
    expect(!svg_squisher::bake_path_transform(invalid, translate).has_value(),
           "transform parser rejects an invalid single-character arc flag");
    expect(!svg_squisher::flatten_path_subpaths(invalid).has_value(),
           "geometry parser rejects an invalid single-character arc flag");
    expect(svg_squisher::build_curve_fallback_outline(
             invalid, 4.0, "round", "miter", 4.0).empty(),
           "stroke parser rejects an invalid single-character arc flag");
  }
}

void test_svg_number_grammar() {
  expect(svg_squisher::path_data_is_valid("M.5.5L5. 1e-3"),
         "path validator accepts SVG decimals and exponents");
  expect(svg_squisher::path_data_is_valid("M1-2L.5-.25"),
         "path validator accepts sign and decimal-point token boundaries");

  for (const std::string& invalid : {
         std::string("M0x1p2 0"),
         std::string("M0 0L1e 2"),
         std::string("M0 0L1e+ 2"),
         std::string("M0 0L. 2")}) {
    expect(!svg_squisher::path_data_is_valid(invalid),
           "path validator rejects a non-SVG number: " + invalid);
  }

  const std::string tokens = ".5 5. 1e-3-.25";
  std::size_t position = 0;
  double value = 0.0;
  expect(svg_squisher::parse_number_token(tokens, position, value) && near(value, 0.5),
         "shared lexer accepts a leading-dot decimal");
  expect(svg_squisher::parse_number_token(tokens, position, value) && near(value, 5.0),
         "shared lexer accepts a trailing-dot decimal");
  expect(svg_squisher::parse_number_token(tokens, position, value) && near(value, 0.001),
         "shared lexer accepts an exponent");
  expect(svg_squisher::parse_number_token(tokens, position, value) && near(value, -0.25),
         "shared lexer starts a new number at an adjacent sign");

  for (const std::string& invalid : {
         std::string("0x1p2"), std::string("1e"), std::string("1e+")}) {
    std::size_t invalid_position = 0;
    expect(!svg_squisher::parse_number_token(invalid, invalid_position, value),
           "shared lexer rejects a non-SVG token: " + invalid);
  }
}

void expect_safe_outline(const std::string& source, const std::string& description) {
  const std::string outline = svg_squisher::build_curve_fallback_outline(
    source, 4.0, "round", "miter", 4.0);
  expect(!outline.empty(), description + " produces an outline");
  expect(outline.find("nan") == std::string::npos &&
         outline.find("inf") == std::string::npos,
         description + " produces only finite coordinates");

  const auto bounds = svg_squisher::path_bbox(outline);
  expect(bounds.has_value(), description + " produces parseable path data");
  if (bounds) {
    expect(std::isfinite(bounds->min_x) && std::isfinite(bounds->min_y) &&
           std::isfinite(bounds->max_x) && std::isfinite(bounds->max_y),
           description + " has finite bounds");
  }
}

void test_subdivided_curve_strokes() {
  expect_safe_outline("M10 50 Q50 0 90 50", "open subdivided quadratic stroke");
  expect_safe_outline(
    "M10 50 Q50 0 90 50 Q50 100 10 50 Z",
    "closed subdivided quadratic stroke");
  expect_safe_outline(
    "M10 50 C20 -20 80 -20 90 50 L90 75",
    "mixed subdivided cubic stroke");
}

void test_transform_retention() {
  StyleState dashed_style;
  dashed_style.stroke_width = "2";
  dashed_style.stroke_dasharray = "2 2";

  std::vector<PathEntry> dashed;
  svg_squisher::append_path_entry(
    dashed,
    "M0 5H20",
    "scale(2)",
    "none",
    "black",
    dashed_style,
    false,
    true);
  expect(dashed.size() == 1, "live dashed stroke emits one path entry");
  if (dashed.size() == 1) {
    expect(dashed.front().d == "M0 5H20",
           "live dashed stroke keeps its source geometry");
    expect(dashed.front().transform == "scale(2)",
           "live dashed stroke keeps the transform that scales its metrics");
    expect(dashed.front().stroke_width == "2" &&
           dashed.front().stroke_dasharray == "2 2",
           "live dashed stroke retains its width and dash pattern");
  }

  StyleState fill_style;
  std::vector<PathEntry> gradient;
  svg_squisher::append_path_entry(
    gradient,
    "M0 0L10 0L10 10Z",
    "translate(5 7)",
    "url(#paint)",
    "none",
    fill_style,
    true,
    false);
  expect(gradient.size() == 1 &&
         gradient.front().d == "M0 0L10 0L10 10Z" &&
         gradient.front().transform == "translate(5 7)",
         "URL paint keeps geometry and paint in the same transformed coordinate system");

  std::vector<PathEntry> solid;
  svg_squisher::append_path_entry(
    solid,
    "M0 0L10 0L10 10Z",
    "translate(5 7)",
    "red",
    "none",
    fill_style,
    true,
    false);
  expect(solid.size() == 1 && solid.front().transform.empty() &&
         solid.front().d == "M5,7L15,7L15,17Z",
         "solid fill geometry still bakes a safe transform");
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
    {"implicit-moveto", test_implicit_moveto},
    {"malformed-no-progress", test_malformed_no_progress},
    {"compact-arc-flags", test_compact_arc_flags},
    {"svg-number-grammar", test_svg_number_grammar},
    {"subdivided-curve-strokes", test_subdivided_curve_strokes},
    {"transform-retention", test_transform_retention},
  };

  const std::string selected = argc > 1 ? argv[1] : "";
  bool ran_test = false;
  for (const auto& [name, test] : tests) {
    if (!selected.empty() && selected != name) continue;
    ran_test = true;
    test();
  }

  if (!ran_test) {
    std::cerr << "Unknown test case: " << selected << '\n';
    return 2;
  }
  if (failures != 0) return 1;

  std::cout << "parser/stroke tests passed\n";
  return 0;
}
