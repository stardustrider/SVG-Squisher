#include "svg_entry.h"

#include "svg_computed_style.h"
#include "svg_geometry.h"
#include "svg_paint.h"
#include "svg_util.h"

namespace svg_squisher {

PathEntryAnalysis analyze_path_entry(const PathEntry& entry) {
  PathEntryAnalysis analysis;
  analysis.stroke_width = entry.stroke_width;
  analysis.stroke_dasharray = entry.stroke_dasharray;
  analysis.stroke_miterlimit = entry.stroke_miterlimit;
  analysis.fill_rule = entry.fill_rule;
  analysis.opacity = parse_double_string(entry.opacity, 1.0);
  analysis.has_non_default_opacity = entry.opacity != "1";
  analysis.fill_opacity = parse_double_string(entry.fill_opacity, 1.0);
  analysis.stroke_opacity = parse_double_string(entry.stroke_opacity, 1.0);
  analysis.has_non_default_fill_opacity =
    !entry.fill_opacity.empty() && entry.fill_opacity != "1";
  analysis.has_non_default_stroke_opacity =
    !entry.stroke_opacity.empty() && entry.stroke_opacity != "1";
  StyleState style_for_cap;
  style_for_cap.stroke_linecap = entry.stroke_linecap;
  analysis.stroke_linecap = compute_style(style_for_cap).stroke_linecap;
  StyleState style_for_join;
  style_for_join.stroke_linejoin = entry.stroke_linejoin;
  analysis.stroke_linejoin = compute_style(style_for_join).stroke_linejoin;

  const ParsedPaint parsed_fill = parse_paint(entry.fill, analysis.opacity * analysis.fill_opacity);
  analysis.fill_paint.value = entry.fill;
  analysis.fill_paint.is_gradient = entry.emit_fill && parsed_fill.kind == PaintKind::Url;
  analysis.fill_paint.brightness = paint_brightness(entry.fill);

  const ParsedPaint parsed_stroke = parse_paint(entry.stroke, analysis.opacity * analysis.stroke_opacity);
  analysis.stroke_paint.value = entry.stroke;
  analysis.stroke_paint.is_gradient = entry.emit_stroke && parsed_stroke.kind == PaintKind::Url;
  analysis.stroke_paint.brightness = paint_brightness(entry.stroke);

  analysis.is_stroke_shape =
    entry.emit_stroke ||
    (!entry.emit_stroke && entry.emit_fill && paint_equals(entry.fill, entry.stroke));

  return analysis;
}

}  // namespace svg_squisher
