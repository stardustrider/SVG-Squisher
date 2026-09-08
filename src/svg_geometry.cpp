#include "svg_geometry.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "svg_path_data.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

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

void append_curve_points(std::vector<Point>& points,
                         const std::function<Point(double)>& sampler,
                         int steps) {
  for (int i = 1; i <= steps; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(steps);
    points.push_back(sampler(t));
  }
}

constexpr double kPi = 3.14159265358979323846;

struct ArcCenterParameters {
  Point center{};
  double radius_x = 0.0;
  double radius_y = 0.0;
  double rotation = 0.0;
  double start_angle = 0.0;
  double sweep_angle = 0.0;
};

bool points_equal(Point lhs, Point rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y;
}

std::optional<ArcCenterParameters> compute_arc_center(
    Point start,
    double radius_x,
    double radius_y,
    double x_axis_rotation_deg,
    int large_arc_flag,
    int sweep_flag,
    Point end) {
  radius_x = std::abs(radius_x);
  radius_y = std::abs(radius_y);
  if (radius_x <= 0.0 || radius_y <= 0.0 || points_equal(start, end)) {
    return std::nullopt;
  }

  ArcCenterParameters arc;
  arc.rotation = std::fmod(x_axis_rotation_deg, 360.0) * kPi / 180.0;
  const double cos_rotation = std::cos(arc.rotation);
  const double sin_rotation = std::sin(arc.rotation);
  const double half_delta_x = (start.x - end.x) / 2.0;
  const double half_delta_y = (start.y - end.y) / 2.0;
  const double transformed_x =
      cos_rotation * half_delta_x + sin_rotation * half_delta_y;
  const double transformed_y =
      -sin_rotation * half_delta_x + cos_rotation * half_delta_y;

  double unit_x = transformed_x / radius_x;
  double unit_y = transformed_y / radius_y;
  double radii_scale_squared = unit_x * unit_x + unit_y * unit_y;
  if (!std::isfinite(radii_scale_squared)) return std::nullopt;
  if (radii_scale_squared > 1.0) {
    const double scale = std::sqrt(radii_scale_squared);
    radius_x *= scale;
    radius_y *= scale;
    unit_x /= scale;
    unit_y /= scale;
    radii_scale_squared = unit_x * unit_x + unit_y * unit_y;
  }
  if (radii_scale_squared <= 0.0 || !std::isfinite(radius_x) ||
      !std::isfinite(radius_y)) {
    return std::nullopt;
  }

  double center_scale = std::sqrt(std::max(
      0.0, (1.0 - radii_scale_squared) / radii_scale_squared));
  if (large_arc_flag == sweep_flag) center_scale = -center_scale;
  const double transformed_center_x = center_scale * radius_x * unit_y;
  const double transformed_center_y = -center_scale * radius_y * unit_x;

  arc.center = {
    cos_rotation * transformed_center_x -
        sin_rotation * transformed_center_y +
        start.x * 0.5 + end.x * 0.5,
    sin_rotation * transformed_center_x +
        cos_rotation * transformed_center_y +
        start.y * 0.5 + end.y * 0.5,
  };
  arc.radius_x = radius_x;
  arc.radius_y = radius_y;

  const double start_unit_x =
      (transformed_x - transformed_center_x) / radius_x;
  const double start_unit_y =
      (transformed_y - transformed_center_y) / radius_y;
  const double end_unit_x =
      (-transformed_x - transformed_center_x) / radius_x;
  const double end_unit_y =
      (-transformed_y - transformed_center_y) / radius_y;
  arc.start_angle = std::atan2(start_unit_y, start_unit_x);
  arc.sweep_angle = std::atan2(
      start_unit_x * end_unit_y - start_unit_y * end_unit_x,
      start_unit_x * end_unit_x + start_unit_y * end_unit_y);
  if (!sweep_flag && arc.sweep_angle > 0.0) arc.sweep_angle -= 2.0 * kPi;
  if (sweep_flag && arc.sweep_angle < 0.0) arc.sweep_angle += 2.0 * kPi;

  if (!std::isfinite(arc.center.x) || !std::isfinite(arc.center.y) ||
      !std::isfinite(arc.start_angle) || !std::isfinite(arc.sweep_angle)) {
    return std::nullopt;
  }
  return arc;
}

Point arc_point(const ArcCenterParameters& arc, double angle) {
  const double cos_rotation = std::cos(arc.rotation);
  const double sin_rotation = std::sin(arc.rotation);
  const double cos_angle = std::cos(angle);
  const double sin_angle = std::sin(angle);
  return {
    arc.center.x + arc.radius_x * cos_rotation * cos_angle -
        arc.radius_y * sin_rotation * sin_angle,
    arc.center.y + arc.radius_x * sin_rotation * cos_angle +
        arc.radius_y * cos_rotation * sin_angle,
  };
}

