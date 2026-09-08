#include "svg_path_data.h"

#include <cctype>
#include <optional>
#include <string>

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

bool is_number_start(char ch) {
  return std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
         ch == '-' || ch == '+' || ch == '.';
}

bool is_number_end(char ch) {
  return std::isdigit(static_cast<unsigned char>(ch)) != 0 || ch == '.';
}

// The shared number lexer deliberately accepts comma-wsp before a token so it
// can serve length lists too. Path data has a stricter grammar: one comma may
// only separate two numbers, never precede the first parameter, follow the last
// parameter, or sit next to another comma.
bool path_separators_are_valid(const std::string& data) {
  for (std::size_t index = 0; index < data.size(); ++index) {
    const char ch = data[index];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0 &&
        !is_svg_whitespace(ch)) {
      return false;
    }
    if (ch != ',') continue;

    std::size_t before = index;
    while (before > 0 && is_svg_whitespace(data[before - 1])) --before;
    if (before == 0 || !is_number_end(data[before - 1])) return false;

    std::size_t after = index + 1;
    while (after < data.size() && is_svg_whitespace(data[after])) ++after;
    if (after >= data.size() || !is_number_start(data[after])) return false;
  }
  return true;
}

bool next_is_number(const std::string& data, std::size_t cursor) {
  skip_separators(data, cursor);
  if (cursor >= data.size()) return false;
  return is_number_start(data[cursor]);
}

bool parse_nonnegative_number_token(const std::string& data,
                                    std::size_t& position,
                                    double& value) {
  std::size_t cursor = position;
  skip_separators(data, cursor);
  if (cursor >= data.size() || data[cursor] == '+' || data[cursor] == '-') {
    return false;
  }
  return parse_number_token(data, position, value);
}

}  // namespace

std::optional<ParsedPath> parse_path_data(const std::string& data) {
  if (!path_separators_are_valid(data)) return std::nullopt;

  ParsedPath path;
  std::size_t position = 0;
  char command = 0;
  Point current{0.0, 0.0};
  Point subpath_start{0.0, 0.0};
  Point last_cubic_control{0.0, 0.0};
  Point last_quadratic_control{0.0, 0.0};
  bool has_last_cubic = false;
  bool has_last_quadratic = false;
  bool first_command = true;

  while (true) {
    skip_separators(data, position);
    if (position >= data.size()) break;

    if (is_command_char(data[position])) {
      command = data[position++];
    } else if (command == 0) {
      return std::nullopt;
    }

    const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
    char normalized_command =
        static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
    if (first_command && normalized_command != 'M') return std::nullopt;

    if (normalized_command == 'Z') {
      path.segments.push_back({PathSegmentKind::Close, current, {}, {}, subpath_start});
      current = subpath_start;
      has_last_cubic = false;
      has_last_quadratic = false;
      first_command = false;
      command = 0;
      continue;
    }

    if (!next_is_number(data, position)) return std::nullopt;

    while (next_is_number(data, position)) {
      PathSegment segment;
      segment.start = current;

      if (normalized_command == 'M' || normalized_command == 'L') {
        double x = 0.0;
        double y = 0.0;
        if (!parse_number_token(data, position, x) ||
            !parse_number_token(data, position, y)) {
          return std::nullopt;
        }
        if (relative) {
          x += current.x;
          y += current.y;
        }
        segment.kind = normalized_command == 'M' ? PathSegmentKind::Move
                                                  : PathSegmentKind::Line;
        segment.end = {x, y};
        current = segment.end;
        if (normalized_command == 'M') {
          subpath_start = current;
          command = relative ? 'l' : 'L';
          normalized_command = 'L';
        }
      } else if (normalized_command == 'H') {
        double x = 0.0;
        if (!parse_number_token(data, position, x)) return std::nullopt;
        if (relative) x += current.x;
        segment.kind = PathSegmentKind::Line;
        segment.end = {x, current.y};
        current = segment.end;
      } else if (normalized_command == 'V') {
        double y = 0.0;
        if (!parse_number_token(data, position, y)) return std::nullopt;
        if (relative) y += current.y;
        segment.kind = PathSegmentKind::Line;
        segment.end = {current.x, y};
        current = segment.end;
      } else if (normalized_command == 'C') {
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        double x = 0.0;
        double y = 0.0;
        if (!parse_number_token(data, position, x1) ||
            !parse_number_token(data, position, y1) ||
            !parse_number_token(data, position, x2) ||
            !parse_number_token(data, position, y2) ||
            !parse_number_token(data, position, x) ||
            !parse_number_token(data, position, y)) {
          return std::nullopt;
        }
        if (relative) {
          x1 += current.x;
          y1 += current.y;
          x2 += current.x;
          y2 += current.y;
          x += current.x;
          y += current.y;
        }
        segment.kind = PathSegmentKind::Cubic;
        segment.control1 = {x1, y1};
        segment.control2 = {x2, y2};
        segment.end = {x, y};
        current = segment.end;
        last_cubic_control = segment.control2;
      } else if (normalized_command == 'S') {
        double x2 = 0.0;
        double y2 = 0.0;
        double x = 0.0;
        double y = 0.0;
        if (!parse_number_token(data, position, x2) ||
            !parse_number_token(data, position, y2) ||
            !parse_number_token(data, position, x) ||
            !parse_number_token(data, position, y)) {
          return std::nullopt;
        }
        segment.kind = PathSegmentKind::Cubic;
        segment.control1 = has_last_cubic
            ? Point{2.0 * current.x - last_cubic_control.x,
                    2.0 * current.y - last_cubic_control.y}
            : current;
        if (relative) {
          x2 += current.x;
          y2 += current.y;
          x += current.x;
          y += current.y;
        }
        segment.control2 = {x2, y2};
        segment.end = {x, y};
        current = segment.end;
        last_cubic_control = segment.control2;
      } else if (normalized_command == 'Q') {
        double x1 = 0.0;
        double y1 = 0.0;
        double x = 0.0;
        double y = 0.0;
        if (!parse_number_token(data, position, x1) ||
            !parse_number_token(data, position, y1) ||
            !parse_number_token(data, position, x) ||
            !parse_number_token(data, position, y)) {
          return std::nullopt;
        }
        if (relative) {
          x1 += current.x;
          y1 += current.y;
          x += current.x;
          y += current.y;
        }
        segment.kind = PathSegmentKind::Quadratic;
        segment.control1 = {x1, y1};
        segment.end = {x, y};
        current = segment.end;
        last_quadratic_control = segment.control1;
      } else if (normalized_command == 'T') {
        double x = 0.0;
        double y = 0.0;
        if (!parse_number_token(data, position, x) ||
            !parse_number_token(data, position, y)) {
          return std::nullopt;
        }
        segment.kind = PathSegmentKind::Quadratic;
        segment.control1 = has_last_quadratic
            ? Point{2.0 * current.x - last_quadratic_control.x,
                    2.0 * current.y - last_quadratic_control.y}
            : current;
        if (relative) {
          x += current.x;
          y += current.y;
        }
        segment.end = {x, y};
        current = segment.end;
        last_quadratic_control = segment.control1;
      } else if (normalized_command == 'A') {
        double radius_x = 0.0;
        double radius_y = 0.0;
        double rotation = 0.0;
        double x = 0.0;
        double y = 0.0;
        int large_arc = 0;
        int sweep = 0;
        if (!parse_nonnegative_number_token(data, position, radius_x) ||
            !parse_nonnegative_number_token(data, position, radius_y) ||
            !parse_number_token(data, position, rotation) ||
            !parse_arc_flag(data, position, large_arc) ||
            !parse_arc_flag(data, position, sweep) ||
            !parse_number_token(data, position, x) ||
            !parse_number_token(data, position, y) ||
            !valid_svg_arc_parameters(radius_x, radius_y, large_arc, sweep)) {
          return std::nullopt;
        }
        if (relative) {
          x += current.x;
          y += current.y;
        }
        segment.kind = PathSegmentKind::Arc;
        segment.radius_x = radius_x;
        segment.radius_y = radius_y;
        segment.x_axis_rotation = rotation;
        segment.large_arc = large_arc != 0;
        segment.sweep = sweep != 0;
        segment.end = {x, y};
        current = segment.end;
      } else {
        return std::nullopt;
      }

      const bool is_cubic = segment.kind == PathSegmentKind::Cubic;
      const bool is_quadratic = segment.kind == PathSegmentKind::Quadratic;
      if (!is_cubic) has_last_cubic = false;
      else has_last_cubic = true;
      if (!is_quadratic) has_last_quadratic = false;
      else has_last_quadratic = true;
      path.segments.push_back(segment);
      first_command = false;

      skip_separators(data, position);
      if (position < data.size() && is_command_char(data[position])) break;
    }
  }

  return path;
}

