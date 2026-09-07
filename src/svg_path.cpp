#include "svg_path.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "svg_geometry.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

bool is_command_char(char ch) {
  switch (ch) {
    case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v':
    case 'C': case 'c': case 'S': case 's': case 'Q': case 'q': case 'T': case 't':
    case 'A': case 'a': case 'Z': case 'z':
      return true;
    default:
      return false;
  }
}

bool paint_uses_url(const std::string& paint) {
  const std::string normalized = lower_copy(trim(paint));
  return normalized.rfind("url(", 0) == 0;
}

}  // namespace

bool path_data_is_valid(const std::string& d) {
  return flatten_path_subpaths(d).has_value();
}

std::size_t count_path_commands(const std::string& d) {
  std::size_t count = 0;
  return flatten_path_subpaths(d, &count).has_value() ? count : 0;
}

std::optional<std::string> bake_path_transform(const std::string& d, const Matrix& matrix) {
  if (matrix_is_identity(matrix)) return d;

  std::size_t pos = 0;
  char cmd = 0;
  char prev_cmd = 0;
  Point current{0.0, 0.0};
  Point subpath_start{0.0, 0.0};
  Point last_cubic_ctrl{0.0, 0.0};
  Point last_quad_ctrl{0.0, 0.0};
  bool has_last_cubic = false;
  bool has_last_quad = false;
  std::string out;

  auto next_is_number = [&](std::size_t cursor) {
    skip_separators(d, cursor);
    if (cursor >= d.size()) return false;
    const char ch = d[cursor];
    return std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.';
  };

  while (true) {
    skip_separators(d, pos);
    if (pos >= d.size()) break;
    const std::size_t iteration_start = pos;

    if (is_command_char(d[pos])) {
      cmd = d[pos++];
    } else if (cmd == 0) {
      return std::nullopt;
    }

    const bool relative = std::islower(static_cast<unsigned char>(cmd)) != 0;
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

    if (upper == 'Z') {
      out += "Z";
      current = subpath_start;
      has_last_cubic = false;
      has_last_quad = false;
      prev_cmd = upper;
      if (pos == iteration_start) return std::nullopt;
      continue;
    }

    if (!next_is_number(pos)) return std::nullopt;

    while (next_is_number(pos)) {
      if (upper == 'M') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) {
          x += current.x;
          y += current.y;
        }
        current = {x, y};
        subpath_start = current;
        out += append_point_cmd('M', apply_matrix(matrix, current));
        cmd = relative ? 'l' : 'L';
        upper = 'L';
        prev_cmd = 'M';
        has_last_cubic = false;
        has_last_quad = false;
      } else if (upper == 'L') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) {
          x += current.x;
          y += current.y;
        }
        current = {x, y};
        out += append_point_cmd('L', apply_matrix(matrix, current));
        prev_cmd = 'L';
        has_last_cubic = false;
        has_last_quad = false;
      } else if (upper == 'H') {
        double x = 0.0;
        if (!parse_number_token(d, pos, x)) return std::nullopt;
        if (relative) x += current.x;
        current.x = x;
        out += append_point_cmd('L', apply_matrix(matrix, current));
        prev_cmd = 'H';
        has_last_cubic = false;
        has_last_quad = false;
      } else if (upper == 'V') {
        double y = 0.0;
        if (!parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) y += current.y;
        current.y = y;
        out += append_point_cmd('L', apply_matrix(matrix, current));
        prev_cmd = 'V';
        has_last_cubic = false;
        has_last_quad = false;
      } else if (upper == 'C') {
        double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x1) || !parse_number_token(d, pos, y1) ||
            !parse_number_token(d, pos, x2) || !parse_number_token(d, pos, y2) ||
            !parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) {
          return std::nullopt;
        }
        if (relative) {
          x1 += current.x; y1 += current.y;
          x2 += current.x; y2 += current.y;
          x += current.x; y += current.y;
        }
        const Point p1 = apply_matrix(matrix, {x1, y1});
        const Point p2 = apply_matrix(matrix, {x2, y2});
        current = {x, y};
        const Point p = apply_matrix(matrix, current);
        out += "C" + fmt(p1.x) + "," + fmt(p1.y) + " " + fmt(p2.x) + "," + fmt(p2.y) + " " + fmt(p.x) + "," + fmt(p.y);
        last_cubic_ctrl = {x2, y2};
        has_last_cubic = true;
        has_last_quad = false;
        prev_cmd = 'C';
      } else if (upper == 'S') {
        double x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x2) || !parse_number_token(d, pos, y2) ||
            !parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) {
          return std::nullopt;
        }
        Point x1y1 = current;
        if (has_last_cubic && (prev_cmd == 'C' || prev_cmd == 'S')) {
          x1y1 = {2 * current.x - last_cubic_ctrl.x, 2 * current.y - last_cubic_ctrl.y};
        }
        if (relative) {
          x2 += current.x; y2 += current.y;
          x += current.x; y += current.y;
        }
        const Point p1 = apply_matrix(matrix, x1y1);
        const Point p2 = apply_matrix(matrix, {x2, y2});
        current = {x, y};
        const Point p = apply_matrix(matrix, current);
        out += "C" + fmt(p1.x) + "," + fmt(p1.y) + " " + fmt(p2.x) + "," + fmt(p2.y) + " " + fmt(p.x) + "," + fmt(p.y);
        last_cubic_ctrl = {x2, y2};
        has_last_cubic = true;
        has_last_quad = false;
        prev_cmd = 'S';
      } else if (upper == 'Q') {
        double x1 = 0.0, y1 = 0.0, x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x1) || !parse_number_token(d, pos, y1) ||
            !parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) {
          return std::nullopt;
        }
        if (relative) {
          x1 += current.x; y1 += current.y;
          x += current.x; y += current.y;
        }
        const Point p1 = apply_matrix(matrix, {x1, y1});
        current = {x, y};
        const Point p = apply_matrix(matrix, current);
        out += "Q" + fmt(p1.x) + "," + fmt(p1.y) + " " + fmt(p.x) + "," + fmt(p.y);
        last_quad_ctrl = {x1, y1};
        has_last_quad = true;
        has_last_cubic = false;
        prev_cmd = 'Q';
      } else if (upper == 'T') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        Point ctrl = current;
        if (has_last_quad && (prev_cmd == 'Q' || prev_cmd == 'T')) {
          ctrl = {2 * current.x - last_quad_ctrl.x, 2 * current.y - last_quad_ctrl.y};
        }
        if (relative) {
          x += current.x; y += current.y;
        }
        const Point p1 = apply_matrix(matrix, ctrl);
        current = {x, y};
        const Point p = apply_matrix(matrix, current);
        out += "Q" + fmt(p1.x) + "," + fmt(p1.y) + " " + fmt(p.x) + "," + fmt(p.y);
        last_quad_ctrl = ctrl;
        has_last_quad = true;
        has_last_cubic = false;
        prev_cmd = 'T';
      } else if (upper == 'A') {
        double rx = 0.0, ry = 0.0, rot = 0.0, x = 0.0, y = 0.0;
        int large_arc = 0, sweep = 0;
        if (!parse_number_token(d, pos, rx) || !parse_number_token(d, pos, ry) ||
            !parse_number_token(d, pos, rot) || !parse_arc_flag(d, pos, large_arc) ||
            !parse_arc_flag(d, pos, sweep) || !parse_number_token(d, pos, x) ||
            !parse_number_token(d, pos, y)) {
          return std::nullopt;
        }
        if (!valid_svg_arc_parameters(rx, ry, large_arc, sweep)) return std::nullopt;
        if (relative) {
          x += current.x; y += current.y;
        }
        if (!matrix_is_scale_translate_only(matrix)) return std::nullopt;
        const double sx = std::abs(matrix.a);
        const double sy = std::abs(matrix.d);
        const double normalized_rotation = std::fmod(std::abs(rot), 180.0);
        const bool arc_axes_match_coordinates =
          normalized_rotation <= 1e-9 || std::abs(normalized_rotation - 180.0) <= 1e-9;
        if (matrix.a <= 0.0 || matrix.d <= 0.0 ||
            (std::abs(sx - sy) > 1e-9 && !arc_axes_match_coordinates)) {
          // Reflections and non-uniform scaling of rotated elliptical arcs require
          // re-solving the ellipse parameters. Preserve the SVG transform instead.
          return std::nullopt;
        }
        current = {x, y};
        const Point p = apply_matrix(matrix, current);
        const int sweep_flag = (matrix.a * matrix.d < 0.0) ? (sweep ? 0 : 1) : sweep;
        out += "A" + fmt(rx * sx) + "," + fmt(ry * sy) + " " + fmt(rot) + " " +
               std::to_string(large_arc) + " " + std::to_string(sweep_flag) + " " +
               fmt(p.x) + "," + fmt(p.y);
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'A';
      } else {
        return std::nullopt;
      }
      skip_separators(d, pos);
      if (pos < d.size() && is_command_char(d[pos])) break;
    }
    if (pos == iteration_start) return std::nullopt;
  }

  return out;
}

bool path_is_closed(const std::string& d) {
  for (std::size_t i = d.size(); i > 0; --i) {
    const char ch = d[i - 1];
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') continue;
    return ch == 'Z' || ch == 'z';
  }
  return false;
}

bool path_has_curve_segments(const std::string& d) {
  for (char ch : d) {
    switch (ch) {
      case 'C': case 'c': case 'S': case 's':
      case 'Q': case 'q': case 'T': case 't':
      case 'A': case 'a':
        return true;
      default:
        break;
    }
  }
  return false;
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
