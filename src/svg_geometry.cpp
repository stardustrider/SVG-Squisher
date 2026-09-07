#include "svg_geometry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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

Point cubic_point(Point p0, Point p1, Point p2, Point p3, double t) {
  const double mt = 1.0 - t;
  const double mt2 = mt * mt;
  const double t2 = t * t;
  return {
    mt2 * mt * p0.x + 3 * mt2 * t * p1.x + 3 * mt * t2 * p2.x + t2 * t * p3.x,
    mt2 * mt * p0.y + 3 * mt2 * t * p1.y + 3 * mt * t2 * p2.y + t2 * t * p3.y,
  };
}

Point quad_point(Point p0, Point p1, Point p2, double t) {
  const double mt = 1.0 - t;
  return {
    mt * mt * p0.x + 2 * mt * t * p1.x + t * t * p2.x,
    mt * mt * p0.y + 2 * mt * t * p1.y + t * t * p2.y,
  };
}

void append_curve_points(std::vector<Point>& points, const std::function<Point(double)>& sampler, int steps) {
  for (int i = 1; i <= steps; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(steps);
    points.push_back(sampler(t));
  }
}

std::vector<Point> approximate_arc(Point start,
                                   double rx,
                                   double ry,
                                   double x_axis_rotation_deg,
                                   int large_arc_flag,
                                   int sweep_flag,
                                   Point end) {
  std::vector<Point> points;
  if (rx <= 0.0 || ry <= 0.0 || (std::abs(start.x - end.x) < 1e-9 && std::abs(start.y - end.y) < 1e-9)) {
    points.push_back(end);
    return points;
  }

  const double phi = x_axis_rotation_deg * 3.14159265358979323846 / 180.0;
  const double cos_phi = std::cos(phi);
  const double sin_phi = std::sin(phi);
  const double dx2 = (start.x - end.x) / 2.0;
  const double dy2 = (start.y - end.y) / 2.0;
  const double x1p = cos_phi * dx2 + sin_phi * dy2;
  const double y1p = -sin_phi * dx2 + cos_phi * dy2;

  rx = std::abs(rx);
  ry = std::abs(ry);

  const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1.0) {
    const double scale = std::sqrt(lambda);
    rx *= scale;
    ry *= scale;
  }

  const double rx2 = rx * rx;
  const double ry2 = ry * ry;
  const double x1p2 = x1p * x1p;
  const double y1p2 = y1p * y1p;
  double numerator = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
  double denominator = rx2 * y1p2 + ry2 * x1p2;
  double factor = 0.0;
  if (denominator > 1e-12) {
    factor = std::sqrt(std::max(0.0, numerator / denominator));
  }
  if (large_arc_flag == sweep_flag) factor = -factor;

  const double cxp = factor * ((rx * y1p) / ry);
  const double cyp = factor * (-(ry * x1p) / rx);
  const double cx = cos_phi * cxp - sin_phi * cyp + (start.x + end.x) / 2.0;
  const double cy = sin_phi * cxp + cos_phi * cyp + (start.y + end.y) / 2.0;

  auto angle_between = [](double ux, double uy, double vx, double vy) {
    const double dot = ux * vx + uy * vy;
    const double det = ux * vy - uy * vx;
    return std::atan2(det, dot);
  };

  const double ux = (x1p - cxp) / rx;
  const double uy = (y1p - cyp) / ry;
  const double vx = (-x1p - cxp) / rx;
  const double vy = (-y1p - cyp) / ry;
  double theta1 = std::atan2(uy, ux);
  double delta_theta = angle_between(ux, uy, vx, vy);

  if (!sweep_flag && delta_theta > 0) delta_theta -= 2.0 * 3.14159265358979323846;
  if (sweep_flag && delta_theta < 0) delta_theta += 2.0 * 3.14159265358979323846;

  const int steps = std::max(4, static_cast<int>(std::ceil(std::abs(delta_theta) / (3.14159265358979323846 / 8.0))));
  for (int i = 1; i <= steps; ++i) {
    const double theta = theta1 + delta_theta * (static_cast<double>(i) / static_cast<double>(steps));
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);
    points.push_back({
      cx + rx * cos_phi * cos_theta - ry * sin_phi * sin_theta,
      cy + rx * sin_phi * cos_theta + ry * cos_phi * sin_theta,
    });
  }
  return points;
}