double positive_angle(double angle) {
  angle = std::fmod(angle, 2.0 * kPi);
  return angle < 0.0 ? angle + 2.0 * kPi : angle;
}

bool angle_is_on_arc(const ArcCenterParameters& arc, double angle) {
  constexpr double kAngleTolerance = 1e-12;
  if (arc.sweep_angle >= 0.0) {
    return positive_angle(angle - arc.start_angle) <=
           arc.sweep_angle + kAngleTolerance;
  }
  return positive_angle(arc.start_angle - angle) <=
         -arc.sweep_angle + kAngleTolerance;
}

bool append_arc_bounds(BBox& box, const PathSegment& segment) {
  bbox_add_point(box, segment.start);
  bbox_add_point(box, segment.end);
  if (segment.radius_x <= 0.0 || segment.radius_y <= 0.0 ||
      points_equal(segment.start, segment.end)) {
    return true;
  }

  const auto arc = compute_arc_center(
      segment.start, segment.radius_x, segment.radius_y,
      segment.x_axis_rotation, segment.large_arc ? 1 : 0,
      segment.sweep ? 1 : 0, segment.end);
  if (!arc) return false;

  const double x_extremum = std::atan2(
      -arc->radius_y * std::sin(arc->rotation),
      arc->radius_x * std::cos(arc->rotation));
  const double y_extremum = std::atan2(
      arc->radius_y * std::cos(arc->rotation),
      arc->radius_x * std::sin(arc->rotation));
  for (const double angle : {
           x_extremum, x_extremum + kPi,
           y_extremum, y_extremum + kPi}) {
    if (angle_is_on_arc(*arc, angle)) bbox_add_point(box, arc_point(*arc, angle));
  }
  return true;
}

std::vector<Point> approximate_arc(Point start,
                                   double rx,
                                   double ry,
                                   double x_axis_rotation_deg,
                                   int large_arc_flag,
                                   int sweep_flag,
                                   Point end) {
  std::vector<Point> points;
  if (rx <= 0.0 || ry <= 0.0 || points_equal(start, end)) {
    points.push_back(end);
    return points;
  }

  const auto arc = compute_arc_center(
      start, rx, ry, x_axis_rotation_deg,
      large_arc_flag, sweep_flag, end);
  if (!arc) {
    points.push_back(end);
    return points;
  }

  const int steps = std::max(
      4, static_cast<int>(std::ceil(std::abs(arc->sweep_angle) / (kPi / 8.0))));
  for (int i = 1; i <= steps; ++i) {
    const double angle = arc->start_angle + arc->sweep_angle *
        (static_cast<double>(i) / static_cast<double>(steps));
    points.push_back(i == steps ? end : arc_point(*arc, angle));
  }
  return points;
}

std::vector<ParsedPath> split_subpaths(const ParsedPath& path) {
  std::vector<ParsedPath> subpaths;
  ParsedPath active;
  for (const PathSegment& segment : path.segments) {
    if (segment.kind == PathSegmentKind::Move && !active.segments.empty()) {
      subpaths.push_back(std::move(active));
      active = ParsedPath{};
    }
    active.segments.push_back(segment);
    if (segment.kind == PathSegmentKind::Close) {
      subpaths.push_back(std::move(active));
      active = ParsedPath{};
    }
  }
  if (!active.segments.empty()) subpaths.push_back(std::move(active));
  return subpaths;
}

