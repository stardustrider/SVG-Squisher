#include "svg_shape.h"

#include <algorithm>
#include <string>
#include <vector>

#include "svg_stroke.h"
#include "svg_util.h"

namespace svg_squisher {

double attr_double(const pugi::xml_node& node, const char* name, double fallback) {
  if (!node.attribute(name)) return fallback;
  double value = 0.0;
  return parse_finite_length(node.attribute(name).as_string(), value) ? value : fallback;
}

std::string rect_to_path(const pugi::xml_node& node) {
  const double x = attr_double(node, "x");
  const double y = attr_double(node, "y");
  const double w = attr_double(node, "width");
  const double h = attr_double(node, "height");
  double rx = attr_double(node, "rx");
  double ry = attr_double(node, "ry");

  if (w <= 0.0 || h <= 0.0) return "";
  if (rx < 0.0) rx = 0.0;
  if (ry < 0.0) ry = 0.0;
  if (rx > 0.0 && ry <= 0.0) ry = rx;
  if (ry > 0.0 && rx <= 0.0) rx = ry;
  rx = std::min(rx, w / 2.0);
  ry = std::min(ry, h / 2.0);

  if (rx <= 0.0 || ry <= 0.0) {
    return "M" + fmt(x) + "," + fmt(y) +
           "H" + fmt(x + w) +
           "V" + fmt(y + h) +
           "H" + fmt(x) +
           "Z";
  }

  return "M" + fmt(x + rx) + "," + fmt(y) +
         "H" + fmt(x + w - rx) +
         "A" + fmt(rx) + "," + fmt(ry) + " 0 0 1 " + fmt(x + w) + "," + fmt(y + ry) +
         "V" + fmt(y + h - ry) +
         "A" + fmt(rx) + "," + fmt(ry) + " 0 0 1 " + fmt(x + w - rx) + "," + fmt(y + h) +
         "H" + fmt(x + rx) +
         "A" + fmt(rx) + "," + fmt(ry) + " 0 0 1 " + fmt(x) + "," + fmt(y + h - ry) +
         "V" + fmt(y + ry) +
         "A" + fmt(rx) + "," + fmt(ry) + " 0 0 1 " + fmt(x + rx) + "," + fmt(y) +
         "Z";
}

std::string circle_to_path(const pugi::xml_node& node) {
  const double cx = attr_double(node, "cx");
  const double cy = attr_double(node, "cy");
  const double r = attr_double(node, "r");
  if (r <= 0.0) return "";
  return "M" + fmt(cx - r) + "," + fmt(cy) +
         "A" + fmt(r) + "," + fmt(r) + " 0 1 0 " + fmt(cx + r) + "," + fmt(cy) +
         "A" + fmt(r) + "," + fmt(r) + " 0 1 0 " + fmt(cx - r) + "," + fmt(cy) +
         "Z";
}

std::string ellipse_to_path(const pugi::xml_node& node) {
  const double cx = attr_double(node, "cx");
  const double cy = attr_double(node, "cy");
  const double rx = attr_double(node, "rx");
  const double ry = attr_double(node, "ry");
  if (rx <= 0.0 || ry <= 0.0) return "";
  return "M" + fmt(cx - rx) + "," + fmt(cy) +
         "A" + fmt(rx) + "," + fmt(ry) + " 0 1 0 " + fmt(cx + rx) + "," + fmt(cy) +
         "A" + fmt(rx) + "," + fmt(ry) + " 0 1 0 " + fmt(cx - rx) + "," + fmt(cy) +
         "Z";
}

std::string circle_stroke_to_ring(const pugi::xml_node& node, double stroke_width) {
  const double cx = attr_double(node, "cx");
  const double cy = attr_double(node, "cy");
  const double r = attr_double(node, "r");
  if (r <= 0.0 || stroke_width <= 0.0) return "";

  const double outer = r + stroke_width / 2.0;
  const double inner = std::max(0.0, r - stroke_width / 2.0);

  std::string d = "M" + fmt(cx - outer) + "," + fmt(cy) +
                  "A" + fmt(outer) + "," + fmt(outer) + " 0 1 0 " + fmt(cx + outer) + "," + fmt(cy) +
                  "A" + fmt(outer) + "," + fmt(outer) + " 0 1 0 " + fmt(cx - outer) + "," + fmt(cy) +
                  "Z";
  if (inner > 0.0) {
    d += " M" + fmt(cx - inner) + "," + fmt(cy) +
         "A" + fmt(inner) + "," + fmt(inner) + " 0 1 1 " + fmt(cx + inner) + "," + fmt(cy) +
         "A" + fmt(inner) + "," + fmt(inner) + " 0 1 1 " + fmt(cx - inner) + "," + fmt(cy) +
         "Z";
  }
  return d;
}

std::string ellipse_stroke_to_ring(const pugi::xml_node& node, double stroke_width) {
  const double rx = attr_double(node, "rx");
  const double ry = attr_double(node, "ry");
  if (rx <= 0.0 || ry <= 0.0 || stroke_width <= 0.0) return "";

  // Unlike a circle, an ellipse's constant-distance stroke edges are not
  // ellipses with radii adjusted by half the stroke width. Reuse the adaptive
  // normal-offset stroker so thick and eccentric ellipses keep their shape.
  return build_curve_fallback_outline(
      ellipse_to_path(node), stroke_width, "butt", "miter", 4.0);
}

std::string line_to_path(const pugi::xml_node& node) {
  return "M" + fmt(attr_double(node, "x1")) + "," + fmt(attr_double(node, "y1")) +
         "L" + fmt(attr_double(node, "x2")) + "," + fmt(attr_double(node, "y2"));
}

std::string points_to_path(const std::string& points, bool close) {
  const auto parsed = parse_points_list(points);
  if (!parsed || parsed->empty()) return "";
  const std::vector<double>& values = *parsed;

  std::string d = "M" + fmt(values[0]) + "," + fmt(values[1]);
  for (std::size_t i = 2; i + 1 < values.size(); i += 2) {
    d += "L" + fmt(values[i]) + "," + fmt(values[i + 1]);
  }
  if (close) d += "Z";
  return d;
}

std::string node_to_path(const pugi::xml_node& node) {
  const std::string name = node.name();
  if (name == "path") return node.attribute("d").as_string();
  if (name == "rect") return rect_to_path(node);
  if (name == "circle") return circle_to_path(node);
  if (name == "ellipse") return ellipse_to_path(node);
  if (name == "line") return line_to_path(node);
  if (name == "polyline") return points_to_path(node.attribute("points").as_string(), false);
  if (name == "polygon") return points_to_path(node.attribute("points").as_string(), true);
  return "";
}

}  // namespace svg_squisher
