#include "svg_stroke.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "svg_geometry.h"
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

struct OutlineStep {
  Point point;
  bool arc = false;
  int sweep = 0;
};

struct StrokeSegment {
  Point start;
  Point end;
  Point dir;
  Point normal;
};

std::string append_line_or_arc(std::string d, const Point& from, const Point& to, bool arc, int sweep, double radius) {
  (void)from;
  if (arc) {
    d += "A" + fmt(radius) + "," + fmt(radius) + " 0 0 " + std::to_string(sweep) + " " + fmt(to.x) + "," + fmt(to.y);
  } else {
    d += "L" + fmt(to.x) + "," + fmt(to.y);
  }
  return d;
}

std::vector<StrokeSegment> build_segments(const std::vector<Point>& points, double half_width, bool closed) {
  std::vector<StrokeSegment> segments;
  const std::size_t count = points.size();
  if (count < 2) return segments;
  const std::size_t limit = closed ? count : count - 1;
  for (std::size_t i = 0; i < limit; ++i) {
    const Point start = points[i];
    const Point end = points[(i + 1) % count];
    const Point delta = end - start;
    const double len = point_length(delta);
    if (len <= 1e-9) continue;
    const Point dir{delta.x / len, delta.y / len};
    const Point normal{(-delta.y / len) * half_width, (delta.x / len) * half_width};
    segments.push_back({start, end, dir, normal});
  }
  return segments;
}

std::string build_fallback_polyline_outline(const std::vector<Point>& points, double half_width, bool closed) {
  if (points.size() < 2) return "";

  std::vector<Point> left;
  std::vector<Point> right;
  left.reserve(points.size());
  right.reserve(points.size());

  auto segment_normal = [&](std::size_t from, std::size_t to) {
    const Point delta = points[to] - points[from];
    const double len = point_length(delta);
    if (len <= 1e-9) return Point{0.0, 0.0};
    return Point{(-delta.y / len) * half_width, (delta.x / len) * half_width};
  };

  for (std::size_t i = 0; i < points.size(); ++i) {
    Point normal{0.0, 0.0};
    if (i == 0) {
      normal = segment_normal(0, 1);
    } else if (i == points.size() - 1) {
      normal = segment_normal(points.size() - 2, points.size() - 1);
    } else {
      const Point n1 = segment_normal(i - 1, i);
      const Point n2 = segment_normal(i, i + 1);
      normal = {(n1.x + n2.x) / 2.0, (n1.y + n2.y) / 2.0};
      const double len = point_length(normal);
      if (len > 1e-9) {
        normal = {normal.x * (half_width / len), normal.y * (half_width / len)};
      } else {
        normal = n2;
      }
    }
    left.push_back(points[i] + normal);
    right.push_back(points[i] - normal);
  }

  std::string d = "M" + fmt(left.front().x) + "," + fmt(left.front().y);
  for (std::size_t i = 1; i < left.size(); ++i) {
    d += "L" + fmt(left[i].x) + "," + fmt(left[i].y);
  }

  if (closed) {
    for (std::size_t i = right.size(); i-- > 0;) {
      d += "L" + fmt(right[i].x) + "," + fmt(right[i].y);
    }
    d += "Z";
    return d;
  }

  d += "L" + fmt(right.back().x) + "," + fmt(right.back().y);
  for (std::size_t i = right.size() - 1; i-- > 0;) {
    d += "L" + fmt(right[i].x) + "," + fmt(right[i].y);
  }
  d += "Z";
  return d;
}

std::optional<Point> line_intersection(Point p1, Point d1, Point p2, Point d2) {
  const double det = d1.x * (-d2.y) - d1.y * (-d2.x);
  if (std::abs(det) <= 1e-10) return std::nullopt;
  const Point delta = p2 - p1;
  const double t = (delta.x * (-d2.y) - delta.y * (-d2.x)) / det;
  return p1 + d1 * t;
}

std::vector<OutlineStep> compute_join_steps(const std::vector<StrokeSegment>& segments,
                                            int sign,
                                            const std::string& linejoin,
                                            double half_width,
                                            double miter_limit) {
  std::vector<OutlineStep> steps;
  const std::size_t count = segments.size();
  for (std::size_t i = 0; i < count; ++i) {
    const StrokeSegment& s1 = segments[i];
    const StrokeSegment& s2 = segments[(i + 1) % count];
    const double cross = s1.dir.x * s2.dir.y - s1.dir.y * s2.dir.x;

    const Point p1{s1.end.x + sign * s1.normal.x, s1.end.y + sign * s1.normal.y};
    const Point p2{s2.start.x + sign * s2.normal.x, s2.start.y + sign * s2.normal.y};
    const bool is_outer = (sign > 0 && cross < 0) || (sign < 0 && cross > 0);

    if (is_outer && linejoin == "round") {
      steps.push_back({p1, false, 0});
      steps.push_back({p2, true, cross > 0 ? 1 : 0});
      continue;
    }

    if (is_outer && linejoin == "bevel") {
      steps.push_back({p1, false, 0});
      steps.push_back({p2, false, 0});
      continue;
    }

    if (const auto intersection = line_intersection(p1, s1.dir, p2, s2.dir)) {
      if (is_outer) {
        const Point offset = *intersection - s1.end;
        const double miter_distance = point_length(offset);
        if (miter_distance <= miter_limit * half_width) {
          steps.push_back({*intersection, false, 0});
        } else {
          steps.push_back({p1, false, 0});
          steps.push_back({p2, false, 0});
        }
      } else {
        steps.push_back({*intersection, false, 0});
      }
    } else {
      steps.push_back({p1, false, 0});
    }
  }
  return steps;
}

