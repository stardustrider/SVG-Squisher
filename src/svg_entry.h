#pragma once

#include <optional>
#include <string>

#include "svg_computed_style.h"
#include "svg_squisher.h"

namespace svg_squisher {

struct PathPaintInfo {
  std::string value;
  bool is_gradient = false;
  std::optional<double> brightness;
};

struct PathEntryAnalysis {
  double opacity = 1.0;
  bool has_non_default_opacity = false;
  double fill_opacity = 1.0;
  double stroke_opacity = 1.0;
  bool has_non_default_fill_opacity = false;
  bool has_non_default_stroke_opacity = false;
  bool is_stroke_shape = false;
  StrokeLineCap stroke_linecap = StrokeLineCap::Butt;
  StrokeLineJoin stroke_linejoin = StrokeLineJoin::Miter;
  std::string stroke_width;
  std::string stroke_dasharray;
  std::string stroke_miterlimit;
  std::string fill_rule;
  PathPaintInfo fill_paint;
  PathPaintInfo stroke_paint;
};

PathEntryAnalysis analyze_path_entry(const PathEntry& entry);

}  // namespace svg_squisher