double subpath_signed_area(const ParsedPath& path) {
  const auto subpaths = parse_straight_subpaths(serialize_path_data(path));
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

std::string reverse_subpath(const ParsedPath& path) {
  const std::string serialized = serialize_path_data(path);
  const auto subpaths = parse_straight_subpaths(serialized);
  if (!subpaths || subpaths->empty()) return serialized;
  const StrokeSubpath& subpath = subpaths->front();
  if (subpath.points.empty()) return serialized;

  std::vector<Point> points = subpath.points;
  std::reverse(points.begin(), points.end());
  std::string output = "M" + fmt(points.front().x) + "," + fmt(points.front().y);
  for (std::size_t i = 1; i < points.size(); ++i) {
    output += "L" + fmt(points[i].x) + "," + fmt(points[i].y);
  }
  if (subpath.closed) output += "Z";
  return output;
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
  const auto parsed = parse_path_data(d);
  if (!parsed) return std::nullopt;

  std::vector<StrokeSubpath> subpaths;
  StrokeSubpath active;
  auto flush = [&]() {
    if (!active.points.empty()) {
      subpaths.push_back(active);
      active = StrokeSubpath{};
    }
  };

  for (const PathSegment& segment : parsed->segments) {
    switch (segment.kind) {
      case PathSegmentKind::Move:
        flush();
        active.points.push_back(segment.end);
        break;
      case PathSegmentKind::Line:
        if (active.points.empty()) active.points.push_back(segment.start);
        active.points.push_back(segment.end);
        break;
      case PathSegmentKind::Close:
        active.closed = true;
        flush();
        break;
      case PathSegmentKind::Cubic:
      case PathSegmentKind::Quadratic:
      case PathSegmentKind::Arc:
        return std::nullopt;
    }
  }
  flush();
  return subpaths;
}

std::optional<std::vector<StrokeSubpath>> flatten_path_subpaths(
    const std::string& d,
    std::size_t* semantic_command_count) {
  const auto parsed = parse_path_data(d);
  if (!parsed) return std::nullopt;
  if (semantic_command_count) *semantic_command_count = parsed->segments.size();

  std::vector<StrokeSubpath> subpaths;
  StrokeSubpath active;
  auto flush = [&]() {
    if (!active.points.empty()) {
      subpaths.push_back(active);
      active = StrokeSubpath{};
    }
  };
  const auto push_point = [&](Point point) {
    if (active.points.empty() ||
        std::abs(active.points.back().x - point.x) > 1e-6 ||
        std::abs(active.points.back().y - point.y) > 1e-6) {
      active.points.push_back(point);
    }
  };

  for (const PathSegment& segment : parsed->segments) {
    switch (segment.kind) {
      case PathSegmentKind::Move:
        flush();
        push_point(segment.end);
        break;
      case PathSegmentKind::Line:
        if (active.points.empty()) push_point(segment.start);
        push_point(segment.end);
        break;
      case PathSegmentKind::Cubic:
        if (active.points.empty()) push_point(segment.start);
        append_curve_points(active.points, [&](double t) {
          return cubic_point(segment.start, segment.control1,
                             segment.control2, segment.end, t);
        }, 12);
        break;
      case PathSegmentKind::Quadratic:
        if (active.points.empty()) push_point(segment.start);
        append_curve_points(active.points, [&](double t) {
          return quad_point(segment.start, segment.control1, segment.end, t);
        }, 10);
        break;
      case PathSegmentKind::Arc: {
        if (active.points.empty()) push_point(segment.start);
        const auto points = approximate_arc(
            segment.start, segment.radius_x, segment.radius_y,
            segment.x_axis_rotation, segment.large_arc ? 1 : 0,
            segment.sweep ? 1 : 0, segment.end);
        for (const Point point : points) push_point(point);
        break;
      }
      case PathSegmentKind::Close:
        active.closed = true;
        flush();
        break;
    }
  }
  flush();
  return subpaths;
}

std::string convert_evenodd_to_nonzero(const std::string& d) {
  const auto parsed = parse_path_data(d);
  if (!parsed) return d;
  const std::vector<ParsedPath> subpaths = split_subpaths(*parsed);
  if (subpaths.size() <= 1) return d;

  const bool outer_clockwise = subpath_signed_area(subpaths.front()) >= 0.0;
  std::string output = serialize_path_data(subpaths.front());
  for (std::size_t i = 1; i < subpaths.size(); ++i) {
    const bool hole_clockwise = subpath_signed_area(subpaths[i]) >= 0.0;
    output += " ";
    output += hole_clockwise == outer_clockwise
        ? reverse_subpath(subpaths[i])
        : serialize_path_data(subpaths[i]);
  }
  return output;
}

std::optional<BBox> path_bbox(const std::string& d) {
  const auto parsed = parse_path_data(d);
  if (!parsed) return std::nullopt;

  BBox box{
    std::numeric_limits<double>::infinity(),
    std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
  };
  for (const PathSegment& segment : parsed->segments) {
    switch (segment.kind) {
      case PathSegmentKind::Move:
      case PathSegmentKind::Line:
      case PathSegmentKind::Close:
        bbox_add_point(box, segment.end);
        break;
      case PathSegmentKind::Cubic:
        bbox_add_point(box, segment.control1);
        bbox_add_point(box, segment.control2);
        bbox_add_point(box, segment.end);
        break;
      case PathSegmentKind::Quadratic:
        bbox_add_point(box, segment.control1);
        bbox_add_point(box, segment.end);
        break;
      case PathSegmentKind::Arc:
        if (!append_arc_bounds(box, segment)) return std::nullopt;
        break;
    }
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
