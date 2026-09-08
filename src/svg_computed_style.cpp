#include "svg_computed_style.h"

#include <algorithm>
#include <string>

#include "svg_paint.h"
#include "svg_text.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

StrokeLineCap parse_linecap(const std::string& value) {
  const std::string normalized = lower_copy(trim(value));
  if (normalized == "round") return StrokeLineCap::Round;
  if (normalized == "square") return StrokeLineCap::Square;
  if (normalized.empty() || normalized == "butt") return StrokeLineCap::Butt;
  return StrokeLineCap::Unknown;
}

StrokeLineJoin parse_linejoin(const std::string& value) {
  const std::string normalized = lower_copy(trim(value));
  if (normalized == "round") return StrokeLineJoin::Round;
  if (normalized == "bevel") return StrokeLineJoin::Bevel;
  if (normalized.empty() || normalized == "miter") return StrokeLineJoin::Miter;
  return StrokeLineJoin::Unknown;
}

TextAnchorMode parse_text_anchor(const std::string& value) {
  const std::string normalized = lower_copy(trim(value));
  if (normalized == "middle") return TextAnchorMode::Middle;
  if (normalized == "end") return TextAnchorMode::End;
  return TextAnchorMode::Start;
}

double non_negative_number(const std::string& value, double fallback) {
  const double parsed = parse_double_string(value, fallback);
  return parsed >= 0.0 ? parsed : fallback;
}

}  // namespace

ComputedStyle compute_style(const StyleState& style) {
  ComputedStyle computed;
  computed.opacity = std::clamp(parse_double_string(style.opacity, 1.0), 0.0, 1.0);
  computed.fill_opacity = std::clamp(parse_double_string(style.fill_opacity, 1.0), 0.0, 1.0);
  computed.stroke_opacity = std::clamp(parse_double_string(style.stroke_opacity, 1.0), 0.0, 1.0);
  computed.stroke_width = non_negative_number(style.stroke_width, 1.0);
  computed.stroke_miterlimit = parse_double_string(style.stroke_miterlimit, 4.0);
  if (computed.stroke_miterlimit < 1.0) computed.stroke_miterlimit = 4.0;
  computed.font_size = non_negative_number(style.font_size, 16.0);
  computed.letter_spacing = parse_svg_length(style.letter_spacing, 0.0);
  computed.stroke_linecap = parse_linecap(style.stroke_linecap);
  computed.stroke_linejoin = parse_linejoin(style.stroke_linejoin);
  computed.text_anchor = parse_text_anchor(style.text_anchor);
  computed.displayed = lower_copy(trim(style.display)) != "none";
  const std::string visibility = lower_copy(trim(style.visibility));
  computed.visible = visibility != "hidden" && visibility != "collapse";
  computed.has_fill = parse_paint(style.fill, combined_opacity(style.opacity, style.fill_opacity)).visible;
  computed.has_stroke = parse_paint(style.stroke, combined_opacity(style.opacity, style.stroke_opacity)).visible;
  computed.has_dash_pattern = !style.stroke_dasharray.empty() && lower_copy(style.stroke_dasharray) != "none";
  return computed;
}

std::string to_string(StrokeLineCap linecap) {
  switch (linecap) {
    case StrokeLineCap::Round: return "round";
    case StrokeLineCap::Square: return "square";
    case StrokeLineCap::Unknown: return "undefined";
    case StrokeLineCap::Butt:
    default:
      return "butt";
  }
}

std::string to_string(StrokeLineJoin linejoin) {
  switch (linejoin) {
    case StrokeLineJoin::Round: return "round";
    case StrokeLineJoin::Bevel: return "bevel";
    case StrokeLineJoin::Unknown: return "undefined";
    case StrokeLineJoin::Miter:
    default:
      return "miter";
  }
}

}  // namespace svg_squisher