std::string build_closed_stroke_outline(const std::vector<Point>& points,
                                        double half_width,
                                        const std::string& linejoin,
                                        double miter_limit) {
  std::vector<Point> clean_points = points;
  if (clean_points.size() >= 2) {
    const Point first = clean_points.front();
    const Point last = clean_points.back();
    if (std::abs(first.x - last.x) < 0.01 && std::abs(first.y - last.y) < 0.01) {
      clean_points.pop_back();
    }
  }
  if (clean_points.size() < 3) return "";

  const std::vector<StrokeSegment> segments = build_segments(clean_points, half_width, true);
  if (segments.size() < 2) return "";

  const std::vector<OutlineStep> outer = compute_join_steps(segments, 1, linejoin, half_width, miter_limit);
  const std::vector<OutlineStep> inner = compute_join_steps(segments, -1, linejoin, half_width, miter_limit);
  if (outer.empty() || inner.empty()) return "";

  std::string d = "M" + fmt(outer.front().point.x) + "," + fmt(outer.front().point.y);
  for (std::size_t i = 1; i < outer.size(); ++i) {
    d = append_line_or_arc(d, outer[i - 1].point, outer[i].point, outer[i].arc, outer[i].sweep, half_width);
  }
  d += "Z";

  d += " M" + fmt(inner.back().point.x) + "," + fmt(inner.back().point.y);
  for (std::size_t i = inner.size() - 1; i-- > 0;) {
    const OutlineStep& from = inner[i + 1];
    const OutlineStep& to = inner[i];
    const bool arc = from.arc;
    const int sweep = from.sweep == 1 ? 0 : 1;
    d = append_line_or_arc(d, from.point, to.point, arc, sweep, half_width);
  }
  if (inner.front().arc) {
    d += "A" + fmt(half_width) + "," + fmt(half_width) + " 0 0 " +
         std::to_string(inner.front().sweep == 1 ? 0 : 1) + " " +
         fmt(inner.back().point.x) + "," + fmt(inner.back().point.y);
  }
  d += "Z";
  return d;
}

std::string build_open_stroke_outline(const std::vector<Point>& points,
                                      double half_width,
                                      const std::string& linejoin,
                                      const std::string& linecap,
                                      double miter_limit) {
  const std::vector<StrokeSegment> segments = build_segments(points, half_width, false);
  if (segments.empty()) return "";

  std::vector<OutlineStep> left;
  std::vector<OutlineStep> right;
  const StrokeSegment& first = segments.front();
  const StrokeSegment& last = segments.back();

  if (linecap == "square") {
    const Point extension = first.dir * (-half_width);
    left.push_back({first.start + extension + first.normal, false, 0});
    right.push_back({first.start + extension - first.normal, false, 0});
  } else {
    left.push_back({first.start + first.normal, false, 0});
    right.push_back({first.start - first.normal, false, 0});
  }

  for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
    const StrokeSegment& s1 = segments[i];
    const StrokeSegment& s2 = segments[i + 1];
    const double cross = s1.dir.x * s2.dir.y - s1.dir.y * s2.dir.x;

    for (int side_index = 0; side_index < 2; ++side_index) {
      const bool is_left = side_index == 0;
      const int sign = is_left ? 1 : -1;
      std::vector<OutlineStep>& steps = is_left ? left : right;
      const Point p1{s1.end.x + sign * s1.normal.x, s1.end.y + sign * s1.normal.y};
      const Point p2{s2.start.x + sign * s2.normal.x, s2.start.y + sign * s2.normal.y};
      const bool is_outer = (is_left && cross < 0) || (!is_left && cross > 0);

      if (is_outer && linejoin == "round") {
        steps.push_back({p1, false, 0});
        steps.push_back({p2, true, cross > 0 ? 1 : 0});
        continue;
      }

      if (is_outer && linejoin == "bevel") {
        steps.push_back({p1, false, 0});
        steps.push_back({p2, false, 0});
        continue;
      }

      if (const auto intersection = line_intersection(p1, s1.dir, p2, s2.dir)) {
        if (is_outer) {
          const Point offset = *intersection - s1.end;
          const double miter_distance = point_length(offset);
          if (miter_distance <= miter_limit * half_width) {
            steps.push_back({*intersection, false, 0});
          } else {
            steps.push_back({p1, false, 0});
            steps.push_back({p2, false, 0});
          }
        } else {
          steps.push_back({*intersection, false, 0});
        }
      } else {
        steps.push_back({p1, false, 0});
      }
    }
  }

  if (linecap == "square") {
    const Point extension = last.dir * half_width;
    left.push_back({last.end + extension + last.normal, false, 0});
    right.push_back({last.end + extension - last.normal, false, 0});
  } else {
    left.push_back({last.end + last.normal, false, 0});
    right.push_back({last.end - last.normal, false, 0});
  }

  std::string d;
  if (linecap == "round") {
    d = "M" + fmt(right.front().point.x) + "," + fmt(right.front().point.y);
    d += "A" + fmt(half_width) + "," + fmt(half_width) + " 0 0 0 " +
         fmt(left.front().point.x) + "," + fmt(left.front().point.y);
  } else {
    d = "M" + fmt(left.front().point.x) + "," + fmt(left.front().point.y);
  }

  for (std::size_t i = 1; i < left.size(); ++i) {
    d = append_line_or_arc(d, left[i - 1].point, left[i].point, left[i].arc, left[i].sweep, half_width);
  }

  if (linecap == "round") {
    d += "A" + fmt(half_width) + "," + fmt(half_width) + " 0 0 0 " +
         fmt(right.back().point.x) + "," + fmt(right.back().point.y);
  } else {
    d += "L" + fmt(right.back().point.x) + "," + fmt(right.back().point.y);
  }

  for (std::size_t i = right.size() - 1; i-- > 0;) {
    const OutlineStep& from = right[i + 1];
    const OutlineStep& to = right[i];
    d = append_line_or_arc(d, from.point, to.point, from.arc, from.sweep == 1 ? 0 : 1, half_width);
  }
  d += "Z";
  return d;
}