std::string serialize_path_data(const ParsedPath& path) {
  std::string output;
  std::optional<PathSegmentKind> previous_kind;

  const auto append_number = [&](double value) {
    const std::string number = fmt(value);
    if (!output.empty() &&
        std::isalpha(static_cast<unsigned char>(output.back())) == 0 &&
        number.front() != '-' && number.front() != '+') {
      output += ' ';
    }
    output += number;
  };

  for (const PathSegment& segment : path.segments) {
    const bool repeatable = segment.kind != PathSegmentKind::Move &&
                            segment.kind != PathSegmentKind::Close;
    const bool implicit_line_after_move =
        previous_kind == PathSegmentKind::Move && segment.kind == PathSegmentKind::Line;
    const bool omit_command = implicit_line_after_move ||
        (repeatable && previous_kind == segment.kind);

    switch (segment.kind) {
      case PathSegmentKind::Move:
        output += "M";
        append_number(segment.end.x);
        append_number(segment.end.y);
        break;
      case PathSegmentKind::Line:
        if (!omit_command) output += "L";
        append_number(segment.end.x);
        append_number(segment.end.y);
        break;
      case PathSegmentKind::Cubic:
        if (!omit_command) output += "C";
        append_number(segment.control1.x);
        append_number(segment.control1.y);
        append_number(segment.control2.x);
        append_number(segment.control2.y);
        append_number(segment.end.x);
        append_number(segment.end.y);
        break;
      case PathSegmentKind::Quadratic:
        if (!omit_command) output += "Q";
        append_number(segment.control1.x);
        append_number(segment.control1.y);
        append_number(segment.end.x);
        append_number(segment.end.y);
        break;
      case PathSegmentKind::Arc:
        if (!omit_command) output += "A";
        append_number(segment.radius_x);
        append_number(segment.radius_y);
        append_number(segment.x_axis_rotation);
        // Arc flags have fixed one-character width. Keeping one separator before
        // them lets the two flags and a positive x coordinate be concatenated.
        output += " ";
        output += segment.large_arc ? '1' : '0';
        output += segment.sweep ? '1' : '0';
        output += fmt(segment.end.x);
        append_number(segment.end.y);
        break;
      case PathSegmentKind::Close:
        output += "Z";
        break;
    }
    previous_kind = segment.kind;
  }
  return output;
}

}  // namespace svg_squisher