std::vector<std::string> split_subpaths(const std::string& d) {
  std::vector<std::string> subpaths;
  std::size_t pos = 0;
  char cmd = 0;
  std::string current;

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
      if ((cmd == 'M' || cmd == 'm') && !current.empty()) {
        subpaths.push_back(trim(current));
        current.clear();
      }
      current += cmd;
    } else if (cmd == 0) {
      break;
    }

    if (std::toupper(static_cast<unsigned char>(cmd)) == 'Z') {
      if (!current.empty()) {
        subpaths.push_back(trim(current));
        current.clear();
      }
      if (pos == iteration_start) return {};
      continue;
    }

    if (!next_is_number(pos)) return {};

    while (next_is_number(pos)) {
      double value = 0.0;
      if (!parse_number_token(d, pos, value)) break;
      current += (current.back() == ' ' || current.back() == ',' || is_command_char(current.back())) ? "" : ",";
      current += fmt(value);
      skip_separators(d, pos);
      if (pos < d.size() && is_command_char(d[pos])) {
        if (std::toupper(static_cast<unsigned char>(d[pos])) == 'M' && !current.empty()) {
          subpaths.push_back(trim(current));
          current.clear();
        }
        break;
      }
      if (pos < d.size()) current += " ";
    }
    if (pos == iteration_start) return {};
  }

  if (!trim(current).empty()) subpaths.push_back(trim(current));
  return subpaths;
}

double subpath_signed_area(const std::string& d) {
  const auto subpaths = parse_straight_subpaths(d);
  if (!subpaths || subpaths->empty()) return 0.0;
  const std::vector<Point>& points = subpaths->front().points;
  if (points.size() < 3) return 0.0;

  double area = 0.0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const Point& a = points[i];
    const Point& b = points[(i + 1) % points.size()];
    area += a.x * b.y - b.x * a.y;
  }
  return area / 2.0;
}

std::string reverse_subpath(const std::string& d) {
  const auto subpaths = parse_straight_subpaths(d);
  if (!subpaths || subpaths->empty()) return d;
  const StrokeSubpath& subpath = subpaths->front();
  if (subpath.points.empty()) return d;

  std::vector<Point> points = subpath.points;
  std::reverse(points.begin(), points.end());

  std::string out = "M" + fmt(points.front().x) + "," + fmt(points.front().y);
  for (std::size_t i = 1; i < points.size(); ++i) {
    out += "L" + fmt(points[i].x) + "," + fmt(points[i].y);
  }
  if (subpath.closed) out += "Z";
  return out;
}

}  // namespace

std::string append_point_cmd(char cmd, Point p) {
  return std::string(1, cmd) + fmt(p.x) + "," + fmt(p.y);
}

Point operator+(Point lhs, Point rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y}; }
Point operator-(Point lhs, Point rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y}; }
Point operator*(Point lhs, double scalar) { return {lhs.x * scalar, lhs.y * scalar}; }
double point_length(Point p) { return std::sqrt(p.x * p.x + p.y * p.y); }

void bbox_add_point(BBox& box, Point p) {
  if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
  box.min_x = std::min(box.min_x, p.x);
  box.min_y = std::min(box.min_y, p.y);
  box.max_x = std::max(box.max_x, p.x);
  box.max_y = std::max(box.max_y, p.y);
}

bool bbox_valid(const BBox& box) {
  return std::isfinite(box.min_x) && std::isfinite(box.min_y) &&
         std::isfinite(box.max_x) && std::isfinite(box.max_y);
}

double bbox_width(const BBox& box) { return bbox_valid(box) ? box.max_x - box.min_x : 0.0; }
double bbox_height(const BBox& box) { return bbox_valid(box) ? box.max_y - box.min_y : 0.0; }