enum class CurveKind {
  Line,
  Quadratic,
  Cubic,
};

struct CurveSegment {
  CurveKind kind = CurveKind::Line;
  Point p0{};
  Point p1{};
  Point p2{};
  Point p3{};
};

struct CurveSubpath {
  std::vector<CurveSegment> segments;
  bool closed = false;
};

struct OffsetCurveSegment {
  CurveKind kind = CurveKind::Line;
  Point p0{};
  Point p1{};
  Point p2{};
  Point p3{};
  CurveSegment source{};
};

struct ArcCenterParams {
  Point center{};
  double rx = 0.0;
  double ry = 0.0;
  double phi = 0.0;
  double theta1 = 0.0;
  double delta_theta = 0.0;
};

Point lerp(Point a, Point b, double t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

Point normalize_or_zero(Point p) {
  const double len = point_length(p);
  if (len <= 1e-9) return {0.0, 0.0};
  return {p.x / len, p.y / len};
}

Point left_normal(Point tangent, double offset) {
  return {-tangent.y * offset, tangent.x * offset};
}

Point transform_arc_point(const ArcCenterParams& arc, double theta) {
  const double cos_phi = std::cos(arc.phi);
  const double sin_phi = std::sin(arc.phi);
  const double cos_theta = std::cos(theta);
  const double sin_theta = std::sin(theta);
  return {
    arc.center.x + arc.rx * cos_phi * cos_theta - arc.ry * sin_phi * sin_theta,
    arc.center.y + arc.rx * sin_phi * cos_theta + arc.ry * cos_phi * sin_theta,
  };
}

std::optional<ArcCenterParams> compute_arc_center(Point start,
                                                  double rx,
                                                  double ry,
                                                  double x_axis_rotation_deg,
                                                  int large_arc_flag,
                                                  int sweep_flag,
                                                  Point end) {
  if (rx <= 0.0 || ry <= 0.0) return std::nullopt;
  if (std::abs(start.x - end.x) < 1e-9 && std::abs(start.y - end.y) < 1e-9) return std::nullopt;

  ArcCenterParams arc;
  arc.phi = x_axis_rotation_deg * 3.14159265358979323846 / 180.0;
  const double cos_phi = std::cos(arc.phi);
  const double sin_phi = std::sin(arc.phi);
  const double dx2 = (start.x - end.x) / 2.0;
  const double dy2 = (start.y - end.y) / 2.0;
  const double x1p = cos_phi * dx2 + sin_phi * dy2;
  const double y1p = -sin_phi * dx2 + cos_phi * dy2;

  arc.rx = std::abs(rx);
  arc.ry = std::abs(ry);

  const double lambda = (x1p * x1p) / (arc.rx * arc.rx) + (y1p * y1p) / (arc.ry * arc.ry);
  if (lambda > 1.0) {
    const double scale = std::sqrt(lambda);
    arc.rx *= scale;
    arc.ry *= scale;
  }

  const double rx2 = arc.rx * arc.rx;
  const double ry2 = arc.ry * arc.ry;
  const double x1p2 = x1p * x1p;
  const double y1p2 = y1p * y1p;
  const double denominator = rx2 * y1p2 + ry2 * x1p2;
  if (denominator <= 1e-12) return std::nullopt;

  const double numerator = std::max(0.0, rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2);
  double factor = std::sqrt(numerator / denominator);
  if (large_arc_flag == sweep_flag) factor = -factor;

  const double cxp = factor * ((arc.rx * y1p) / arc.ry);
  const double cyp = factor * (-(arc.ry * x1p) / arc.rx);
  arc.center = {
    cos_phi * cxp - sin_phi * cyp + (start.x + end.x) / 2.0,
    sin_phi * cxp + cos_phi * cyp + (start.y + end.y) / 2.0,
  };

  auto angle_between = [](double ux, double uy, double vx, double vy) {
    return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  };

  const double ux = (x1p - cxp) / arc.rx;
  const double uy = (y1p - cyp) / arc.ry;
  const double vx = (-x1p - cxp) / arc.rx;
  const double vy = (-y1p - cyp) / arc.ry;
  arc.theta1 = std::atan2(uy, ux);
  arc.delta_theta = angle_between(ux, uy, vx, vy);

  if (!sweep_flag && arc.delta_theta > 0) arc.delta_theta -= 2.0 * 3.14159265358979323846;
  if (sweep_flag && arc.delta_theta < 0) arc.delta_theta += 2.0 * 3.14159265358979323846;

  return arc;
}

std::vector<CurveSegment> arc_to_curve_segments(Point start,
                                                double rx,
                                                double ry,
                                                double x_axis_rotation_deg,
                                                int large_arc_flag,
                                                int sweep_flag,
                                                Point end) {
  if (const auto arc = compute_arc_center(start, rx, ry, x_axis_rotation_deg, large_arc_flag, sweep_flag, end)) {
    const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(arc->delta_theta) / (3.14159265358979323846 / 2.0))));
    const double step = arc->delta_theta / static_cast<double>(pieces);
    const double cos_phi = std::cos(arc->phi);
    const double sin_phi = std::sin(arc->phi);
    std::vector<CurveSegment> segments;
    segments.reserve(static_cast<std::size_t>(pieces));

    for (int i = 0; i < pieces; ++i) {
      const double theta0 = arc->theta1 + step * static_cast<double>(i);
      const double theta1 = theta0 + step;
      const double alpha = (4.0 / 3.0) * std::tan((theta1 - theta0) / 4.0);
      const double cos0 = std::cos(theta0);
      const double sin0 = std::sin(theta0);
      const double cos1 = std::cos(theta1);
      const double sin1 = std::sin(theta1);

      const Point p0 = (i == 0) ? start : transform_arc_point(*arc, theta0);
      const Point p3 = (i == pieces - 1) ? end : transform_arc_point(*arc, theta1);
      const std::array<double, 2> c1_unit{cos0 - alpha * sin0, sin0 + alpha * cos0};
      const std::array<double, 2> c2_unit{cos1 + alpha * sin1, sin1 - alpha * cos1};

      const Point c1{
        arc->center.x + arc->rx * cos_phi * c1_unit[0] - arc->ry * sin_phi * c1_unit[1],
        arc->center.y + arc->rx * sin_phi * c1_unit[0] + arc->ry * cos_phi * c1_unit[1],
      };
      const Point c2{
        arc->center.x + arc->rx * cos_phi * c2_unit[0] - arc->ry * sin_phi * c2_unit[1],
        arc->center.y + arc->rx * sin_phi * c2_unit[0] + arc->ry * cos_phi * c2_unit[1],
      };

      segments.push_back({CurveKind::Cubic, p0, c1, c2, p3});
    }
    return segments;
  }

  return {{CurveKind::Line, start, end, {}, {}}};
}

