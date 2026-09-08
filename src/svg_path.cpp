#include "svg_path.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "svg_path_data.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

bool paint_uses_url(const std::string& paint) {
  const CssUrlAnalysis urls = analyze_css_urls(paint);
  return urls.has_url && !urls.has_unsafe_url;
}

}  // namespace

bool path_data_is_valid(const std::string& d) {
  return parse_path_data(d).has_value();
}

std::size_t count_path_commands(const std::string& d) {
  const auto parsed = parse_path_data(d);
  return parsed ? parsed->segments.size() : 0;
}

std::optional<std::string> bake_path_transform(const std::string& d, const Matrix& matrix) {
  const auto parsed = parse_path_data(d);
  if (!parsed) return std::nullopt;
  if (matrix_is_identity(matrix)) return d;

  ParsedPath transformed;
  transformed.segments.reserve(parsed->segments.size());
  for (const PathSegment& source : parsed->segments) {
    PathSegment segment = source;
    segment.start = apply_matrix(matrix, source.start);
    segment.end = apply_matrix(matrix, source.end);
    segment.control1 = apply_matrix(matrix, source.control1);
    segment.control2 = apply_matrix(matrix, source.control2);

    if (source.kind == PathSegmentKind::Arc) {
      if (!matrix_is_scale_translate_only(matrix)) return std::nullopt;
      const double scale_x = std::abs(matrix.a);
      const double scale_y = std::abs(matrix.d);
      const double normalized_rotation = std::fmod(std::abs(source.x_axis_rotation), 180.0);
      const bool arc_axes_match_coordinates =
          normalized_rotation <= 1e-9 ||
          std::abs(normalized_rotation - 180.0) <= 1e-9;
      if (matrix.a <= 0.0 || matrix.d <= 0.0 ||
          (std::abs(scale_x - scale_y) > 1e-9 && !arc_axes_match_coordinates)) {
        return std::nullopt;
      }
      segment.radius_x = source.radius_x * scale_x;
      segment.radius_y = source.radius_y * scale_y;
      segment.sweep = matrix.a * matrix.d < 0.0 ? !source.sweep : source.sweep;
    }
    transformed.segments.push_back(segment);
  }
  return serialize_path_data(transformed);
}

bool path_is_closed(const std::string& d) {
  const auto parsed = parse_path_data(d);
  return parsed && !parsed->segments.empty() &&
         parsed->segments.back().kind == PathSegmentKind::Close;
}

bool path_has_curve_segments(const std::string& d) {
  const auto parsed = parse_path_data(d);
  if (!parsed) return false;
  return std::any_of(parsed->segments.begin(), parsed->segments.end(),
                     [](const PathSegment& segment) {
    return segment.kind == PathSegmentKind::Cubic ||
           segment.kind == PathSegmentKind::Quadratic ||
           segment.kind == PathSegmentKind::Arc;
  });
}

void append_path_entry(std::vector<PathEntry>& out_paths,
                       const std::string& d,
                       const std::string& transform,
                       const std::string& fill,
                       const std::string& stroke,
                       const StyleState& style,
                       bool emit_fill,
                       bool emit_stroke) {
  if ((!emit_fill && !emit_stroke) || d.empty()) return;

  const Matrix matrix = parse_transform(transform);
  const bool uses_coordinate_dependent_paint =
    (emit_fill && paint_uses_url(fill)) || (emit_stroke && paint_uses_url(stroke));
  const bool retain_transform =
    !matrix_is_identity(matrix) && (emit_stroke || uses_coordinate_dependent_paint);
  const std::optional<std::string> baked =
    retain_transform ? std::nullopt : bake_path_transform(d, matrix);

  PathEntry entry;
  entry.d = baked.value_or(d);
  entry.transform = baked ? "" : transform;
  entry.fill = fill;
  entry.fill_opacity = style.fill_opacity;
  entry.stroke = stroke;
  entry.stroke_opacity = style.stroke_opacity;
  entry.stroke_width = style.stroke_width;
  entry.stroke_dasharray = style.stroke_dasharray;
  entry.stroke_linecap = style.stroke_linecap;
  entry.stroke_linejoin = style.stroke_linejoin;
  entry.stroke_miterlimit = style.stroke_miterlimit;
  entry.fill_rule = style.fill_rule;
  entry.opacity = style.opacity;
  entry.emit_fill = emit_fill;
  entry.emit_stroke = emit_stroke;
  out_paths.push_back(std::move(entry));
}

}  // namespace svg_squisher