std::optional<std::vector<StrokeSubpath>> parse_straight_subpaths(const std::string& d) {
  std::vector<StrokeSubpath> subpaths;
  std::size_t pos = 0;
  char cmd = 0;
  Point current{0.0, 0.0};
  Point subpath_start{0.0, 0.0};
  StrokeSubpath active;

  auto flush_active = [&]() {
    if (!active.points.empty()) {
      subpaths.push_back(active);
      active = StrokeSubpath{};
    }
  };

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
      active.closed = true;
      current = subpath_start;
      flush_active();
      if (pos == iteration_start) return std::nullopt;
      continue;
    }

    if (upper != 'M' && upper != 'L' && upper != 'H' && upper != 'V') {
      return std::nullopt;
    }

    if (!next_is_number(pos)) return std::nullopt;

    while (next_is_number(pos)) {
      if (upper == 'M') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        flush_active();
        current = {x, y};
        subpath_start = current;
        active.points.push_back(current);
        cmd = relative ? 'l' : 'L';
        upper = 'L';
      } else if (upper == 'L') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        current = {x, y};
        active.points.push_back(current);
      } else if (upper == 'H') {
        double x = 0.0;
        if (!parse_number_token(d, pos, x)) return std::nullopt;
        if (relative) x += current.x;
        current.x = x;
        active.points.push_back(current);
      } else if (upper == 'V') {
        double y = 0.0;
        if (!parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) y += current.y;
        current.y = y;
        active.points.push_back(current);
      }
      skip_separators(d, pos);
      if (pos < d.size() && is_command_char(d[pos])) break;
    }
    if (pos == iteration_start) return std::nullopt;
  }

  flush_active();
  return subpaths;
}