double point_line_distance(Point p, Point a, Point b) {
  const Point ab = b - a;
  const double len = point_length(ab);
  if (len <= 1e-9) return point_length(p - a);
  return std::abs((p.x - a.x) * ab.y - (p.y - a.y) * ab.x) / len;
}

Point curve_start_tangent(const CurveSegment& segment) {
  switch (segment.kind) {
    case CurveKind::Line:
      return normalize_or_zero(segment.p1 - segment.p0);
    case CurveKind::Quadratic: {
      Point tangent = normalize_or_zero(segment.p1 - segment.p0);
      if (point_length(tangent) <= 1e-9) tangent = normalize_or_zero(segment.p2 - segment.p0);
      return tangent;
    }
    case CurveKind::Cubic: {
      Point tangent = normalize_or_zero(segment.p1 - segment.p0);
      if (point_length(tangent) <= 1e-9) tangent = normalize_or_zero(segment.p2 - segment.p0);
      if (point_length(tangent) <= 1e-9) tangent = normalize_or_zero(segment.p3 - segment.p0);
      return tangent;
    }
  }
  return {0.0, 0.0};
}

Point curve_end_tangent(const CurveSegment& segment) {
  switch (segment.kind) {
    case CurveKind::Line:
      return normalize_or_zero(segment.p1 - segment.p0);
    case CurveKind::Quadratic: {
      Point tangent = normalize_or_zero(segment.p2 - segment.p1);
      if (point_length(tangent) <= 1e-9) tangent = normalize_or_zero(segment.p2 - segment.p0);
      return tangent;
    }
    case CurveKind::Cubic: {
      Point tangent = normalize_or_zero(segment.p3 - segment.p2);
      if (point_length(tangent) <= 1e-9) tangent = normalize_or_zero(segment.p3 - segment.p1);
      if (point_length(tangent) <= 1e-9) tangent = normalize_or_zero(segment.p3 - segment.p0);
      return tangent;
    }
  }
  return {0.0, 0.0};
}

double curve_flatness(const CurveSegment& segment) {
  switch (segment.kind) {
    case CurveKind::Line:
      return 0.0;
    case CurveKind::Quadratic:
      return point_line_distance(segment.p1, segment.p0, segment.p2);
    case CurveKind::Cubic:
      return std::max(point_line_distance(segment.p1, segment.p0, segment.p3),
                      point_line_distance(segment.p2, segment.p0, segment.p3));
  }
  return 0.0;
}

double tangent_turn(const CurveSegment& segment) {
  const Point start = curve_start_tangent(segment);
  const Point end = curve_end_tangent(segment);
  const double dot = std::clamp(start.x * end.x + start.y * end.y, -1.0, 1.0);
  return std::acos(dot);
}

bool should_subdivide_curve(const CurveSegment& segment, double half_width, int depth) {
  if (segment.kind == CurveKind::Line) return false;
  if (depth >= 8) return false;
  const double flatness_limit = std::max(0.1, half_width * 0.2);
  return curve_flatness(segment) > flatness_limit || tangent_turn(segment) > 0.35;
}

