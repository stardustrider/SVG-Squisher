#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "svg_geometry.h"
#include "svg_path.h"
#include "svg_path_data.h"
#include "svg_shape.h"
#include "svg_stroke.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace {

using svg_squisher::BBox;
using svg_squisher::Matrix;
using svg_squisher::PathEntry;
using svg_squisher::Point;
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

void test_css_url_classification() {
  for (const std::string& value : {
         std::string("url(https://evil.invalid/paint.svg)"),
         std::string(R"(\75\72\6c(http://127.0.0.1:47931/leak.svg))"),
         std::string(R"(\55\52\4c ( https://evil.invalid/mixed.svg ))"),
         std::string(R"(u\72l(  '//evil.invalid/quoted.svg' ))"),
         std::string("url/**/(data:image/svg+xml,unsafe)"),
         std::string(R"(\2f\2a url(http://evil.invalid/wrapped.svg)\2a\2f)"),
         std::string("url()")}) {
    const svg_squisher::CssUrlAnalysis analysis =
      svg_squisher::analyze_css_urls(value);
    expect(analysis.has_url && analysis.has_unsafe_url,
           "external, escaped, and malformed CSS URLs fail closed: " + value);
  }

  for (const std::string& value : {
         std::string("none"),
         std::string("red"),
         std::string("#75726c"),
         std::string("curl(http://benign.invalid/name)"),
         std::string("var(--paint)")}) {
    const svg_squisher::CssUrlAnalysis analysis =
      svg_squisher::analyze_css_urls(value);
    expect(!analysis.has_url && !analysis.has_unsafe_url,
           "benign non-URL CSS values remain unclassified: " + value);
  }

  for (const std::string& value : {
         std::string("url(#paint)"),
         std::string("URL( '#BrandGradient' )"),
         std::string(R"(\75\72\6c(\23 paint))"),
         std::string(R"(u\72l ( "#paint" ))")}) {
    const svg_squisher::CssUrlAnalysis analysis =
      svg_squisher::analyze_css_urls(value);
    expect(analysis.has_url && !analysis.has_unsafe_url &&
             analysis.local_fragment_ids.size() == 1,
           "plain and escaped local-fragment CSS URLs remain safe: " + value);
  }
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
    expect(*absolute == "M5 5 25 5 25 25Z",
           "absolute coordinate pairs after moveto become lineto commands");
  }

  const auto relative = svg_squisher::bake_path_transform(
    "m1 2 3 4 5 6", translate);
  expect(relative.has_value(), "transform parser accepts relative implicit lines");
  if (relative) {
    expect(*relative == "M6 7 9 11 14 17",
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
  expect(!svg_squisher::path_data_is_valid("M0 0A+1 2 0 0 1 5 5"),
         "path validator rejects an explicitly signed positive arc radius");
  expect(!svg_squisher::path_data_is_valid("M0 0A-0 2 0 0 1 5 5"),
         "path validator rejects a signed zero arc radius");
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
    expect(*baked == "M2 3A5 5 0 0112 23",
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
  expect(svg_squisher::path_data_is_valid("M0 0A.5 5. 0 01.5-.25"),
         "unsigned arc radii retain valid compact decimal and flag syntax");

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
         std::string("M0 0L. 2"),
         std::string("M,0 0L10 10"),
         std::string("M0,,0L10 10"),
         std::string("M0 0,L10 10"),
         std::string("M0 0L10 10,"),
         std::string("M0\v0L10 10")}) {
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

  expect(svg_squisher::parse_number_list("0, 0 10,10") ==
             std::vector<double>({0.0, 0.0, 10.0, 10.0}),
         "number lists accept SVG comma-wsp separators");
  for (const std::string& invalid : {
         std::string(",0 0 10 10"),
         std::string("0,,0 10 10"),
         std::string("0 0 10 10,"),
         std::string("0\v0 10 10")}) {
    expect(svg_squisher::parse_number_list(invalid).empty(),
           "number lists reject malformed comma-wsp: " + invalid);
  }

  const auto compact_points = svg_squisher::parse_points_list("0-10 20-30");
  expect(compact_points && *compact_points ==
             std::vector<double>({0.0, -10.0, 20.0, -30.0}),
         "points lists accept the compact negative-coordinate separator");
  const auto empty_points = svg_squisher::parse_points_list(" \t\r\n ");
  expect(empty_points && empty_points->empty(),
         "an empty points list is valid and contains no coordinate pairs");
  for (const std::string& invalid : {
         std::string("0+10"),
         std::string("0-10-20-30"),
         std::string("0\v10")}) {
    expect(!svg_squisher::parse_points_list(invalid),
           "points lists reject non-grammar separators: " + invalid);
  }

  const auto zero_viewbox = svg_squisher::parse_viewbox("0 0 0 10");
  expect(zero_viewbox && (*zero_viewbox)[2] == 0.0,
         "viewBox parsing accepts a zero dimension");
  expect(!svg_squisher::parse_viewbox("0. 0 10 10"),
         "viewBox parsing rejects a trailing-dot general SVG number");

  expect(svg_squisher::transform_is_valid(
             "translate(5, 6), scale(2) rotate(15 10 10)"),
         "transform lists accept valid comma-wsp between functions and arguments");
  for (const std::string& invalid : {
         std::string(",translate(5 6)"),
         std::string("translate(5,,6)"),
         std::string("translate(5 6),"),
         std::string("translate(5 6),,scale(2)"),
         std::string("translate(5 6)scale(2)"),
         std::string("translate\v(5)"),
         std::string("\ftranslate(5)")}) {
    expect(!svg_squisher::transform_is_valid(invalid),
           "transform parser rejects malformed comma-wsp: " + invalid);
  }
}

void test_normalized_ast_round_trip() {
  const std::string source =
      "m1 2 3-4 5 6c1 2 3 4 5 6 7 8 9 10 11 12"
      "a5 6 0 0110-20zM9 9";
  const auto parsed = svg_squisher::parse_path_data(source);
  expect(parsed.has_value(), "normalized path parser accepts mixed compact data");
  if (!parsed) return;

  const std::string compact = svg_squisher::serialize_path_data(*parsed);
  const auto reparsed = svg_squisher::parse_path_data(compact);
  expect(reparsed.has_value(), "compact normalized path data parses again");
  if (reparsed) {
    expect(svg_squisher::serialize_path_data(*reparsed) == compact,
           "normalized path serialization is deterministic after a round trip");
    expect(reparsed->segments.size() == parsed->segments.size(),
           "compact command elision preserves semantic segment count");
  }
  expect(compact.find('L') == std::string::npos,
         "the first line after moveto uses safe implicit-line syntax");
  expect(std::count(compact.begin(), compact.end(), 'C') == 1,
         "repeated cubic commands elide the command letter");
  expect(compact.find(" 01") != std::string::npos,
         "arc flags use their compact fixed-width representation");

  const auto smooth = svg_squisher::parse_path_data(
      "M0 0C10 0 20 0 30 0S50 0 60 0Q70 0 80 0T100 0");
  expect(smooth.has_value() && smooth->segments.size() == 5,
         "normalized parser resolves smooth curve commands");
  if (smooth && smooth->segments.size() == 5) {
    expect(near(smooth->segments[2].control1.x, 40.0) &&
           near(smooth->segments[4].control1.x, 90.0),
           "smooth cubic and quadratic controls are reflected in the AST");
  }

  const auto repeated_moves = svg_squisher::parse_path_data("M0 0M1 1");
  const std::string repeated_moves_compact =
      repeated_moves ? svg_squisher::serialize_path_data(*repeated_moves) : "";
  expect(repeated_moves.has_value() &&
         std::count(repeated_moves_compact.begin(), repeated_moves_compact.end(), 'M') == 2,
         "separate moveto commands remain explicit and unambiguous");
  expect(svg_squisher::path_data_is_valid(""),
         "empty path data retains the prior valid empty-path contract");
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

void test_zero_length_stroke_caps() {
  const std::string round = svg_squisher::build_straight_stroke_outline(
      "M20 20L20 20", 10.0, "round", "miter", 4.0);
  const auto round_bounds = svg_squisher::path_bbox(round);
  expect(!round.empty() &&
             std::count(round.begin(), round.end(), 'A') == 2 &&
             round_bounds.has_value() && near(round_bounds->min_x, 15.0) &&
             near(round_bounds->min_y, 15.0) &&
             near(round_bounds->max_x, 25.0) &&
             near(round_bounds->max_y, 25.0),
         "a zero-length round-capped line becomes a full centered circle");

  const std::string square = svg_squisher::build_straight_stroke_outline(
      "M20 20L20 20", 10.0, "square", "miter", 4.0);
  const auto square_bounds = svg_squisher::path_bbox(square);
  expect(!square.empty() && square.find('A') == std::string::npos &&
             square_bounds.has_value() && near(square_bounds->min_x, 15.0) &&
             near(square_bounds->min_y, 15.0) &&
             near(square_bounds->max_x, 25.0) &&
             near(square_bounds->max_y, 25.0),
         "a zero-length square-capped line becomes a full centered square");

  expect(svg_squisher::build_straight_stroke_outline(
             "M20 20L20 20", 10.0, "butt", "miter", 4.0).empty(),
         "a zero-length butt-capped line remains empty");
  expect(svg_squisher::build_straight_stroke_outline(
             "M20 20", 10.0, "round", "miter", 4.0).empty(),
         "a moveto without a drawing command remains empty");

  const std::string closed = svg_squisher::build_straight_stroke_outline(
      "M8 9Z", 4.0, "round", "miter", 4.0);
  const auto closed_bounds = svg_squisher::path_bbox(closed);
  expect(closed_bounds.has_value() && near(closed_bounds->min_x, 6.0) &&
             near(closed_bounds->min_y, 7.0) &&
             near(closed_bounds->max_x, 10.0) &&
             near(closed_bounds->max_y, 11.0),
         "a zero-length closed subpath still receives its round cap shape");

  const std::string curve = svg_squisher::build_curve_fallback_outline(
      "M30 30C30 30 30 30 30 30", 6.0, "round", "miter", 4.0);
  const auto curve_bounds = svg_squisher::path_bbox(curve);
  expect(curve_bounds.has_value() && near(curve_bounds->min_x, 27.0) &&
             near(curve_bounds->min_y, 27.0) &&
             near(curve_bounds->max_x, 33.0) &&
             near(curve_bounds->max_y, 33.0),
         "a constant curve receives the same zero-length round cap shape");

  StyleState fill_style;
  std::vector<PathEntry> anisotropic;
  svg_squisher::append_path_entry(
      anisotropic,
      round,
      "matrix(2 0 0 3 5 -2)",
      "red",
      "none",
      fill_style,
      true,
      false);
  const auto transformed_bounds = anisotropic.size() == 1
      ? svg_squisher::path_bbox(anisotropic.front().d)
      : std::nullopt;
  expect(anisotropic.size() == 1 && anisotropic.front().transform.empty() &&
             transformed_bounds.has_value() &&
             near(transformed_bounds->min_x, 35.0) &&
             near(transformed_bounds->min_y, 43.0) &&
             near(transformed_bounds->max_x, 55.0) &&
             near(transformed_bounds->max_y, 73.0),
         "an anisotropic transform maps a round cap to the correct ellipse bounds");

  std::vector<PathEntry> sheared;
  svg_squisher::append_path_entry(
      sheared,
      round,
      "matrix(1 0.25 0.3 1 4 7)",
      "red",
      "none",
      fill_style,
      true,
      false);
  expect(sheared.size() == 1 && sheared.front().d == round &&
             sheared.front().transform == "matrix(1 0.25 0.3 1 4 7)",
         "a sheared round cap retains its transform when arc baking is unsafe");
}

void test_retraced_stroke_outline() {
  const std::string source = "M10 50H90H10";
  const std::string flat = svg_squisher::build_straight_stroke_outline(
      source, 12.0, "butt", "miter", 4.0);
  const auto flat_bounds = svg_squisher::path_bbox(flat);
  expect(flat == "M10,56L90,56L90,44L10,44Z" &&
             flat_bounds.has_value() && near(flat_bounds->min_x, 10.0) &&
             near(flat_bounds->max_x, 90.0) && near(flat_bounds->min_y, 44.0) &&
             near(flat_bounds->max_y, 56.0),
         "an immediate full retrace produces one non-self-crossing stroke region");

  const std::string rounded = svg_squisher::build_straight_stroke_outline(
      source, 12.0, "round", "round", 4.0);
  const auto rounded_bounds = svg_squisher::path_bbox(rounded);
  expect(rounded_bounds.has_value() && near(rounded_bounds->min_x, 4.0) &&
             near(rounded_bounds->max_x, 96.0) &&
             near(rounded_bounds->min_y, 44.0) &&
             near(rounded_bounds->max_y, 56.0),
         "a retrace keeps the line cap at its coincident endpoints and the round join at its turn");

  const std::string cap_only = svg_squisher::build_straight_stroke_outline(
      source, 12.0, "round", "miter", 4.0);
  const auto cap_only_bounds = svg_squisher::path_bbox(cap_only);
  expect(cap_only_bounds.has_value() && near(cap_only_bounds->min_x, 4.0) &&
             near(cap_only_bounds->max_x, 90.0),
         "a retrace does not turn a miter join into an extra round cap");

  const std::string join_only = svg_squisher::build_straight_stroke_outline(
      source, 12.0, "butt", "round", 4.0);
  const auto join_only_bounds = svg_squisher::path_bbox(join_only);
  expect(join_only_bounds.has_value() && near(join_only_bounds->min_x, 10.0) &&
             near(join_only_bounds->max_x, 96.0),
         "a retrace applies a round join independently from its butt line cap");

  expect(svg_squisher::build_straight_stroke_outline(
             "M10 50H90H10H60", 12.0, "butt", "miter", 4.0).empty(),
         "a retrace embedded in a longer subpath declines an unsafe self-crossing outline");
  expect(svg_squisher::build_straight_stroke_outline(
             "M10 50H90H10Z", 12.0, "butt", "miter", 4.0).empty(),
         "a closed retrace also declines an unsafe self-crossing outline");
}

void test_eccentric_ellipse_stroke_outline() {
  pugi::xml_document document;
  const pugi::xml_parse_result parsed = document.load_string(
      "<ellipse cx='50' cy='50' rx='46' ry='8'/>");
  expect(parsed, "ellipse stroke fixture parses");
  if (!parsed) return;

  const std::string outline =
      svg_squisher::ellipse_stroke_to_ring(document.document_element(), 14.0);
  const auto bounds = svg_squisher::path_bbox(outline);
  expect(!outline.empty() && outline.find('C') != std::string::npos &&
             bounds.has_value() && near(bounds->min_x, -3.0) &&
             near(bounds->max_x, 103.0) && near(bounds->min_y, 35.0) &&
             near(bounds->max_y, 65.0),
         "an eccentric ellipse uses adaptive normal offsets with exact axial stroke bounds");

  pugi::xml_document collapsed_document;
  const pugi::xml_parse_result collapsed_parsed = collapsed_document.load_string(
      "<ellipse cx='50' cy='50' rx='20' ry='4'/>");
  expect(collapsed_parsed, "collapsed-inner ellipse stroke fixture parses");
  if (!collapsed_parsed) return;
  const std::string collapsed_outline = svg_squisher::ellipse_stroke_to_ring(
      collapsed_document.document_element(), 12.0);
  const auto collapsed_bounds = svg_squisher::path_bbox(collapsed_outline);
  expect(!collapsed_outline.empty() && collapsed_bounds.has_value() &&
             near(collapsed_bounds->min_x, 24.0) &&
             near(collapsed_bounds->max_x, 76.0) &&
             near(collapsed_bounds->min_y, 40.0) &&
             near(collapsed_bounds->max_y, 60.0),
         "an ellipse whose inner offset collapses still emits its complete stroke region");
}

void test_close_continuations_and_arc_bounds() {
  const auto straight = svg_squisher::parse_straight_subpaths(
      "M0 0L4 0ZL8 8");
  expect(straight.has_value() && straight->size() == 2 &&
             straight->back().points.size() == 2,
         "straight consumers seed a new drawable run after closepath");
  if (straight && straight->size() == 2 &&
      straight->back().points.size() == 2) {
    expect(near(straight->back().points.front().x, 0.0) &&
               near(straight->back().points.front().y, 0.0) &&
               near(straight->back().points.back().x, 8.0) &&
               near(straight->back().points.back().y, 8.0),
           "post-close lines start at the closed subpath origin");
  }

  const auto flattened = svg_squisher::flatten_path_subpaths(
      "M0 0L4 0ZQ4 8 8 8");
  expect(flattened.has_value() && flattened->size() == 2 &&
             flattened->back().points.size() > 2 &&
             near(flattened->back().points.front().x, 0.0) &&
             near(flattened->back().points.front().y, 0.0) &&
             near(flattened->back().points.back().x, 8.0) &&
             near(flattened->back().points.back().y, 8.0),
         "curve flattening preserves the current point after closepath");

  const std::string continuation_outline =
      svg_squisher::build_straight_stroke_outline(
          "M2 2L10 2ZL18 18", 2.0, "butt", "miter", 4.0);
  const auto continuation_bounds =
      svg_squisher::path_bbox(continuation_outline);
  expect(continuation_bounds.has_value() &&
             continuation_bounds->max_x > 18.0 &&
             continuation_bounds->max_y > 18.0,
         "stroke outlining retains drawable geometry after closepath");

  const std::string closed_curve_outline =
      svg_squisher::build_curve_fallback_outline(
          "M2 18Q10 2 18 18Z", 2.0, "butt", "miter", 4.0);
  const auto closed_curve_bounds = svg_squisher::path_bbox(closed_curve_outline);
  expect(closed_curve_bounds.has_value() && closed_curve_bounds->max_y >= 18.99,
         "closed curve strokes include the implicit closing line and its joins");

  const auto corrected_arc =
      svg_squisher::path_bbox("M0 0A10 10 0 0 1 100 0");
  expect(corrected_arc.has_value() &&
             std::abs(corrected_arc->min_x - 0.0) <= 1e-6 &&
             std::abs(corrected_arc->max_x - 100.0) <= 1e-6 &&
             std::abs(corrected_arc->min_y + 50.0) <= 1e-6 &&
             std::abs(corrected_arc->max_y - 0.0) <= 1e-6,
         "arc bounds apply the SVG radius-correction algorithm");

  const auto quarter_turned_arc =
      svg_squisher::path_bbox("M50 80A30 10 90 0 1 50 20");
  expect(quarter_turned_arc.has_value() &&
             std::abs(quarter_turned_arc->min_x - 40.0) <= 1e-6 &&
             std::abs(quarter_turned_arc->max_x - 50.0) <= 1e-6 &&
             std::abs(quarter_turned_arc->min_y - 20.0) <= 1e-6 &&
             std::abs(quarter_turned_arc->max_y - 80.0) <= 1e-6,
         "arc bounds include rotated-ellipse extrema on the active sweep");

  const std::string rotated_arc = "M5 30A12 28 37 1 0 80 70";
  const auto rotated_bounds = svg_squisher::path_bbox(rotated_arc);
  const auto rotated_points = svg_squisher::flatten_path_subpaths(rotated_arc);
  bool contains_flattened_points = rotated_bounds.has_value() &&
      rotated_points.has_value() && !rotated_points->empty();
  if (contains_flattened_points) {
    for (const Point point : rotated_points->front().points) {
      contains_flattened_points =
          contains_flattened_points &&
          point.x >= rotated_bounds->min_x - 1e-6 &&
          point.x <= rotated_bounds->max_x + 1e-6 &&
          point.y >= rotated_bounds->min_y - 1e-6 &&
          point.y <= rotated_bounds->max_y + 1e-6;
    }
  }
  expect(contains_flattened_points,
         "rotated arc bounds contain every sampled point on the sweep");

  const auto zero_sweep = svg_squisher::path_bbox("M7 9A20 30 15 1 1 7 9");
  expect(zero_sweep.has_value() && near(zero_sweep->min_x, 7.0) &&
             near(zero_sweep->max_x, 7.0) && near(zero_sweep->min_y, 9.0) &&
             near(zero_sweep->max_y, 9.0),
         "an arc with identical endpoints contributes no ellipse sweep");
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
         solid.front().d == "M5 7 15 7 15 17Z",
         "solid fill geometry still bakes a safe transform");
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
    {"css-url-classification", test_css_url_classification},
    {"implicit-moveto", test_implicit_moveto},
    {"malformed-no-progress", test_malformed_no_progress},
    {"compact-arc-flags", test_compact_arc_flags},
    {"svg-number-grammar", test_svg_number_grammar},
    {"normalized-ast-round-trip", test_normalized_ast_round_trip},
    {"subdivided-curve-strokes", test_subdivided_curve_strokes},
    {"zero-length-stroke-caps", test_zero_length_stroke_caps},
    {"retraced-stroke-outline", test_retraced_stroke_outline},
    {"eccentric-ellipse-stroke-outline", test_eccentric_ellipse_stroke_outline},
    {"close-continuations-and-arc-bounds",
     test_close_continuations_and_arc_bounds},
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