std::optional<std::vector<StrokeSubpath>> flatten_path_subpaths(
    const std::string& d,
    std::size_t* semantic_command_count) {
  std::vector<StrokeSubpath> subpaths;
  std::size_t parsed_commands = 0;
  std::size_t pos = 0;
  char cmd = 0;
  char prev_cmd = 0;
  Point current{0.0, 0.0};
  Point subpath_start{0.0, 0.0};
  Point last_cubic_ctrl{0.0, 0.0};
  Point last_quad_ctrl{0.0, 0.0};
  bool has_last_cubic = false;
  bool has_last_quad = false;
  bool first_command = true;
  StrokeSubpath active;

  auto flush_active = [&]() {
    if (!active.points.empty()) {
      subpaths.push_back(active);
      active = StrokeSubpath{};
    }
  };

  auto push_point = [&](Point p) {
    if (active.points.empty() ||
        std::abs(active.points.back().x - p.x) > 1e-6 ||
        std::abs(active.points.back().y - p.y) > 1e-6) {
      active.points.push_back(p);
    }
  };

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
    if (first_command && upper != 'M') return std::nullopt;

    if (upper == 'Z') {
      active.closed = true;
      current = subpath_start;
      flush_active();
      has_last_cubic = false;
      has_last_quad = false;
      prev_cmd = 'Z';
      ++parsed_commands;
      first_command = false;
      cmd = 0;
      if (pos == iteration_start) return std::nullopt;
      continue;
    }

    if (!next_is_number(pos)) return std::nullopt;

    while (next_is_number(pos)) {
      if (upper == 'M') {
        double x=0.0,y=0.0;
        if (!parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        flush_active();
        current = {x,y};
        subpath_start = current;
        push_point(current);
        cmd = relative ? 'l' : 'L';
        upper = 'L';
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'M';
      } else if (upper == 'L') {
        double x=0.0,y=0.0;
        if (!parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        current = {x,y};
        push_point(current);
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'L';
      } else if (upper == 'H') {
        double x=0.0;
        if (!parse_number_token(d,pos,x)) return std::nullopt;
        if (relative) x += current.x;
        current.x = x;
        push_point(current);
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'H';
      } else if (upper == 'V') {
        double y=0.0;
        if (!parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) y += current.y;
        current.y = y;
        push_point(current);
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'V';
      } else if (upper == 'C') {
        double x1=0,y1=0,x2=0,y2=0,x=0,y=0;
        if (!parse_number_token(d,pos,x1) || !parse_number_token(d,pos,y1) ||
            !parse_number_token(d,pos,x2) || !parse_number_token(d,pos,y2) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x1 += current.x; y1 += current.y; x2 += current.x; y2 += current.y; x += current.x; y += current.y; }
        const Point p0 = current;
        const Point p1{x1,y1}, p2{x2,y2}, p3{x,y};
        append_curve_points(active.points, [&](double t) { return cubic_point(p0,p1,p2,p3,t); }, 12);
        current = p3;
        last_cubic_ctrl = p2;
        has_last_cubic = true;
        has_last_quad = false;
        prev_cmd = 'C';
      } else if (upper == 'S') {
        double x2=0,y2=0,x=0,y=0;
        if (!parse_number_token(d,pos,x2) || !parse_number_token(d,pos,y2) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        Point p1 = current;
        if (has_last_cubic && (prev_cmd == 'C' || prev_cmd == 'S')) {
          p1 = {2 * current.x - last_cubic_ctrl.x, 2 * current.y - last_cubic_ctrl.y};
        }
        if (relative) { x2 += current.x; y2 += current.y; x += current.x; y += current.y; }
        const Point p0 = current;
        const Point p2{x2,y2}, p3{x,y};
        append_curve_points(active.points, [&](double t) { return cubic_point(p0,p1,p2,p3,t); }, 12);
        current = p3;
        last_cubic_ctrl = p2;
        has_last_cubic = true;
        has_last_quad = false;
        prev_cmd = 'S';
      } else if (upper == 'Q') {
        double x1=0,y1=0,x=0,y=0;
        if (!parse_number_token(d,pos,x1) || !parse_number_token(d,pos,y1) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x1 += current.x; y1 += current.y; x += current.x; y += current.y; }
        const Point p0 = current;
        const Point p1{x1,y1}, p2{x,y};
        append_curve_points(active.points, [&](double t) { return quad_point(p0,p1,p2,t); }, 10);
        current = p2;
        last_quad_ctrl = p1;
        has_last_quad = true;
        has_last_cubic = false;
        prev_cmd = 'Q';
      } else if (upper == 'T') {
        double x=0,y=0;
        if (!parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        Point ctrl = current;
        if (has_last_quad && (prev_cmd == 'Q' || prev_cmd == 'T')) {
          ctrl = {2 * current.x - last_quad_ctrl.x, 2 * current.y - last_quad_ctrl.y};
        }
        if (relative) { x += current.x; y += current.y; }
        const Point p0 = current;
        const Point p2{x,y};
        append_curve_points(active.points, [&](double t) { return quad_point(p0,ctrl,p2,t); }, 10);
        current = p2;
        last_quad_ctrl = ctrl;
        has_last_quad = true;
        has_last_cubic = false;
        prev_cmd = 'T';
      } else if (upper == 'A') {
        double rx=0,ry=0,rot=0,x=0,y=0;
        int large=0,sweep=0;
        if (!parse_number_token(d,pos,rx) || !parse_number_token(d,pos,ry) ||
            !parse_number_token(d,pos,rot) || !parse_arc_flag(d,pos,large) ||
            !parse_arc_flag(d,pos,sweep) || !parse_number_token(d,pos,x) ||
            !parse_number_token(d,pos,y)) return std::nullopt;
        if (!valid_svg_arc_parameters(rx, ry, large, sweep)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        const Point end{x,y};
        const auto arc_points = approximate_arc(current, rx, ry, rot, large, sweep, end);
        active.points.insert(active.points.end(), arc_points.begin(), arc_points.end());
        current = end;
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'A';
      } else {
        return std::nullopt;
      }

      ++parsed_commands;
      first_command = false;

      skip_separators(d, pos);
      if (pos < d.size() && is_command_char(d[pos])) break;
    }
    if (pos == iteration_start) return std::nullopt;
  }

  flush_active();
  if (semantic_command_count) *semantic_command_count = parsed_commands;
  return subpaths;
}

std::string convert_evenodd_to_nonzero(const std::string& d) {
  const std::vector<std::string> subpaths = split_subpaths(d);
  if (subpaths.empty()) return d;
  if (subpaths.size() <= 1) return d;

  const bool outer_cw = subpath_signed_area(subpaths.front()) >= 0.0;
  std::string out = subpaths.front();
  for (std::size_t i = 1; i < subpaths.size(); ++i) {
    const bool hole_cw = subpath_signed_area(subpaths[i]) >= 0.0;
    out += " ";
    out += (hole_cw == outer_cw) ? reverse_subpath(subpaths[i]) : subpaths[i];
  }
  return out;
}

std::optional<BBox> path_bbox(const std::string& d) {
  std::size_t pos = 0;
  char cmd = 0;
  Point current{0.0, 0.0};
  BBox box{
    std::numeric_limits<double>::infinity(),
    std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity()
  };

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
      if (pos == iteration_start) return std::nullopt;
      continue;
    }

    if (!next_is_number(pos)) return std::nullopt;

    while (next_is_number(pos)) {
      if (upper == 'M' || upper == 'L') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        current = {x, y};
        bbox_add_point(box, current);
        if (upper == 'M') {
          cmd = relative ? 'l' : 'L';
          upper = 'L';
        }
      } else if (upper == 'H') {
        double x = 0.0;
        if (!parse_number_token(d, pos, x)) return std::nullopt;
        if (relative) x += current.x;
        current.x = x;
        bbox_add_point(box, current);
      } else if (upper == 'V') {
        double y = 0.0;
        if (!parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) y += current.y;
        current.y = y;
        bbox_add_point(box, current);
      } else if (upper == 'C') {
        double x1=0,y1=0,x2=0,y2=0,x=0,y=0;
        if (!parse_number_token(d,pos,x1) || !parse_number_token(d,pos,y1) ||
            !parse_number_token(d,pos,x2) || !parse_number_token(d,pos,y2) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x1 += current.x; y1 += current.y; x2 += current.x; y2 += current.y; x += current.x; y += current.y; }
        bbox_add_point(box, {x1,y1});
        bbox_add_point(box, {x2,y2});
        current = {x,y};
        bbox_add_point(box, current);
      } else if (upper == 'Q') {
        double x1=0,y1=0,x=0,y=0;
        if (!parse_number_token(d,pos,x1) || !parse_number_token(d,pos,y1) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x1 += current.x; y1 += current.y; x += current.x; y += current.y; }
        bbox_add_point(box, {x1,y1});
        current = {x,y};
        bbox_add_point(box, current);
      } else if (upper == 'A') {
        double rx=0,ry=0,rot=0,x=0,y=0;
        int large=0,sweep=0;
        if (!parse_number_token(d,pos,rx) || !parse_number_token(d,pos,ry) ||
            !parse_number_token(d,pos,rot) || !parse_arc_flag(d,pos,large) ||
            !parse_arc_flag(d,pos,sweep) || !parse_number_token(d,pos,x) ||
            !parse_number_token(d,pos,y)) return std::nullopt;
        if (!valid_svg_arc_parameters(rx, ry, large, sweep)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        bbox_add_point(box, {x - rx, y - ry});
        bbox_add_point(box, {x + rx, y + ry});
        current = {x,y};
        bbox_add_point(box, current);
      } else {
        return std::nullopt;
      }
      skip_separators(d, pos);
      if (pos < d.size() && is_command_char(d[pos])) break;
    }
    if (pos == iteration_start) return std::nullopt;
  }

  if (!bbox_valid(box)) return std::nullopt;
  return box;
}

bool bbox_contains(const std::optional<BBox>& outer,
                   const std::optional<BBox>& inner,
                   double tolerance) {
  if (!outer || !inner) return false;
  return outer->min_x <= inner->min_x + tolerance &&
         outer->min_y <= inner->min_y + tolerance &&
         outer->max_x >= inner->max_x - tolerance &&
         outer->max_y >= inner->max_y - tolerance;
}

}  // namespace svg_squisher