std::pair<CurveSegment, CurveSegment> split_quadratic(const CurveSegment& segment) {
  const Point p01 = lerp(segment.p0, segment.p1, 0.5);
  const Point p12 = lerp(segment.p1, segment.p2, 0.5);
  const Point p0112 = lerp(p01, p12, 0.5);
  return {
    CurveSegment{CurveKind::Quadratic, segment.p0, p01, p0112, {}},
    CurveSegment{CurveKind::Quadratic, p0112, p12, segment.p2, {}}
  };
}

std::pair<CurveSegment, CurveSegment> split_cubic(const CurveSegment& segment) {
  const Point p01 = lerp(segment.p0, segment.p1, 0.5);
  const Point p12 = lerp(segment.p1, segment.p2, 0.5);
  const Point p23 = lerp(segment.p2, segment.p3, 0.5);
  const Point p0112 = lerp(p01, p12, 0.5);
  const Point p1223 = lerp(p12, p23, 0.5);
  const Point mid = lerp(p0112, p1223, 0.5);
  return {
    CurveSegment{CurveKind::Cubic, segment.p0, p01, p0112, mid},
    CurveSegment{CurveKind::Cubic, mid, p1223, p23, segment.p3}
  };
}

Point offset_polygon_intersection(Point a0, Point a1, Point b0, Point b1, Point fallback) {
  const Point da = a1 - a0;
  const Point db = b1 - b0;
  if (const auto hit = line_intersection(a0, da, b0, db)) return *hit;
  return fallback;
}

OffsetCurveSegment offset_curve_segment_once(const CurveSegment& segment, double half_width, int side) {
  const double offset = half_width * static_cast<double>(side);
  if (segment.kind == CurveKind::Line) {
    const Point tangent = curve_start_tangent(segment);
    const Point normal = left_normal(tangent, offset);
    return {CurveKind::Line, segment.p0 + normal, segment.p1 + normal, {}, {}, segment};
  }

  if (segment.kind == CurveKind::Quadratic) {
    const Point n01 = left_normal(normalize_or_zero(segment.p1 - segment.p0), offset);
    const Point n12 = left_normal(normalize_or_zero(segment.p2 - segment.p1), offset);
    const Point q0 = segment.p0 + n01;
    const Point q2 = segment.p2 + n12;
    const Point q1 = offset_polygon_intersection(
      segment.p0 + n01,
      segment.p1 + n01,
      segment.p1 + n12,
      segment.p2 + n12,
      segment.p1 + Point{(n01.x + n12.x) * 0.5, (n01.y + n12.y) * 0.5});
    return {CurveKind::Quadratic, q0, q1, q2, {}, segment};
  }

  const Point n01 = left_normal(normalize_or_zero(segment.p1 - segment.p0), offset);
  const Point n12 = left_normal(normalize_or_zero(segment.p2 - segment.p1), offset);
  const Point n23 = left_normal(normalize_or_zero(segment.p3 - segment.p2), offset);
  const Point q0 = segment.p0 + n01;
  const Point q3 = segment.p3 + n23;
  const Point q1 = offset_polygon_intersection(
    segment.p0 + n01,
    segment.p1 + n01,
    segment.p1 + n12,
    segment.p2 + n12,
    segment.p1 + Point{(n01.x + n12.x) * 0.5, (n01.y + n12.y) * 0.5});
  const Point q2 = offset_polygon_intersection(
    segment.p1 + n12,
    segment.p2 + n12,
    segment.p2 + n23,
    segment.p3 + n23,
    segment.p2 + Point{(n12.x + n23.x) * 0.5, (n12.y + n23.y) * 0.5});
  return {CurveKind::Cubic, q0, q1, q2, q3, segment};
}

void append_offset_curve_segments(const CurveSegment& segment,
                                  double half_width,
                                  int side,
                                  int depth,
                                  std::vector<OffsetCurveSegment>& out_segments) {
  if (should_subdivide_curve(segment, half_width, depth)) {
    if (segment.kind == CurveKind::Quadratic) {
      const auto [left, right] = split_quadratic(segment);
      append_offset_curve_segments(left, half_width, side, depth + 1, out_segments);
      append_offset_curve_segments(right, half_width, side, depth + 1, out_segments);
      return;
    }
    if (segment.kind == CurveKind::Cubic) {
      const auto [left, right] = split_cubic(segment);
      append_offset_curve_segments(left, half_width, side, depth + 1, out_segments);
      append_offset_curve_segments(right, half_width, side, depth + 1, out_segments);
      return;
    }
  }
  out_segments.push_back(offset_curve_segment_once(segment, half_width, side));
}

