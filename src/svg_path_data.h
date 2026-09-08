#pragma once

#include <optional>
#include <string>
#include <vector>

#include "svg_transform.h"

namespace svg_squisher {

enum class PathSegmentKind {
  Move,
  Line,
  Cubic,
  Quadratic,
  Arc,
  Close,
};

// A normalized path segment. Coordinates are absolute, horizontal and vertical
// lines are represented as Line segments, and smooth curves contain their
// resolved control points. This is the single parsed representation consumed by
// transforms, bounds, flattening, validation, and stroke outlining.
struct PathSegment {
  PathSegmentKind kind = PathSegmentKind::Move;
  Point start{};
  Point control1{};
  Point control2{};
  Point end{};
  double radius_x = 0.0;
  double radius_y = 0.0;
  double x_axis_rotation = 0.0;
  bool large_arc = false;
  bool sweep = false;
};

struct ParsedPath {
  std::vector<PathSegment> segments;
};

std::optional<ParsedPath> parse_path_data(const std::string& data);
std::string serialize_path_data(const ParsedPath& path);

}  // namespace svg_squisher
