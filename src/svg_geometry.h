#pragma once

#include <optional>
#include <string>
#include <vector>

#include "svg_transform.h"

namespace svg_squisher {

struct StrokeSubpath {
  std::vector<Point> points;
  bool closed = false;
};

struct BBox {
  double min_x;
  double min_y;
  double max_x;
  double max_y;
};

std::string append_point_cmd(char cmd, Point p);
Point operator+(Point lhs, Point rhs);
Point operator-(Point lhs, Point rhs);
Point operator*(Point lhs, double scalar);
double point_length(Point p);
void bbox_add_point(BBox& box, Point p);
bool bbox_valid(const BBox& box);
double bbox_width(const BBox& box);
double bbox_height(const BBox& box);

std::optional<std::vector<StrokeSubpath>> parse_straight_subpaths(const std::string& d);
std::optional<std::vector<StrokeSubpath>> flatten_path_subpaths(
    const std::string& d,
    std::size_t* semantic_command_count = nullptr);
std::string convert_evenodd_to_nonzero(const std::string& d);
std::optional<BBox> path_bbox(const std::string& d);
bool bbox_contains(const std::optional<BBox>& outer,
                   const std::optional<BBox>& inner,
                   double tolerance = 0.75);

}  // namespace svg_squisher