std::optional<std::vector<CurveSubpath>> parse_curve_subpaths(const std::string& d) {
  std::vector<CurveSubpath> subpaths;
  std::size_t pos = 0;
  char cmd = 0;
  char prev_cmd = 0;
  Point current{0.0, 0.0};
  Point subpath_start{0.0, 0.0};
  Point last_cubic_ctrl{0.0, 0.0};
  Point last_quad_ctrl{0.0, 0.0};
  bool has_last_cubic = false;
  bool has_last_quad = false;
  CurveSubpath active;

  auto flush_active = [&]() {
    if (!active.segments.empty()) {
      subpaths.push_back(active);
      active = CurveSubpath{};
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
      has_last_cubic = false;
      has_last_quad = false;
      prev_cmd = 'Z';
      if (pos == iteration_start) return std::nullopt;
      continue;
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
        cmd = relative ? 'l' : 'L';
        upper = 'L';
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'M';
      } else if (upper == 'L') {
        double x = 0.0, y = 0.0;
        if (!parse_number_token(d, pos, x) || !parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        const Point next{x, y};
        active.segments.push_back({CurveKind::Line, current, next, {}, {}});
        current = next;
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'L';
      } else if (upper == 'H') {
        double x = 0.0;
        if (!parse_number_token(d, pos, x)) return std::nullopt;
        if (relative) x += current.x;
        const Point next{x, current.y};
        active.segments.push_back({CurveKind::Line, current, next, {}, {}});
        current = next;
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'H';
      } else if (upper == 'V') {
        double y = 0.0;
        if (!parse_number_token(d, pos, y)) return std::nullopt;
        if (relative) y += current.y;
        const Point next{current.x, y};
        active.segments.push_back({CurveKind::Line, current, next, {}, {}});
        current = next;
        has_last_cubic = false;
        has_last_quad = false;
        prev_cmd = 'V';
      } else if (upper == 'Q') {
        double x1=0,y1=0,x=0,y=0;
        if (!parse_number_token(d,pos,x1) || !parse_number_token(d,pos,y1) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x1 += current.x; y1 += current.y; x += current.x; y += current.y; }
        const Point control{x1, y1};
        const Point next{x, y};
        active.segments.push_back({CurveKind::Quadratic, current, control, next, {}});
        current = next;
        last_quad_ctrl = control;
        has_last_quad = true;
        has_last_cubic = false;
        prev_cmd = 'Q';
      } else if (upper == 'T') {
        double x=0,y=0;
        if (!parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        Point control = current;
        if (has_last_quad && (prev_cmd == 'Q' || prev_cmd == 'T')) {
          control = {2 * current.x - last_quad_ctrl.x, 2 * current.y - last_quad_ctrl.y};
        }
        if (relative) { x += current.x; y += current.y; }
        const Point next{x, y};
        active.segments.push_back({CurveKind::Quadratic, current, control, next, {}});
        current = next;
        last_quad_ctrl = control;
        has_last_quad = true;
        has_last_cubic = false;
        prev_cmd = 'T';
      } else if (upper == 'C') {
        double x1=0,y1=0,x2=0,y2=0,x=0,y=0;
        if (!parse_number_token(d,pos,x1) || !parse_number_token(d,pos,y1) ||
            !parse_number_token(d,pos,x2) || !parse_number_token(d,pos,y2) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        if (relative) { x1 += current.x; y1 += current.y; x2 += current.x; y2 += current.y; x += current.x; y += current.y; }
        const Point c1{x1, y1};
        const Point c2{x2, y2};
        const Point next{x, y};
        active.segments.push_back({CurveKind::Cubic, current, c1, c2, next});
        current = next;
        last_cubic_ctrl = c2;
        has_last_cubic = true;
        has_last_quad = false;
        prev_cmd = 'C';
      } else if (upper == 'S') {
        double x2=0,y2=0,x=0,y=0;
        if (!parse_number_token(d,pos,x2) || !parse_number_token(d,pos,y2) ||
            !parse_number_token(d,pos,x) || !parse_number_token(d,pos,y)) return std::nullopt;
        Point c1 = current;
        if (has_last_cubic && (prev_cmd == 'C' || prev_cmd == 'S')) {
          c1 = {2 * current.x - last_cubic_ctrl.x, 2 * current.y - last_cubic_ctrl.y};
        }
        if (relative) { x2 += current.x; y2 += current.y; x += current.x; y += current.y; }
        const Point c2{x2, y2};
        const Point next{x, y};
        active.segments.push_back({CurveKind::Cubic, current, c1, c2, next});
        current = next;
        last_cubic_ctrl = c2;
        has_last_cubic = true;
        has_last_quad = false;
        prev_cmd = 'S';
      } else if (upper == 'A') {
        double rx = 0.0, ry = 0.0, rot = 0.0, x = 0.0, y = 0.0;
        int large = 0, sweep = 0;
        if (!parse_number_token(d, pos, rx) || !parse_number_token(d, pos, ry) ||
            !parse_number_token(d, pos, rot) || !parse_arc_flag(d, pos, large) ||
            !parse_arc_flag(d, pos, sweep) || !parse_number_token(d, pos, x) ||
            !parse_number_token(d, pos, y)) return std::nullopt;
        if (!valid_svg_arc_parameters(rx, ry, large, sweep)) return std::nullopt;
        if (relative) { x += current.x; y += current.y; }
        const Point next{x, y};
        for (const CurveSegment& segment : arc_to_curve_segments(
               current, rx, ry, rot, large, sweep, next)) {
          active.segments.push_back(segment);
        }
        current = next;
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

  flush_active();
  return subpaths;
}

Point offset_segment_start(const OffsetCurveSegment& segment) { return segment.p0; }

Point offset_segment_end(const OffsetCurveSegment& segment) {
  switch (segment.kind) {
    case CurveKind::Line: return segment.p1;
    case CurveKind::Quadratic: return segment.p2;
    case CurveKind::Cubic: return segment.p3;
  }
  return segment.p0;
}

void append_offset_segment_forward(std::string& d, const OffsetCurveSegment& segment) {
  switch (segment.kind) {
    case CurveKind::Line:
      d += "L" + fmt(segment.p1.x) + "," + fmt(segment.p1.y);
      break;
    case CurveKind::Quadratic:
      d += "Q" + fmt(segment.p1.x) + "," + fmt(segment.p1.y) + " " +
           fmt(segment.p2.x) + "," + fmt(segment.p2.y);
      break;
    case CurveKind::Cubic:
      d += "C" + fmt(segment.p1.x) + "," + fmt(segment.p1.y) + " " +
           fmt(segment.p2.x) + "," + fmt(segment.p2.y) + " " +
           fmt(segment.p3.x) + "," + fmt(segment.p3.y);
      break;
  }
}

void append_offset_segment_reverse(std::string& d, const OffsetCurveSegment& segment) {
  switch (segment.kind) {
    case CurveKind::Line:
      d += "L" + fmt(segment.p0.x) + "," + fmt(segment.p0.y);
      break;
    case CurveKind::Quadratic:
      d += "Q" + fmt(segment.p1.x) + "," + fmt(segment.p1.y) + " " +
           fmt(segment.p0.x) + "," + fmt(segment.p0.y);
      break;
    case CurveKind::Cubic:
      d += "C" + fmt(segment.p2.x) + "," + fmt(segment.p2.y) + " " +
           fmt(segment.p1.x) + "," + fmt(segment.p1.y) + " " +
           fmt(segment.p0.x) + "," + fmt(segment.p0.y);
      break;
  }
}

void append_join(std::string& d,
                 Point from,
                 Point to,
                 Point join_point,
                 Point prev_tangent,
                 Point next_tangent,
                 int side,
                 const std::string& linejoin,
                 double half_width,
                 double miter_limit) {
  if (point_length(to - from) <= 1e-5) return;

  const double cross = prev_tangent.x * next_tangent.y - prev_tangent.y * next_tangent.x;
  const bool is_outer = (side > 0 && cross < 0) || (side < 0 && cross > 0);

  if (is_outer && linejoin == "round") {
    d = append_line_or_arc(d, from, to, true, cross > 0 ? 1 : 0, half_width);
    return;
  }

  if (is_outer && linejoin == "miter") {
    if (const auto intersection = line_intersection(from, prev_tangent, to, next_tangent)) {
      if (point_length(*intersection - join_point) <= miter_limit * half_width) {
        if (point_length(*intersection - from) > 1e-5) {
          d += "L" + fmt(intersection->x) + "," + fmt(intersection->y);
        }
      }
    }
  }

  d += "L" + fmt(to.x) + "," + fmt(to.y);
}

std::string build_open_curve_outline(const CurveSubpath& subpath,
                                     double half_width,
                                     const std::string& linecap,
                                     const std::string& linejoin,
                                     double miter_limit) {
  if (subpath.segments.empty()) return "";

  std::vector<OffsetCurveSegment> left_segments;
  std::vector<OffsetCurveSegment> right_segments;
  for (const CurveSegment& segment : subpath.segments) {
    append_offset_curve_segments(segment, half_width, 1, 0, left_segments);
    append_offset_curve_segments(segment, half_width, -1, 0, right_segments);
  }
  if (left_segments.empty() || right_segments.empty()) return "";

  const Point start_tangent = curve_start_tangent(subpath.segments.front());
  const Point end_tangent = curve_end_tangent(subpath.segments.back());
  Point left_start = offset_segment_start(left_segments.front());
  Point right_start = offset_segment_start(right_segments.front());
  Point left_end = offset_segment_end(left_segments.back());
  Point right_end = offset_segment_end(right_segments.back());

  if (linecap == "square") {
    const Point start_extension = start_tangent * (-half_width);
    const Point end_extension = end_tangent * half_width;
    left_start = left_start + start_extension;
    right_start = right_start + start_extension;
    left_end = left_end + end_extension;
    right_end = right_end + end_extension;
  }

  std::string d;
  if (linecap == "round") {
    d = "M" + fmt(right_start.x) + "," + fmt(right_start.y);
    d += "A" + fmt(half_width) + "," + fmt(half_width) + " 0 0 0 " +
         fmt(left_start.x) + "," + fmt(left_start.y);
  } else {
    d = "M" + fmt(left_start.x) + "," + fmt(left_start.y);
    if (point_length(offset_segment_start(left_segments.front()) - left_start) > 1e-5) {
      d += "L" + fmt(offset_segment_start(left_segments.front()).x) + "," + fmt(offset_segment_start(left_segments.front()).y);
    }
  }

  append_offset_segment_forward(d, left_segments.front());
  for (std::size_t i = 1; i < left_segments.size(); ++i) {
    append_join(
      d,
      offset_segment_end(left_segments[i - 1]),
      offset_segment_start(left_segments[i]),
      left_segments[i].source.p0,
      curve_end_tangent(left_segments[i - 1].source),
      curve_start_tangent(left_segments[i].source),
      1,
      linejoin,
      half_width,
      miter_limit);
    append_offset_segment_forward(d, left_segments[i]);
  }

  if (linecap == "round") {
    d += "A" + fmt(half_width) + "," + fmt(half_width) + " 0 0 0 " +
         fmt(right_end.x) + "," + fmt(right_end.y);
  } else {
    if (point_length(left_end - offset_segment_end(left_segments.back())) > 1e-5) {
      d += "L" + fmt(left_end.x) + "," + fmt(left_end.y);
    }
    d += "L" + fmt(right_end.x) + "," + fmt(right_end.y);
  }

  for (std::size_t i = right_segments.size(); i-- > 0;) {
    if (i + 1 < right_segments.size()) {
      append_join(
        d,
        offset_segment_start(right_segments[i + 1]),
        offset_segment_end(right_segments[i]),
        right_segments[i + 1].source.p0,
        curve_start_tangent(right_segments[i + 1].source),
        curve_end_tangent(right_segments[i].source),
        -1,
        linejoin,
        half_width,
        miter_limit);
    }
    append_offset_segment_reverse(d, right_segments[i]);
  }

  if (linecap != "round" && point_length(right_start - offset_segment_start(right_segments.front())) > 1e-5) {
    d += "L" + fmt(right_start.x) + "," + fmt(right_start.y);
  }
  d += "Z";
  return d;
}

std::string build_closed_curve_outline(const CurveSubpath& subpath,
                                       double half_width,
                                       const std::string& linejoin,
                                       double miter_limit) {
  if (subpath.segments.empty()) return "";

  std::vector<OffsetCurveSegment> left_segments;
  std::vector<OffsetCurveSegment> right_segments;
  for (const CurveSegment& segment : subpath.segments) {
    append_offset_curve_segments(segment, half_width, 1, 0, left_segments);
    append_offset_curve_segments(segment, half_width, -1, 0, right_segments);
  }
  if (left_segments.empty() || right_segments.empty()) return "";

  std::string d = "M" + fmt(offset_segment_start(left_segments.front()).x) + "," +
                  fmt(offset_segment_start(left_segments.front()).y);
  append_offset_segment_forward(d, left_segments.front());
  for (std::size_t i = 1; i < left_segments.size(); ++i) {
    append_join(
      d,
      offset_segment_end(left_segments[i - 1]),
      offset_segment_start(left_segments[i]),
      left_segments[i].source.p0,
      curve_end_tangent(left_segments[i - 1].source),
      curve_start_tangent(left_segments[i].source),
      1,
      linejoin,
      half_width,
      miter_limit);
    append_offset_segment_forward(d, left_segments[i]);
  }
  append_join(
    d,
    offset_segment_end(left_segments.back()),
    offset_segment_start(left_segments.front()),
    left_segments.front().source.p0,
    curve_end_tangent(left_segments.back().source),
    curve_start_tangent(left_segments.front().source),
    1,
    linejoin,
    half_width,
    miter_limit);
  d += "Z";

  d += " M" + fmt(offset_segment_end(right_segments.back()).x) + "," +
       fmt(offset_segment_end(right_segments.back()).y);
  for (std::size_t i = right_segments.size(); i-- > 0;) {
    append_offset_segment_reverse(d, right_segments[i]);
    if (i > 0) {
      append_join(
        d,
        offset_segment_start(right_segments[i]),
        offset_segment_end(right_segments[i - 1]),
        right_segments[i].source.p0,
        curve_start_tangent(right_segments[i].source) * -1.0,
        curve_end_tangent(right_segments[i - 1].source) * -1.0,
        -1,
        linejoin,
        half_width,
        miter_limit);
    }
  }
  append_join(
    d,
    offset_segment_start(right_segments.front()),
    offset_segment_end(right_segments.back()),
    right_segments.front().source.p0,
    curve_start_tangent(right_segments.front().source) * -1.0,
    curve_end_tangent(right_segments.back().source) * -1.0,
    -1,
    linejoin,
    half_width,
    miter_limit);
  d += "Z";
  return d;
}

}  // namespace

std::string build_straight_stroke_outline(const std::string& d,
                                          double stroke_width,
                                          const std::string& linecap,
                                          const std::string& linejoin,
                                          double miter_limit) {
  auto subpaths = parse_straight_subpaths(d);
  if (!subpaths) {
    subpaths = flatten_path_subpaths(d);
  }
  if (!subpaths) return "";

  const double half_width = stroke_width / 2.0;
  std::string combined;
  for (const StrokeSubpath& subpath : *subpaths) {
    if (subpath.points.size() < 2) continue;
    std::string part = subpath.closed
      ? build_closed_stroke_outline(subpath.points, half_width, linejoin, miter_limit)
      : build_open_stroke_outline(subpath.points, half_width, linejoin, linecap, miter_limit);
    if (part.empty()) {
      part = build_fallback_polyline_outline(subpath.points, half_width, subpath.closed);
    }
    if (!part.empty()) {
      if (!combined.empty()) combined += " ";
      combined += part;
    }
  }
  return combined;
}

std::string build_curve_fallback_outline(const std::string& d,
                                         double stroke_width,
                                         const std::string& linecap,
                                         const std::string& linejoin,
                                         double miter_limit) {
  const double half_width = stroke_width / 2.0;

  if (const auto curve_subpaths = parse_curve_subpaths(d)) {
    std::string combined;
    for (const CurveSubpath& subpath : *curve_subpaths) {
      if (subpath.segments.empty()) {
        combined.clear();
        break;
      }
      const std::string part = subpath.closed
        ? build_closed_curve_outline(subpath, half_width, linejoin, miter_limit)
        : build_open_curve_outline(subpath, half_width, linecap, linejoin, miter_limit);
      if (part.empty()) {
        combined.clear();
        break;
      }
      if (!combined.empty()) combined += " ";
      combined += part;
    }
    if (!combined.empty()) return combined;
  }

  const auto subpaths = flatten_path_subpaths(d);
  if (!subpaths) return "";

  std::string combined;
  for (const StrokeSubpath& subpath : *subpaths) {
    if (subpath.points.size() < 2) continue;
    const std::string part = build_fallback_polyline_outline(subpath.points, half_width, subpath.closed);
    if (!part.empty()) {
      if (!combined.empty()) combined += " ";
      combined += part;
    }
  }
  return combined;
}

}  // namespace svg_squisher
