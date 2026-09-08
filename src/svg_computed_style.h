#pragma once

#include <string>

#include "svg_style.h"

namespace svg_squisher {

enum class StrokeLineCap {
  Butt,
  Round,
  Square,
  Unknown,
};

enum class StrokeLineJoin {
  Miter,
  Round,
  Bevel,
  Unknown,
};

enum class TextAnchorMode {
  Start,
  Middle,
  End,
};

struct ComputedStyle {
  bool displayed = true;
  bool visible = true;
  bool has_fill = false;
  bool has_stroke = false;
  bool has_dash_pattern = false;
  double stroke_width = 1.0;
  double stroke_miterlimit = 4.0;
  double font_size = 16.0;
  double letter_spacing = 0.0;
  double opacity = 1.0;
  double fill_opacity = 1.0;
  double stroke_opacity = 1.0;
  StrokeLineCap stroke_linecap = StrokeLineCap::Butt;
  StrokeLineJoin stroke_linejoin = StrokeLineJoin::Miter;
  TextAnchorMode text_anchor = TextAnchorMode::Start;
};

ComputedStyle compute_style(const StyleState& style);

std::string to_string(StrokeLineCap linecap);

std::string to_string(StrokeLineJoin linejoin);

}  // namespace svg_squisher
